// Frees the arm32 backend's published code (see jit_backend_release_script in jit_shared.h).

#include "zc/scripting/jit/jit_shared.h"
#include "zc/scripting/jit/arm32/jit_arm32_memory.h"

// A compiled function's exec is the start of its published block (jit_arm32.cpp). Functions
// that were never compiled point at the driver's stub instead, which jit_arm32_exec_free
// ignores (it only frees blocks it handed out).
void jit_backend_release_script(JittedScript* j_script)
{
	for (auto& j_fn : j_script->compiled_functions)
		jit_arm32_exec_free((void*)j_fn.exec);
	for (auto& j_fn : j_script->pending_compiled_jit_functions)
		jit_arm32_exec_free((void*)j_fn.exec);
	j_script->compiled_functions.clear();
	j_script->pending_compiled_jit_functions.clear();
}
