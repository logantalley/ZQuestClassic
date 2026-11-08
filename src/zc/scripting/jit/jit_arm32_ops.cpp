// arm32 JIT backend: the data commands (see jit_arm32_internal.h and docs/jit-arm32.md).
//
// Everything here computes values without changing control flow or the ZASM stack pointer. The
// semantics are those of the a64 backend's compile_single_command (jit_a64.cpp), which match the
// interpreter (ffscript.cpp), with these ARM11-specific choices:
//
// - x / 10000 (rounding toward zero) is a magic multiply (smull), exactly as GCC compiles it.
// - The fixed-point multiply ((int64)a * b) / 10000, the divide ((int64)a * 10000) / b and the
//   modulo call small C helpers below (there is no hardware divide, and a 64-bit division by a
//   constant would otherwise go through __aeabi_ldivmod). Their semantics are checked against the
//   interpreter's formulas by arm32_ops_semantics_test.cpp, which includes this file with
//   ZC_ARM32_OPS_MATH_ONLY defined to get just the pure helpers.
// - Shift counts are masked to 5 bits, like x64/a64 (and the x86 interpreter): an ARM register
//   shift uses the count's low byte instead.
//
// Register use: values live in R0-R3 only; R12 and LR are scratch for constants and the magic
// multiply. A z-register load or store that is not of a D/G register or SWITCHKEY is a call into C
// that clobbers R0-R3, R12 and LR (see arm32_load_zreg), so two-operand commands load in an order
// (or with a save on the native stack) that keeps the first value alive.

#include <cstdint>

namespace arm32_ops_math {

// The magic multiplier for a signed 32-bit x / 10000: (hi32(x * M) >> 12) - (x >> 31).
constexpr uint32_t kDiv10000Magic = 1759218605u; // 0x68DB8BAD
constexpr int kDiv10000Shift = 12;

// (int32)(((int64)a * b) / 10000), rounding toward zero and wrapping to 32 bits like the
// interpreter's do_mult. Only the low 32 bits of the quotient are needed, so the 64-bit
// division is a long division by 10000 in base 2^16 over the product's magnitude, which needs
// only 32-bit divisions by a constant (multiplies).
inline int32_t fixmul(int32_t a, int32_t b)
{
	int64_t p = (int64_t)a * b;
	uint64_t u = p < 0 ? 0 - (uint64_t)p : (uint64_t)p; // <= 2^62
	uint32_t uh = (uint32_t)(u >> 32);
	uint32_t ul = (uint32_t)u;
	// The quotient's bits from uh / 10000 are all above bit 31: only the remainder matters.
	uint32_t r0 = uh % 10000;
	uint32_t t1 = (r0 << 16) | (ul >> 16); // < 10000 * 2^16
	uint32_t q1 = t1 / 10000;              // < 2^16
	uint32_t r1 = t1 % 10000;
	uint32_t t2 = (r1 << 16) | (ul & 0xFFFF);
	uint32_t q2 = t2 / 10000;              // < 2^16
	uint32_t q = (q1 << 16) + q2;
	return (int32_t)(p < 0 ? 0 - q : q);
}

// (int32)(((int64)a * 10000) / b) for b != 0, rounding toward zero and wrapping to 32 bits like
// the interpreter's do_div. Uses a 32-bit division when the scaled dividend fits (it is a multiple
// of 10000, so never INT_MIN, and INT_MIN / -1 cannot happen).
inline int32_t fixdiv(int32_t a, int32_t b)
{
	int64_t n = (int64_t)a * 10000;
	if (n == (int32_t)n)
		return (int32_t)n / b;
	return (int32_t)(n / b);
}

// The interpreter's result for a division by zero: sign(a) * MAX_SIGNED_32, where sign(0) is 1.
inline int32_t fixdiv_by_zero_result(int32_t a)
{
	return a < 0 ? -2147483647 : 2147483647;
}

// a % b for b != 0, like the interpreter's do_mod (x % -1 is 0, which also avoids INT_MIN % -1).
// A modulo by zero is a % 1, which is 0.
inline int32_t mod(int32_t a, int32_t b)
{
	return b == -1 ? 0 : a % b;
}

// FLOOR / CEILING of a fixed-point value, like the interpreter's zfix doFloor / doCeil (the
// a64/x64 double-based sequence gives the same result for every int32, checked by the test).
inline int32_t fixfloor(int32_t v)
{
	int32_t q = v / 10000;
	if (v % 10000 && v < 0)
		--q;
	return (int32_t)((uint32_t)q * 10000u);
}

inline int32_t fixceil(int32_t v)
{
	int32_t q = v / 10000;
	if (v % 10000 && v > 0)
		++q;
	return (int32_t)((uint32_t)q * 10000u);
}

} // namespace arm32_ops_math

