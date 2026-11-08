// 32-bit ARM (ARMv6K, ARM state - the 3DS's ARM11) JIT backend: the core.
//
// See docs/jit-arm32.md. The driver lives in jit_shared.cpp and the codegen policy the native
// backends share in jit_codegen_shared.h; this file provides the code generator's core (function
// setup and teardown, the shared emit loop's hooks, control flow, waits, the interpreter
// fallback, the ZASM stack and the z-register access helpers), and jit_arm32_ops.cpp the data
// commands. jit_a64.cpp is the semantic reference for every command here.
//
// Differences from the a64 backend:
//
// - No register allocator: fixed register roles (jit_arm32_internal.h), with R0-R3, R12 and LR
//   as scratch. Values never live across a command boundary in a scratch register.
// - The D-register cache is off (phase 1): every D-register access is a load or store. The
//   shared flush policy is still applied, over a cache that is never populated.
// - Direct native calls between compiled functions are off: CALLFUNC/RETURNFUNC always go
//   through the driver.
// - No hardware divide: the one division in here (GOTOTABLE's key / 10000) is a reciprocal
//   multiply, exact for every int32 (checked exhaustively against C's truncating division).
//
// Machine frame (8-byte aligned, as the EABI requires at calls):
//
//   [sp, #0], [sp, #4]   spill slots (see load_two_zregs)
//   [sp, #8]...          the saved R4-R12, LR (R12 only to keep the alignment)

#include "zc/scripting/jit/jit_arm32_internal.h"
#include "zc/scripting/jit/arm32/arm32_asm.h"
#include "zc/scripting/jit/arm32/jit_arm32_memory.h"
#include "zc/scripting/jit/jit_codegen_shared.h"
#include "zc/scripting/jit/jit_shared.h"
#include "zc/scripting/jit/jit.h"
#include "base/general.h"
#include "base/util.h"
#include "core/qrs.h"
#include "core/zdefs.h"
#include "components/zasm/defines.h"
#include "components/zasm/pc.h"
#include "components/zasm/serialize.h"
#include "components/zasm/table.h"
#include "zc/ffscript.h"
#include "zc/script_debug.h"
#include "zc/zasm_utils.h"
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <string>

using namespace arm32;

extern ScriptDebugHandle* runtime_script_debug_handle;

namespace {

// Core-private compilation state, beside the Arm32State shared with jit_arm32_ops.cpp.
struct CoreState
{
	Arm32State& s;
	// When to end the "lookahead" for stack pointer bounds checking (and start checking again).
	pc_t num_push_commands_in_row_end_pc = 0;
	// The shared exit for a failed stack bounds check, bound after the epilogue if used.
	Label L_StackOverflow;
};

constexpr int32_t ctx_off(size_t member_offset)
{
	return (int32_t)member_offset;
}
#define CTX(member) ctx_off(offsetof(JittedExecutionContext, member))

// --- C helpers called from compiled code (the same as a64's) ---

void debug_pre_command(int32_t pc, uint32_t sp)
{

	ri->pc = pc;
	ri->sp = sp;
	if (runtime_script_debug_handle)
		runtime_script_debug_handle->pre_command();
}

int32_t get_register_and_restore_sp(int32_t arg, uint32_t sp, pc_t pc)
{

	ri->pc = pc;
	ri->sp = sp;
	return get_register(arg);
}

int32_t get_register_jit(int32_t arg, pc_t pc)
{

	ri->pc = pc;
	return get_register(arg);
}

void set_register_and_restore_sp(int32_t arg, int32_t value, uint32_t sp, pc_t pc)
{

	ri->pc = pc;
	ri->sp = sp;
	set_register(arg, value);
}

void set_register_jit(int32_t arg, int32_t value, pc_t pc)
{

	ri->pc = pc;
	set_register(arg, value);
}

void set_guarded_register(int32_t arg, int32_t value, pc_t pc)
{

	ri->pc = pc;
	do_set(arg, value);
}

// --- small emission helpers ---

// rd <- rn + value, as a sequence of add/sub of encodable 8-bit chunks: no scratch register
// and no flags (so it is safe between a comparison and its consumer, and with rd == rn).
void add_const(Assembler& a, Reg rd, Reg rn, int32_t value)
{
	if (value == 0)
	{
		if (rd != rn)
			a.mov(rd, rn);
		return;
	}

	bool negative = value < 0;
	uint32_t v = negative ? (uint32_t)0 - (uint32_t)value : (uint32_t)value;
	Reg cur = rn;
	while (v)
	{
		int p = __builtin_ctz(v) & ~1;
		uint32_t chunk = v & (0xFFu << p);
		if (negative)
			a.sub_imm(rd, cur, chunk);
		else
			a.add_imm(rd, cur, chunk);
		cur = rd;
		v &= ~chunk;
	}
}

// rt <- [base + offset], using only rt as a temporary for out-of-range offsets.
void load_word(Assembler& a, Reg rt, Reg base, int32_t offset)
{
	if (offset >= -4095 && offset <= 4095)
	{
		a.ldr(rt, base, offset);
		return;
	}

	int32_t low = offset >= 0 ? (offset & 0xFFF) : -(int32_t)((-offset) & 0xFFF);
	add_const(a, rt, base, offset - low);
	a.ldr(rt, rt, low);
}

// [base + offset] <- rt, using `tmp` (!= rt, != base) for out-of-range offsets.
void store_word(Assembler& a, Reg rt, Reg base, int32_t offset, Reg tmp)
{
	if (offset >= -4095 && offset <= 4095)
	{
		a.str(rt, base, offset);
		return;
	}

	int32_t low = offset >= 0 ? (offset & 0xFFF) : -(int32_t)((-offset) & 0xFFF);
	add_const(a, tmp, base, offset - low);
	a.str(rt, tmp, low);
}

// rd <- rd * 10000, without a scratch register or flags (10000 = 625 * 16 = 5^4 * 2^4).
void mul_10000_in_place(Assembler& a, Reg rd)
{
	for (int i = 0; i < 4; i++)
		a.add(rd, rd, rd, Shift::LSL, 2);
	a.mov(rd, rd, Shift::LSL, 4);
}

// A scratch register for an address, other than `avoid`. LR first: data commands use R12 as
// the default scratch of the assembler's _imm forms.
Reg addr_scratch(Reg avoid)
{
	return avoid == LR ? R12 : LR;
}

void set_ctx_pc_imm(Assembler& a, int32_t pc, Reg tmp = R1)
{
	a.mov_imm(tmp, (uint32_t)pc);
	a.str(tmp, kCtx, CTX(pc));
}

// R0 <- result, then the single epilogue.
void exit_with(Arm32State& s, int32_t result)
{
	s.a.mov_imm(R0, (uint32_t)result);
	s.a.b(s.L_End);
}

bool is_d_register(int r)
{
	return r >= D(0) && r < D(INITIAL_D);
}

bool is_global_register(int r)
{
	return r >= GD(0) && r < GD(MAX_GLOBAL_VARIABLES);
}

bool is_script_inst_register(int r)
{
	return r >= SCRIPT_INST_VARS(0) && r < SCRIPT_INST_VARS(MAX_SCRIPT_INST_VARIABLES);
}

} // namespace

