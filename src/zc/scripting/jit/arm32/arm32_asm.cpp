// See arm32_asm.h. Encodings follow the ARM Architecture Reference Manual (ARMv6, ARM state).

#include "arm32_asm.h"

#include <cstdio>
#include <utility>

namespace arm32 {

namespace {

// Data-processing opcodes (bits 24-21).
enum Op : uint32_t
{
	OP_AND = 0, OP_EOR = 1, OP_SUB = 2, OP_RSB = 3, OP_ADD = 4, OP_ADC = 5, OP_SBC = 6,
	OP_RSC = 7, OP_TST = 8, OP_TEQ = 9, OP_CMP = 10, OP_CMN = 11, OP_ORR = 12, OP_MOV = 13,
	OP_BIC = 14, OP_MVN = 15,
};

constexpr uint32_t I_BIT = 1u << 25;
constexpr uint32_t S_BIT = 1u << 20;
constexpr uint32_t U_BIT = 1u << 23;

uint32_t rotl32(uint32_t v, unsigned n)
{
	n &= 31;
	return n ? (v << n) | (v >> (32 - n)) : v;
}

uint32_t rotr32(uint32_t v, unsigned n)
{
	return rotl32(v, (32 - (n & 31)) & 31);
}

// Splits `v` into as few as possible 8-bit chunks at even bit positions (each one an
// encodable immediate). Returns the chunk count (0 for v == 0, at most 4).
int split_chunks(uint32_t v, uint32_t out[4])
{
	int best = 5;
	uint32_t best_chunks[5] = {};
	for (unsigned start = 0; start < 32; start += 2)
	{
		uint32_t rest = v;
		uint32_t chunks[5];
		int n = 0;
		unsigned pos = start;
		while (rest && n < best)
		{
			unsigned k = 0;
			while (!((rest >> ((pos + k) & 31)) & 1))
				k++;
			unsigned q = ((pos + k) & 31) & ~1u;
			uint32_t mask = rotl32(0xff, q);
			chunks[n++] = rest & mask;
			rest &= ~mask;
			pos = q + 8;
		}
		if (!rest && n < best)
		{
			best = n;
			for (int i = 0; i < n; i++)
				best_chunks[i] = chunks[i];
		}
	}
	// Prefer whole bytes when they are no longer (easier to read in listings).
	int bytes = 0;
	for (int i = 0; i < 4; i++)
		bytes += ((v >> (8 * i)) & 0xff) != 0;
	if (bytes <= best)
	{
		int n = 0;
		for (int i = 0; i < 4; i++)
			if (v & (0xffu << (8 * i)))
				out[n++] = v & (0xffu << (8 * i));
		return n;
	}
	for (int i = 0; i < best; i++)
		out[i] = best_chunks[i];
	return best;
}

uint32_t imm_field(uint32_t value)
{
	uint32_t enc = 0;
	encode_imm(value, enc);
	return enc;
}

const char* const reg_names[16] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
	"r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
};

const char* const cond_names[16] = {
	"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
	"hi", "ls", "ge", "lt", "gt", "le", "", "nv",
};

const char* const shift_names[4] = {"lsl", "lsr", "asr", "ror"};

const char* const dp_names[16] = {
	"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
	"tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn",
};

std::string fmt_imm(uint32_t v)
{
	char buf[16];
	if (v <= 255)
		snprintf(buf, sizeof(buf), "#%u", (unsigned)v);
	else
		snprintf(buf, sizeof(buf), "#0x%x", (unsigned)v);
	return buf;
}

std::string reg_list(uint32_t mask)
{
	std::string s = "{";
	bool first = true;
	for (int i = 0; i < 16; i++)
	{
		if (!(mask & (1u << i)))
			continue;
		if (!first)
			s += ", ";
		s += reg_names[i];
		first = false;
	}
	return s + "}";
}

// Shifter operand of a register-form data-processing instruction (bits 11-0).
std::string fmt_shifter_reg(uint32_t w)
{
	std::string s = reg_names[w & 15];
	unsigned type = (w >> 5) & 3;
	if (w & (1u << 4))
	{
		s += ", ";
		s += shift_names[type];
		s += " ";
		s += reg_names[(w >> 8) & 15];
		return s;
	}
	unsigned amount = (w >> 7) & 31;
	if (amount == 0)
	{
		if (type == 0)
			return s;
		if (type == 3)
			return s + ", rrx";
		amount = 32;
	}
	return s + ", " + shift_names[type] + " #" + std::to_string(amount);
}

// A disassembly of the instruction forms this assembler emits (anything else is shown as
// .word). The syntax is GNU as unified syntax, so a listing line can be reassembled.
std::string disassemble(uint32_t w, size_t at)
{
	char buf[96];
	unsigned cond = w >> 28;
	const char* cs = cond_names[cond];
	auto R = [&](int shift) { return reg_names[(w >> shift) & 15]; };

	if (w == 0xe320f000)
		return "nop";
	if ((w & 0xfff000f0) == 0xe1200070)
	{
		snprintf(buf, sizeof(buf), "bkpt #%u", (unsigned)(((w >> 4) & 0xfff0) | (w & 0xf)));
		return buf;
	}
	if (cond == 15)
		goto unknown;

	if ((w & 0x0ffffff0) == 0x012fff10 || (w & 0x0ffffff0) == 0x012fff30)
	{
		snprintf(buf, sizeof(buf), "%s%s %s", (w & 0x20) ? "blx" : "bx", cs, R(0));
		return buf;
	}
	if ((w & 0x0fff0ff0) == 0x016f0f10)
	{
		snprintf(buf, sizeof(buf), "clz%s %s, %s", cs, R(12), R(0));
		return buf;
	}
	if ((w & 0x0fc000f0) == 0x00000090)
	{
		bool acc = w & (1u << 21);
		const char* s = (w & S_BIT) ? "s" : "";
		if (acc)
			snprintf(buf, sizeof(buf), "mla%s%s %s, %s, %s, %s", s, cs, R(16), R(0), R(8), R(12));
		else
			snprintf(buf, sizeof(buf), "mul%s%s %s, %s, %s", s, cs, R(16), R(0), R(8));
		return buf;
	}
	if ((w & 0x0fa000f0) == 0x00800090)
	{
		snprintf(buf, sizeof(buf), "%s%s%s %s, %s, %s, %s", (w & (1u << 22)) ? "smull" : "umull",
			(w & S_BIT) ? "s" : "", cs, R(12), R(16), R(0), R(8));
		return buf;
	}
	// Halfword / signed byte, immediate offset, pre-indexed, no writeback.
	if ((w & 0x0f600090) == 0x01400090 && (w & 0x60))
	{
		bool load = w & (1u << 20);
		unsigned sh = (w >> 5) & 3;
		const char* name = nullptr;
		if (!load && sh == 1)
			name = "strh";
		else if (load && sh == 1)
			name = "ldrh";
		else if (load && sh == 2)
			name = "ldrsb";
		else if (load && sh == 3)
			name = "ldrsh";
		if (!name)
			goto unknown;
		unsigned imm = ((w >> 4) & 0xf0) | (w & 0xf);
		if (imm == 0 && (w & U_BIT))
			snprintf(buf, sizeof(buf), "%s%s %s, [%s]", name, cs, R(12), R(16));
		else
			snprintf(buf, sizeof(buf), "%s%s %s, [%s, #%s%u]", name, cs, R(12), R(16),
				(w & U_BIT) ? "" : "-", imm);
		return buf;
	}
	// Word / byte, pre-indexed, no writeback.
	if ((w & 0x0d200000) == 0x05000000 && !((w & I_BIT) && (w & 0x10)))
	{
		const char* name = (w & (1u << 20)) ? ((w & (1u << 22)) ? "ldrb" : "ldr")
		                                    : ((w & (1u << 22)) ? "strb" : "str");
		const char* sign = (w & U_BIT) ? "" : "-";
		if (w & I_BIT)
		{
			std::string sh = fmt_shifter_reg(w & 0xfff);
			snprintf(buf, sizeof(buf), "%s%s %s, [%s, %s%s]", name, cs, R(12), R(16), sign, sh.c_str());
		}
		else if ((w & 0xfff) == 0 && (w & U_BIT))
			snprintf(buf, sizeof(buf), "%s%s %s, [%s]", name, cs, R(12), R(16));
		else
			snprintf(buf, sizeof(buf), "%s%s %s, [%s, #%s%u]", name, cs, R(12), R(16), sign,
				(unsigned)(w & 0xfff));
		return buf;
	}
	if ((w & 0x0fff0000) == 0x092d0000 || (w & 0x0fff0000) == 0x08bd0000)
	{
		bool push = (w & 0x0fff0000) == 0x092d0000;
		uint32_t mask = w & 0xffff;
		std::string regs = reg_list(mask);
		if (mask & (mask - 1))
			snprintf(buf, sizeof(buf), "%s%s %s", push ? "push" : "pop", cs, regs.c_str());
		else
			snprintf(buf, sizeof(buf), "%s%s sp!, %s", push ? "stmdb" : "ldmia", cs, regs.c_str());
		return buf;
	}
	if ((w & 0x0e000000) == 0x0a000000)
	{
		int32_t off = (int32_t)(w << 8) >> 6;
		snprintf(buf, sizeof(buf), "%s%s 0x%zx", (w & (1u << 24)) ? "bl" : "b", cs,
			(size_t)((int64_t)at + 8 + off));
		return buf;
	}
	// Data processing.
	if ((w & 0x0c000000) == 0)
	{
		// Exclude the multiply / extra load-store / misc space of the register forms.
		if (!(w & I_BIT) && (w & 0x90) == 0x90)
			goto unknown;
		unsigned op = (w >> 21) & 15;
		bool s = w & S_BIT;
		bool compare = op >= OP_TST && op <= OP_CMN;
		if (compare && !s)
			goto unknown;
		std::string operand2;
		if (w & I_BIT)
		{
			uint32_t value = rotr32(w & 0xff, ((w >> 8) & 15) * 2);
			if (imm_field(value) == (w & 0xfff))
				operand2 = fmt_imm(value);
			else // not the canonical encoding of the value: show the rotation
				operand2 = "#" + std::to_string(w & 0xff) + ", " + std::to_string(((w >> 8) & 15) * 2);
		}
		else
			operand2 = fmt_shifter_reg(w & 0xfff);
		std::string name = std::string(dp_names[op]) + (s && !compare ? "s" : "") + cs;
		if (compare)
			snprintf(buf, sizeof(buf), "%s %s, %s", name.c_str(), R(16), operand2.c_str());
		else if (op == OP_MOV || op == OP_MVN)
			snprintf(buf, sizeof(buf), "%s %s, %s", name.c_str(), R(12), operand2.c_str());
		else
			snprintf(buf, sizeof(buf), "%s %s, %s, %s", name.c_str(), R(12), R(16), operand2.c_str());
		return buf;
	}

unknown:
	snprintf(buf, sizeof(buf), ".word 0x%08x", (unsigned)w);
	return buf;
}

} // namespace