#ifndef ZC_ARM32_OPS_MATH_ONLY

#include "zc/scripting/jit/jit_arm32_internal.h"
#include "base/general.h"
#include "base/zapp.h"
#include "components/zasm/defines.h"
#include "core/zdefs.h"
#include "zc/ffscript.h"

using namespace arm32;

extern script_data* curscript;
// From ffscript.cpp (declared locally by the other backends too).
void script_store_object(uint32_t offset, uint32_t new_id);
void script_remove_object_ref(int32_t offset);

// --- C helpers called from compiled code ---
// Called through arm32_call_c (which saves the ZASM pc first) when they can log an error, or
// directly (Assembler::call) when they are pure.

static int32_t arm32_helper_fixmul(int32_t a, int32_t b)
{
	return arm32_ops_math::fixmul(a, b);
}

static int32_t arm32_helper_div(int32_t a, int32_t b)
{
	if (b == 0)
	{
		scripting_log_error_with_context("Attempted to divide by zero!");
		return arm32_ops_math::fixdiv_by_zero_result(a);
	}
	return arm32_ops_math::fixdiv(a, b);
}

static int32_t arm32_helper_div_nonzero(int32_t a, int32_t b)
{
	return arm32_ops_math::fixdiv(a, b);
}

static int32_t arm32_helper_mod(int32_t a, int32_t b)
{
	if (b == 0)
	{
		scripting_log_error_with_context("Attempted to modulo by zero!");
		return 0;
	}
	return arm32_ops_math::mod(a, b);
}

static int32_t arm32_helper_mod_nonzero(int32_t a, int32_t b)
{
	return arm32_ops_math::mod(a, b);
}

static void arm32_helper_log_mod_0()
{
	scripting_log_error_with_context("Attempted to modulo by zero!");
}

static int32_t arm32_helper_floor(int32_t v)
{
	return arm32_ops_math::fixfloor(v);
}

static int32_t arm32_helper_ceil(int32_t v)
{
	return arm32_ops_math::fixceil(v);
}

// jit_pod_write takes six arguments; the EABI passes only four in registers. The object type
// (arg3) is a property of the command, so look it up from the pc instead, as jit_writepodarr does.
static void arm32_helper_pod_write(int32_t arrayptr, int32_t index, int32_t value, int32_t pc)
{
	int32_t type = curscript->zasm_script->zasm[pc].arg3;
	jit_pod_write(arrayptr, index, value, type, pc, 0);
}

// --- emission helpers ---