// --- z-register access (jit_arm32_internal.h) ---

bool arm32_zreg_access_calls_c(int r, bool is_write)
{
	if (is_d_register(r) || is_global_register(r) || is_script_inst_register(r))
		return false;
	if (r == SWITCHKEY)
		return false;
	if (r == SP || r == SP2)
		return false; // reads are inline; writes are unsupported (a compile-time fatal error)
	if (auto ref = script_ref_for_register_jit(r))
		return is_write && ref->scaled;
	return true;
}

void arm32_call_c(Arm32State& s, const void* fn)
{
	auto& a = s.a;
	// Arguments are already in R0-R3, so only R12 (the call's own scratch) is free here.
	a.mov_imm(R12, (uint32_t)s.pc);
	store_word(a, R12, kRegisters, jit_ri_offset(offsetof(refInfo, pc)), LR);
	store_word(a, kSp, kRegisters, jit_ri_offset(offsetof(refInfo, sp)), LR);
	a.str(kSp, kCtx, CTX(sp));
	a.call(fn, R12);
}

void arm32_load_zreg(Arm32State& s, Reg dst, int r)
{
	auto& a = s.a;

	if (is_d_register(r))
	{
		a.ldr(dst, kRegisters, r * 4);
	}
	else if (is_global_register(r))
	{
		a.ldr(dst, kCtx, CTX(global_registers));
		load_word(a, dst, dst, (r - GD(0)) * 4);
	}
	else if (is_script_inst_register(r))
	{
		a.ldr(dst, kCtx, CTX(script_registers));
		load_word(a, dst, dst, (r - SCRIPT_INST_VARS(0)) * 4);
	}
	else if (r == SP)
	{
		a.mov(dst, kSp);
		mul_10000_in_place(a, dst);
	}
	else if (r == SP2)
	{
		a.mov(dst, kSp);
	}
	else if (r == SWITCHKEY)
	{
		a.mov(dst, kSwitchKey);
	}
	else if (auto ref = script_ref_for_register_jit(r))
	{
		load_word(a, dst, kRegisters, jit_script_ref_offset(ref->ref));
		if (ref->scaled)
			mul_10000_in_place(a, dst);
	}
	else if (does_register_use_stack(r))
	{
		// With the D-register cache off there is nothing to flush for dependent registers.
		a.mov_imm(R0, (uint32_t)r);
		a.mov(R1, kSp);
		a.mov_imm(R2, (uint32_t)s.pc); // Needed for accurate stack trace should an error occur.
		a.call((const void*)get_register_and_restore_sp, R12);
		if (dst != R0)
			a.mov(dst, R0);
	}
	else
	{
		a.mov_imm(R0, (uint32_t)r);
		a.mov_imm(R1, (uint32_t)s.pc); // Needed for accurate stack trace should an error occur.
		a.call((const void*)get_register_jit, R12);
		if (dst != R0)
			a.mov(dst, R0);
	}
}