Cond invert(Cond c)
{
	return (Cond)(c ^ 1);
}

bool encode_imm(uint32_t value, uint32_t& encoded)
{
	for (unsigned i = 0; i < 32; i += 2)
	{
		uint32_t a = rotl32(value, i);
		if (a <= 0xff)
		{
			encoded = ((i / 2) << 8) | a;
			return true;
		}
	}
	return false;
}

// --- buffer, labels, errors ---

const std::vector<uint8_t>& Assembler::code() const
{
	return buffer;
}

size_t Assembler::offset() const
{
	return buffer.size();
}

Label Assembler::new_label()
{
	Label l;
	l.id = (int)label_offsets.size();
	label_offsets.push_back(-1);
	return l;
}

bool Assembler::check_label(Label label)
{
	if (label.id < 0 || (size_t)label.id >= label_offsets.size())
	{
		set_error("invalid label");
		return false;
	}
	return true;
}

void Assembler::bind(Label label)
{
	if (!check_label(label))
		return;
	if (label_offsets[label.id] >= 0)
	{
		set_error("label " + std::to_string(label.id) + " bound twice");
		return;
	}
	label_offsets[label.id] = (int64_t)buffer.size();
}

bool Assembler::is_bound(Label label) const
{
	return label.id >= 0 && (size_t)label.id < label_offsets.size() && label_offsets[label.id] >= 0;
}