namespace {

bool is_direct_zreg(int r)
{
	// Read and written without a call into C (see arm32_load_zreg).
	return (r >= D(0) && r < D(INITIAL_D)) ||
		(r >= GD(0) && r < GD(MAX_GLOBAL_VARIABLES)) ||
		r == SWITCHKEY;
}

// dst <- src / 10000, rounding toward zero. dst may equal src. Clobbers R12, LR.
void emit_div10000(Arm32State& s, Reg dst, Reg src)
{
	auto& a = s.a;
	a.mov_imm(R12, arm32_ops_math::kDiv10000Magic);
	a.smull(LR, R12, src, R12);                             // R12 = hi32(src * M)
	a.mov(LR, src, Shift::ASR, 31);                         // LR = src < 0 ? -1 : 0
	a.rsb(dst, LR, R12, Shift::ASR, arm32_ops_math::kDiv10000Shift); // dst = (R12 >> 12) - LR
}

// dst <- src * 10000 (wrapping). dst may equal src. Clobbers R12.
void emit_mul10000(Arm32State& s, Reg dst, Reg src)
{
	s.a.mov_imm(R12, 10000);
	s.a.mul(dst, R12, src);
}

// R0 <- z-register ra, R1 <- z-register rb, whichever of them needs a call into C.
// Clobbers R2, R3, R12, LR.
void load_two(Arm32State& s, int ra, int rb)
{
	if (is_direct_zreg(rb))
	{
		arm32_load_zreg(s, R0, ra);
		arm32_load_zreg(s, R1, rb);
	}
	else if (is_direct_zreg(ra))
	{
		arm32_load_zreg(s, R1, rb);
		arm32_load_zreg(s, R0, ra);
	}
	else
	{
		// Keep rb's value on the native stack across ra's load (8 bytes, to keep the stack
		// 8-byte aligned for the call).
		arm32_load_zreg(s, R0, rb);
		s.a.push(reg_mask(R0) | reg_mask(R1));
		arm32_load_zreg(s, R0, ra);
		s.a.pop(reg_mask(R1) | reg_mask(R2)); // R1 <- the saved R0
	}
}

// dst <- rSFRAME + offset. Clobbers R12 (and LR if rSFRAME were not a D register).
void load_sframe_plus(Arm32State& s, Reg dst, int offset)
{
	arm32_load_zreg(s, dst, rSFRAME);
	if (offset)
		s.a.add_imm(dst, dst, (uint32_t)offset, R12);
}

} // namespace

bool arm32_ops_is_compiled(int command)
{
	switch (command)
	{
	case NOP:
	case SETR:
	case SETV:
	case ADDR:
	case ADDV:
	case SUBR:
	case SUBV:
	case SUBV2:
	case MULTR:
	case MULTV:
	case DIVR:
	case DIVV:
	case MODR:
	case MODV:
	case ANDR:
	case ANDV:
	case ORR:
	case ORV:
	case XORR:
	case XORV:
	case ANDR32:
	case ANDV32:
	case ORR32:
	case ORV32:
	case XORR32:
	case XORV32:
	case BITNOT:
	case BITNOT32:
	case LSHIFTR:
	case LSHIFTV:
	case RSHIFTR:
	case RSHIFTV:
	case LSHIFTR32:
	case LSHIFTV32:
	case RSHIFTR32:
	case RSHIFTV32:
	case ABS:
	case MINR:
	case MINV:
	case MAXR:
	case MAXV:
	case CASTBOOLF:
	case CASTBOOLI:
	case FLOOR:
	case CEILING:
	case LOAD:
	case LOADD:
	case LOADI:
	case STORE:
	case STOREV:
	case STORED:
	case STOREDV:
	case STOREI:
	case STORE_OBJECT:
	case PEEK:
	case STACKWRITEATVV:
	case REF_REMOVE:
	case REF_INC:
	case REF_DEC:
	case MARK_TYPE_STACK:
	case ARRAYSIZE:
	case READPODARRAYR:
	case READPODARRAYV:
	case WRITEPODARRAYRR:
	case WRITEPODARRAYRV:
	case WRITEPODARRAYVR:
	case WRITEPODARRAYVV:
	case WRITEPODARRAY:
	case ZCLASS_READ:
	case ZCLASS_WRITE:
	case ALLOCATEMEMV:
		return true;
	}

	return false;
}

