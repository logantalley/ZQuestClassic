# 3DS port of zplayer — proposal

Anchor document for the 3DS port. Agents working on the port treat this as the
contract: file ownership, seams and naming below are fixed unless this file is
updated first.

## Goal

Build `zplayer` (the game runtime only — no editor, no ZScript compiler, no
launcher) for the Nintendo 3DS, packaged as `.3dsx` (Homebrew Launcher) and
`.cia` (installable title), produced by a GitHub Actions workflow.

Target: **New 3DS / New 2DS XL** (804 MHz, 124 MB app memory via
`SystemModeExt`). Old 3DS may boot but is not a goal.

## Principles

- **Terse and modular.** Port code lives in `src/platform/3ds/`. Shared code
  gets *capability* checks, never scattered `__3DS__` checks.
- **Reuse the web port's seams.** Every place the code already special-cases
  `__EMSCRIPTEN__` because a desktop feature is missing (processes, file
  dialogs, shaders, curl, threads) is a candidate seam; prefer extending those
  over inventing new ones.
- **No forks of big deps.** Allegro and SDL are patched with small `git apply`
  patches; real code goes in-tree.
- **Desktop builds are unaffected.** Everything is behind `ZC_3DS`.

## Architecture

```
zplayer (unchanged game code)
  └─ zalleg / allegro-legacy (A4 API)            unchanged
       └─ Allegro 5, ALLEGRO_SDL=ON, no OpenGL   small patch + in-tree soft display
            └─ SDL 2.32.10 (has upstream N3DS backend: video fb, audio/ndsp,
               joystick/hid, touch, threads, timers)
                 └─ libctru / devkitARM
```

### Key decisions

| Area | Decision |
|---|---|
| Toolchain | devkitARM via `devkitpro/devkitarm` container, `$DEVKITPRO/cmake/3DS.cmake` toolchain file. CMake option `ZC_3DS` auto-set when `NINTENDO_3DS` is defined. |
| Linking | Fully static (`BUILD_SHARED_LIBS OFF`, `zcsound` static, Allegro `SHARED OFF`) — same as the Emscripten path. |
| SDL | Reuse the existing `FetchContent` SDL 2.32.10, but on 3DS enable video, audio, events, joystick, timers, threads. |
| Allegro | `ALLEGRO_SDL=ON`, OpenGL off. Display is a **software display**: backbuffer is a memory bitmap; `flip` converts it into the SDL window surface (`SDL_GetWindowSurface`/`SDL_UpdateWindowSurface`). `al_create_shader` returns NULL (ZC already tolerates this — CRT filter just disables). |
| Screens | Top screen 400×240 shows the game 1:1, centered (ZC renders 256×224 + letterbox). Bottom screen: unused in v1 (later: touch subscreen/menu). |
| JIT | `JIT_BACKEND=none` (ARM32; interpreter only). |
| Parallelism | No TBB → existing poolSTL fallback. `worker_pool` sized to 1 (or run inline). |
| Off | curl, websockets (`WANT_WEBSOCKETS=OFF`), stduuid, NFD, sentry, crashpad, imgui debugger window, launcher/updater, process spawning. |
| Audio | Allegro SDL audio driver → SDL N3DS (ndsp). OGG/MP3/DUMB/GME via existing paths. MIDI: best-effort; silent if unsupported in v1. |
| Input | SDL N3DS joystick → Allegro SDL joystick driver. Default button map in the 3DS default config (A/B/X/Y/L/R/Start/Select, D-pad + Circle Pad). |
| Files | RomFS (`romfs:/`) holds read-only `resources/` subset (`zc.cfg` defaults, `modules/`, `sfx.dat`, fonts, `base_config`). Writable root `sdmc:/3ds/zquestclassic/` holds quests, saves, config, logs. CWD is set there on boot. |
| Memory | CIA requests `SystemModeExt: 124MB`; `osSetSpeedupEnable(true)` at boot. |

## Layout and ownership