size_t Assembler::label_offset(Label label) const
{
	return is_bound(label) ? (size_t)label_offsets[label.id] : 0;
}

uint32_t Assembler::word_at(size_t at) const
{
	return (uint32_t)buffer[at] | ((uint32_t)buffer[at + 1] << 8) | ((uint32_t)buffer[at + 2] << 16) |
	       ((uint32_t)buffer[at + 3] << 24);
}

void Assembler::patch(size_t at, uint32_t word)
{
	buffer[at] = (uint8_t)word;
	buffer[at + 1] = (uint8_t)(word >> 8);
	buffer[at + 2] = (uint8_t)(word >> 16);
	buffer[at + 3] = (uint8_t)(word >> 24);
}

bool Assembler::finalize()
{
	for (const Fixup& f : fixups)
	{
		if (f.label < 0 || (size_t)f.label >= label_offsets.size() || label_offsets[f.label] < 0)
		{
			set_error("unbound label " + std::to_string(f.label));
			continue;
		}
		int64_t delta = label_offsets[f.label] - ((int64_t)f.at + 8);
		if (f.kind == Fixup::Kind::Branch)
		{
			int64_t words = delta >> 2;
			if (words < -(1 << 23) || words >= (1 << 23))
			{
				set_error("branch to label " + std::to_string(f.label) + " out of range");
				continue;
			}
			patch(f.at, (word_at(f.at) & 0xff000000) | ((uint32_t)words & 0x00ffffff));
		}
		else
		{
			// add/sub rd, pc, #b[9:2]; add/sub rd, rd, #b[17:10]; add/sub rd, rd, #b[25:18]
			// (offsets are word aligned, so bits 1:0 are always zero).
			uint32_t op = delta >= 0 ? OP_ADD : OP_SUB;
			uint64_t mag = delta >= 0 ? (uint64_t)delta : (uint64_t)-delta;
			if (mag >= (1u << 26))
			{
				set_error("adr to label " + std::to_string(f.label) + " out of range");
				continue;
			}
			for (int i = 0; i < 3; i++)
			{
				uint32_t w = word_at(f.at + 4 * i);
				uint32_t chunk = (uint32_t)mag & (0xffu << (2 + 8 * i));
				w = (w & ~(0xfu << 21) & ~0xfffu) | (op << 21) | imm_field(chunk);
				patch(f.at + 4 * i, w);
			}
		}
	}
	finalized = true;
	return ok();
}

