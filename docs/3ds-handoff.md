# 3DS port: handoff for new sessions

Start here. This summarizes a long session (Nov 2026) that ported `zplayer` to the New
3DS and built a 32-bit ARM JIT for it, and says how to continue with an orchestrator and a
team of agents. Read with `AGENTS.md`, `docs/3ds-port.md`, `docs/jit.md`, `docs/jit-arm32.md`.

## State (branch `3ds/build-diag`, 68619ae or later)

Works (Azahar emulator, and on a real New 3DS where noted by the user):
- Builds in CI (`.github/workflows/3ds.yml`) as `.3dsx` and `.cia`; quest picker driven
  by buttons; music/sfx correct; 60 fps on simple quests; HOME-menu close exits cleanly.
- Large quests load (Rite of the Storm, Origin) after memory work: paged combo table
  (`ZC_LOW_MEMORY`, `paged_combos` in `src/core/combo.h`), sprite lists that grow, small
  title assets (`resources/3ds/`), Ogg title theme.
- **arm32 JIT** (`src/zc/scripting/jit/jit_arm32*.cpp`, `arm32/arm32_asm.*`), on by
  default on the 3DS (`cmake/zc_3ds.cmake`, `resources/3ds/zc.cfg jit = 1`). Every replay in
  `tests/replays/{playground,z3,misc}` and the full 195k-frame classic_1st passes in Azahar
  with and without it; ~0.9M ZASM of script-heavy quests compiled with no miscompile. Rite of
  the Storm: 16-18 fps interpreted -> 36 fps JIT (first room).
- **Last fix (68619ae), NOT yet confirmed on hardware:** the app crashed at launch on the
  user's 3DS (libctru heap allocation failed: it sizes the heap from the resource limit, but
  the APPLICATION region has less free). `platform_3ds.cpp` now overrides
  `__system_allocateHeaps`, capping at `osGetMemRegionFree`. Real heap is ~74 MB (not the
  98 MB Azahar gave before) - memory tests done earlier in Azahar overstated headroom.
  **First thing: ask the user to confirm a CI build of 68619ae+ launches.**

## Unmerged work (pushed as branches, based on older 3ds/build-diag)

| branch | content | status |
|---|---|---|
| `3ds/wip-jit-release-and-hot-compile` | 87f4d83 frees a script's JIT code on release (committed, tested per agent); a WIP commit: compile hot functions synchronously when `jit_threads = 0` instead of precompiling everything | first commit likely good (re-verify); WIP unfinished |
| `3ds/wip-jit-direct-calls` | WIP: direct native calls between compiled functions (phase 2 step 1) | unfinished, untested |
| `3ds/wip-heap-profile` | temporary malloc-tracking instrumentation | reference only, never merge |

## Open work, in priority order

1. Hardware confirmation of the launch fix (user). If it still crashes, read the Luma dump
   (`/luma/dumps/arm11/*.dmp`; parse: magic, regs r0-r12 sp lr pc cpsr at offset 40; Luma
   reports pc/lr 8 bytes early for svcBreak) and map pc with `arm-none-eabi-addr2line` on a
   local build of the same commit (layout matches CI).
2. **Heap budget (~74 MB on hardware).** Profile after quest load (link_to_the_heavens used
   95 MB in Azahar with no scripts; its overworld map needs a 5.6 MB bitmap and failed).
   Suspects: per-tile 256-byte unpacked tile buffers (4-bit tiles), map screens, sfx samples.
   Implement the biggest safe reductions under `ZC_LOW_MEMORY`, keeping desktop identical.
3. **JIT compile-when-hot on the 3DS** (finish the WIP): precompiling everything OOMs big
   quests (hollow_forest, crucible_quest, darkness_within). Also merge the code-release fix.
4. **JIT phase 2 speed:** direct native calls (finish WIP), then the D-register cache
   (R9-R11 reserved), honoring `jit_codegen_shared.h`'s flush policy. Profile showed driver
   overhead per CALLFUNC (std::map/upper_bound in `jit_shared.cpp`).
5. Later: bottom-screen touch UI; ZC desktop dialogs (System menu) still half-size and not
   button-driven; Android/Vita ports were rated feasible (Android best) - see session notes
   in git log.

## Environment setup (each fresh container)

Nothing here survives a new container except the repo. Rebuild the toolchain:

```bash
# devkitARM: pull the devkitpro/devkitarm:20260610 image layers from Docker Hub (no daemon
# needed) and extract /opt/devkitpro
D=/home/user/dkp_img; mkdir -p $D/root && cd $D
TOKEN=$(curl -sS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:devkitpro/devkitarm:pull" | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
M=$(curl -sS -H "Authorization: Bearer $TOKEN" -H 'Accept: application/vnd.oci.image.manifest.v1+json' \
  https://registry-1.docker.io/v2/devkitpro/devkitarm/manifests/sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b)
echo "$M" | python3 -c 'import sys,json;[print(l["digest"]) for l in json.load(sys.stdin)["layers"]]' | tail -2 |
  while read dg; do curl -sSL -H "Authorization: Bearer $TOKEN" https://registry-1.docker.io/v2/devkitpro/devkitarm/blobs/$dg | tar xz -C root opt; done
ln -sfn $D/root/opt/devkitpro /opt/devkitpro
apt-get install -y --no-install-recommends flex ccache xdotool imagemagick pulseaudio pulseaudio-utils libopengl0 libegl1 libgl1-mesa-dri libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-xinerama0 libxcb-shape0 libxcb-xkb1
# bannertool + makerom (for the .cia): see .github/workflows/3ds.yml (pinned URLs + sha256)
```

