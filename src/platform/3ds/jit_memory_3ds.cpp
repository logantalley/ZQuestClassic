// Executable memory for the arm32 JIT (see src/zc/scripting/jit/arm32/jit_arm32_memory.h).
//
// Code lives in page-aligned chunks of the heap that are remapped read/write/execute with
// svcControlProcessMemory (MEMOP_PROT). Publishing copies the code in, writes the data cache
// back (svcFlushProcessDataCache) and invalidates the instruction cache over the range with
// Luma3DS's svcInvalidateInstructionCacheRange (custom SVC 0x93): the stock kernel has no
// user-callable instruction cache maintenance. So the JIT needs Luma3DS (which every
// homebrew setup has, and which also lifts the SVC access checks) or an emulator that
// implements the same SVCs (Azahar does; it does not enforce page permissions, and has no
// svcControlProcessMemory, so that step is skipped there).
//
// Freed code goes back to a free list and is reused; the instruction cache is invalidated on
// every publish, so stale instructions from earlier code at the same address are never run.

#include "platform/3ds/platform_3ds.h"
#include "zc/scripting/jit/arm32/jit_arm32_memory.h"

#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <malloc.h>
#include <string>
#include <unordered_map>

namespace {

constexpr u32 PAGE = 0x1000;
constexpr u32 CHUNK_SIZE = 1024 * 1024;
constexpr u32 ALIGN = 16;

// svcGetSystemInfo types that only exist on Luma3DS and on emulators.
constexpr u32 SYSINFO_LUMA = 0x10000;
constexpr u32 SYSINFO_EMULATOR = 0x20000;

Result svc_invalidate_icache_range(u32 addr, u32 size)
{
	register u32 r0 asm("r0") = addr;
	register u32 r1 asm("r1") = size;
	asm volatile("svc 0x93" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "memory");
	return r0;
}

struct State
{
	bool available = false;
	bool emulator = false;
	Handle process = 0;
	LightLock lock;
	// address -> size, for free and for allocated blocks.
	std::map<u32, u32> free_blocks;
	std::unordered_map<u32, u32> used_blocks;
	std::string why;
};

void check(State& s);

// Checked on first use. (A function-local static is initialized exactly once, even when the
// JIT compiles on worker threads.)
State& state()
{
	static State s = []{
		State s;
		check(s);
		return s;
	}();
	return s;
}

bool make_executable(State& s, u32 addr, u32 size)
{
	if (s.emulator)
		return true;
	Result res = svcControlProcessMemory(s.process, addr, 0, size, MEMOP_PROT,
		MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
	if (R_FAILED(res))
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "svcControlProcessMemory failed: 0x%08lX", (unsigned long)res);
		s.why = buf;
		return false;
	}
	return true;
}

// Adds a new executable chunk of at least `size` bytes to the free list.
bool grow(State& s, u32 size)
{
	u32 chunk = size > CHUNK_SIZE ? (size + PAGE - 1) & ~(PAGE - 1) : CHUNK_SIZE;
	void* mem = memalign(PAGE, chunk);
	if (!mem)
	{
		s.why = "out of memory";
		return false;
	}
	u32 addr = (u32)mem;
	if (!make_executable(s, addr, chunk))
	{
		free(mem);
		return false;
	}
	s.free_blocks[addr] = chunk;
	return true;
}

void check(State& s)
{
	LightLock_Init(&s.lock);

	s64 info;
	s.emulator = R_SUCCEEDED(svcGetSystemInfo(&info, SYSINFO_EMULATOR, 0));
	if (!s.emulator && R_FAILED(svcGetSystemInfo(&info, SYSINFO_LUMA, 0)))
	{
		s.why = "needs Luma3DS (for instruction cache maintenance)";
		return;
	}
	// The kernel takes a real handle here, not the current-process pseudo handle.
	if (R_FAILED(svcDuplicateHandle(&s.process, CUR_PROCESS_HANDLE)))
	{
		s.why = "svcDuplicateHandle failed";
		return;
	}
	s.available = grow(s, CHUNK_SIZE);
}

}

bool jit_arm32_exec_available()
{
	return state().available;
}

void* jit_arm32_exec_publish(const void* code, size_t size)
{
	State& s = state();
	if (!s.available || size == 0)
		return nullptr;

	u32 need = (size + ALIGN - 1) & ~(ALIGN - 1);
	LightLock_Lock(&s.lock);
	auto it = s.free_blocks.begin();
	while (it != s.free_blocks.end() && it->second < need)
		++it;
	if (it == s.free_blocks.end())
	{
		if (!grow(s, need))
		{
			LightLock_Unlock(&s.lock);
			return nullptr;
		}
		it = s.free_blocks.begin();
		while (it->second < need)
			++it;
	}
	u32 addr = it->first, block = it->second;
	s.free_blocks.erase(it);
	if (block > need)
		s.free_blocks[addr + need] = block - need;
	s.used_blocks[addr] = need;
	LightLock_Unlock(&s.lock);

	memcpy((void*)addr, code, size);
	svcFlushProcessDataCache(s.process, addr, need);
	svc_invalidate_icache_range(addr, need);
	return (void*)addr;
}

void jit_arm32_exec_free(void* code)
{
	if (!code)
		return;
	State& s = state();
	LightLock_Lock(&s.lock);
	auto used = s.used_blocks.find((u32)code);
	if (used != s.used_blocks.end())
	{
		u32 addr = used->first, size = used->second;
		s.used_blocks.erase(used);
		// Merge with the neighbors.
		auto next = s.free_blocks.lower_bound(addr);
		if (next != s.free_blocks.end() && next->first == addr + size)
		{
			size += next->second;
			next = s.free_blocks.erase(next);
		}
		if (next != s.free_blocks.begin())
		{
			auto prev = std::prev(next);
			if (prev->first + prev->second == addr)
			{
				prev->second += size;
				LightLock_Unlock(&s.lock);
				return;
			}
		}
		s.free_blocks[addr] = size;
	}
	LightLock_Unlock(&s.lock);
}

bool zc_3ds_jit_selftest(std::string& log)
{
	char buf[256];
	bool available = jit_arm32_exec_available();
	State& s = state();
	snprintf(buf, sizeof(buf), "[jit-arm32-selftest] exec available: %d (%s)%s\n", available,
		s.emulator ? "emulator" : "console", s.why.empty() ? "" : (" - " + s.why).c_str());
	log += buf;
	if (!available)
		return false;

	// mov r0, #42; bx lr - and a second function at the same address afterwards, to check
	// that freed code is not run from a stale instruction cache.
	static const u32 code42[] = {0xE3A0002A, 0xE12FFF1E};
	static const u32 code7[] = {0xE3A00007, 0xE12FFF1E};
	bool ok = true;
	void* first = nullptr;
	for (int i = 0; i < 2; i++)
	{
		const u32* code = i == 0 ? code42 : code7;
		int expected = i == 0 ? 42 : 7;
		void* fn = jit_arm32_exec_publish(code, sizeof(code42));
		int result = fn ? ((int (*)())fn)() : -1;
		snprintf(buf, sizeof(buf), "[jit-arm32-selftest] published at %p, returned %d (expected %d)%s\n",
			fn, result, expected, i == 1 && fn != first ? " (not reused)" : "");
		log += buf;
		ok &= result == expected;
		if (i == 0)
			first = fn;
		jit_arm32_exec_free(fn);
	}
	log += ok ? "[jit-arm32-selftest] PASS\n" : "[jit-arm32-selftest] FAIL\n";
	return ok;
}