bool Assembler::ok() const
{
	return error_message.empty();
}

const std::string& Assembler::error() const
{
	return error_message;
}

void Assembler::set_error(std::string message)
{
	// Keep the first error: later ones are often a consequence of it.
	if (error_message.empty())
		error_message = std::to_string(buffer.size()) + ": " + std::move(message);
}

void Assembler::comment(std::string text)
{
	if (!pending_comment.empty())
		pending_comment += "; ";
	pending_comment += std::move(text);
}

std::string Assembler::listing() const
{
	std::string out;
	size_t ci = 0;
	char buf[48];
	for (size_t at = 0; at + 4 <= buffer.size(); at += 4)
	{
		while (ci < comments.size() && comments[ci].first <= at)
		{
			out += "          ; " + comments[ci].second + "\n";
			ci++;
		}
		uint32_t w = word_at(at);
		snprintf(buf, sizeof(buf), "%08zx: %08x  ", at, (unsigned)w);
		out += buf;
		out += disassemble(w, at);
		out += "\n";
	}
	for (; ci < comments.size(); ci++)
		out += "          ; " + comments[ci].second + "\n";
	if (!pending_comment.empty())
		out += "          ; " + pending_comment + "\n";
	return out;
}

void Assembler::emit32(uint32_t word)
{
	if (!pending_comment.empty())
	{
		comments.emplace_back(buffer.size(), std::move(pending_comment));
		pending_comment.clear();
	}
	buffer.push_back((uint8_t)word);
	buffer.push_back((uint8_t)(word >> 8));
	buffer.push_back((uint8_t)(word >> 16));
	buffer.push_back((uint8_t)(word >> 24));
}

// --- encoding helpers ---

bool Assembler::check_reg(Reg r)
{
	if ((unsigned)r > 15)
	{
		set_error("invalid register " + std::to_string((unsigned)r));
		return false;
	}
	return true;
}

bool Assembler::check_cond(Cond c)
{
	if ((unsigned)c > 14)
	{
		set_error("invalid condition " + std::to_string((unsigned)c));
		return false;
	}
	return true;
}

bool Assembler::shift_operand(Reg rm, Shift shift, int amount, uint32_t& out)
{
	if (!check_reg(rm))
		return false;
	unsigned type = (unsigned)shift & 3;
	if ((unsigned)shift > 3)
	{
		set_error("invalid shift");
		return false;
	}
	bool valid;
	switch (shift)
	{
		case Shift::LSL: valid = amount >= 0 && amount <= 31; break;
		case Shift::LSR:
		case Shift::ASR: valid = amount >= 0 && amount <= 32; break;
		default: valid = amount >= 0 && amount <= 31; break;
	}
	if (!valid)
	{
		set_error("invalid shift amount " + std::to_string(amount));
		return false;
	}
	if (amount == 0)
	{
		// No shift (LSR/ASR/ROR #0 would mean #32 / RRX: use the plain register instead).
		out = rm;
		return true;
	}
	out = ((uint32_t)(amount & 31) << 7) | (type << 5) | rm;
	return true;
}

void Assembler::dp(uint32_t op, Cond c, bool s, Reg rd, Reg rn, uint32_t operand2, bool imm)
{
	if (!check_cond(c) || !check_reg(rd) || !check_reg(rn))
		return;
	emit32(((uint32_t)c << 28) | (imm ? I_BIT : 0) | (op << 21) | (s ? S_BIT : 0) |
	       ((uint32_t)rn << 16) | ((uint32_t)rd << 12) | operand2);
}

void Assembler::dp_reg(uint32_t op, Reg rd, Reg rn, Reg rm, Shift shift, int amount, Cond c, bool s)
{
	uint32_t operand2;
	if (!shift_operand(rm, shift, amount, operand2))
		return;
	dp(op, c, s, rd, rn, operand2, false);
}

bool Assembler::dp_try_imm(uint32_t op, Reg rd, Reg rn, uint32_t imm, Cond c, bool s)
{
	uint32_t enc;
	if (!encode_imm(imm, enc))
		return false;
	dp(op, c, s, rd, rn, enc, true);
	return true;
}

