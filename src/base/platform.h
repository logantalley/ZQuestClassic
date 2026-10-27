#ifndef BASE_PLATFORM_H_
#define BASE_PLATFORM_H_

// Capabilities some platforms lack. Shared code checks these rather than platform macros.

// Can launch child processes.
#if defined(__EMSCRIPTEN__) || defined(__3DS__)
#define ZC_HAS_PROCESSES 0
#else
#define ZC_HAS_PROCESSES 1
#endif

// Has Allegro's native dialog addon (message boxes, text log windows).
#if defined(__EMSCRIPTEN__) || defined(__3DS__)
#define ZC_HAS_NATIVE_DIALOGS 0
#else
#define ZC_HAS_NATIVE_DIALOGS 1
#endif

// The executable's folder is a real path that the working directory should follow.
#if defined(__EMSCRIPTEN__) || defined(__3DS__)
#define ZC_HAS_EXE_DIR 0
#else
#define ZC_HAS_EXE_DIR 1
#endif

// Can map read-only pages of memory.
#if defined(__EMSCRIPTEN__) || defined(__3DS__)
#define ZC_HAS_MEMORY_PROTECTION 0
#else
#define ZC_HAS_MEMORY_PROTECTION 1
#endif

// Can open more than one window (ex: the ZScript debugger).
#if defined(__EMSCRIPTEN__) || defined(__3DS__)
#define ZC_HAS_MULTIPLE_WINDOWS 0
#else
#define ZC_HAS_MULTIPLE_WINDOWS 1
#endif

// Spreading work over threads pays off (more than one fast core).
#if defined(__3DS__)
#define ZC_HAS_THREADS 0
#else
#define ZC_HAS_THREADS 1
#endif

#endif
