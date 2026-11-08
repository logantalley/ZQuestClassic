#ifndef ZC_JIT_ARM32_INTERNAL_H_
#define ZC_JIT_ARM32_INTERNAL_H_

// Internal interface between the two halves of the arm32 JIT backend (see docs/jit-arm32.md):
//
//   jit_arm32.cpp      - the core: function setup and teardown, the shared emit loop's hooks,
//                        control flow (comparisons, GOTO*, CALLFUNC/RETURNFUNC, QUIT), waits,
//                        interpreter fallback, the ZASM stack (PUSH*/POP*), and the z-register
//                        access helpers declared below.
//   jit_arm32_ops.cpp  - the data commands: arithmetic, logic, shifts, min/max, fixed-point
//                        multiply/divide, SETR/SETV, LOAD/STORE and friends - everything that
//                        computes values without changing control flow or the stack pointer.
//
// Only translation units of the arm32 backend include this header.

#include "zc/scripting/jit/arm32/arm32_asm.h"
#include "zc/scripting/jit/jit_shared.h"
#include "components/zasm/defines.h"

#include <map>

struct ffscript;
struct zasm_script;

namespace arm32 {

// Registers with a fixed role for the whole compiled function. All are callee-saved under
// the ARM EABI, so they survive calls into C; the core's prologue saves them and loads them
// from the JittedExecutionContext, and its epilogue restores them.
constexpr Reg kCtx = R4;        // JittedExecutionContext*
constexpr Reg kRegisters = R5;  // ctx->registers (== ri->d, so d[r] is at [kRegisters, #4*r];
                                // other refInfo members via jit_ri_offset)
constexpr Reg kStackBase = R6;  // ctx->stack_base (the ZASM stack, int32_t slots)
constexpr Reg kSp = R7;         // the ZASM stack index (ctx->sp); written back by the core
constexpr Reg kSwitchKey = R8;  // the SWITCHKEY register, which the JIT keeps in a register
// R9-R11: reserved for a later D-register cache (see docs/jit-arm32.md). Do not use.
//
// Scratch for data commands: R0-R3, R12, LR. Any call into C (including the z-register
// helpers below, for registers that are not D/G/SWITCHKEY) clobbers R0-R3, R12 and LR.

// The machine stack pointer and pc. components/zasm/defines.h #defines SP and PC (they are
// z-registers), which hides arm32::SP / arm32::PC in every translation unit that includes it
// after arm32_asm.h - this header does. Use these instead.
constexpr Reg kHostSp = static_cast<Reg>(13);
constexpr Reg kHostPc = static_cast<Reg>(15);

// The core's frame has two 4-byte spill slots at [kHostSp, #0] and [kHostSp, #4], free for any
// command to use within its own code (e.g. to keep a value across a z-register access that
// calls into C). Nothing is kept there between commands.
constexpr int kSpillSlot0 = 0;
constexpr int kSpillSlot1 = 4;

} // namespace arm32

// Compilation state for one piece of a function. Owned by the core; the data-command half
// reads it and emits through `a`.
struct Arm32State
{
	arm32::Assembler& a;
	zasm_script* script;
	JittedScript* j_script;
	pc_t pc;         // the command being compiled
	pc_t start_pc;
	pc_t final_pc;
	bool runtime_debugging;
	// Set by anything that changes kSp, so the epilogue writes it back to ctx->sp.
	bool modified_stack = false;

	// Core-private bookkeeping (labels for GOTO targets, resume points, exit stubs, ...).
	std::map<int, arm32::Label> goto_labels;
	std::map<int, arm32::Label> resume_labels;
	std::map<int, arm32::Label> exit_stubs;
	arm32::Label L_End;
};

// --- Implemented by the core (jit_arm32.cpp), for the data commands ---

// dst <- the value of z-register r. D and G registers and SWITCHKEY are read directly; any
// other register goes through get_register (a call into C: clobbers R0-R3, R12, LR - so do
// not hold values in those across it, except dst itself).
void arm32_load_zreg(Arm32State& s, arm32::Reg dst, int r);

// z-register r <- src (D/G/SWITCHKEY directly, else set_register via C: clobbers R0-R3, R12,
// LR, so `src` may be any register but its value is gone afterwards if it is one of those).
void arm32_store_zreg(Arm32State& s, int r, arm32::Reg src);

// z-register r <- imm.
void arm32_store_zreg_imm(Arm32State& s, int r, int32_t imm);

// Calls a C function with the ZASM pc (and ctx->sp, from kSp) saved where the engine expects
// them first, so engine code that reports errors or inspects the stack sees a consistent
// state. Arguments go in R0-R3 beforehand; the result is in R0.
void arm32_call_c(Arm32State& s, const void* fn);

// dst <- &stack[index_reg + offset_words] (stack_base + 4*(index + offset)).
// Uses only dst, and leaves the flags alone.
void arm32_stack_addr(Arm32State& s, arm32::Reg dst, arm32::Reg index_reg, int offset_words = 0);

// What the z-register helpers clobber, besides their destination:
// - arm32_load_zreg: nothing when the register is read directly (dst is the only register
//   written); otherwise everything a call into C clobbers.
// - arm32_store_zreg: LR when written directly (R12 instead if src is LR); otherwise everything
//   a call into C clobbers. arm32_store_zreg_imm holds the value in R12, so written directly it
//   clobbers R12 and LR.
// The direct paths never change the flags.
//
// Whether accessing z-register r (a read, or a write if is_write) calls into C, i.e. clobbers
// R0-R3, R12, LR and the flags. With two registers to read, read one that calls C first (or keep
// it in a spill slot across the second).
bool arm32_zreg_access_calls_c(int r, bool is_write);

// --- Implemented by the data-command half (jit_arm32_ops.cpp), for the core ---

// Whether the data-command half compiles this command. The core compiles the control-flow
// and stack commands itself and treats everything else not listed here as interpreted.
bool arm32_ops_is_compiled(int command);

// Emits code for `op` (only called when arm32_ops_is_compiled(op.command)). Must not change
// control flow or kSp, and must leave the fixed registers (R4-R11) as they were.
void arm32_ops_compile(Arm32State& s, const ffscript& op);

#endif