```
src/platform/3ds/
  CMakeLists.txt         defines target `zc_3ds` (STATIC); linked into zplayer when ZC_3DS
  platform_3ds.h/.cpp    boot (romfs, speedup, sdmc dirs, chdir), shutdown, paths
  allegro/
    allegro_3ds.cmake    included after FetchContent_MakeAvailable(allegro5) when ZC_3DS:
                         applies patch, injects soft display source into `allegro`
    sdl_soft_display.c   Allegro display vtable over an SDL window surface
third_party/
  allegro5_3ds.patch     minimal: build w/o GL under SDL, select soft display on __3DS__
  sdl2_3ds.patch         only if SDL needs one
packaging/3ds/
  zplayer.rsf            makerom spec (CIA)
  icon.png, banner.png, banner.wav (or generated)
  romfs.cmake            stages romfs dir from resources/
resources/3ds/
  zc.cfg                 3DS-default config (controls, video, sound)
.github/workflows/3ds.yml  builds .3dsx and .cia, uploads both as artifacts
```

Shared code changes:

- Root `CMakeLists.txt`: one `ZC_3DS` block near the `EMSCRIPTEN` dep block
  for dep toggles, plus `if(ZC_3DS)` skips for editor/launcher/zscript/tests.
  Keep each edit a few lines; push logic into `cmake/zc_3ds.cmake` if it grows.
- Capability macros in `src/base/platform.h` (new, tiny), e.g.
  `ZC_HAS_PROCESSES`, `ZC_HAS_SHADERS`, `ZC_HAS_NATIVE_DIALOGS`,
  `ZC_HAS_THREADS`. Existing `__EMSCRIPTEN__` checks that mean "capability
  missing" may be migrated to these where touched; don't mass-refactor.

## Packaging

- `.3dsx`: devkitPro CMake helpers `ctr_generate_smdh` + `ctr_create_3dsx`
  with `ROMFS`.
- `.cia`: `bannertool` (banner + icon) then `makerom -f cia -rsf
  packaging/3ds/zplayer.rsf -target t -elf zplayer.elf -icon ... -banner ...`
  with RomFS. Both tools downloaded from pinned GitHub releases (version +
  sha256) in CI.
- Unique ID / product code: `0xF2C5A` / `CTR-H-ZQCP` (homebrew range).

## CI

`.github/workflows/3ds.yml`:

- Triggers: `push` to `3ds-port` and `3ds/**`, `pull_request` touching
  port paths, `workflow_dispatch`.
- Job runs in a pinned `devkitpro/devkitarm` image; installs host
  `flex bison python3 ninja`; configures Release with the 3DS toolchain;
  builds `zplayer`; packages; uploads `zplayer.3dsx` and `zplayer.cia`.
- `ccache` cache keyed on the commit, restoring by branch.
- No LFS (quests are supplied on the SD card).

## Milestones

1. **M1 build** — configure + compile all of zplayer's deps and sources for ARM; link.
2. **M2 package** — `.3dsx` and `.cia` produced and uploaded by CI.
3. **M3 boot** — title screen renders on hardware/Citra/Azahar; input works.
4. **M4 play** — load a quest from SD, play, save; audio works.
5. Later — bottom-screen touch UI, MIDI, Old 3DS performance.

M1–M2 are the scope of the initial agent run; M3–M4 need hardware or an
emulator and a human in the loop.

## Workflow (agents)

Phase 1, parallel, each in its own worktree/branch:

- **build** — root CMake, `cmake/zc_3ds.cmake`, `packaging/3ds/`,
  `resources/3ds/`, `.github/workflows/3ds.yml`. Iterates on CI via the
  `fork` remote.
- **allegro** — `src/platform/3ds/allegro/`, `third_party/allegro5_3ds.patch`
  (and `sdl2_3ds.patch` if needed).
- **zc** — `src/platform/3ds/` (minus `allegro/`), `src/base/platform.h`,
  capability guards in shared code.

Phase 2: **integrate** — merge the three branches into `3ds-port`, iterate CI
until both artifacts are produced, keep the diff terse.

Rules for all agents: never push to `origin`; push only to `fork`. Don't touch
files owned by another agent in phase 1 — note needed changes in the final
report instead. Commits follow AGENTS.md; port commits use a `!` (hidden from
the changelog), e.g. `feat!: 3ds soft display for allegro`.