int Assembler::mov_imm_length(uint32_t value)
{
	uint32_t enc, chunks[4];
	if (encode_imm(value, enc) || encode_imm(~value, enc))
		return 1;
	int a = split_chunks(value, chunks);
	int b = split_chunks(~value, chunks);
	return a < b ? a : b;
}

void Assembler::dp3_imm(uint32_t op, Reg rd, Reg rn, uint32_t imm, Reg scratch, Cond c, bool s)
{
	if (dp_try_imm(op, rd, rn, imm, c, s))
		return;

	// The same operation with a transformed immediate. Flags are identical for these (for
	// add/sub the only exceptions are imm 0 and 0x80000000, both encodable, so they never
	// get here); and/bic would differ in C, so only without s.
	switch (op)
	{
		case OP_ADD: if (dp_try_imm(OP_SUB, rd, rn, 0u - imm, c, s)) return; break;
		case OP_SUB: if (dp_try_imm(OP_ADD, rd, rn, 0u - imm, c, s)) return; break;
		case OP_ADC: if (dp_try_imm(OP_SBC, rd, rn, ~imm, c, s)) return; break;
		case OP_SBC: if (dp_try_imm(OP_ADC, rd, rn, ~imm, c, s)) return; break;
		case OP_AND: if (!s && dp_try_imm(OP_BIC, rd, rn, ~imm, c, s)) return; break;
		case OP_BIC: if (!s && dp_try_imm(OP_AND, rd, rn, ~imm, c, s)) return; break;
	}

	// Without flags, a chain of the operation over the immediate's chunks needs no scratch,
	// and is used when it is no longer than building the constant.
	uint32_t built = imm;
	uint32_t built_op = op;
	int scratch_len = mov_imm_length(imm) + 1;
	if (op == OP_ADD || op == OP_SUB)
	{
		int neg_len = mov_imm_length(0u - imm) + 1;
		if (neg_len < scratch_len)
		{
			scratch_len = neg_len;
			built = 0u - imm;
			built_op = op == OP_ADD ? OP_SUB : OP_ADD;
		}
	}
	if (!s)
	{
		uint32_t chunks[4];
		int n = 5;
		uint32_t chain_op = op;
		switch (op)
		{
			case OP_ADD:
			case OP_SUB:
			{
				uint32_t neg_chunks[4];
				n = split_chunks(imm, chunks);
				int m = split_chunks(0u - imm, neg_chunks);
				if (m < n)
				{
					n = m;
					chain_op = op == OP_ADD ? OP_SUB : OP_ADD;
					for (int i = 0; i < m; i++)
						chunks[i] = neg_chunks[i];
				}
				break;
			}
			case OP_ORR:
			case OP_EOR:
			case OP_BIC:
				n = split_chunks(imm, chunks);
				break;
			case OP_AND:
				n = split_chunks(~imm, chunks);
				chain_op = OP_BIC;
				break;
		}
		if (n <= 4 && n <= scratch_len)
		{
			for (int i = 0; i < n; i++)
				dp(chain_op, c, false, rd, i == 0 ? rn : rd, imm_field(chunks[i]), true);
			return;
		}
	}

	if (scratch == rn)
	{
		set_error("scratch register must differ from rn");
		return;
	}
	mov_imm(scratch, built, c);
	dp(built_op, c, s, rd, rn, scratch, false);
}

// --- data processing ---

void Assembler::mov(Reg rd, Reg rm, Cond c, bool s)
{
	dp_reg(OP_MOV, rd, R0, rm, Shift::LSL, 0, c, s);
}

void Assembler::mov(Reg rd, Reg rm, Shift shift, int amount, Cond c, bool s)
{
	dp_reg(OP_MOV, rd, R0, rm, shift, amount, c, s);
}

void Assembler::mov(Reg rd, Reg rm, Shift shift, Reg rs, Cond c, bool s)
{
	if (!check_reg(rm) || !check_reg(rs) || (unsigned)shift > 3)
	{
		set_error("invalid register-shifted operand");
		return;
	}
	dp(OP_MOV, c, s, rd, R0, ((uint32_t)rs << 8) | ((uint32_t)shift << 5) | (1u << 4) | rm, false);
}

void Assembler::mvn(Reg rd, Reg rm, Cond c, bool s)
{
	dp_reg(OP_MVN, rd, R0, rm, Shift::LSL, 0, c, s);
}