void arm32_ops_compile(Arm32State& s, const ffscript& op)
{
	auto& a = s.a;
	const int command = op.command;
	const int arg1 = op.arg1;
	int arg2 = op.arg2;
	const int32_t pc = (int32_t)s.pc;

	switch (command)
	{
		case NOP:
			break;

		case SETV:
		{
			// For test_jit_runtime_debug_test.
			static bool jit_runtime_debug_test_force_bug = get_flag_bool("-jit-runtime-debug-test-force-bug").value_or(false);
			if (jit_runtime_debug_test_force_bug) arg2++;

			arm32_store_zreg_imm(s, arg1, arg2);
		}
		break;

		case SETR:
		{
			arm32_load_zreg(s, R0, arg2);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		// --- arithmetic ---

		case ADDV:
		case SUBV:
		{
			arm32_load_zreg(s, R0, arg1);
			if (command == ADDV)
				a.add_imm(R0, R0, (uint32_t)arg2, R12);
			else
				a.sub_imm(R0, R0, (uint32_t)arg2, R12);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case ADDR:
		case SUBR:
		{
			load_two(s, arg1, arg2);
			if (command == ADDR)
				a.add(R0, R0, R1);
			else
				a.sub(R0, R0, R1);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case SUBV2:
		{
			// reg[arg2] = arg1 - reg[arg2]
			arm32_load_zreg(s, R0, arg2);
			a.rsb_imm(R0, R0, (uint32_t)arg1, R12);
			arm32_store_zreg(s, arg2, R0);
		}
		break;

		case MULTV:
		case MULTR:
		{
			if (command == MULTV)
			{
				arm32_load_zreg(s, R0, arg1);
				a.mov_imm(R1, (uint32_t)arg2);
			}
			else
			{
				load_two(s, arg1, arg2);
			}
			a.call((const void*)&arm32_helper_fixmul, R12);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case DIVV:
		case DIVR:
		{
			if (command == DIVV)
			{
				arm32_load_zreg(s, R0, arg1);
				a.mov_imm(R1, (uint32_t)arg2);
			}
			else
			{
				load_two(s, arg1, arg2);
			}
			// The interpreter logs and returns sign(a) * MAX_SIGNED_32 for a zero divisor. A64 has
			// no guard for DIVV (the compiler never emits DIVV 0); keep the interpreter's behavior.
			if (command == DIVV && arg2 != 0)
				a.call((const void*)&arm32_helper_div_nonzero, R12);
			else
				arm32_call_c(s, (const void*)&arm32_helper_div);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case MODV:
		{
			if (arg2 == 0)
			{
				// x % 0 logs an error and yields x % 1, which is 0.
				arm32_call_c(s, (const void*)&arm32_helper_log_mod_0);
				arm32_store_zreg_imm(s, arg1, 0);
			}
			else if (arg2 == 1 || arg2 == -1)
			{
				arm32_store_zreg_imm(s, arg1, 0);
			}
			else
			{
				arm32_load_zreg(s, R0, arg1);
				a.mov_imm(R1, (uint32_t)arg2);
				a.call((const void*)&arm32_helper_mod_nonzero, R12);
				arm32_store_zreg(s, arg1, R0);
			}
		}
		break;

		case MODR:
		{
			load_two(s, arg1, arg2);
			arm32_call_c(s, (const void*)&arm32_helper_mod);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case ABS:
		{
			// Wraps for INT_MIN, like the other backends.
			arm32_load_zreg(s, R0, arg1);
			a.cmp_imm(R0, 0);
			a.rsb_imm(R0, R0, 0, R12, LT);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case MINR:
		case MAXR:
		{
			load_two(s, arg1, arg2);
			a.cmp(R0, R1);
			a.mov(R0, R1, command == MAXR ? LT : GT);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case MINV:
		case MAXV:
		{
			arm32_load_zreg(s, R0, arg1);
			a.cmp_imm(R0, arg2, R12);
			a.mov_imm(R0, (uint32_t)arg2, command == MAXV ? LT : GT);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case CASTBOOLI:
		case CASTBOOLF:
		{
			// val = val ? (CASTBOOLI ? 10000 : 1) : 0
			arm32_load_zreg(s, R0, arg1);
			a.cmp_imm(R0, 0);
			a.mov_imm(R0, command == CASTBOOLI ? 10000 : 1, NE);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case FLOOR:
		case CEILING:
		{
			arm32_load_zreg(s, R0, arg1);
			a.call(command == FLOOR ? (const void*)&arm32_helper_floor : (const void*)&arm32_helper_ceil, R12);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		// --- bitwise (fixed point: the integer parts, rescaled) ---

		case ANDV:
		case ORV:
		case XORV:
		{
			arm32_load_zreg(s, R0, arg1);
			emit_div10000(s, R0, R0);
			uint32_t imm = (uint32_t)(arg2 / 10000);
			if (command == ANDV) a.and__imm(R0, R0, imm, R12);
			else if (command == ORV) a.orr_imm(R0, R0, imm, R12);
			else a.eor_imm(R0, R0, imm, R12);
			emit_mul10000(s, R0, R0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case ANDR:
		case ORR:
		case XORR:
		{
			load_two(s, arg1, arg2);
			emit_div10000(s, R0, R0);
			emit_div10000(s, R1, R1);
			if (command == ANDR) a.and_(R0, R0, R1);
			else if (command == ORR) a.orr(R0, R0, R1);
			else a.eor(R0, R0, R1);
			emit_mul10000(s, R0, R0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case ANDV32:
		case ORV32:
		case XORV32:
		{
			arm32_load_zreg(s, R0, arg1);
			if (command == ANDV32) a.and__imm(R0, R0, (uint32_t)arg2, R12);
			else if (command == ORV32) a.orr_imm(R0, R0, (uint32_t)arg2, R12);
			else a.eor_imm(R0, R0, (uint32_t)arg2, R12);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case ANDR32:
		case ORR32:
		case XORR32:
		{
			load_two(s, arg1, arg2);
			if (command == ANDR32) a.and_(R0, R0, R1);
			else if (command == ORR32) a.orr(R0, R0, R1);
			else a.eor(R0, R0, R1);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case BITNOT:
		{
			// reg = (~(reg / 10000)) * 10000
			arm32_load_zreg(s, R0, arg1);
			emit_div10000(s, R0, R0);
			a.mvn(R0, R0);
			emit_mul10000(s, R0, R0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case BITNOT32:
		{
			arm32_load_zreg(s, R0, arg1);
			a.mvn(R0, R0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		// --- shifts (count = arg / 10000, masked to 5 bits like x64/a64) ---

		case LSHIFTV:
		case RSHIFTV:
		case LSHIFTV32:
		case RSHIFTV32:
		{
			bool scaled = command == LSHIFTV || command == RSHIFTV;
			bool left = command == LSHIFTV || command == LSHIFTV32;
			int k = (arg2 / 10000) & 31;
			arm32_load_zreg(s, R0, arg1);
			if (scaled)
				emit_div10000(s, R0, R0);
			if (k)
				a.mov(R0, R0, left ? Shift::LSL : Shift::ASR, k);
			if (scaled)
				emit_mul10000(s, R0, R0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case LSHIFTR:
		case RSHIFTR:
		case LSHIFTR32:
		case RSHIFTR32:
		{
			bool scaled = command == LSHIFTR || command == RSHIFTR;
			bool left = command == LSHIFTR || command == LSHIFTR32;
			load_two(s, arg1, arg2);
			if (scaled)
				emit_div10000(s, R0, R0);
			emit_div10000(s, R1, R1);
			a.and__imm(R1, R1, 31, R12);
			a.mov(R0, R0, left ? Shift::LSL : Shift::ASR, R1);
			if (scaled)
				emit_mul10000(s, R0, R0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		// --- the ZASM stack (no change to sp) ---

		case LOAD:
		{
			// reg[arg1] = stack[rSFRAME + arg2]
			arm32_load_zreg(s, R0, rSFRAME);
			arm32_stack_addr(s, R1, R0, arg2);
			a.ldr(R0, R1, 0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case LOADD:
		case LOADI:
		{
			// LOADD: reg[arg1] = stack[(rSFRAME + arg2) / 10000]
			// LOADI: reg[arg1] = stack[reg[arg2] / 10000]
			if (command == LOADD)
				load_sframe_plus(s, R0, arg2);
			else
				arm32_load_zreg(s, R0, arg2);
			emit_div10000(s, R0, R0);
			arm32_stack_addr(s, R1, R0, 0);
			a.ldr(R0, R1, 0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case STORE:
		case STOREV:
		{
			// stack[rSFRAME + arg2] = STORE ? reg[arg1] : arg1
			if (command == STORE)
				arm32_load_zreg(s, R0, arg1);
			arm32_load_zreg(s, R1, rSFRAME);
			arm32_stack_addr(s, R2, R1, arg2);
			if (command == STOREV)
				a.mov_imm(R0, (uint32_t)arg1);
			a.str(R0, R2, 0);
		}
		break;

		case STORED:
		case STOREDV:
		{
			// stack[(rSFRAME + arg2) / 10000] = STORED ? reg[arg1] : arg1
			if (command == STORED)
				arm32_load_zreg(s, R0, arg1);
			load_sframe_plus(s, R1, arg2);
			emit_div10000(s, R1, R1);
			arm32_stack_addr(s, R2, R1, 0);
			if (command == STOREDV)
				a.mov_imm(R0, (uint32_t)arg1);
			a.str(R0, R2, 0);
		}
		break;

		case STOREI:
		{
			// stack[reg[arg2] / 10000] = reg[arg1]
			load_two(s, arg1, arg2);
			emit_div10000(s, R1, R1);
			arm32_stack_addr(s, R2, R1, 0);
			a.str(R0, R2, 0);
		}
		break;

		case PEEK:
		{
			arm32_stack_addr(s, R1, kSp, 0);
			a.ldr(R0, R1, 0);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case STACKWRITEATVV:
		{
			// stack[sp + arg2] = arg1. Like a64, no bounds check.
			arm32_stack_addr(s, R1, kSp, arg2);
			a.mov_imm(R0, (uint32_t)arg1);
			a.str(R0, R1, 0);
		}
		break;

		// --- object references (calls into the engine, as a64) ---

		case STORE_OBJECT:
		{
			// script_store_object(rSFRAME + arg2, reg[arg1])
			arm32_load_zreg(s, R1, arg1);
			load_sframe_plus(s, R0, arg2);
			arm32_call_c(s, (const void*)&script_store_object);
		}
		break;

		case REF_REMOVE:
		{
			load_sframe_plus(s, R0, arg1);
			arm32_call_c(s, (const void*)&script_remove_object_ref);
		}
		break;

		case REF_INC:
		case REF_DEC:
		{
			// arg1 is the object's stack offset from rSFRAME, or -1 for `this`.
			bool is_this = arg1 == -1;
			if (is_this)
				a.mov_imm(R1, 0);
			else
				load_sframe_plus(s, R1, arg1);
			a.mov_imm(R0, is_this ? 1 : 0);
			a.mov_imm(R2, command == REF_INC ? 1 : 0);
			a.mov_imm(R3, (uint32_t)pc);
			arm32_call_c(s, (const void*)&jit_ref_change);
		}
		break;

		case MARK_TYPE_STACK:
		{
			load_sframe_plus(s, R0, arg2);
			a.mov_imm(R1, arg1 ? 1 : 0);
			arm32_call_c(s, (const void*)&jit_mark_type_stack);
		}
		break;

		case ARRAYSIZE:
		{
			// rEXP1 = size of the array in reg[arg1].
			arm32_load_zreg(s, R0, arg1);
			a.mov_imm(R1, (uint32_t)pc);
			arm32_call_c(s, (const void*)&jit_arraysize);
			arm32_store_zreg(s, rEXP1, R0);
		}
		break;

		case ALLOCATEMEMV:
		{
			// reg[arg1] = allocate(size=arg2/10000, object_type=arg3).
			a.mov_imm(R0, (uint32_t)(arg2 / 10000));
			a.mov_imm(R1, (uint32_t)op.arg3);
			a.mov_imm(R2, (uint32_t)pc);
			arm32_call_c(s, (const void*)&jit_allocatemem);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		// --- POD arrays and class members (calls into the engine, as a64) ---

		case READPODARRAYR:
		case READPODARRAYV:
		{
			// reg[arg1] = array[rINDEX][index]
			if (command == READPODARRAYR)
			{
				arm32_load_zreg(s, R1, arg2);
				emit_div10000(s, R1, R1);
			}
			else
			{
				a.mov_imm(R1, (uint32_t)(arg2 / 10000));
			}
			arm32_load_zreg(s, R0, rINDEX);
			a.mov_imm(R2, (uint32_t)pc);
			a.mov_imm(R3, 0); // no_neg: honor the negative-index QR
			arm32_call_c(s, (const void*)&jit_pod_read);
			arm32_store_zreg(s, arg1, R0);
		}
		break;

		case WRITEPODARRAYRR:
		case WRITEPODARRAYRV:
		case WRITEPODARRAYVR:
		case WRITEPODARRAYVV:
		{
			// array[rINDEX][index] = value. First letter of the suffix is the index operand kind,
			// second is the value operand kind (R=register, V=immediate).
			bool index_is_reg = command == WRITEPODARRAYRR || command == WRITEPODARRAYRV;
			bool value_is_reg = command == WRITEPODARRAYRR || command == WRITEPODARRAYVR;

			if (index_is_reg && value_is_reg)
			{
				load_two(s, arg1, arg2); // R0 = index, R1 = value
				a.mov(R2, R1);
				emit_div10000(s, R1, R0);
			}
			else if (index_is_reg)
			{
				arm32_load_zreg(s, R1, arg1);
				emit_div10000(s, R1, R1);
				a.mov_imm(R2, (uint32_t)arg2);
			}
			else if (value_is_reg)
			{
				arm32_load_zreg(s, R2, arg2);
				a.mov_imm(R1, (uint32_t)(arg1 / 10000));
			}
			else
			{
				a.mov_imm(R1, (uint32_t)(arg1 / 10000));
				a.mov_imm(R2, (uint32_t)arg2);
			}
			arm32_load_zreg(s, R0, rINDEX);
			a.mov_imm(R3, (uint32_t)pc);
			arm32_call_c(s, (const void*)&arm32_helper_pod_write);
		}
		break;

		case WRITEPODARRAY:
		{
			// Bulk-initialize array (reg arg1) from the instruction's constant vector.
			arm32_load_zreg(s, R0, arg1);
			a.mov_imm(R1, (uint32_t)pc);
			arm32_call_c(s, (const void*)&jit_writepodarr);
		}
		break;

		case ZCLASS_READ:
		{
			// rEXP1 = object(reg[arg1]).data[arg2]
			arm32_load_zreg(s, R0, arg1);
			a.mov_imm(R1, (uint32_t)arg2);
			a.mov_imm(R2, (uint32_t)pc);
			arm32_call_c(s, (const void*)&jit_class_read);
			arm32_store_zreg(s, rEXP1, R0);
		}
		break;

		case ZCLASS_WRITE:
		{
			// object(reg[arg1]).data[arg2] = rEXP1
			arm32_load_zreg(s, R0, arg1);
			arm32_load_zreg(s, R2, rEXP1);
			a.mov_imm(R1, (uint32_t)arg2);
			a.mov_imm(R3, (uint32_t)pc);
			arm32_call_c(s, (const void*)&jit_class_write);
		}
		break;

		default:
			// arm32_ops_is_compiled and this switch must agree.
			a.bkpt(0xD0);
			break;
	}
}

#endif // ZC_ARM32_OPS_MATH_ONLY
