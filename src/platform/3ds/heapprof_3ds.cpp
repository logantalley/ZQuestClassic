// TEMPORARY heap profiler for the 3DS build (not to be committed).
// Wraps malloc/calloc/realloc/free/memalign and operator new/new[]; every block gets a 16-byte
// header {orig, size, caller, magic}; live bytes are aggregated per caller.
#include <3ds.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);
}

namespace {

constexpr uint32_t kMagic = 0x48505246; // HPRF
constexpr uint32_t kBig = 256 * 1024;

struct Header
{
	void* orig;
	uint32_t size;
	uint32_t caller;
	uint32_t magic;
	uint32_t pad;
};
static_assert(sizeof(Header) == 20 || sizeof(Header) == 16 || true, "");
constexpr size_t kHdr = 16; // we only use the first 16 bytes (orig,size,caller,magic)

struct Entry
{
	uint32_t caller;
	uint32_t count;
	int64_t small_bytes;
	int64_t big_bytes;
	uint32_t big_count;
};
constexpr int kTable = 16384;
Entry table[kTable];
int64_t total_live;
uint32_t total_count;
LightLock lock;
bool lock_init;

void do_lock()
{
	if (!lock_init)
	{
		LightLock_Init(&lock);
		lock_init = true;
	}
	LightLock_Lock(&lock);
}

Entry* find(uint32_t caller)
{
	uint32_t h = (caller >> 2) * 2654435761u;
	for (int i = 0; i < kTable; i++)
	{
		Entry& e = table[(h + i) & (kTable - 1)];
		if (e.caller == caller)
			return &e;
		if (e.caller == 0)
		{
			e.caller = caller;
			return &e;
		}
	}
	return &table[0];
}

void account(uint32_t caller, uint32_t size, int sign)
{
	do_lock();
	Entry* e = find(caller ? caller : 1);
	if (size >= kBig)
	{
		e->big_bytes += sign * (int64_t)size;
		e->big_count += sign;
	}
	else
		e->small_bytes += sign * (int64_t)size;
	e->count += sign;
	total_live += sign * (int64_t)size;
	total_count += sign;
	LightLock_Unlock(&lock);
}

inline uint32_t* hdr_of(void* p)
{
	return (uint32_t*)((char*)p - kHdr);
}

void* finish(void* orig, void* user, size_t size, uint32_t caller)
{
	uint32_t* h = hdr_of(user);
	h[0] = (uint32_t)orig;
	h[1] = (uint32_t)size;
	h[2] = caller;
	h[3] = kMagic;
	account(caller, (uint32_t)size, 1);
	return user;
}

void* tracked_malloc(size_t size, uint32_t caller)
{
	void* orig = __real_malloc(size + kHdr);
	if (!orig)
		return nullptr;
	return finish(orig, (char*)orig + kHdr, size, caller);
}

bool is_tracked(void* p)
{
	return p && hdr_of(p)[3] == kMagic;
}

void tracked_free(void* p)
{
	if (!p)
		return;
	if (!is_tracked(p))
	{
		__real_free(p);
		return;
	}
	uint32_t* h = hdr_of(p);
	account(h[2], h[1], -1);
	h[3] = 0;
	__real_free((void*)h[0]);
}

} // namespace

extern "C" {

void* __wrap_malloc(size_t size)
{
	return tracked_malloc(size, (uint32_t)__builtin_return_address(0));
}

void* __wrap_calloc(size_t n, size_t size)
{
	size_t total = n * size;
	void* p = tracked_malloc(total, (uint32_t)__builtin_return_address(0));
	if (p)
		memset(p, 0, total);
	return p;
}

void __wrap_free(void* p)
{
	tracked_free(p);
}

void* __wrap_realloc(void* p, size_t size)
{
	uint32_t caller = (uint32_t)__builtin_return_address(0);
	if (!p)
		return tracked_malloc(size, caller);
	if (!is_tracked(p))
		return __real_realloc(p, size);
	uint32_t old = hdr_of(p)[1];
	void* n = tracked_malloc(size, caller);
	if (!n)
		return nullptr;
	memcpy(n, p, old < size ? old : size);
	tracked_free(p);
	return n;
}

void* __wrap_memalign(size_t align, size_t size)
{
	if (align < 8)
		align = 8;
	size_t off = align >= kHdr ? align : kHdr;
	void* orig = __real_memalign(align, size + off);
	if (!orig)
		return nullptr;
	return finish(orig, (char*)orig + off, size, (uint32_t)__builtin_return_address(0));
}

void heapprof_dump(const char* tag)
{
	char path[128];
	snprintf(path, sizeof(path), "sdmc:/3ds/zquestclassic/heap_%s.txt", tag);
	struct mallinfo mi = mallinfo();
	// Copy the table first: fopen/fprintf allocate.
	static Entry copy[kTable];
	do_lock();
	memcpy(copy, table, sizeof(table));
	int64_t live = total_live;
	uint32_t count = total_count;
	LightLock_Unlock(&lock);
	FILE* f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "tag %s arena %d inuse %d free %d tracked_live %lld tracked_count %lu\n", tag,
		mi.arena, mi.uordblks, mi.fordblks, (long long)live, (unsigned long)count);
	for (int i = 0; i < kTable; i++)
	{
		const Entry& e = copy[i];
		if (e.caller && (e.small_bytes || e.big_bytes))
			fprintf(f, "%08lx %lld %lld %lu %lu\n", (unsigned long)e.caller, (long long)e.small_bytes,
				(long long)e.big_bytes, (unsigned long)e.count, (unsigned long)e.big_count);
	}
	fclose(f);
}

}

// operator new: attribute to the caller of new, not to libstdc++.
void* operator new(size_t size)
{
	void* p = tracked_malloc(size ? size : 1, (uint32_t)__builtin_return_address(0));
	if (!p)
		throw std::bad_alloc();
	return p;
}
void* operator new[](size_t size)
{
	void* p = tracked_malloc(size ? size : 1, (uint32_t)__builtin_return_address(0));
	if (!p)
		throw std::bad_alloc();
	return p;
}
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
	return tracked_malloc(size ? size : 1, (uint32_t)__builtin_return_address(0));
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
	return tracked_malloc(size ? size : 1, (uint32_t)__builtin_return_address(0));
}
void operator delete(void* p) noexcept { tracked_free(p); }
void operator delete[](void* p) noexcept { tracked_free(p); }
void operator delete(void* p, size_t) noexcept { tracked_free(p); }
void operator delete[](void* p, size_t) noexcept { tracked_free(p); }
