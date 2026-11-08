#ifndef ZC_JIT_ARM32_ASM_H_
#define ZC_JIT_ARM32_ASM_H_

// A small assembler for 32-bit ARM (ARMv6K, ARM state - the Nintendo 3DS's ARM11), used by
// the arm32 JIT backend (jit_arm32.cpp) in place of asmjit, which has no 32-bit ARM support.
//
// It emits only what the backend needs, into a growable buffer, with forward-referenceable
// labels resolved by finalize(). Code is position independent - branches are pc-relative -
// except where the backend materializes absolute addresses (C functions it calls), so the
// finalized bytes can be copied anywhere (see jit_arm32_memory.h).
//
// ARMv6K has no MOVW/MOVT and no hardware divide: mov_imm builds any 32-bit constant from
// up to four instructions, and division is a call into C.
//
// Errors (an unencodable operand, an unbound label, a branch out of range) do not throw:
// they are recorded, emission continues, and ok()/error() report them, so a backend can
// discard the function and fall back to the interpreter.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace arm32 {

enum Reg : uint8_t
{
	R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12,
	SP = 13, LR = 14, PC = 15,
};

// Bitmask of registers for push/pop (ldm/stm), e.g. reg_mask(R4) | reg_mask(LR).
constexpr uint16_t reg_mask(Reg r) { return (uint16_t)(1u << r); }

enum Cond : uint8_t
{
	EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, VS = 6, VC = 7,
	HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13, AL = 14,
};

// The condition that is true exactly when `c` is false (c must not be AL).
Cond invert(Cond c);

enum class Shift : uint8_t { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };

struct Label
{
	int id = -1;
	bool valid() const { return id >= 0; }
};

// Whether `value` fits an ARM data-processing immediate (an 8-bit value rotated right by an
// even amount). If so, `encoded` receives the 12-bit field.
bool encode_imm(uint32_t value, uint32_t& encoded);

class Assembler
{
public:
	// --- buffer, labels, errors ---

	// Bytes emitted so far (each instruction is 4 bytes, little endian).
	const std::vector<uint8_t>& code() const;
	size_t offset() const;

	Label new_label();
	// Binds `label` to the current offset. Each label may be bound once.
	void bind(Label label);
	bool is_bound(Label label) const;
	// Byte offset of a bound label from the start of the code.
	size_t label_offset(Label label) const;

	// Resolves every pending branch to its label. Call once, after the last instruction.
	// Returns ok().
	bool finalize();

	bool ok() const;
	const std::string& error() const;

	// Attaches a comment to the next emitted instruction, shown in listing().
	void comment(std::string text);
	// A disassembly-like listing of the emitted code (offset, word, mnemonic, comments), for
	// the JIT's debug dumps (DEBUG_JIT_PRINT_ASM). Does not need to be a full disassembler -
	// the mnemonic of each emitted instruction, as the assembler knows it, is enough.
	std::string listing() const;

	// Emits a raw instruction word.
	void emit32(uint32_t word);

	// --- data processing ---
	// `s` = set flags. Register-shifted forms take an immediate shift amount (0-31; for
	// LSR/ASR 1-32, with 32 encoded as 0).

	void mov(Reg rd, Reg rm, Cond c = AL, bool s = false);
	void mov(Reg rd, Reg rm, Shift shift, int amount, Cond c = AL, bool s = false);
	// rd <- rm shifted by the low byte of rs.
	void mov(Reg rd, Reg rm, Shift shift, Reg rs, Cond c = AL, bool s = false);
	void mvn(Reg rd, Reg rm, Cond c = AL, bool s = false);
	// Any 32-bit constant: mov or mvn when encodable, otherwise mov + up to three orr (or the
	// shortest equivalent sequence). Never touches the flags. Conditional if c != AL (every
	// instruction of the sequence carries the condition).
	void mov_imm(Reg rd, uint32_t value, Cond c = AL);

