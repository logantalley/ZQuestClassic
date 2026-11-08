# arm32 JIT backend (3DS) - design and work plan

A JIT backend for 32-bit ARM (ARMv6K, ARM state: the New 3DS's ARM11), so scripted quests run
at playable speed on the 3DS. Read `docs/jit.md` first: this backend plugs into the same
architecture-neutral driver (`jit_shared.{h,cpp}`) and codegen policy (`jit_codegen_shared.h`)
as x64 and a64, and only replaces the code generator.

Why it is needed: on the 3DS, script-heavy quests are interpreter-bound (Rite of the Storm:
~60% of samples in `run_script_int`, 16-18 fps), and asmjit cannot emit 32-bit ARM.

## Shape

| Piece | Files | Owner |
|---|---|---|
| Assembler | `src/zc/scripting/jit/arm32/arm32_asm.{h,cpp}`, host test `src/zc/scripting/jit/arm32/arm32_asm_test.cpp` | agent A |
| Executable memory (3DS), build wiring, emulator test harness | `src/zc/scripting/jit/arm32/jit_arm32_memory.h` (contract), `src/platform/3ds/jit_memory_3ds.cpp`, `CMakeLists.txt` (JIT_BACKEND), `cmake/zc_3ds.cmake`, `src/zc/scripting/jit/jit.h`, `src/zc/scripting/jit/jit_arm32.h`, `packaging/3ds/zplayer.rsf`, `src/platform/3ds/platform_3ds.cpp` (args file), `scripts/3ds_emu.py` | agent B |
| Backend core | `src/zc/scripting/jit/jit_arm32.cpp` | agent C |
| Data commands | `src/zc/scripting/jit/jit_arm32_ops.cpp` | agent D |
| Contracts | `arm32_asm.h`, `jit_arm32_memory.h`, `jit_arm32_internal.h`, this file | orchestrator |

Each agent edits only its own files. Changing a contract header needs the orchestrator: note
the change you need in your final report instead (or, if blocked, make the smallest additive
change and call it out).

Shared code (`jit_shared.*`, `jit_codegen_shared.h`, `ffscript.cpp`, ...) is off limits except
where this file says otherwise; it is used unchanged by x64/a64 and must keep working there.

## Decisions

- **No register allocator.** Fixed register roles (`jit_arm32_internal.h`): R4 ctx, R5
  `registers` (ri->d), R6 stack base, R7 ZASM sp, R8 SWITCHKEY. R9-R11 reserved for a later
  D-register cache. Scratch: R0-R3, R12, LR.
- **Phase 1 runs with the D-register cache off** (`use_cached_regs = false`): D registers are
  loaded/stored at every use. The shared flush policy is still called (with an always-empty
  cache it emits nothing). A cache over R9-R11 (or a fixed D->register mapping) is phase 2.
- **Calls into C** use `Assembler::call` (absolute address in R12, `blx`). Everything else is
  pc-relative, so finalized code can be copied anywhere.
- **No hardware divide** on ARM11: DIV/MOD and the fixed-point multiply's /10000 call small C
  helpers (or a reciprocal multiply where exact).
- **Resume dispatch** as on x64/a64: `ctx->resume_address` holds an absolute address
  (published base + label offset); entry branches to it with `bx`.
- **Direct native calls between compiled functions** (`-jit-direct-calls`): off in phase 1
  (`direct_entry = false`, CALLFUNC/RETURNFUNC go through the driver).
- **Pieces** (`jit_split_function`) must work: exits to other pieces go through exit stubs
  that set ctx->pc and return EXEC_RESULT_CONTINUE, as on a64.
- The commands the backend must compile (control flow, compare producers/consumers, waits,
  stack pointer changes) are listed in `jit_command_is_compiled_shared`'s first part; data
  commands are optional and can be left to the interpreter one by one (useful for bisecting
  miscompiles).

## Semantics reference

`jit_a64.cpp` is the reference implementation for every command: same ctx protocol, same
exec results, same helper functions (`run_script_jit_one`/`run_script_jit_sequence`,
`get_register_jit`, ...). When in doubt, do what a64 does.

## Building

3DS build (devkitARM is at `/opt/devkitpro`, see `cmake/zc_3ds.cmake`):

```bash
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export CCACHE_BASEDIR=$PWD   # share the ccache between worktrees
cmake -B build_3ds -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build_3ds -t zplayer_3dsx
```

Until agent B's wiring lands, a TU can be compile-checked with the flags of an existing one:
`ninja -C build_3ds -t commands CMakeFiles/zplayer.dir/src/zc/ffscript.o`.

The machine has 4 cores shared by every agent: build with `-j2`.

## Testing

- **Assembler:** host unit test comparing emitted words with `arm-none-eabi-as`/`objdump`
  from devkitARM for every instruction form (`-march=armv6k -marm`).
- **Backend:** in the Azahar emulator (`/home/user/azahar/squashfs-root/AppRun`; see agent B's
  `scripts/3ds_emu.py` once it exists). The 3DS build reads extra command-line arguments from
  `sdmc:/3ds/zquestclassic/args.txt` (agent B), so a replay can be run with and without the JIT
  (`-replay <path> -jit` / `-no-jit`, plus `-jit-precompile` and `-jit-fatal-compile-errors`)
  and the results compared.
- Emulator isolation - several agents may run Azahar at once. Each must use:
  - its own `HOME` (copy `/root/.config/azahar-emu` and `/root/.local/share/azahar-emu` into
    e.g. `/home/user/emu_<agent>/`), which gives it its own config and virtual SD card;
  - its own X display (`Xvfb :<n>`; A none, B :101, C :102, D :103, review :104);
  - its own GDB stub port (`gdbstub_port` in its `qt-config.ini`: B 24701, C 24702, D 24703,
    review 24704).
  The emulator pauses at boot until a GDB client connects when the stub is on; connect and
  detach (or set `use_gdbstub=false`) to let it run.
- Quests: `tests/replays/**` (Git LFS - pulled), `tests/replays/classic_1st/classic_1st.qst`.

## Commits

Commit in your worktree branch with AGENTS.md-style messages (a `!` - the port is hidden from
the changelog, e.g. `feat(zc)!: arm32 jit assembler`), ending with the attribution lines in
the task. Do not push; the orchestrator merges into `3ds/build-diag`.