void Assembler::mov_imm(Reg rd, uint32_t value, Cond c)
{
	if (dp_try_imm(OP_MOV, rd, R0, value, c, false))
		return;
	if (dp_try_imm(OP_MVN, rd, R0, ~value, c, false))
		return;
	uint32_t a[4], b[4];
	int na = split_chunks(value, a);
	int nb = split_chunks(~value, b);
	if (na <= nb)
	{
		dp(OP_MOV, c, false, rd, R0, imm_field(a[0]), true);
		for (int i = 1; i < na; i++)
			dp(OP_ORR, c, false, rd, rd, imm_field(a[i]), true);
	}
	else
	{
		dp(OP_MVN, c, false, rd, R0, imm_field(b[0]), true);
		for (int i = 1; i < nb; i++)
			dp(OP_BIC, c, false, rd, rd, imm_field(b[i]), true);
	}
}

#define ZC_ARM32_DP3_IMPL(name, OPC) \
	void Assembler::name(Reg rd, Reg rn, Reg rm, Cond c, bool s) \
	{ \
		dp_reg(OPC, rd, rn, rm, Shift::LSL, 0, c, s); \
	} \
	void Assembler::name(Reg rd, Reg rn, Reg rm, Shift shift, int amount, Cond c, bool s) \
	{ \
		dp_reg(OPC, rd, rn, rm, shift, amount, c, s); \
	} \
	void Assembler::name##_imm(Reg rd, Reg rn, uint32_t imm, Reg scratch, Cond c, bool s) \
	{ \
		dp3_imm(OPC, rd, rn, imm, scratch, c, s); \
	}
ZC_ARM32_DP3_IMPL(add, OP_ADD)
ZC_ARM32_DP3_IMPL(sub, OP_SUB)
ZC_ARM32_DP3_IMPL(rsb, OP_RSB)
ZC_ARM32_DP3_IMPL(adc, OP_ADC)
ZC_ARM32_DP3_IMPL(sbc, OP_SBC)
ZC_ARM32_DP3_IMPL(and_, OP_AND)
ZC_ARM32_DP3_IMPL(orr, OP_ORR)
ZC_ARM32_DP3_IMPL(eor, OP_EOR)
ZC_ARM32_DP3_IMPL(bic, OP_BIC)
#undef ZC_ARM32_DP3_IMPL

void Assembler::cmp(Reg rn, Reg rm, Cond c)
{
	dp_reg(OP_CMP, R0, rn, rm, Shift::LSL, 0, c, true);
}

void Assembler::cmp_imm(Reg rn, int32_t imm, Reg scratch, Cond c)
{
	uint32_t v = (uint32_t)imm;
	if (dp_try_imm(OP_CMP, R0, rn, v, c, true))
		return;
	// cmn #-imm sets the same flags as cmp #imm, except for imm 0 and INT_MIN (encodable).
	if (dp_try_imm(OP_CMN, R0, rn, 0u - v, c, true))
		return;
	if (scratch == rn)
	{
		set_error("scratch register must differ from rn");
		return;
	}
	if (mov_imm_length(0u - v) < mov_imm_length(v))
	{
		mov_imm(scratch, 0u - v, c);
		dp(OP_CMN, c, true, R0, rn, scratch, false);
	}
	else
	{
		mov_imm(scratch, v, c);
		dp(OP_CMP, c, true, R0, rn, scratch, false);
	}
}

void Assembler::cmn(Reg rn, Reg rm, Cond c)
{
	dp_reg(OP_CMN, R0, rn, rm, Shift::LSL, 0, c, true);
}

void Assembler::tst(Reg rn, Reg rm, Cond c)
{
	dp_reg(OP_TST, R0, rn, rm, Shift::LSL, 0, c, true);
}

void Assembler::tst_imm(Reg rn, uint32_t imm, Reg scratch, Cond c)
{
	if (dp_try_imm(OP_TST, R0, rn, imm, c, true))
		return;
	if (scratch == rn)
	{
		set_error("scratch register must differ from rn");
		return;
	}
	mov_imm(scratch, imm, c);
	dp(OP_TST, c, true, R0, rn, scratch, false);
}

void Assembler::teq(Reg rn, Reg rm, Cond c)
{
	dp_reg(OP_TEQ, R0, rn, rm, Shift::LSL, 0, c, true);
}

// --- multiplies ---

void Assembler::mul(Reg rd, Reg rm, Reg rs, Cond c, bool s)
{
	if (!check_cond(c) || !check_reg(rd) || !check_reg(rm) || !check_reg(rs))
		return;
	if (rd == PC || rm == PC || rs == PC)
	{
		set_error("pc operand to mul");
		return;
	}
	emit32(((uint32_t)c << 28) | (s ? S_BIT : 0) | ((uint32_t)rd << 16) | ((uint32_t)rs << 8) | 0x90 | rm);
}

void Assembler::mla(Reg rd, Reg rm, Reg rs, Reg rn, Cond c)
{
	if (!check_cond(c) || !check_reg(rd) || !check_reg(rm) || !check_reg(rs) || !check_reg(rn))
		return;
	if (rd == PC || rm == PC || rs == PC || rn == PC)
	{
		set_error("pc operand to mla");
		return;
	}
	emit32(((uint32_t)c << 28) | 0x00200090 | ((uint32_t)rd << 16) | ((uint32_t)rn << 12) |
	       ((uint32_t)rs << 8) | rm);
}

