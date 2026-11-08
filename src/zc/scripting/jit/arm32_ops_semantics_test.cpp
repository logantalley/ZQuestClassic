// Host-side test of the arm32 JIT's arithmetic helpers (jit_arm32_ops.cpp) against the
// interpreter's formulas (ffscript.cpp: do_mult, do_div, do_mod, do_floor, do_ceiling) and the
// x64/a64 backends' FLOOR/CEILING sequence.
//
// Standalone, not part of the build:
//   g++ -std=c++20 -O2 -I src src/zc/scripting/jit/arm32_ops_semantics_test.cpp -o /tmp/t && /tmp/t
// Add -DEXHAUSTIVE to check the single-operand functions for every int32 (a few seconds).

#define ZC_ARM32_OPS_MATH_ONLY
#include "zc/scripting/jit/jit_arm32_ops.cpp"

#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace arm32_ops_math;

static int failures = 0;

#define CHECK_EQ(what, got, want, a, b)                                                             \
	do                                                                                              \
	{                                                                                               \
		if ((got) != (want))                                                                        \
		{                                                                                           \
			if (failures++ < 20)                                                                    \
				printf("FAIL %s(%" PRId32 ", %" PRId32 "): got %" PRId32 ", want %" PRId32 "\n",   \
					what, (int32_t)(a), (int32_t)(b), (int32_t)(got), (int32_t)(want));             \
		}                                                                                           \
	} while (0)

// --- the interpreter's formulas ---

static int32_t ref_mult(int32_t a, int32_t b)
{
	int64_t temp = b;
	int32_t temp2 = a;
	return int32_t((temp * temp2) / 10000);
}

static int32_t ref_div(int32_t a, int32_t b)
{
	int64_t temp = b;
	int64_t temp2 = a;
	if (temp == 0)
		return int32_t((temp2 < 0 ? -1 : 1) * 2147483647LL);
	return int32_t((temp2 * 10000) / temp);
}

static int32_t ref_mod(int32_t a, int32_t b)
{
	int32_t temp = b;
	int32_t temp2 = a;
	if (temp == 0)
		temp = 1;
	return temp == -1 ? 0 : temp2 % temp;
}

// zfix::doFloor / doCeil.
static int32_t ref_floor(int32_t val)
{
	int32_t v = val / 10000;
	if (val % 10000 && val < 0) --v;
	return (int32_t)((uint32_t)v * 10000u);
}

static int32_t ref_ceil(int32_t val)
{
	int32_t v = val / 10000;
	if (val % 10000 && val > 0) ++v;
	return (int32_t)((uint32_t)v * 10000u);
}

// x64/a64: cvtsi2sd, mulsd by the double 1e-4 (bits 4547007122018943789), round, truncate, imul.
static double tenk_inv()
{
	uint64_t bits = 4547007122018943789ULL;
	double d;
	memcpy(&d, &bits, sizeof d);
	return d;
}

static int32_t jit_floor(int32_t v, bool ceil)
{
	volatile double y = (double)v * tenk_inv();
	double r = ceil ? std::ceil(y) : std::floor(y);
	return (int32_t)((uint32_t)(int32_t)r * 10000u);
}

// The emitted div10000 sequence: smull lo, hi, x, M; (hi asr 12) - (x asr 31).
static int32_t emitted_div10000(int32_t x)
{
	int64_t p = (int64_t)x * (int64_t)(int32_t)kDiv10000Magic;
	int32_t hi = (int32_t)(p >> 32);
	return (hi >> kDiv10000Shift) - (x >> 31);
}

// The emitted modulo dispatch: MODV with a constant divisor, MODR through the helper.
static int32_t helper_mod(int32_t a, int32_t b)
{
	return b == 0 ? 0 : mod(a, b);
}

static int32_t helper_div(int32_t a, int32_t b)
{
	return b == 0 ? fixdiv_by_zero_result(a) : fixdiv(a, b);
}

int main()
{
	std::vector<int32_t> edges = {
		0, 1, -1, 2, -2, 3, -3, 9999, -9999, 10000, -10000, 10001, -10001, 19999, -19999, 20000,
		-20000, 46340, -46340, 46341, -46341, 65535, 65536, -65536, 99999, 100000, -100000,
		214748, -214748, 214749, -214749, 2147483, -2147483, 1000000, -1000000, 10000000,
		-10000000, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, INT_MIN, INT_MIN + 1, INT_MAX - 1,
		0x40000000, -0x40000000, 0x12345678, -0x12345678, 1759218605, -1759218605, 429496729,
		-429496729,
	};

	long checks = 0;
	auto check_pair = [&](int32_t a, int32_t b) {
		CHECK_EQ("fixmul", fixmul(a, b), ref_mult(a, b), a, b);
		CHECK_EQ("div", helper_div(a, b), ref_div(a, b), a, b);
		CHECK_EQ("mod", helper_mod(a, b), ref_mod(a, b), a, b);
		checks += 3;
	};

	for (int32_t a : edges)
		for (int32_t b : edges)
			check_pair(a, b);

	std::mt19937 rng(12345);
	std::uniform_int_distribution<uint32_t> any;
	for (int i = 0; i < 20000000; i++)
	{
		int32_t a = (int32_t)any(rng);
		int32_t b = (int32_t)any(rng);
		// Mix in small magnitudes, where most real values are.
		if (i & 1) a >>= (any(rng) % 31);
		if (i & 2) b >>= (any(rng) % 31);
		check_pair(a, b);
	}

	auto check_single = [&](int32_t v) {
		CHECK_EQ("div10000", emitted_div10000(v), v / 10000, v, 0);
		CHECK_EQ("floor", fixfloor(v), ref_floor(v), v, 0);
		CHECK_EQ("ceil", fixceil(v), ref_ceil(v), v, 0);
		CHECK_EQ("floor/jit", fixfloor(v), jit_floor(v, false), v, 0);
		CHECK_EQ("ceil/jit", fixceil(v), jit_floor(v, true), v, 0);
		checks += 5;
	};

#ifdef EXHAUSTIVE
	for (int64_t v = INT_MIN; v <= INT_MAX; v++)
		check_single((int32_t)v);
#else
	for (int32_t v : edges)
		check_single(v);
	for (int i = 0; i < 20000000; i++)
		check_single((int32_t)any(rng));
#endif

	printf("%ld checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