void arm32_store_zreg(Arm32State& s, int r, Reg src)
{
	auto& a = s.a;

	if (is_d_register(r))
	{
		a.str(src, kRegisters, r * 4);
	}
	else if (is_global_register(r))
	{
		Reg addr = addr_scratch(src);
		a.ldr(addr, kCtx, CTX(global_registers));
		store_word(a, src, addr, (r - GD(0)) * 4, addr == LR ? R12 : LR);
	}
	else if (is_script_inst_register(r))
	{
		Reg addr = addr_scratch(src);
		a.ldr(addr, kCtx, CTX(script_registers));
		store_word(a, src, addr, (r - SCRIPT_INST_VARS(0)) * 4, addr == LR ? R12 : LR);
	}
	else if (r == SP || r == SP2)
	{
		// TODO (as on a64)
		Z_error_fatal("Unimplemented: set SP");
	}
	else if (r == SWITCHKEY)
	{
		a.mov(kSwitchKey, src);
	}
	else if (auto ref = script_ref_for_register_jit(r); ref && !ref->scaled)
	{
		store_word(a, src, kRegisters, jit_script_ref_offset(ref->ref), addr_scratch(src));
	}
	else
	{
		// Everything else (including a scaled ref, whose / 10000 set_register does) goes
		// through C. Move the value into R1 first: src may be R0.
		if (src != R1)
			a.mov(R1, src);
		a.mov_imm(R0, (uint32_t)r);
		if (does_register_use_stack(r))
		{
			a.mov(R2, kSp);
			a.mov_imm(R3, (uint32_t)s.pc); // Needed for accurate stack trace should an error occur.
			a.call((const void*)set_register_and_restore_sp, R12);
		}
		else if (is_guarded_script_register(r))
		{
			// Some registers have an extra check when writing to them.
			a.mov_imm(R2, (uint32_t)s.pc);
			a.call((const void*)set_guarded_register, R12);
		}
		else
		{
			a.mov_imm(R2, (uint32_t)s.pc);
			a.call((const void*)set_register_jit, R12);
		}
	}
}

void arm32_store_zreg_imm(Arm32State& s, int r, int32_t imm)
{
	// R12 holds the value, so a direct store clobbers only R12 and LR (its address scratch); the
	// C paths move it to R1 themselves.
	s.a.mov_imm(R12, (uint32_t)imm);
	arm32_store_zreg(s, r, R12);
}

void arm32_stack_addr(Arm32State& s, Reg dst, Reg index_reg, int offset_words)
{
	s.a.add(dst, kStackBase, index_reg, Shift::LSL, 2);
	add_const(s.a, dst, dst, offset_words * 4);
}

namespace {

// Loads two z-registers, whichever may call into C (which clobbers the other's scratch
// register) first; if both do, the first is kept in a spill slot across the second.
void load_two_zregs(Arm32State& s, Reg da, int ra, Reg db, int rb)
{
	bool a_calls = arm32_zreg_access_calls_c(ra, false);
	bool b_calls = arm32_zreg_access_calls_c(rb, false);
	if (!a_calls)
	{
		arm32_load_zreg(s, db, rb);
		arm32_load_zreg(s, da, ra);
	}
	else if (!b_calls)
	{
		arm32_load_zreg(s, da, ra);
		arm32_load_zreg(s, db, rb);
	}
	else
	{
		arm32_load_zreg(s, da, ra);
		s.a.str(da, kHostSp, kSpillSlot0);
		arm32_load_zreg(s, db, rb);
		s.a.ldr(da, kHostSp, kSpillSlot0);
	}
}

bool core_command_is_compiled(int command)
{
	if (command_is_wait(command))
		return true;
	if (command_uses_comparison_result(command) || command_sets_comparison_result(command))
		return true;

	switch (command)
	{
	case NOP:
	case GOTO:
	case QUIT:
	case CALLFUNC:
	case RETURNFUNC:
	case POP:
	case POPARGS:
	case PUSHR:
	case PUSHV:
	case PUSHARGSR:
	case PUSHARGSV:
	case GOTOTABLE:
	case GOTORANGES:
		return true;
	}

	return false;
}

bool command_is_compiled(int command)
{
	return core_command_is_compiled(command) || arm32_ops_is_compiled(command);
}

bool instruction_is_compiled(const ffscript& op)
{
	return command_is_compiled(op.command) && !jit_instruction_runs_in_interpreter(op);
}

// The D-register cache is off in this phase: the shared flush policy runs over a cache that is
// never populated, so it emits nothing (but stays the single place the flush points are decided,
// for when a cache is added).
struct NoReg {};

struct CacheOps
{
	using Reg = NoReg;

	Arm32State& s;
	DRegCache<NoReg>& cache;

