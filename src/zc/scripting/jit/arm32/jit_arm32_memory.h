#ifndef ZC_JIT_ARM32_MEMORY_H_
#define ZC_JIT_ARM32_MEMORY_H_

// Executable memory for the arm32 JIT backend. Platform specific: the 3DS implementation
// lives in src/platform/3ds/ (it needs kernel services to map memory executable and to keep
// the data and instruction caches coherent).

#include <cstddef>
#include <cstdint>

// Whether code can be made executable at all (e.g. the process may lack the permission).
// Checked once at startup; if false the backend compiles nothing and the interpreter runs
// every script.
bool jit_arm32_exec_available();

// Copies `size` bytes of finalized, position-independent machine code into executable
// memory, makes it visible to instruction fetch, and returns its address (nullptr on
// failure). The memory stays valid until jit_arm32_exec_free.
void* jit_arm32_exec_publish(const void* code, size_t size);

void jit_arm32_exec_free(void* code);

#endif