- **Azahar emulator:** the user must upload the Linux AppImage (~103 MB) split under 30 MB
  (`split -b 25m`); `cat` the parts, `chmod +x`, `--appimage-extract` into
  `/home/user/azahar`. Run under Xvfb with `LIBGL_ALWAYS_SOFTWARE=1`. Put a 64 KB zero file at
  `sdmc:/3ds/dspfirm.cdc` or there is no audio. GitHub release downloads of Azahar are blocked
  by the session's network policy.
- **Build:** `cmake -B build_3ds -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
  -DCMAKE_BUILD_TYPE=Release && cmake --build build_3ds -t zplayer_3dsx zplayer_cia`
  (`export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`).
- **Desktop build** (to check shared-code changes): needs `libgl1-mesa-dev libglu1-mesa-dev
  libx11-dev libxcursor-dev libxi-dev libxinerama-dev libxrandr-dev libxpm-dev libxext-dev
  libgtk-3-dev libasound2-dev libpulse-dev`; configure with `-DWANT_SENTRY=OFF
  -DWANT_ZUPDATER=OFF -DWANT_ERROR_CHECKING=OFF` (GCC 13 trips -Werror in untouched files).
  Replay tests: `pip install cutie intervaltree colorlog readchar PyGithub joblib boto3`, then
  `git lfs pull` the replays/quests you need (many are LFS pointers), then
  `python3 tests/run_replay_tests.py --filter playground --filter z3 --filter misc --build_folder build_desktop`
  (147 pass).
- **Running the 3DS build:** `scripts/3ds_emu.py` (isolated HOME/display/gdb port, writes
  `sdmc:/3ds/zquestclassic/args.txt`, waits for exit, prints results). Example:
  `python3 scripts/3ds_emu.py --3dsx build_3ds/zplayer.3dsx --home /home/user/emu_x --display 101 --gdb-port 24701 --timeout 900 --file tests/replays/classic_1st/classic_1st_lvl1.zplay=replays/classic_1st_lvl1.zplay --file tests/replays/classic_1st/classic_1st.qst=replays/classic_1st.qst --args='-assert replays/classic_1st_lvl1.zplay -replay-exit-when-done -v0 -headless -jit -jit-precompile -jit-fatal-compile-errors'`.
  `-frame N` stops a replay early; `-replay-batch` runs many per boot. Some replays name
  quests under `resources/quests/` (prefer those over same-named files in the replay folder).
- **Debugging in Azahar:** GDB stub (`use_gdbstub`, port in `qt-config.ini`; the app waits
  for a client at boot); `arm-none-eabi-gdb` with the matching `zplayer.elf`. Sampling
  profiler = attach, `thread 1`, `bt`, detach, repeat. `lastfps` global = frames/s (emulated
  time, so host load doesn't skew it). Audio check: PulseAudio null sink + `parec`.

## Gotchas learned the hard way

- Emulators don't enforce the APPLICATION region's free memory, SVC permissions or page
  protection. Anything memory/permission related must be confirmed on hardware.
- libctru defaults: 32 KB main stack (we use 4 MB), linear heap up to 32 MB (we use 8 MB).
- The `.3dsx` RomFS packer skips dotfiles (hence `romfs_id.txt`).
- `components/zasm/defines.h` #defines `SP` and `PC`; the arm32 backend uses `kHostSp`/`kHostPc`.
- Shift counts: masked to 5 bits explicitly in the interpreter (32-bit ARM differs).
- The commit-msg hook isn't executable here; follow AGENTS.md commit rules by hand.

## How to run the team (fresh sessions)

**Orchestrator session** (one): owns `3ds/build-diag`, merges, pushes, keeps this file
current. It writes the contract (files owned per agent, interfaces) before spawning, gives
each agent a self-contained prompt (agents start cold), and spawns them with
`isolation: worktree` in the background. Merge with cherry-pick or ff (the default merge
message is awkward with the hooks). Verify worktrees are based on `3ds/build-diag` (some
started from `main`). Keep its own context small: delegate builds/emulator runs to agents.

**Agents** (2-4, one task area each, own files only). Every agent prompt should include:
- Read `AGENTS.md`, this file, and the task's docs. Set up the environment per this file if
  missing (or reuse `/opt/devkitpro`, `/home/user/azahar` if the container still has them).
- Base: `git merge-base --is-ancestor <orchestrator's commit> HEAD`, else reset to
  `3ds/build-diag`.
- Emulator isolation: own HOME copy (`/home/user/emu_<agent>`), own display (:101-:104), own
  gdb port (24701-24704). **Never `pkill Xvfb`/`pkill -f AppRun` globally** - kill your own
  PIDs only (a global pkill destroyed other agents' runs).
- 4 cores are shared: build with `-j2`, `export CCACHE_BASEDIR=$PWD`.
- Temporary instrumentation never gets committed.
- Commit in the worktree (AGENTS.md style; port work uses `!`), don't push; final report:
  branch, commits, what was verified and how, what wasn't.

**Suggested first wave** (after item 1 is confirmed):
- Agent "heap": item 2 (start from `3ds/wip-heap-profile` for the instrumentation idea).
- Agent "jit-driver": item 3 (rebase `3ds/wip-jit-release-and-hot-compile`, finish, verify
  hollow_forest/crucible/darkness_within with `-jit` and desktop replays).
- Agent "jit-speed": item 4 (rebase `3ds/wip-jit-direct-calls`), measuring Rite of the Storm
  fps (`lastfps`) before/after; run the playground suite in the 4 JIT configs
  (precompile, `-jit-max-piece-size 50`, compile-when-hot) after each step.
- A review agent (read-only) over each batch before merging to `3ds/build-diag`.

Usage limits hit every ~5 hours during this session; agents' work survives and can be
resumed with a message.