	void emit_store_d(NoReg, int) { Z_error_fatal("[jit ERROR] arm32: the D-register cache is off\n"); }
	void annotate(const char* text) { if (DEBUG_JIT_PRINT_ASM) s.a.comment(text); }
	void flush_cache() { cache.flush(*this); }
	bool is_instruction_compiled(const ffscript& op) { return instruction_is_compiled(op); }
};

// Where a jump to `target_pc` branches to: the target's label, or for a target outside this
// piece, an exit stub that continues there through the driver.
Label goto_target(Arm32State& s, int target_pc)
{
	if (target_pc >= (int)s.start_pc && target_pc <= (int)s.final_pc)
		return s.goto_labels[target_pc];
	auto [it, fresh] = s.exit_stubs.try_emplace(target_pc);
	if (fresh)
		it->second = s.a.new_label();
	return it->second;
}

Label stack_overflow_label(CoreState& cs)
{
	if (!cs.L_StackOverflow.valid())
		cs.L_StackOverflow = cs.s.a.new_label();
	return cs.L_StackOverflow;
}

// Exits with RUNSCRIPT_JIT_STACK_OVERFLOW unless (index + offset) < MAX_STACK_SIZE (unsigned).
// Clobbers R0 (when offset != 0), R12 and the flags.
void check_sp(CoreState& cs, Reg index, int offset = 0)
{
	auto& a = cs.s.a;
	Reg val = index;
	if (offset != 0)
	{
		add_const(a, R0, index, offset);
		val = R0;
	}
	a.cmp_imm(val, MAX_STACK_SIZE, R12);
	a.b(stack_overflow_label(cs), HS);
}

// Check for stack overflows, but only once per contiguous series of PUSH (or POP) commands.
void handle_check_sp_push(CoreState& cs)
{
	auto& s = cs.s;
	if (s.pc >= cs.num_push_commands_in_row_end_pc)
	{
		auto scan = jit_scan_push_run(s.script, s.pc, s.final_pc);
		check_sp(cs, kSp, -scan.max_stack_delta);
		cs.num_push_commands_in_row_end_pc = scan.end_pc;
	}
}

void modify_sp(Arm32State& s, int delta)
{
	add_const(s.a, kSp, kSp, delta);
	s.modified_stack = true;
}

// Writes `val` to the `amount` slots [kSp, kSp + amount - 1] (kSp already decremented).
// Clobbers R1, R2 and the flags.
void do_stack_push_many(Arm32State& s, int amount, Reg val)
{
	auto& a = s.a;
	a.add(R1, kStackBase, kSp, Shift::LSL, 2);
	if (amount < 8)
	{
		for (int i = 0; i < amount; i++)
			a.str(val, R1, 4 * i);
		return;
	}

	a.mov_imm(R2, (uint32_t)amount);
	Label L_loop = a.new_label();
	a.bind(L_loop);
	a.str(val, R1, 0);
	a.add_imm(R1, R1, 4);
	a.sub_imm(R2, R2, 1, R12, AL, true);
	a.b(L_loop, NE);
}

Cond cmp_flags_cond(int cmp, bool& always, bool& never)
{
	always = never = false;
	switch (cmp & CMP_FLAGS)
	{
		case CMP_GT: return GT;
		case CMP_GT|CMP_EQ: return GE;
		case CMP_LT: return LT;
		case CMP_LT|CMP_EQ: return LE;
		case CMP_EQ: return EQ;
		case CMP_GT|CMP_LT: return NE;
		case CMP_GT|CMP_LT|CMP_EQ: always = true; return AL;
		default: never = true; return AL;
	}
}

// The comparison consumers. Everything here must leave the flags alone until the last read of
// them (a following consumer may read them too): mov_imm, ldr/str and add_const don't touch them.
void compile_compare(CoreState& cs, const ffscript& op)
{
	auto& s = cs.s;
	auto& a = s.a;
	int command = op.command;
	int arg1 = op.arg1;
	int arg2 = op.arg2;
	int arg3 = op.arg3;

	auto set_bool = [&](Cond c, uint32_t true_value) {
		a.mov_imm(R0, 0);
		a.mov_imm(R0, true_value, c);
		arm32_store_zreg(s, arg1, R0);
	};

	switch (command)
	{
		case GOTOCMP:
		{
			bool always, never;
			Cond c = cmp_flags_cond(arg2, always, never);
			if (never)
				break;
			a.b(goto_target(s, arg1), always ? AL : c);
			break;
		}
		case GOTOTRUE: a.b(goto_target(s, arg1), EQ); break;
		case GOTOFALSE: a.b(goto_target(s, arg1), NE); break;
		case GOTOMORE: a.b(goto_target(s, arg1), GE); break;
		case GOTOLESS: a.b(goto_target(s, arg1), get_qr(qr_GOTOLESSNOTEQUAL) ? LT : LE); break;

		case SETCMP:
		{
			uint32_t true_value = (arg2 & CMP_SETI) ? 10000 : 1;
			bool always, never;
			Cond c = cmp_flags_cond(arg2, always, never);
			if (never)
				arm32_store_zreg_imm(s, arg1, 0);
			else if (always)
				arm32_store_zreg_imm(s, arg1, (int32_t)true_value);
			else
				set_bool(c, true_value);
			break;
		}
		case SETTRUE: set_bool(EQ, 1); break;
		case SETTRUEI: set_bool(EQ, 10000); break;
		case SETFALSE: set_bool(NE, 1); break;
		case SETFALSEI: set_bool(NE, 10000); break;
		case SETMORE: set_bool(GE, 1); break;
		case SETMOREI: set_bool(GE, 10000); break;
		case SETLESS: set_bool(LE, 1); break;
		case SETLESSI: set_bool(LE, 10000); break;

		case STACKWRITEATVV_IF:
		{
			// TODO (as on a64): needs to check for stack overflow.
			// Write the value arg1 on the stack at offset arg2, but only conditionally.
			bool always, never;
			Cond c = cmp_flags_cond(arg3, always, never);
			if (never)
				break;
			arm32_stack_addr(s, R1, kSp, arg2);
			a.mov_imm(R0, (uint32_t)arg1);
			a.str(R0, R1, 0, always ? AL : c);
			break;
		}

		default:
			Z_error_fatal("[jit ERROR] Unimplemented command: %s", zasm_op_to_string(command, arg1, arg2, arg3, nullptr, nullptr).c_str());
	}
}

// R0 <- 0/1 for whether R0 is nonzero (R0 is a fresh copy of the register, so it may change).
void cast_bool_r0(Assembler& a)
{
	a.cmp_imm(R0, 0);
	a.mov_imm(R0, 1, NE);
}

bool next_reads_cmp_bool(Arm32State& s)
{
	const auto& next = s.script->zasm[s.pc + 1];
	return (next.command == GOTOCMP || next.command == SETCMP) && (next.arg2 & CMP_BOOL);
}

// Defer to the ZASM command interpreter for 1+ commands.
void compile_command_interpreter(CoreState& cs, int count, bool is_wait = false)
{
	auto& s = cs.s;
	auto& a = s.a;

	// Compiled code keeps SWITCHKEY in kSwitchKey, the interpreter in ri->switchkey. Data
	// commands may be left to the interpreter one by one (see docs/jit-arm32.md), so an
	// interpreted command could read or write it: hand it over around any batch that names it.
	bool uses_switchkey = false;
	for (int j = 0; j < count; j++)
	{
		const auto& op = s.script->zasm[s.pc + j];
		if (op.arg1 == SWITCHKEY || op.arg2 == SWITCHKEY || op.arg3 == SWITCHKEY)
			uses_switchkey = true;
	}
	int32_t switchkey_offset = jit_ri_offset(offsetof(refInfo, switchkey));
	if (uses_switchkey)
		store_word(a, kSwitchKey, kRegisters, switchkey_offset, LR);

	a.ldr(R0, kCtx, CTX(j_instance));
	a.mov_imm(R1, (uint32_t)s.pc);
	a.mov(R2, kSp);
	if (count == 1)
	{
		a.call((const void*)run_script_jit_one, R12);
	}
	else
	{
		a.mov_imm(R3, (uint32_t)count);
		a.call((const void*)run_script_jit_sequence, R12);
	}

	if (is_wait)
	{
		set_ctx_pc_imm(a, s.pc + 1);
		// Only wait if the return value is RUNSCRIPT_STOPPED (this supports conditional waits).
		// If actually waiting, the ret code stays RUNSCRIPT_OK (ctx->ret_code's default).
		a.cmp_imm(R0, RUNSCRIPT_STOPPED);
		a.mov_imm(R0, EXEC_RESULT_EXIT, EQ);
		a.b(s.L_End, EQ);

		a.bind(s.resume_labels[s.pc]);
		return;
	}

	if (uses_switchkey)
		load_word(a, kSwitchKey, kRegisters, switchkey_offset);

	if (jit_instructions_move_sp(s.script, s.pc, count))
	{
		// See command_moves_sp.
		load_word(a, kSp, kRegisters, jit_ri_offset(offsetof(refInfo, sp)));
		s.modified_stack = true;
	}

	bool could_return_not_ok = false;
	for (int j = 0; j < count; j++)
	{
		if (instruction_could_return_not_ok(s.script->zasm[s.pc + j]))
		{
			could_return_not_ok = true;
			break;
		}
	}

	if (could_return_not_ok)
	{
		// If it isn't RUNSCRIPT_OK, exit with it. The interpreter already set ri->pc to where the
		// script resumes (like after a Hero->Lift() of itself), so exit with that pc.
		Label L_noret = a.new_label();
		a.cmp_imm(R0, RUNSCRIPT_OK);
		a.b(L_noret, EQ);
		a.str(R0, kCtx, CTX(ret_code));
		load_word(a, R1, kRegisters, jit_ri_offset(offsetof(refInfo, pc)));
		a.str(R1, kCtx, CTX(pc));
		exit_with(s, EXEC_RESULT_EXIT);
		a.bind(L_noret);
	}
}

// Every command here must be reflected in core_command_is_compiled!
void compile_single_command(CoreState& cs, const ffscript& instr)
{
	auto& s = cs.s;
	auto& a = s.a;
	int command = instr.command;
	int arg1 = instr.arg1;
	int arg2 = instr.arg2;

	if (!core_command_is_compiled(command))
	{
		arm32_ops_compile(s, instr);
		return;
	}

	switch (command)
	{
		case NOP:
			break;
		case QUIT:
		{
			compile_command_interpreter(cs, 1);
			a.mov_imm(R1, RUNSCRIPT_STOPPED);
			a.str(R1, kCtx, CTX(ret_code));
			exit_with(s, EXEC_RESULT_EXIT);
		}
		break;
		case GOTO:
		{
			// Mostly all GOTOs are within the same function. The only exception is the end of
			// the compiler-generated Init script (the part that sets up globals), which jumps to
			// the user-defined init script; and pieces of a function jump between each other.
			// Out-of-piece targets go through an exit stub.
			a.b(goto_target(s, arg1));
		}
		break;
		case CALLFUNC:
		{
			// Through the driver (direct native calls are off in this phase).
			set_ctx_pc_imm(a, s.pc);
			a.mov_imm(R1, (uint32_t)arg1);
			a.str(R1, kCtx, CTX(call_pc));
			exit_with(s, EXEC_RESULT_CALL);
			if (s.pc != s.final_pc)
				a.bind(s.resume_labels[s.pc]);
		}
		break;
		case RETURNFUNC:
		{
			exit_with(s, EXEC_RESULT_RETURN);
		}
		break;
		case PUSHV:
		{
			handle_check_sp_push(cs);
			modify_sp(s, -1);
			a.mov_imm(R0, (uint32_t)arg1);
			a.str(R0, kStackBase, kSp, 2);
		}
		break;
		case PUSHR:
		{
			handle_check_sp_push(cs);
			// Read the register before moving sp (it may read the stack through C).
			arm32_load_zreg(s, R0, arg1);
			modify_sp(s, -1);
			a.str(R0, kStackBase, kSp, 2);
		}
		break;
		case PUSHARGSR:
		case PUSHARGSV:
		{
			handle_check_sp_push(cs);
			if (arg2 < 1)
				break; // do nothing

			if (command == PUSHARGSR)
				arm32_load_zreg(s, R0, arg1);
			else
				a.mov_imm(R0, (uint32_t)arg1);
			modify_sp(s, -arg2);
			do_stack_push_many(s, arg2, R0);
		}
		break;
		case POP:
		{
			a.ldr(R0, kStackBase, kSp, 2);
			modify_sp(s, 1);
			arm32_store_zreg(s, arg1, R0);
		}
		break;
		case POPARGS:
		{
			// ri->sp += num;
			modify_sp(s, arg2);
			// word read = ri->sp - 1;
			a.sub_imm(R1, kSp, 1);
			check_sp(cs, R1);
			// Skip setting the "null" register (unless runtime debugging).
			if (arg1 != D(5) || s.runtime_debugging)
			{
				a.ldr(R0, kStackBase, R1, 2);
				arm32_store_zreg(s, arg1, R0);
			}
		}
		break;
		case GOTORANGES:
		{
			// Range search; see the opcode's comment in defines.h. Binary search over the sorted
			// ranges, comparing the key itself - no index math, so non-integral keys inside a
			// range work like the interpreter's.
			const auto& table = *s.script->literals.vec(instr);
			size_t num_ranges = gotoranges_count(table);
			Label default_label = goto_target(s, table[0]);
			if (num_ranges == 0)
			{
				a.b(default_label);
				break;
			}

			arm32_load_zreg(s, R0, arg1);
			// Each leaf still bounds-checks its own range: the search only narrows to the range a
			// matching key would be in, and a key in a gap between ranges reaches a leaf without
			// belonging to it.
			auto emit_tree = [&](auto&& self, size_t lo, size_t hi) -> void {
				if (lo == hi)
				{
					auto range = gotoranges_at(table, lo);
					a.cmp_imm(R0, range.start, R12);
					a.b(default_label, LT);
					a.cmp_imm(R0, range.end, R12);
					a.b(default_label, GT);
					a.b(goto_target(s, range.target));
					return;
				}
				size_t mid = (lo + hi + 1) / 2;
				Label right = a.new_label();
				a.cmp_imm(R0, gotoranges_at(table, mid).start, R12);
				a.b(right, GE);
				self(self, lo, mid - 1);
				a.bind(right);
				self(self, mid, hi);
			};
			emit_tree(emit_tree, 0, num_ranges - 1);
		}
		break;
		case GOTOTABLE:
		{
			// Dense-switch dispatch; see the opcode's comment in defines.h. Emitted as a balanced
			// compare tree over the table index.
			const auto& table = *s.script->literals.vec(instr);
			int32_t min_key = table[0];
			size_t num_targets = table.size() - 2;
			Label default_label = goto_target(s, table[1]);
			if (num_targets == 0)
			{
				a.b(default_label);
				break;
			}

			auto runs = gototable_compress(table);

			// R0 = idx = key - min_key
			arm32_load_zreg(s, R0, arg1);
			a.sub_imm(R0, R0, (uint32_t)min_key, R12);
			// R1 = q = idx / 10000, truncating: (smulh(idx, 0x68DB8BAD) >> 12) - (idx >> 31).
			a.mov_imm(R1, 0x68DB8BAD);
			a.smull(R2, R3, R0, R1);
			a.mov(R1, R0, Shift::ASR, 31);
			a.rsb(R1, R1, R3, Shift::ASR, 12);
			// A key out of range, or not an exact multiple of 10000 past min_key, takes the
			// default target.
			a.mov_imm(R2, 10000);
			a.mul(R3, R1, R2);
			a.cmp(R0, R3);
			a.b(default_label, NE);
			a.cmp_imm(R1, 0, R12);
			a.b(default_label, LT);
			a.cmp_imm(R1, (int32_t)num_targets, R12);
			a.b(default_label, GE);

			// Binary search over runs of equal targets.
			auto emit_tree = [&](auto&& self, size_t lo, size_t hi) -> void {
				if (lo == hi)
				{
					a.b(goto_target(s, runs[lo].target));
					return;
				}
				size_t mid = (lo + hi + 1) / 2;
				Label right = a.new_label();
				a.cmp_imm(R1, runs[mid].start, R12);
				a.b(right, GE);
				self(self, lo, mid - 1);
				a.bind(right);
				self(self, mid, hi);
			};
			emit_tree(emit_tree, 0, runs.size() - 1);
		}
		break;

		// Comparison producers.
		case STRCMPR:
		case STRICMPR:
		{
			// The helper returns the strcmp result, and comparing it against 0 yields exactly the
			// flags check_cmp derives from a string comparison.
			load_two_zregs(s, R0, arg1, R1, arg2);
			a.mov_imm(R2, command == STRICMPR ? 1 : 0);
			a.mov_imm(R3, (uint32_t)s.pc);
			a.call((const void*)jit_string_compare, R12);
			a.cmp_imm(R0, 0);
		}
		break;
		case COMPAREV:
		{
			int val = arg2;
			arm32_load_zreg(s, R0, arg1);
			if (next_reads_cmp_bool(s))
			{
				val = val ? 1 : 0;
				cast_bool_r0(a);
			}
			a.cmp_imm(R0, val, R12);
		}
		break;
		case COMPAREV2:
		{
			int val = arg1;
			arm32_load_zreg(s, R0, arg2);
			if (next_reads_cmp_bool(s))
			{
				val = val ? 1 : 0;
				cast_bool_r0(a);
			}
			a.mov_imm(R1, (uint32_t)val);
			a.cmp(R1, R0);
		}
		break;
		case COMPARER:
		{
			load_two_zregs(s, R0, arg1, R1, arg2);
			if (next_reads_cmp_bool(s))
			{
				a.cmp_imm(R0, 0);
				a.mov_imm(R0, 1, NE);
				a.cmp_imm(R1, 0);
				a.mov_imm(R1, 1, NE);
			}
			a.cmp(R0, R1);
		}
		break;

		default:
			Z_error_fatal("[jit ERROR] Unimplemented command: %s", zasm_op_to_string(command, arg1, arg2, instr.arg3, nullptr, nullptr).c_str());
	}
}

// Hooks for the shared emit loop (jit_emit_function_body in jit_codegen_shared.h).
struct LoopOps
{
	CoreState& cs;
	DRegCache<NoReg>& cache;
	zasm_script* script;
	JittedScript* j_script;
	const JitFunctionAnalysis& analysis;