void Assembler::mull(uint32_t base, Reg rdlo, Reg rdhi, Reg rm, Reg rs, Cond c)
{
	if (!check_cond(c) || !check_reg(rdlo) || !check_reg(rdhi) || !check_reg(rm) || !check_reg(rs))
		return;
	// Only rdlo == rdhi and pc operands are unpredictable on ARMv6 (rdhi == rs etc. is fine).
	if (rdlo == rdhi)
	{
		set_error("rdlo and rdhi must differ");
		return;
	}
	if (rdlo == PC || rdhi == PC || rm == PC || rs == PC)
	{
		set_error("pc operand to a long multiply");
		return;
	}
	emit32(((uint32_t)c << 28) | base | ((uint32_t)rdhi << 16) | ((uint32_t)rdlo << 12) |
	       ((uint32_t)rs << 8) | rm);
}

void Assembler::smull(Reg rdlo, Reg rdhi, Reg rm, Reg rs, Cond c)
{
	mull(0x00c00090, rdlo, rdhi, rm, rs, c);
}

void Assembler::umull(Reg rdlo, Reg rdhi, Reg rm, Reg rs, Cond c)
{
	mull(0x00800090, rdlo, rdhi, rm, rs, c);
}

void Assembler::clz(Reg rd, Reg rm, Cond c)
{
	if (!check_cond(c) || !check_reg(rd) || !check_reg(rm))
		return;
	if (rd == PC || rm == PC)
	{
		set_error("pc operand to clz");
		return;
	}
	emit32(((uint32_t)c << 28) | 0x016f0f10 | ((uint32_t)rd << 12) | rm);
}

// --- loads/stores ---

void Assembler::mem_word(bool load, bool byte, Reg rt, Reg rn, int32_t offset, Cond c)
{
	if (!check_cond(c) || !check_reg(rt) || !check_reg(rn))
		return;
	if (offset < -4095 || offset > 4095)
	{
		set_error("load/store offset " + std::to_string(offset) + " out of range");
		return;
	}
	uint32_t u = offset >= 0 ? U_BIT : 0;
	uint32_t mag = offset >= 0 ? (uint32_t)offset : (uint32_t)-offset;
	emit32(((uint32_t)c << 28) | 0x05000000 | u | (byte ? 1u << 22 : 0) | (load ? 1u << 20 : 0) |
	       ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | mag);
}

void Assembler::mem_half(bool load, uint32_t sh, Reg rt, Reg rn, int32_t offset, Cond c)
{
	if (!check_cond(c) || !check_reg(rt) || !check_reg(rn))
		return;
	if (offset < -255 || offset > 255)
	{
		set_error("halfword/signed load/store offset " + std::to_string(offset) + " out of range");
		return;
	}
	uint32_t u = offset >= 0 ? U_BIT : 0;
	uint32_t mag = offset >= 0 ? (uint32_t)offset : (uint32_t)-offset;
	emit32(((uint32_t)c << 28) | 0x01400090 | u | (load ? 1u << 20 : 0) | ((uint32_t)rn << 16) |
	       ((uint32_t)rt << 12) | ((mag & 0xf0) << 4) | (sh << 5) | (mag & 0xf));
}

void Assembler::mem_reg(bool load, Reg rt, Reg rn, Reg rm, int lsl, bool subtract, Cond c)
{
	if (!check_cond(c) || !check_reg(rt) || !check_reg(rn) || !check_reg(rm))
		return;
	if (lsl < 0 || lsl > 31)
	{
		set_error("invalid index shift " + std::to_string(lsl));
		return;
	}
	emit32(((uint32_t)c << 28) | 0x07000000 | (subtract ? 0 : U_BIT) | (load ? 1u << 20 : 0) |
	       ((uint32_t)rn << 16) | ((uint32_t)rt << 12) | ((uint32_t)lsl << 7) | rm);
}

void Assembler::ldr(Reg rt, Reg rn, int32_t offset, Cond c) { mem_word(true, false, rt, rn, offset, c); }
void Assembler::str(Reg rt, Reg rn, int32_t offset, Cond c) { mem_word(false, false, rt, rn, offset, c); }
void Assembler::ldrb(Reg rt, Reg rn, int32_t offset, Cond c) { mem_word(true, true, rt, rn, offset, c); }
void Assembler::strb(Reg rt, Reg rn, int32_t offset, Cond c) { mem_word(false, true, rt, rn, offset, c); }
void Assembler::ldrh(Reg rt, Reg rn, int32_t offset, Cond c) { mem_half(true, 1, rt, rn, offset, c); }
void Assembler::strh(Reg rt, Reg rn, int32_t offset, Cond c) { mem_half(false, 1, rt, rn, offset, c); }
void Assembler::ldrsb(Reg rt, Reg rn, int32_t offset, Cond c) { mem_half(true, 2, rt, rn, offset, c); }
void Assembler::ldrsh(Reg rt, Reg rn, int32_t offset, Cond c) { mem_half(true, 3, rt, rn, offset, c); }