	// rd <- rn OP rm, rd <- rn OP (rm shifted), rd <- rn OP imm.
	// For the _imm forms: when imm is not encodable, the constant is built in `scratch` with
	// mov_imm first (scratch must differ from rn). add_imm/sub_imm also try the opposite
	// operation with the negated immediate before using scratch.
#define ZC_ARM32_DP3(name) \
	void name(Reg rd, Reg rn, Reg rm, Cond c = AL, bool s = false); \
	void name(Reg rd, Reg rn, Reg rm, Shift shift, int amount, Cond c = AL, bool s = false); \
	void name##_imm(Reg rd, Reg rn, uint32_t imm, Reg scratch = R12, Cond c = AL, bool s = false);
	ZC_ARM32_DP3(add)
	ZC_ARM32_DP3(sub)
	ZC_ARM32_DP3(rsb)
	ZC_ARM32_DP3(adc)
	ZC_ARM32_DP3(sbc)
	ZC_ARM32_DP3(and_)
	ZC_ARM32_DP3(orr)
	ZC_ARM32_DP3(eor)
	ZC_ARM32_DP3(bic)
#undef ZC_ARM32_DP3

	// Comparisons (always set flags).
	void cmp(Reg rn, Reg rm, Cond c = AL);
	void cmp_imm(Reg rn, int32_t imm, Reg scratch = R12, Cond c = AL); // uses cmn for negatives
	void cmn(Reg rn, Reg rm, Cond c = AL);
	void tst(Reg rn, Reg rm, Cond c = AL);
	void tst_imm(Reg rn, uint32_t imm, Reg scratch = R12, Cond c = AL);
	void teq(Reg rn, Reg rm, Cond c = AL);

	// Multiplies. rd and rm must differ for mul/mla on pre-v6 cores; ARMv6 lifts that, but
	// prefer distinct registers anyway.
	void mul(Reg rd, Reg rm, Reg rs, Cond c = AL, bool s = false);
	void mla(Reg rd, Reg rm, Reg rs, Reg rn, Cond c = AL);           // rd <- rm*rs + rn
	void smull(Reg rdlo, Reg rdhi, Reg rm, Reg rs, Cond c = AL);     // signed 32x32 -> 64
	void umull(Reg rdlo, Reg rdhi, Reg rm, Reg rs, Cond c = AL);

	void clz(Reg rd, Reg rm, Cond c = AL);

	// --- loads/stores ---
	// Immediate offsets: word/byte forms take -4095..4095, halfword/signed forms -255..255.
	// Out-of-range offsets are an error unless a scratch register is given, in which case
	// the address is computed into it first.

	void ldr(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void str(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void ldrb(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void strb(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void ldrh(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void strh(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void ldrsh(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void ldrsb(Reg rt, Reg rn, int32_t offset = 0, Cond c = AL);
	void ldr_far(Reg rt, Reg rn, int32_t offset, Reg scratch);
	void str_far(Reg rt, Reg rn, int32_t offset, Reg scratch);

	// Register offset: [rn, rm, LSL #shift] (shift 0-31), e.g. a word array index.
	void ldr(Reg rt, Reg rn, Reg rm, int lsl = 0, Cond c = AL);
	void str(Reg rt, Reg rn, Reg rm, int lsl = 0, Cond c = AL);

	// stmdb sp!, {mask} / ldmia sp!, {mask}.
	void push(uint16_t mask, Cond c = AL);
	void pop(uint16_t mask, Cond c = AL);

	// --- control flow ---

	void b(Label target, Cond c = AL);
	void bl(Label target, Cond c = AL);
	void bx(Reg rm, Cond c = AL);
	void blx(Reg rm, Cond c = AL);
	// Calls an absolute address (a C function): mov_imm(scratch, addr); blx scratch.
	void call(const void* fn, Reg scratch = R12);
	// rd <- the address of `label`, pc-relative so it stays position independent. Emits a
	// fixed-size sequence (its size cannot depend on the label, which may be bound later)
	// that reaches any offset within the function.
	void adr(Reg rd, Label label);

	void nop();
	void bkpt(uint16_t imm = 0);

private:
	// The private members are a suggestion; the implementation may change them freely.
	struct Fixup
	{
		size_t at;
		int label;
		enum class Kind : uint8_t { Branch, Adr } kind;
	};

	void set_error(std::string message);

	std::vector<uint8_t> buffer;
	std::vector<int64_t> label_offsets; // -1 while unbound
	std::vector<Fixup> fixups;
	std::vector<std::pair<size_t, std::string>> comments;
	std::vector<std::pair<size_t, std::string>> mnemonics;
	std::string pending_comment;
	std::string error_message;
	bool finalized = false;
};

} // namespace arm32

#endif