	void set_pc(pc_t i) { cs.s.pc = i; }
	void cache_flush_policy(pc_t i, pc_t block_id, bool is_block_start)
	{
		CacheOps ops{cs.s, cache};
		jit_reg_cache_flush_policy(ops, cache, j_script, analysis, script, i, block_id, is_block_start);
	}
	void loop_extras([[maybe_unused]] int command) {}
	bool has_goto_label(pc_t i) { return cs.s.goto_labels.contains(i); }
	void bind_goto_label(pc_t i) { cs.s.a.bind(cs.s.goto_labels[i]); }
	void emit_comment_nop(const char* text)
	{
		cs.s.a.comment(text);
		cs.s.a.nop();
	}
	void emit_debug_pre_command(pc_t i)
	{
		cs.s.a.mov_imm(R0, (uint32_t)i);
		cs.s.a.mov(R1, kSp);
		cs.s.a.call((const void*)debug_pre_command, R12);
	}
	bool is_command_compiled(int command) { return ::command_is_compiled(command); }
	bool is_instruction_compiled(const ffscript& op) { return instruction_is_compiled(op); }
	void compile_compare(const ffscript& op) { ::compile_compare(cs, op); }
	void compile_wait() { compile_command_interpreter(cs, 1, true); }
	void compile_uncompiled_batch(int count) { compile_command_interpreter(cs, count); }
	void compile_command(const ffscript& op) { compile_single_command(cs, op); }
};

// jit_create_labels wants a compiler with newLabel().
struct LabelFactory
{
	Assembler& a;
	Label newLabel() { return a.new_label(); }
};

bool exec_available()
{
	static bool available = jit_arm32_exec_available();
	return available;
}

} // namespace