void Assembler::mem_far(bool load, Reg rt, Reg rn, int32_t offset, Reg scratch)
{
	if (offset >= -4095 && offset <= 4095)
	{
		mem_word(load, false, rt, rn, offset, AL);
		return;
	}
	if (scratch == rn || (!load && scratch == rt))
	{
		set_error("scratch register must differ from rn (and rt for stores)");
		return;
	}
	uint32_t mag = offset >= 0 ? (uint32_t)offset : 0u - (uint32_t)offset;
	uint32_t enc;
	// add/sub scratch, rn, #high; ldr/str rt, [scratch, #±low].
	if (encode_imm(mag & ~0xfffu, enc))
	{
		dp(offset >= 0 ? OP_ADD : OP_SUB, AL, false, scratch, rn, enc, true);
		mem_word(load, false, rt, scratch, offset >= 0 ? (int32_t)(mag & 0xfff) : -(int32_t)(mag & 0xfff), AL);
		return;
	}
	// mov scratch, #±offset; ldr/str rt, [rn, ±scratch].
	bool subtract = mov_imm_length(mag) < mov_imm_length((uint32_t)offset);
	mov_imm(scratch, subtract ? mag : (uint32_t)offset);
	mem_reg(load, rt, rn, scratch, 0, subtract, AL);
}

void Assembler::ldr_far(Reg rt, Reg rn, int32_t offset, Reg scratch) { mem_far(true, rt, rn, offset, scratch); }
void Assembler::str_far(Reg rt, Reg rn, int32_t offset, Reg scratch) { mem_far(false, rt, rn, offset, scratch); }

void Assembler::ldr(Reg rt, Reg rn, Reg rm, int lsl, Cond c) { mem_reg(true, rt, rn, rm, lsl, false, c); }
void Assembler::str(Reg rt, Reg rn, Reg rm, int lsl, Cond c) { mem_reg(false, rt, rn, rm, lsl, false, c); }

void Assembler::push(uint16_t mask, Cond c)
{
	if (!check_cond(c))
		return;
	if (!mask)
	{
		set_error("push of an empty register list");
		return;
	}
	emit32(((uint32_t)c << 28) | 0x092d0000 | mask);
}

void Assembler::pop(uint16_t mask, Cond c)
{
	if (!check_cond(c))
		return;
	if (!mask)
	{
		set_error("pop of an empty register list");
		return;
	}
	emit32(((uint32_t)c << 28) | 0x08bd0000 | mask);
}

// --- control flow ---

void Assembler::branch(uint32_t base, Label target, Cond c)
{
	if (!check_cond(c) || !check_label(target))
		return;
	fixups.push_back({buffer.size(), target.id, Fixup::Kind::Branch});
	emit32(((uint32_t)c << 28) | base);
}

void Assembler::b(Label target, Cond c) { branch(0x0a000000, target, c); }
void Assembler::bl(Label target, Cond c) { branch(0x0b000000, target, c); }

void Assembler::bx(Reg rm, Cond c)
{
	if (!check_cond(c) || !check_reg(rm))
		return;
	emit32(((uint32_t)c << 28) | 0x012fff10 | rm);
}

void Assembler::blx(Reg rm, Cond c)
{
	if (!check_cond(c) || !check_reg(rm))
		return;
	emit32(((uint32_t)c << 28) | 0x012fff30 | rm);
}

void Assembler::call(const void* fn, Reg scratch)
{
	mov_imm(scratch, (uint32_t)(uintptr_t)fn);
	blx(scratch);
}

void Assembler::adr(Reg rd, Label label)
{
	if (!check_reg(rd) || !check_label(label))
		return;
	if (rd == PC)
	{
		set_error("adr into pc");
		return;
	}
	fixups.push_back({buffer.size(), label.id, Fixup::Kind::Adr});
	dp(OP_ADD, AL, false, rd, PC, 0, true);
	dp(OP_ADD, AL, false, rd, rd, 0, true);
	dp(OP_ADD, AL, false, rd, rd, 0, true);
}

void Assembler::nop()
{
	emit32(0xe320f000);
}

void Assembler::bkpt(uint16_t imm)
{
	emit32(0xe1200070 | ((uint32_t)(imm & 0xfff0) << 4) | (imm & 0xf));
}

} // namespace arm32
