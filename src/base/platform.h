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

// Has a keyboard and mouse to drive the desktop-style dialogs (ex: the quest file browser).
#if defined(__3DS__)
#define ZC_HAS_DESKTOP_INPUT 0
#else
#define ZC_HAS_DESKTOP_INPUT 1
#endif

// Too little memory for tables sized to the engine's maximums (ex: ~20 MB of combos): size
// them to what a quest uses instead.
#if defined(__3DS__)
#define ZC_LOW_MEMORY 1
#else
#define ZC_LOW_MEMORY 0
#endif

// Spreading work over threads pays off (more than one fast core).
#if defined(__3DS__)
#define ZC_HAS_THREADS 0
#else
#define ZC_HAS_THREADS 1
#endif

#endif