std::optional<JittedFunction> jit_backend_compile_function(zasm_script* script, JittedScript* j_script, pc_t piece_id)
{
	if (!exec_available())
		return std::nullopt;

	const JitPiece& piece = j_script->pieces[piece_id];
	const ZasmFunction& fn = j_script->structured_zasm.functions[piece.fn_id];
	pc_t start_pc = piece.start_pc;
	pc_t final_pc = piece.final_pc;

	size_t size_no_nops = jit_piece_size_no_nops(script, start_pc, final_pc);

	auto start_time = std::chrono::steady_clock::now();

	bool runtime_debugging = script_debug_is_runtime_debugging() == 2;

	jit_printf("[jit] compile function start (name: %s, start: %d, len: %zu)\n", fn.name().c_str(), start_pc, size_no_nops);

	Assembler a;
	Arm32State s{
		.a = a,
		.script = script,
		.j_script = j_script,
		.pc = start_pc,
		.start_pc = start_pc,
		.final_pc = final_pc,
		.runtime_debugging = runtime_debugging,
	};
	CoreState cs{.s = s};
	DRegCache<NoReg> cache;

	LabelFactory label_factory{a};
	jit_create_labels(s, label_factory, script, fn, start_pc, final_pc);
	s.L_End = a.new_label();

	// Prologue. R12 is saved only to keep the frame 8-byte aligned; the 8 bytes below the saved
	// registers are spill slots.
	a.push(reg_mask(R4) | reg_mask(R5) | reg_mask(R6) | reg_mask(R7) | reg_mask(R8) | reg_mask(R9) |
		reg_mask(R10) | reg_mask(R11) | reg_mask(R12) | reg_mask(LR));
	a.sub_imm(kHostSp, kHostSp, 8);
	a.mov(kCtx, R0);
	a.ldr(kRegisters, kCtx, CTX(registers));
	a.ldr(kStackBase, kCtx, CTX(stack_base));
	a.ldr(kSp, kCtx, CTX(sp));
	// A SWITCHKEY read can occur linearly before any write (e.g. a branch target that saves the
	// outer switch's key), so give it a defined start value, like a64.
	a.mov_imm(kSwitchKey, 0);

	// If resuming after a call or wait, or entering a piece mid-way (see JitPiece), jump to the
	// resume label at ctx->resume_address. A resume label bound at the command with pc K is where
	// execution resumes at K+1. Zero means a normal function start.
	if (!s.resume_labels.empty())
	{
		a.ldr(R0, kCtx, CTX(resume_address));
		a.cmp_imm(R0, 0);
		a.bx(R0, NE);
	}

	std::string comment;
	std::map<int, int> uncompiled_command_counts;

	JitFunctionAnalysis analysis_storage;
	const JitFunctionAnalysis& analysis = jit_analyze_function(script, j_script, fn, analysis_storage);
	LoopOps loop_ops{cs, cache, script, j_script, analysis};
	jit_emit_function_body(loop_ops, script, j_script, analysis, start_pc, final_pc, runtime_debugging, comment, uncompiled_command_counts);

	if (DEBUG_JIT_PRINT_ASM)
	{
		a.comment("fall-thru");
		a.nop();
	}

	if (final_pc != fn.final_pc)
	{
		// Continue in the function's next piece.
		CacheOps ops{s, cache};
		jit_reg_cache_flush_at_piece_end(ops, cache, j_script, analysis, script, final_pc);
		set_ctx_pc_imm(a, final_pc + 1);
		a.mov_imm(R0, EXEC_RESULT_CONTINUE);
	}
	else if (fn.id == j_script->structured_zasm.functions.back().id)
	{
		// The last command for the last function is 0xFFFF. But, it's not included as part of the
		// function bounds (final_pc will be the one just before it - see zasm_construct_structured).
		// Global init scripts rely on this to actually end the script.
		a.mov_imm(R1, RUNSCRIPT_STOPPED);
		a.str(R1, kCtx, CTX(ret_code));
		a.mov_imm(R0, EXEC_RESULT_EXIT);
	}
	else
	{
		a.mov_imm(R0, EXEC_RESULT_UNKNOWN);
	}

	// The single epilogue. Every exit branches here with the exec result in R0. kSp is written
	// back unconditionally: it was loaded from ctx->sp, so this is always correct, and it is a
	// single store.
	a.bind(s.L_End);
	a.comment("epilogue");
	a.str(kSp, kCtx, CTX(sp));
	a.add_imm(kHostSp, kHostSp, 8);
	a.pop(reg_mask(R4) | reg_mask(R5) | reg_mask(R6) | reg_mask(R7) | reg_mask(R8) | reg_mask(R9) |
		reg_mask(R10) | reg_mask(R11) | reg_mask(R12) | reg_mask(kHostPc));

	// Jumps out of this piece: continue at the target pc through the driver.
	for (auto& [pc, label] : s.exit_stubs)
	{
		a.bind(label);
		if (DEBUG_JIT_PRINT_ASM)
			a.comment(fmt::format("exit stub -> {}", pc));
		set_ctx_pc_imm(a, pc);
		exit_with(s, EXEC_RESULT_CONTINUE);
	}

	if (cs.L_StackOverflow.valid())
	{
		a.bind(cs.L_StackOverflow);
		a.comment("stack overflow");
		a.mov_imm(R1, RUNSCRIPT_JIT_STACK_OVERFLOW);
		a.str(R1, kCtx, CTX(ret_code));
		exit_with(s, EXEC_RESULT_EXIT);
	}

	if (!a.finalize())
	{
		// The generated code is unusable, so fall back to the interpreter. jit_error makes it
		// abort under -jit-fatal-compile-errors (CI).
		jit_error("[jit ERROR] discarding function due to emit error: %s (name: %s, start: %d, len: %zu)\n", a.error().c_str(), fn.name().c_str(), start_pc, size_no_nops);
		return std::nullopt;
	}

	const auto& bytes = a.code();
	void* base = jit_arm32_exec_publish(bytes.data(), bytes.size());
	if (!base)
	{
		jit_error("[jit ERROR] compile function failed: could not publish %zu bytes of code (name: %s, start: %d)\n", bytes.size(), fn.name().c_str(), start_pc);
		return std::nullopt;
	}

	// Published code is never freed, like the other native backends' (asmjit's JitRuntime keeps
	// everything until exit; jit_release does not free code).

	std::map<pc_t, uintptr_t> pc_to_resume_address;
	for (const auto& [pc, label] : s.resume_labels)
		pc_to_resume_address[pc] = (uintptr_t)base + a.label_offset(label);

	auto end_time = std::chrono::steady_clock::now();
	int32_t compile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

	jit_printf("[jit] compile function end   (name: %s, start: %d, len: %zu, ms: %d)\n", fn.name().c_str(), start_pc, size_no_nops, compile_ms);

	if (auto debug_handle = j_script->debug_handle.get())
	{
		std::string listing = DEBUG_JIT_PRINT_ASM ? a.listing() : std::string();
		jit_print_compile_debug_dump(debug_handle, fn.name(), start_pc, compile_ms, bytes.size(), size_no_nops, uncompiled_command_counts, listing.c_str());
	}

	JittedFunction j_fn{
		.exec = (JittedFunctionImpl)base,
		.id = piece_id,
		.pc_to_resume_address = std::move(pc_to_resume_address),
	};
	return j_fn;
}
