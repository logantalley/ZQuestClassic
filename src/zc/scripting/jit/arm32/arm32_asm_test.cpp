// Host-side test for the arm32 JIT assembler (arm32_asm.cpp). Standalone:
//
//   g++ -std=c++20 -O2 arm32_asm_test.cpp arm32_asm.cpp -o arm32_asm_test && ./arm32_asm_test
//
// (or run_arm32_asm_test.sh). It checks:
//  1. encodings against GNU as from devkitARM (ARM_AS / ARM_OBJCOPY override the paths):
//     a table of instruction forms written as assembler text, plus every listing() line of
//     everything the test emits reassembled - which also checks the listing's mnemonics;
//  2. semantics of the multi-instruction sequences (mov_imm, the *_imm fallbacks, ldr_far,
//     adr) with a small evaluator for the emitted subset;
//  3. label fixups, range errors and the other recorded errors.

#include "arm32_asm.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace arm32;

static int passes, failures;

#define CHECK(cond, ...) \
	do { \
		if (cond) \
			passes++; \
		else \
		{ \
			failures++; \
			if (failures <= 60) \
			{ \
				printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

static const char* const REG[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
	"r11", "r12", "sp", "lr", "pc"};
static const char* const COND[15] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge",
	"lt", "gt", "le", ""};
static const char* const SHIFT[4] = {"lsl", "lsr", "asr", "ror"};

static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

static std::vector<uint32_t> words_of(const Assembler& a)
{
	std::vector<uint32_t> w(a.code().size() / 4);
	memcpy(w.data(), a.code().data(), w.size() * 4);
	return w;
}

// --- reference assembler ---

static std::string tmp_dir;

static std::string tool(const char* env, const char* name)
{
	if (const char* p = getenv(env))
		return p;
	return std::string("/opt/devkitpro/devkitARM/bin/") + name;
}

static bool gas(const std::string& text, std::vector<uint32_t>& out)
{
	std::string s = tmp_dir + "/t.s", o = tmp_dir + "/t.o", bin = tmp_dir + "/t.bin";
	FILE* f = fopen(s.c_str(), "w");
	if (!f)
		return false;
	fprintf(f, ".syntax unified\n.arm\n.text\n%s\n", text.c_str());
	fclose(f);
	std::string cmd = tool("ARM_AS", "arm-none-eabi-as") + " -march=armv6k -o " + o + " " + s +
	                  " && " + tool("ARM_OBJCOPY", "arm-none-eabi-objcopy") + " -O binary -j .text " + o +
	                  " " + bin;
	if (system(cmd.c_str()) != 0)
	{
		printf("assembler failed: %s (source kept in %s)\n", cmd.c_str(), s.c_str());
		return false;
	}
	f = fopen(bin.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint32_t w;
	while (fread(&w, 4, 1, f) == 1)
		out.push_back(w);
	fclose(f);
	return true;
}

// --- 1. encoding table ---

struct Case
{
	std::string text;
	std::vector<uint32_t> words;
	std::string listing;
};

static std::vector<Case> cases;

static void add_case(std::string text, const std::function<void(Assembler&)>& emit)
{
	Assembler a;
	emit(a);
	a.finalize();
	if (!a.ok())
	{
		CHECK(a.ok(), "case '%s' failed: %s", text.c_str(), a.error().c_str());
		return;
	}
	cases.push_back({std::move(text), words_of(a), a.listing()});
}

static std::string sfx(bool s, Cond c)
{
	return std::string(s ? "s" : "") + COND[c];
}

static void build_cases()
{
	const Reg regs[][3] = {{R0, R1, R2}, {R3, R4, R5}, {R12, LR, R0}, {R7, R7, R8}, {R10, R11, R9},
		{LR, R12, R6}};
	const int amounts[] = {0, 1, 5, 16, 31, 32};

	typedef void (Assembler::*Dp3)(Reg, Reg, Reg, Cond, bool);
	typedef void (Assembler::*Dp3S)(Reg, Reg, Reg, Shift, int, Cond, bool);
	typedef void (Assembler::*Dp3I)(Reg, Reg, uint32_t, Reg, Cond, bool);
	struct Dp3Op { const char* name; Dp3 r; Dp3S sh; Dp3I imm; };
	const Dp3Op ops[] = {
		{"add", &Assembler::add, &Assembler::add, &Assembler::add_imm},
		{"sub", &Assembler::sub, &Assembler::sub, &Assembler::sub_imm},
		{"rsb", &Assembler::rsb, &Assembler::rsb, &Assembler::rsb_imm},
		{"adc", &Assembler::adc, &Assembler::adc, &Assembler::adc_imm},
		{"sbc", &Assembler::sbc, &Assembler::sbc, &Assembler::sbc_imm},
		{"and", &Assembler::and_, &Assembler::and_, &Assembler::and__imm},
		{"orr", &Assembler::orr, &Assembler::orr, &Assembler::orr_imm},
		{"eor", &Assembler::eor, &Assembler::eor, &Assembler::eor_imm},
		{"bic", &Assembler::bic, &Assembler::bic, &Assembler::bic_imm},
	};
	const uint32_t enc_imms[] = {0, 1, 255, 0x100, 0x3fc, 0xff000000, 0xf000000f, 0x80000000, 0x104,
		0x00ab0000, 0xc000003f};

	int ri = 0;
	for (int ci = 0; ci < 15; ci++)
	{
		Cond c = (Cond)ci;
		for (int s = 0; s < 2; s++)
		{
			for (auto& op : ops)
			{
				auto& r = regs[ri++ % 6];
				add_case(fmt("%s%s %s, %s, %s", op.name, sfx(s, c).c_str(), REG[r[0]], REG[r[1]], REG[r[2]]),
					[&](Assembler& a) { (a.*op.r)(r[0], r[1], r[2], c, s); });
				for (int sh = 0; sh < 4; sh++)
					for (int amount : amounts)
					{
						if (amount == 32 && (sh == 0 || sh == 3))
							continue;
						std::string text = fmt("%s%s %s, %s, %s", op.name, sfx(s, c).c_str(), REG[r[0]],
							REG[r[1]], REG[r[2]]);
						if (amount)
							text += fmt(", %s #%d", SHIFT[sh], amount);
						add_case(text, [&](Assembler& a) { (a.*op.sh)(r[0], r[1], r[2], (Shift)sh, amount, c, s); });
					}
				for (uint32_t imm : enc_imms)
					add_case(fmt("%s%s %s, %s, #0x%x", op.name, sfx(s, c).c_str(), REG[r[0]], REG[r[1]], imm),
						[&](Assembler& a) { (a.*op.imm)(r[0], r[1], imm, R12, c, s); });
			}

			// mov / mvn
			for (int i = 0; i < 6; i++)
			{
				Reg rd = regs[i][0], rm = regs[i][2], rs = regs[i][1];
				add_case(fmt("mov%s %s, %s", sfx(s, c).c_str(), REG[rd], REG[rm]),
					[&](Assembler& a) { a.mov(rd, rm, c, s); });
				add_case(fmt("mvn%s %s, %s", sfx(s, c).c_str(), REG[rd], REG[rm]),
					[&](Assembler& a) { a.mvn(rd, rm, c, s); });
				for (int sh = 0; sh < 4; sh++)
				{
					add_case(fmt("mov%s %s, %s, %s %s", sfx(s, c).c_str(), REG[rd], REG[rm], SHIFT[sh], REG[rs]),
						[&](Assembler& a) { a.mov(rd, rm, (Shift)sh, rs, c, s); });
					for (int amount : amounts)
					{
						if (amount == 0 || (amount == 32 && (sh == 0 || sh == 3)))
							continue;
						add_case(fmt("mov%s %s, %s, %s #%d", sfx(s, c).c_str(), REG[rd], REG[rm], SHIFT[sh], amount),
							[&](Assembler& a) { a.mov(rd, rm, (Shift)sh, amount, c, s); });
					}
				}
			}
			add_case(fmt("mul%s r0, r1, r2", sfx(s, c).c_str()), [&](Assembler& a) { a.mul(R0, R1, R2, c, s); });
			add_case(fmt("mul%s r12, lr, r3", sfx(s, c).c_str()), [&](Assembler& a) { a.mul(R12, LR, R3, c, s); });
		}

		// One-instruction mov_imm and the immediate fallbacks that stay one instruction.
		const uint32_t mov_imms[] = {0, 1, 255, 0x400, 0xff000000, 0x80000000, 0xf000000f};
		for (uint32_t v : mov_imms)
		{
			add_case(fmt("mov%s r3, #0x%x", COND[c], v), [&](Assembler& a) { a.mov_imm(R3, v, c); });
			add_case(fmt("mvn%s lr, #0x%x", COND[c], v), [&](Assembler& a) { a.mov_imm(LR, ~v, c); });
		}
		add_case(fmt("sub%s r0, r1, #4", COND[c]), [&](Assembler& a) { a.add_imm(R0, R1, (uint32_t)-4, R12, c); });
		add_case(fmt("adds%s r0, r1, #4", COND[c]), [&](Assembler& a) { a.sub_imm(R0, R1, (uint32_t)-4, R12, c, true); });
		add_case(fmt("sbc%s r0, r1, #0", COND[c]), [&](Assembler& a) { a.adc_imm(R0, R1, 0xffffffff, R12, c); });
		add_case(fmt("adcs%s r0, r1, #0xff", COND[c]), [&](Assembler& a) { a.sbc_imm(R0, R1, 0xffffff00, R12, c, true); });
		add_case(fmt("bic%s r0, r1, #0xff", COND[c]), [&](Assembler& a) { a.and__imm(R0, R1, 0xffffff00, R12, c); });
		add_case(fmt("and%s r0, r1, #0xff", COND[c]), [&](Assembler& a) { a.bic_imm(R0, R1, 0xffffff00, R12, c); });
		// ... and with scratch (s forbids the and/bic swap).
		add_case(fmt("mvn%s r12, #0xff\nands%s r0, r1, r12", COND[c], COND[c]),
			[&](Assembler& a) { a.and__imm(R0, R1, 0xffffff00, R12, c, true); });
		add_case(fmt("mov%s r3, #0x12\norr%s r3, r3, #0x3400\nrsb%s r0, r1, r3", COND[c], COND[c], COND[c]),
			[&](Assembler& a) { a.rsb_imm(R0, R1, 0x3412, R3, c); });

		// Comparisons.
		for (int i = 0; i < 6; i++)
		{
			Reg rn = regs[i][1], rm = regs[i][2];
			add_case(fmt("cmp%s %s, %s", COND[c], REG[rn], REG[rm]), [&](Assembler& a) { a.cmp(rn, rm, c); });
			add_case(fmt("cmn%s %s, %s", COND[c], REG[rn], REG[rm]), [&](Assembler& a) { a.cmn(rn, rm, c); });
			add_case(fmt("tst%s %s, %s", COND[c], REG[rn], REG[rm]), [&](Assembler& a) { a.tst(rn, rm, c); });
			add_case(fmt("teq%s %s, %s", COND[c], REG[rn], REG[rm]), [&](Assembler& a) { a.teq(rn, rm, c); });
		}
		add_case(fmt("cmp%s r0, #0", COND[c]), [&](Assembler& a) { a.cmp_imm(R0, 0, R12, c); });
		add_case(fmt("cmp%s r5, #0xff000000", COND[c]), [&](Assembler& a) { a.cmp_imm(R5, (int32_t)0xff000000, R12, c); });
		add_case(fmt("cmn%s r5, #1", COND[c]), [&](Assembler& a) { a.cmp_imm(R5, -1, R12, c); });
		add_case(fmt("cmn%s r5, #10000 & 0xff00", COND[c]), [&](Assembler& a) { a.cmp_imm(R5, -(10000 & 0xff00), R12, c); });
		add_case(fmt("cmp%s r2, #0x80000000", COND[c]), [&](Assembler& a) { a.cmp_imm(R2, INT32_MIN, R12, c); });
		add_case(fmt("mov%s r12, #0x10\norr%s r12, r12, #0x2700\ncmp%s r1, r12", COND[c], COND[c], COND[c]),
			[&](Assembler& a) { a.cmp_imm(R1, 10000, R12, c); });
		add_case(fmt("tst%s r1, #0x80000000", COND[c]), [&](Assembler& a) { a.tst_imm(R1, 0x80000000, R12, c); });
		add_case(fmt("mvn%s r2, #1\ntst%s r1, r2", COND[c], COND[c]), [&](Assembler& a) { a.tst_imm(R1, 0xfffffffe, R2, c); });

		// Multiplies, clz.
		add_case(fmt("mla%s r0, r1, r2, r3", COND[c]), [&](Assembler& a) { a.mla(R0, R1, R2, R3, c); });
		add_case(fmt("mla%s lr, r12, r9, r10", COND[c]), [&](Assembler& a) { a.mla(LR, R12, R9, R10, c); });
		add_case(fmt("smull%s r0, r1, r2, r3", COND[c]), [&](Assembler& a) { a.smull(R0, R1, R2, R3, c); });
		add_case(fmt("smull%s r12, lr, r4, r11", COND[c]), [&](Assembler& a) { a.smull(R12, LR, R4, R11, c); });
		// RdHi == Rs (and RdLo == Rm) are fine on ARMv6; the divide-by-10000 sequence uses it.
		add_case(fmt("smull%s lr, r12, r0, r12", COND[c]), [&](Assembler& a) { a.smull(LR, R12, R0, R12, c); });
		add_case(fmt("smull%s r0, r1, r0, r1", COND[c]), [&](Assembler& a) { a.smull(R0, R1, R0, R1, c); });
		add_case(fmt("umull%s lr, r12, r0, r12", COND[c]), [&](Assembler& a) { a.umull(LR, R12, R0, R12, c); });
		add_case(fmt("umull%s r0, r1, r2, r3", COND[c]), [&](Assembler& a) { a.umull(R0, R1, R2, R3, c); });
		add_case(fmt("umull%s r7, r6, r5, r4", COND[c]), [&](Assembler& a) { a.umull(R7, R6, R5, R4, c); });
		add_case(fmt("clz%s r0, r1", COND[c]), [&](Assembler& a) { a.clz(R0, R1, c); });
		add_case(fmt("clz%s lr, r12", COND[c]), [&](Assembler& a) { a.clz(LR, R12, c); });

		// Loads and stores.
		typedef void (Assembler::*Mem)(Reg, Reg, int32_t, Cond);
		struct MemOp { const char* name; Mem fn; bool half; };
		const MemOp mems[] = {
			{"ldr", &Assembler::ldr, false}, {"str", &Assembler::str, false},
			{"ldrb", &Assembler::ldrb, false}, {"strb", &Assembler::strb, false},
			{"ldrh", &Assembler::ldrh, true}, {"strh", &Assembler::strh, true},
			{"ldrsh", &Assembler::ldrsh, true}, {"ldrsb", &Assembler::ldrsb, true},
		};
		const int word_offs[] = {0, 1, -1, 4, -4, 255, -255, 256, 4095, -4095, 2048};
		const int half_offs[] = {0, 1, -1, 2, -2, 15, 16, -16, 0xab, 255, -255};
		int mi = 0;
		for (auto& m : mems)
		{
			for (int k = 0; k < 11; k++)
			{
				int off = m.half ? half_offs[k] : word_offs[k];
				auto& r = regs[(mi++) % 6];
				std::string text = off ? fmt("%s%s %s, [%s, #%d]", m.name, COND[c], REG[r[0]], REG[r[1]], off)
				                       : fmt("%s%s %s, [%s]", m.name, COND[c], REG[r[0]], REG[r[1]]);
				add_case(text, [&](Assembler& a) { (a.*m.fn)(r[0], r[1], off, c); });
			}
			add_case(fmt("%s%s r0, [sp, #8]", m.name, COND[c]), [&](Assembler& a) { (a.*m.fn)(R0, SP, 8, c); });
		}
		for (int lsl : {0, 1, 2, 31})
		{
			std::string sh = lsl ? fmt(", lsl #%d", lsl) : "";
			add_case(fmt("ldr%s r0, [r1, r2%s]", COND[c], sh.c_str()), [&](Assembler& a) { a.ldr(R0, R1, R2, lsl, c); });
			add_case(fmt("str%s lr, [r12, r5%s]", COND[c], sh.c_str()), [&](Assembler& a) { a.str(LR, R12, R5, lsl, c); });
		}

		// push / pop
		const uint16_t masks[] = {(uint16_t)reg_mask(R4), (uint16_t)(reg_mask(R4) | reg_mask(LR)),
			0x5fff, (uint16_t)reg_mask(PC), (uint16_t)(reg_mask(R0) | reg_mask(R1) | reg_mask(R12) | reg_mask(PC))};
		for (uint16_t mask : masks)
		{
			std::string list = "{";
			for (int i = 0; i < 16; i++)
				if (mask & (1 << i))
					list += std::string(list.size() > 1 ? ", " : "") + REG[i];
			list += "}";
			add_case(fmt("stmdb%s sp!, %s", COND[c], list.c_str()), [&](Assembler& a) { a.push(mask, c); });
			add_case(fmt("ldmia%s sp!, %s", COND[c], list.c_str()), [&](Assembler& a) { a.pop(mask, c); });
		}

		// Control flow.
		add_case(fmt("bx%s lr", COND[c]), [&](Assembler& a) { a.bx(LR, c); });
		add_case(fmt("bx%s r3", COND[c]), [&](Assembler& a) { a.bx(R3, c); });
		add_case(fmt("blx%s r12", COND[c]), [&](Assembler& a) { a.blx(R12, c); });
		add_case(fmt("blx%s r0", COND[c]), [&](Assembler& a) { a.blx(R0, c); });
		add_case(fmt("b%s 1f\nnop\nnop\n1:", COND[c]), [&](Assembler& a) {
			Label l = a.new_label();
			a.b(l, c);
			a.nop();
			a.nop();
			a.bind(l);
		});
		add_case(fmt("1:\nnop\nb%s 1b\nbl%s 1b", COND[c], COND[c]), [&](Assembler& a) {
			Label l = a.new_label();
			a.bind(l);
			a.nop();
			a.b(l, c);
			a.bl(l, c);
		});
		add_case(fmt("bl%s 1f\nb%s 2f\n1:\nb%s 1b\n2:\nb%s 2b", COND[c], COND[c], COND[c], COND[c]),
			[&](Assembler& a) {
				Label l1 = a.new_label(), l2 = a.new_label();
				a.bl(l1, c);
				a.b(l2, c);
				a.bind(l1);
				a.b(l1, c);
				a.bind(l2);
				a.b(l2, c);
			});
		add_case(fmt("b%s 1f\n1:", COND[c]), [&](Assembler& a) {
			Label l = a.new_label();
			a.b(l, c);
			a.bind(l);
		});
	}

	add_case("nop", [](Assembler& a) { a.nop(); });
	add_case("bkpt #0", [](Assembler& a) { a.bkpt(); });
	add_case("bkpt #0x1234", [](Assembler& a) { a.bkpt(0x1234); });
	add_case("bkpt #0xffff", [](Assembler& a) { a.bkpt(0xffff); });
	add_case(".word 0x12345678", [](Assembler& a) { a.emit32(0x12345678); });
	add_case("mov r12, #0x78\norr r12, r12, #0x5600\norr r12, r12, #0x340000\norr r12, r12, #0x12000000\nblx r12",
		[](Assembler& a) { a.call((const void*)(uintptr_t)0x12345678); });
	add_case("add r0, pc, #4\nadd r0, r0, #0\nadd r0, r0, #0\n1:", [](Assembler& a) {
		Label l = a.new_label();
		a.adr(R0, l);
		a.bind(l);
	});
	add_case("1:\nnop\nsub lr, pc, #12\nsub lr, lr, #0\nsub lr, lr, #0", [](Assembler& a) {
		Label l = a.new_label();
		a.bind(l);
		a.nop();
		a.adr(LR, l);
	});
	add_case("add r1, r2, #0x10000\nldr r0, [r1, #4]", [](Assembler& a) { a.ldr_far(R0, R2, 0x10004, R1); });
	add_case("sub r12, r2, #0x10000\nstr r0, [r12, #-4]", [](Assembler& a) { a.str_far(R0, R2, -0x10004, R12); });
	add_case("ldr r0, [r2, #-4095]", [](Assembler& a) { a.ldr_far(R0, R2, -4095, R12); });
	add_case("add r0, r2, #0x12000\nldr r0, [r0, #0x345]",
		[](Assembler& a) { a.ldr_far(R0, R2, 0x12345, R0); });
	add_case("add r0, r1, #0x45\nadd r0, r0, #0x2300\nadd r0, r0, #0x10000",
		[](Assembler& a) { a.add_imm(R0, R1, 0x12345); });
	add_case("mov r12, #0x56\norr r12, r12, #0x3400\norr r12, r12, #0x120000\nldr r0, [r2, r12]",
		[](Assembler& a) { a.ldr_far(R0, R2, 0x123456, R12); });
	add_case("mvn r12, #0x55\nbic r12, r12, #0x3400\nbic r12, r12, #0x120000\nstr r0, [r2, r12]",
		[](Assembler& a) { a.str_far(R0, R2, -0x123456, R12); });
	add_case("mov r12, #0x1000\norr r12, r12, #0x100000\nldr r0, [r2, -r12]",
		[](Assembler& a) { a.ldr_far(R0, R2, -0x101000, R12); });
	add_case("eorsne r5, r4, #120, 12", [](Assembler& a) { a.emit32(0x12345678); });
	add_case("sub r0, r1, #0x45\nsub r0, r0, #0x2300\nsub r0, r0, #0x10000",
		[](Assembler& a) { a.add_imm(R0, R1, (uint32_t)-0x12345); });
}

// --- 2. evaluator for the data-processing / address subset ---

struct Cpu
{
	uint32_t r[16] = {};
	bool n = false, z = false, c = false, v = false;
	uint32_t pc_base = 0; // address of offset 0
	std::vector<uint32_t> addrs; // effective addresses of loads/stores
	bool bad = false;
};

static bool cond_pass(const Cpu& s, unsigned cond)
{
	switch (cond)
	{
		case EQ: return s.z;
		case NE: return !s.z;
		case HS: return s.c;
		case LO: return !s.c;
		case MI: return s.n;
		case PL: return !s.n;
		case VS: return s.v;
		case VC: return !s.v;
		case HI: return s.c && !s.z;
		case LS: return !s.c || s.z;
		case GE: return s.n == s.v;
		case LT: return s.n != s.v;
		case GT: return !s.z && s.n == s.v;
		case LE: return s.z || s.n != s.v;
		default: return true;
	}
}

static uint32_t awc(uint32_t x, uint32_t y, bool cin, bool& cout, bool& vout)
{
	uint64_t u = (uint64_t)x + y + cin;
	int64_t sg = (int64_t)(int32_t)x + (int32_t)y + cin;
	uint32_t res = (uint32_t)u;
	cout = (u >> 32) != 0;
	vout = (int64_t)(int32_t)res != sg;
	return res;
}

static uint32_t rd_reg(const Cpu& s, unsigned r, uint32_t at)
{
	return r == 15 ? s.pc_base + at + 8 : s.r[r];
}

static uint32_t shift_c(uint32_t val, unsigned type, unsigned amount, bool cin, bool& cout)
{
	cout = cin;
	if (amount == 0)
		return val;
	switch (type)
	{
		case 0:
			if (amount >= 33) { cout = false; return 0; }
			cout = (val >> (32 - amount)) & 1;
			return amount == 32 ? 0 : val << amount;
		case 1:
			if (amount >= 33) { cout = false; return 0; }
			cout = (val >> (amount - 1)) & 1;
			return amount == 32 ? 0 : val >> amount;
		case 2:
			if (amount >= 32) { cout = val >> 31; return (uint32_t)((int32_t)val >> 31); }
			cout = (val >> (amount - 1)) & 1;
			return (uint32_t)((int32_t)val >> amount);
		default:
		{
			amount &= 31;
			uint32_t res = amount ? (val >> amount) | (val << (32 - amount)) : val;
			cout = res >> 31;
			return res;
		}
	}
}

static void step(Cpu& s, uint32_t w, uint32_t at)
{
	if (!cond_pass(s, w >> 28))
		return;
	if ((w & 0x0c000000) == 0x04000000) // ldr/str word/byte
	{
		uint32_t off;
		if (w & (1u << 25))
		{
			bool cdummy;
			off = shift_c(rd_reg(s, w & 15, at), (w >> 5) & 3, (w >> 7) & 31, false, cdummy);
		}
		else
			off = w & 0xfff;
		uint32_t base = rd_reg(s, (w >> 16) & 15, at);
		uint32_t addr = (w & (1u << 23)) ? base + off : base - off;
		s.addrs.push_back(addr);
		if (w & (1u << 20))
			s.r[(w >> 12) & 15] = addr ^ 0x5a5a5a5a; // a recognizable loaded value
		return;
	}
	if ((w & 0x0c000000) != 0 || (!(w & (1u << 25)) && (w & 0x90) == 0x90))
	{
		s.bad = true;
		return;
	}
	unsigned op = (w >> 21) & 15;
	bool sflag = w & (1u << 20);
	unsigned rd = (w >> 12) & 15;
	uint32_t rn = rd_reg(s, (w >> 16) & 15, at);
	uint32_t op2;
	bool sc;
	if (w & (1u << 25))
	{
		unsigned rot = ((w >> 8) & 15) * 2;
		op2 = rot ? ((w & 0xff) >> rot) | ((w & 0xff) << (32 - rot)) : (w & 0xff);
		sc = rot ? op2 >> 31 : s.c;
	}
	else
	{
		uint32_t rm = rd_reg(s, w & 15, at);
		unsigned type = (w >> 5) & 3;
		if (w & 0x10)
			op2 = shift_c(rm, type, s.r[(w >> 8) & 15] & 0xff, s.c, sc);
		else
		{
			unsigned amount = (w >> 7) & 31;
			if (amount == 0 && type == 3)
			{
				sc = rm & 1;
				op2 = (rm >> 1) | ((uint32_t)s.c << 31);
			}
			else
				op2 = shift_c(rm, type, amount == 0 && type != 0 ? 32 : amount, s.c, sc);
		}
	}
	uint32_t res = 0;
	bool c = s.c, v = s.v, write = true, arith = false;
	switch (op)
	{
		case 0: res = rn & op2; break;
		case 1: res = rn ^ op2; break;
		case 2: res = awc(rn, ~op2, true, c, v); arith = true; break;
		case 3: res = awc(~rn, op2, true, c, v); arith = true; break;
		case 4: res = awc(rn, op2, false, c, v); arith = true; break;
		case 5: res = awc(rn, op2, s.c, c, v); arith = true; break;
		case 6: res = awc(rn, ~op2, s.c, c, v); arith = true; break;
		case 7: res = awc(~rn, op2, s.c, c, v); arith = true; break;
		case 8: res = rn & op2; write = false; break;
		case 9: res = rn ^ op2; write = false; break;
		case 10: res = awc(rn, ~op2, true, c, v); arith = true; write = false; break;
		case 11: res = awc(rn, op2, false, c, v); arith = true; write = false; break;
		case 12: res = rn | op2; break;
		case 13: res = op2; break;
		case 14: res = rn & ~op2; break;
		case 15: res = ~op2; break;
	}
	if (write)
		s.r[rd] = res;
	if (sflag)
	{
		s.n = res >> 31;
		s.z = res == 0;
		s.c = arith ? c : sc;
		if (arith)
			s.v = v;
	}
}

static Cpu run(const Assembler& a, Cpu s)
{
	auto w = words_of(a);
	for (size_t i = 0; i < w.size(); i++)
		step(s, w[i], (uint32_t)(i * 4));
	return s;
}

static std::vector<uint32_t> interesting_values()
{
	std::set<uint32_t> vals = {0, 1, 2, 3, 0xffffffff, 0xfffffffe, 0x80000000, 0x7fffffff, 0x80000001, 10000,
		(uint32_t)-10000, 0xff, 0x100, 0x101, 0x1fe, 0x1ff, 0x3fc, 0xff00, 0xffff, 0x10000, 0xffff0000,
		0x00ffff00, 0xff00ff00, 0x00ff00ff, 0x12345678, 0xdeadbeef, 0xcafebabe, 0x55555555, 0xaaaaaaaa,
		0xf000000f, 0x0ff00ff0, 0x80000080, 0xfffff000, 0x00000fff, 0x00001000, 0x01000000, 0xff000000,
		0xfff00001, 0x7fff8000};
	for (unsigned r = 0; r < 32; r += 2)
	{
		auto rot = [&](uint32_t v) { return r ? (v >> r) | (v << (32 - r)) : v; };
		for (uint32_t base : {0xffu, 0x1ffu, 0x100u, 0x80u, 0x81u, 0x1u, 0x17fu, 0x3fcu})
		{
			uint32_t v = rot(base);
			vals.insert(v);
			vals.insert(~v);
			vals.insert(0u - v);
			vals.insert(v + 1);
			vals.insert(v - 1);
		}
	}
	std::mt19937 rng(1234);
	for (int i = 0; i < 150; i++)
	{
		vals.insert(rng());
		vals.insert(rng() & 0xffff);
		vals.insert(rng() | 0xffff0000);
	}
	return {vals.begin(), vals.end()};
}

static Cpu random_cpu(std::mt19937& rng)
{
	Cpu s;
	for (int i = 0; i < 15; i++)
		s.r[i] = rng();
	unsigned f = rng();
	s.n = f & 1;
	s.z = f & 2;
	s.c = f & 4;
	s.v = f & 8;
	s.pc_base = rng() & ~3u;
	return s;
}

static bool same_except(const Cpu& a, const Cpu& b, std::initializer_list<int> skip)
{
	for (int i = 0; i < 15; i++)
	{
		bool skipped = false;
		for (int k : skip)
			skipped |= k == i;
		if (!skipped && a.r[i] != b.r[i])
			return false;
	}
	return true;
}

// Every listing line, all emitted code, for the reassembly check.
static std::string roundtrip_text;
static size_t roundtrip_words;
static std::vector<uint32_t> roundtrip_expected;

static void add_roundtrip(const Assembler& a)
{
	std::string list = a.listing();
	auto words = words_of(a);
	size_t pos = 0, idx = 0;
	while (pos < list.size())
	{
		size_t end = list.find('\n', pos);
		std::string line = list.substr(pos, end - pos);
		pos = end + 1;
		if (line.size() < 20 || line[8] != ':')
			continue;
		std::string mnem = line.substr(20);
		uint32_t w = (uint32_t)strtoul(line.substr(10, 8).c_str(), nullptr, 16);
		CHECK(idx < words.size() && words[idx] == w, "listing word mismatch at %zu", idx);
		// Branches print an offset-relative target; keep them as raw words.
		if (mnem[0] == 'b' && mnem.find(" 0x") != std::string::npos && mnem.rfind("bkpt", 0) != 0 &&
		    mnem.rfind("bic", 0) != 0)
			mnem = fmt(".word 0x%08x", w);
		roundtrip_text += mnem + "\n";
		roundtrip_expected.push_back(w);
		idx++;
	}
	CHECK(idx == words.size(), "listing has %zu instructions, code %zu", idx, words.size());
	roundtrip_words += idx;
}

static void test_sequences()
{
	std::mt19937 rng(42);
	auto vals = interesting_values();
	Assembler big; // everything emitted here, reassembled from its listing at the end

	// mov_imm: value, flags untouched, other registers untouched, at most 4 instructions.
	int max_len = 0;
	for (uint32_t v : vals)
	{
		for (Reg rd : {R0, R7, R12, LR})
		{
			Assembler a;
			a.mov_imm(rd, v);
			Cpu s0 = random_cpu(rng);
			Cpu s = run(a, s0);
			int len = (int)a.code().size() / 4;
			max_len = std::max(max_len, len);
			CHECK(a.ok() && !s.bad && s.r[rd] == v, "mov_imm %s, 0x%08x -> 0x%08x", REG[rd], v, s.r[rd]);
			CHECK(s.n == s0.n && s.z == s0.z && s.c == s0.c && s.v == s0.v, "mov_imm 0x%08x touched flags", v);
			CHECK(same_except(s, s0, {rd}), "mov_imm 0x%08x touched another register", v);
			CHECK(len >= 1 && len <= 4, "mov_imm 0x%08x took %d instructions", v, len);
			uint32_t e;
			if (encode_imm(v, e) || encode_imm(~v, e))
				CHECK(len == 1, "mov_imm 0x%08x: encodable but %d instructions", v, len);
		}
		big.mov_imm(R3, v);
		big.mov_imm(R9, v, NE);
		// Conditional: every instruction carries the condition.
		Assembler a;
		a.mov_imm(R2, v, EQ);
		Cpu s0 = random_cpu(rng);
		for (bool z : {false, true})
		{
			s0.z = z;
			Cpu s = run(a, s0);
			CHECK(s.r[R2] == (z ? v : s0.r[R2]), "conditional mov_imm 0x%08x z=%d", v, z);
		}
	}
	printf("mov_imm: longest sequence over %zu values: %d\n", vals.size(), max_len);

	// Random values: length only (fast).
	{
		long hist[5] = {};
		for (int i = 0; i < 1000000; i++)
		{
			Assembler a;
			uint32_t v = rng();
			a.mov_imm(R0, v);
			size_t len = a.code().size() / 4;
			if (len < 1 || len > 4)
			{
				CHECK(false, "mov_imm 0x%08x took %zu instructions", v, len);
				break;
			}
			hist[len]++;
			if (i % 997 == 0)
			{
				Cpu s = run(a, random_cpu(rng));
				CHECK(s.r[R0] == v, "mov_imm random 0x%08x", v);
			}
		}
		passes++;
		printf("mov_imm on 1M random values: 1:%ld 2:%ld 3:%ld 4:%ld\n", hist[1], hist[2], hist[3], hist[4]);
	}

	// *_imm: results and flags against a reference, with and without s.
	typedef void (Assembler::*Dp3I)(Reg, Reg, uint32_t, Reg, Cond, bool);
	struct Op { const char* name; Dp3I fn; int kind; };
	// kind: 0 add, 1 sub, 2 rsb, 3 adc, 4 sbc, 5 and, 6 orr, 7 eor, 8 bic
	const Op ops[] = {{"add", &Assembler::add_imm, 0}, {"sub", &Assembler::sub_imm, 1},
		{"rsb", &Assembler::rsb_imm, 2}, {"adc", &Assembler::adc_imm, 3}, {"sbc", &Assembler::sbc_imm, 4},
		{"and", &Assembler::and__imm, 5}, {"orr", &Assembler::orr_imm, 6}, {"eor", &Assembler::eor_imm, 7},
		{"bic", &Assembler::bic_imm, 8}};
	const uint32_t rn_vals[] = {0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 10000, 0x12345678};
	int checked = 0;
	for (auto& op : ops)
		for (uint32_t imm : vals)
			for (int s = 0; s < 2; s++)
			{
				struct Regs { Reg rd, rn, scratch; } rs[] = {{R0, R1, R12}, {R4, R4, R12}, {R2, R3, R2}};
				for (auto& rr : rs)
				{
					Assembler a;
					(a.*op.fn)(rr.rd, rr.rn, imm, rr.scratch, AL, s);
					CHECK(a.ok(), "%s_imm 0x%08x: %s", op.name, imm, a.error().c_str());
					if (&rr == &rs[0])
						(big.*op.fn)(R0, R1, imm, R12, s ? AL : GT, s);
					for (uint32_t rnv : rn_vals)
					{
						Cpu s0 = random_cpu(rng);
						s0.r[rr.rn] = rnv;
						Cpu st = run(a, s0);
						bool c = s0.c, v = s0.v;
						uint32_t exp = 0;
						bool arith = op.kind <= 4;
						switch (op.kind)
						{
							case 0: exp = awc(rnv, imm, false, c, v); break;
							case 1: exp = awc(rnv, ~imm, true, c, v); break;
							case 2: exp = awc(~rnv, imm, true, c, v); break;
							case 3: exp = awc(rnv, imm, s0.c, c, v); break;
							case 4: exp = awc(rnv, ~imm, s0.c, c, v); break;
							case 5: exp = rnv & imm; break;
							case 6: exp = rnv | imm; break;
							case 7: exp = rnv ^ imm; break;
							case 8: exp = rnv & ~imm; break;
						}
						bool good = !st.bad && st.r[rr.rd] == exp && same_except(st, s0, {rr.rd, rr.scratch});
						if (s)
						{
							good = good && st.n == (exp >> 31) && st.z == (exp == 0);
							// Logical ops: C comes from the shifter (depends on the encoding).
							if (arith)
								good = good && st.c == c && st.v == v;
							else
								good = good && st.v == s0.v;
						}
						else
							good = good && st.n == s0.n && st.z == s0.z && st.c == s0.c && st.v == s0.v;
						CHECK(good, "%s%s_imm %s, %s(0x%x), 0x%08x, scratch %s -> 0x%08x (expected 0x%08x)",
							op.name, s ? "s" : "", REG[rr.rd], REG[rr.rn], rnv, imm, REG[rr.scratch], st.r[rr.rd], exp);
						checked++;
					}
				}
			}
	printf("*_imm: %d evaluations\n", checked);

	// cmp_imm / tst_imm flags.
	for (uint32_t imm : vals)
	{
		Assembler a, t;
		a.cmp_imm(R1, (int32_t)imm, R12);
		t.tst_imm(R1, imm, R12);
		big.cmp_imm(R1, (int32_t)imm, R12, LE);
		big.tst_imm(R1, imm, R2, AL);
		for (uint32_t rnv : rn_vals)
		{
			Cpu s0 = random_cpu(rng);
			s0.r[R1] = rnv;
			Cpu st = run(a, s0);
			bool c, v;
			uint32_t exp = awc(rnv, ~imm, true, c, v);
			CHECK(!st.bad && st.n == (exp >> 31) && st.z == (exp == 0) && st.c == c && st.v == v &&
				same_except(st, s0, {R12}), "cmp_imm 0x%x, 0x%08x", rnv, imm);
			st = run(t, s0);
			CHECK(!st.bad && st.n == ((rnv & imm) >> 31) && st.z == ((rnv & imm) == 0) && st.v == s0.v &&
				same_except(st, s0, {R12}), "tst_imm 0x%x, 0x%08x", rnv, imm);
		}
	}

	// ldr_far / str_far: effective address.
	std::vector<int32_t> far_offs = {0, 4, -4, 4095, -4095, 4096, -4096, 0x10004, -0x10004, 0x12345, -0x12345,
		0x7ffffffc, (int32_t)0x80000000, 1 << 20, -(1 << 20), 0x00ff0fff, 123456789, -123456789};
	for (int i = 0; i < 50; i++)
		far_offs.push_back((int32_t)rng());
	for (int32_t off : far_offs)
	{
		Assembler a;
		a.ldr_far(R0, R1, off, R12);
		a.str_far(R2, R3, off, R12);
		a.ldr_far(R5, R6, off, R5);
		CHECK(a.ok(), "far %d: %s", off, a.error().c_str());
		big.ldr_far(R0, R1, off, R12);
		big.str_far(R2, R3, off, R12);
		Cpu s0 = random_cpu(rng);
		Cpu st = run(a, s0);
		CHECK(!st.bad && st.addrs.size() == 3 && st.addrs[0] == s0.r[R1] + (uint32_t)off &&
			st.addrs[1] == s0.r[R3] + (uint32_t)off && st.addrs[2] == s0.r[R6] + (uint32_t)off,
			"ldr_far/str_far offset %d", off);
		CHECK(st.r[R0] == ((s0.r[R1] + (uint32_t)off) ^ 0x5a5a5a5a) && st.r[R2] == s0.r[R2],
			"ldr_far/str_far values, offset %d", off);
		CHECK(a.code().size() / 4 <= 3 * 5, "far sequences too long for %d", off);
	}

	// Everything else once, for the reassembly check.
	big.comment("a comment");
	big.ldrsh(R0, R1, -2);
	big.call((const void*)(uintptr_t)0xdeadbeef, LR);
	CHECK(big.ok(), "big: %s", big.error().c_str());
	big.finalize();
	add_roundtrip(big);
	CHECK(big.listing().find("; a comment") != std::string::npos, "comment missing from listing");
}

// adr: position independent, every offset, both directions.
static void test_adr()
{
	std::mt19937 rng(7);
	const size_t dists[] = {0, 4, 8, 12, 16, 1020, 1024, 1028, 4096, 0x3fffc, 0x40000, 0x40004, 0x123454,
		(1u << 24) + 12};
	for (size_t d : dists)
		for (int dir = 0; dir < 2; dir++)
		{
			Assembler a;
			Label l = a.new_label();
			size_t adr_at;
			if (dir == 0) // forward: adr, padding, label
			{
				a.nop();
				adr_at = a.offset();
				a.adr(R3, l);
				for (size_t i = 0; i < d / 4; i++)
					a.emit32(0xe320f000);
				a.bind(l);
			}
			else // backward: label, padding, adr
			{
				a.nop();
				a.bind(l);
				for (size_t i = 0; i < d / 4; i++)
					a.emit32(0xe320f000);
				adr_at = a.offset();
				a.adr(R3, l);
			}
			a.nop();
			CHECK(a.finalize(), "adr finalize: %s", a.error().c_str());
			if (d < 64)
				add_roundtrip(a);
			for (int k = 0; k < 3; k++)
			{
				Cpu s = random_cpu(rng);
				uint32_t base = s.pc_base;
				for (int i = 0; i < 3; i++)
				{
					uint32_t w;
					memcpy(&w, a.code().data() + adr_at + 4 * i, 4);
					step(s, w, (uint32_t)(adr_at + 4 * i));
				}
				CHECK(!s.bad && s.r[R3] == base + a.label_offset(l), "adr dist %zu dir %d base 0x%x: 0x%x vs 0x%x",
					d, dir, base, s.r[R3], (unsigned)(base + a.label_offset(l)));
			}
		}
}

// --- 3. labels and errors ---

static int32_t branch_target(const Assembler& a, size_t at)
{
	uint32_t w;
	memcpy(&w, a.code().data() + at, 4);
	return (int32_t)at + 8 + ((int32_t)(w << 8) >> 6);
}

static void test_labels_and_errors()
{
	// Many branches in both directions, conditional and not, checked by decoding.
	{
		std::mt19937 rng(99);
		Assembler a;
		std::vector<Label> labels;
		for (int i = 0; i < 64; i++)
			labels.push_back(a.new_label());
		std::vector<std::pair<size_t, int>> branches;
		for (int i = 0; i < 2000; i++)
		{
			int li = rng() % 64;
			if (rng() % 8 == 0 && !a.is_bound(labels[li]))
				a.bind(labels[li]);
			branches.push_back({a.offset(), li});
			if (rng() % 2)
				a.b(labels[li], (Cond)(rng() % 15));
			else
				a.bl(labels[li], (Cond)(rng() % 15));
			for (int k = rng() % 4; k > 0; k--)
				a.nop();
		}
		for (auto& l : labels)
			if (!a.is_bound(l))
				a.bind(l);
		CHECK(a.finalize(), "branches: %s", a.error().c_str());
		bool all = true;
		for (auto& [at, li] : branches)
			all &= branch_target(a, at) == (int32_t)a.label_offset(labels[li]);
		CHECK(all, "branch targets");
	}

	auto nops = [](Assembler& a, size_t bytes) {
		for (size_t i = 0; i < bytes / 4; i++)
			a.emit32(0xe320f000);
	};
	// Forward limit: +(2^23 - 1) words from pc.
	{
		Assembler a;
		Label l = a.new_label();
		a.b(l);
		nops(a, ((1u << 23) - 1) * 4 + 4); // label at 8 + 4*(2^23-1)
		a.bind(l);
		CHECK(a.finalize() && branch_target(a, 0) == (int32_t)a.label_offset(l), "max forward branch: %s",
			a.error().c_str());
	}
	{
		Assembler a;
		Label l = a.new_label();
		a.b(l, NE);
		nops(a, ((1u << 23) - 1) * 4 + 8);
		a.bind(l);
		CHECK(!a.finalize() && a.error().find("out of range") != std::string::npos, "forward branch out of range");
	}
	// Backward limit: -2^23 words from pc.
	{
		Assembler a;
		Label l = a.new_label();
		a.bind(l);
		nops(a, (1u << 25) - 8);
		size_t at = a.offset();
		a.bl(l);
		CHECK(a.finalize() && branch_target(a, at) == 0, "max backward branch: %s", a.error().c_str());
	}
	{
		Assembler a;
		Label l = a.new_label();
		a.bind(l);
		nops(a, (1u << 25) - 4);
		a.b(l);
		CHECK(!a.finalize() && a.error().find("out of range") != std::string::npos, "backward branch out of range");
	}
	// adr range: 2^26 - 4 bytes is the farthest.
	{
		Assembler a;
		Label l = a.new_label();
		a.adr(R0, l);
		nops(a, (1u << 26) - 8); // label at 8 + (2^26 - 4)
		a.bind(l);
		CHECK(a.finalize(), "max adr: %s", a.error().c_str());
		Cpu s;
		s.pc_base = 0x10000000;
		for (int i = 0; i < 3; i++)
		{
			uint32_t w;
			memcpy(&w, a.code().data() + 4 * i, 4);
			step(s, w, 4 * i);
		}
		CHECK(s.r[R0] == 0x10000000 + a.label_offset(l), "max adr value");
		Assembler b2;
		Label m = b2.new_label();
		b2.adr(R0, m);
		nops(b2, (1u << 26));
		b2.bind(m);
		CHECK(!b2.finalize() && b2.error().find("out of range") != std::string::npos, "adr out of range");
	}

	struct ErrCase { const char* what; std::function<void(Assembler&)> fn; };
	const ErrCase errs[] = {
		{"unbound label", [](Assembler& a) { Label l = a.new_label(); a.b(l); }},
		{"unbound adr label", [](Assembler& a) { Label l = a.new_label(); a.adr(R0, l); }},
		{"invalid label", [](Assembler& a) { a.b(Label{}); }},
		{"foreign label", [](Assembler& a) { Label l; l.id = 5; a.bl(l); }},
		{"bind twice", [](Assembler& a) { Label l = a.new_label(); a.bind(l); a.bind(l); }},
		{"bind invalid", [](Assembler& a) { a.bind(Label{}); }},
		{"ldr offset", [](Assembler& a) { a.ldr(R0, R1, 4096); }},
		{"strb offset", [](Assembler& a) { a.strb(R0, R1, -4096); }},
		{"ldrh offset", [](Assembler& a) { a.ldrh(R0, R1, 256); }},
		{"ldrsb offset", [](Assembler& a) { a.ldrsb(R0, R1, -256); }},
		{"shift amount", [](Assembler& a) { a.add(R0, R1, R2, Shift::LSL, 32); }},
		{"shift amount ror", [](Assembler& a) { a.mov(R0, R1, Shift::ROR, 32); }},
		{"shift amount neg", [](Assembler& a) { a.mov(R0, R1, Shift::ASR, -1); }},
		{"index shift", [](Assembler& a) { a.ldr(R0, R1, R2, 32); }},
		{"scratch == rn", [](Assembler& a) { a.orr_imm(R0, R12, 0x12345678, R12, AL, true); }},
		{"cmp scratch == rn", [](Assembler& a) { a.cmp_imm(R12, 0x12345678, R12); }},
		{"tst scratch == rn", [](Assembler& a) { a.tst_imm(R1, 0x12345678, R1); }},
		{"far scratch == rn", [](Assembler& a) { a.ldr_far(R0, R1, 0x12345, R1); }},
		{"far store scratch == rt", [](Assembler& a) { a.str_far(R0, R1, 0x12345, R0); }},
		{"empty push", [](Assembler& a) { a.push(0); }},
		{"empty pop", [](Assembler& a) { a.pop(0); }},
		{"adr pc", [](Assembler& a) { Label l = a.new_label(); a.bind(l); a.adr(PC, l); }},
		{"bad register", [](Assembler& a) { a.mov(R0, (Reg)16); }},
		{"bad condition", [](Assembler& a) { a.nop(); a.mov(R0, R1, (Cond)15); }},
		{"smull same", [](Assembler& a) { a.smull(R0, R0, R1, R2); }},
		{"umull same", [](Assembler& a) { a.umull(R3, R3, R1, R2); }},
		{"smull pc", [](Assembler& a) { a.smull(R0, R1, PC, R2); }},
		{"mul pc", [](Assembler& a) { a.mul(PC, R1, R2); }},
		{"clz pc", [](Assembler& a) { a.clz(R0, PC); }},
	};
	for (auto& e : errs)
	{
		Assembler a;
		e.fn(a);
		a.nop();
		bool ok = a.finalize();
		CHECK(!ok && !a.ok() && !a.error().empty(), "expected an error: %s", e.what);
	}
	// Small offsets through the scratch-taking paths do not need a distinct scratch.
	{
		Assembler a;
		a.orr_imm(R0, R12, 0xff, R12);
		a.cmp_imm(R12, 5, R12);
		a.ldr_far(R0, R1, 8, R1);
		CHECK(a.finalize(), "unused scratch == rn: %s", a.error().c_str());
	}
	// Errors keep the first message and do not stop emission.
	{
		Assembler a;
		a.ldr(R0, R1, 5000);
		a.push(0);
		a.nop();
		CHECK(a.error().find("offset") != std::string::npos && a.offset() == 4, "first error kept: %s",
			a.error().c_str());
	}
	CHECK(invert(EQ) == NE && invert(NE) == EQ && invert(HS) == LO && invert(GE) == LT && invert(GT) == LE &&
		invert(HI) == LS && invert(MI) == PL && invert(VS) == VC, "invert");
	uint32_t enc = 0;
	CHECK(encode_imm(0x3fc, enc) && enc == 0xfff, "encode_imm 0x3fc -> 0x%x", enc);
	CHECK(encode_imm(0xf000000f, enc) && enc == 0x2ff, "encode_imm 0xf000000f -> 0x%x", enc);
	CHECK(!encode_imm(0x101, enc) && !encode_imm(0x1fe0000f, enc), "encode_imm unencodable");
}

int main()
{
	char tmpl[] = "/tmp/arm32_asm_test.XXXXXX";
	const char* base = getenv("TMPDIR");
	std::string t = std::string(base ? base : "/tmp") + "/arm32_asm_test.XXXXXX";
	std::vector<char> buf(t.begin(), t.end());
	buf.push_back(0);
	char* dir = mkdtemp(buf.data());
	if (!dir)
		dir = mkdtemp(tmpl);
	if (!dir)
	{
		printf("mkdtemp failed\n");
		return 1;
	}
	tmp_dir = dir;

	// 1. Encoding table.
	build_cases();
	std::string text;
	for (size_t i = 0; i < cases.size(); i++)
		text += cases[i].text + fmt("\n.word 0x%08x\n", 0xf0000000u | (unsigned)i);
	std::vector<uint32_t> ref;
	if (!gas(text, ref))
		return 1;
	size_t pos = 0;
	int table_ok = 0;
	for (size_t i = 0; i < cases.size(); i++)
	{
		std::vector<uint32_t> expect;
		while (pos < ref.size() && ref[pos] != (0xf0000000u | (unsigned)i))
			expect.push_back(ref[pos++]);
		pos++;
		bool same = expect == cases[i].words;
		if (!same)
		{
			std::string got, want;
			for (uint32_t w : cases[i].words)
				got += fmt(" %08x", w);
			for (uint32_t w : expect)
				want += fmt(" %08x", w);
			CHECK(same, "'%s': got%s, as%s\n%s", cases[i].text.c_str(), got.c_str(), want.c_str(),
				cases[i].listing.c_str());
		}
		else
		{
			passes++;
			table_ok++;
		}
	}
	printf("encoding table: %d/%zu forms match as\n", table_ok, cases.size());

	// 2. Sequences (also feeds the reassembly check).
	test_sequences();
	test_adr();

	// Listing reassembly: every table case, plus the sequences.
	for (auto& c : cases)
	{
		Assembler a;
		for (uint32_t w : c.words)
			a.emit32(w);
		add_roundtrip(a);
	}
	std::vector<uint32_t> rt;
	if (!gas(roundtrip_text, rt))
		return 1;
	size_t mismatches = 0;
	{
		// Line-by-line diagnostics on mismatch.
		std::vector<std::string> lines;
		size_t p = 0;
		while (p < roundtrip_text.size())
		{
			size_t e = roundtrip_text.find('\n', p);
			lines.push_back(roundtrip_text.substr(p, e - p));
			p = e + 1;
		}
		if (rt.size() != roundtrip_expected.size())
			CHECK(false, "reassembled listing: %zu words, expected %zu", rt.size(), roundtrip_expected.size());
		for (size_t i = 0; i < rt.size() && i < roundtrip_expected.size(); i++)
			if (rt[i] != roundtrip_expected[i])
			{
				if (++mismatches <= 20)
					CHECK(false, "listing line '%s' reassembles to %08x, emitted %08x", lines[i].c_str(), rt[i],
						roundtrip_expected[i]);
			}
		if (!mismatches && rt.size() == roundtrip_expected.size())
			passes++;
		else
			failures += mismatches > 20 ? (int)(mismatches - 20) : 0;
	}
	printf("listing reassembly: %zu instructions, %zu mismatches\n", roundtrip_words, mismatches);

	// 3. Labels and errors.
	test_labels_and_errors();

	std::string cleanup = "rm -rf " + tmp_dir;
	if (system(cleanup.c_str()) != 0)
		printf("could not remove %s\n", tmp_dir.c_str());

	printf("%d checks passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
