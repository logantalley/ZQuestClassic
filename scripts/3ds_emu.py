#!/usr/bin/env python3
"""Runs a command line of the 3DS build of zplayer in the Azahar emulator.

Sets up an isolated Azahar instance (its own HOME, X display and GDB stub port, so several
can run at once), puts files on its virtual SD card, writes the command line to
sdmc:/3ds/zquestclassic/args.txt (see src/platform/3ds/platform_3ds.cpp), boots the .3dsx,
waits for zplayer to exit, and prints its stdout/stderr, the new part of allegro.log and the
exit code. Exits with zplayer's exit code (124 on timeout, 125 if it died without one).

Relative paths in the command line resolve against sdmc:/3ds/zquestclassic/, which is also
where --file puts files by default. Example - run a replay:

    python scripts/3ds_emu.py --home /home/user/emu_b --display 101 --gdb-port 24701 \\
        --file tests/replays/classic_1st/classic_1st_lvl1.zplay=replays/classic_1st_lvl1.zplay \\
        --file tests/replays/classic_1st/classic_1st.qst=replays/classic_1st.qst \\
        --args='-assert replays/classic_1st_lvl1.zplay -replay-exit-when-done -v0 -headless -no-jit'

Needs Xvfb, and devkitARM's arm-none-eabi-gdb (the stub holds the app at boot until a
debugger connects and detaches).
"""

import argparse
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AZAHAR = Path('/home/user/azahar/squashfs-root/AppRun')
GDB = Path('/opt/devkitpro/devkitARM/bin/arm-none-eabi-gdb')
SOURCE_HOME = Path('/root')
CONFIG_REL = Path('.config/azahar-emu')
DATA_REL = Path('.local/share/azahar-emu')
APP_DIR_REL = Path('sdmc/3ds/zquestclassic')

parser = argparse.ArgumentParser(
    description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
)
parser.add_argument(
    '--3dsx',
    dest='app',
    type=Path,
    default=ROOT / 'build_3ds/zplayer.3dsx',
    help='The .3dsx to run (default: build_3ds/zplayer.3dsx)',
)
parser.add_argument('--args', default='', help='zplayer command line (written to args.txt). Use --args="..." since it starts with a dash.')
parser.add_argument(
    '--file',
    action='append',
    default=[],
    metavar='SRC[=DEST]',
    help='Copy SRC to the SD card, at DEST relative to sdmc:/3ds/zquestclassic/ (default: its file name). Repeatable.',
)
parser.add_argument('--home', type=Path, required=True, help='HOME for this Azahar instance')
parser.add_argument('--display', type=int, required=True, help='X display number (Xvfb)')
parser.add_argument('--gdb-port', type=int, required=True)
parser.add_argument('--timeout', type=float, default=600, help='Seconds to wait for zplayer to exit')
parser.add_argument('--log-lines', type=int, default=60, help='Lines of allegro.log to print')
parser.add_argument(
    '--throttle', action='store_true', help='Run at 3DS speed (default: unthrottled)'
)
parser.add_argument(
    '--keep-running',
    action='store_true',
    help='Leave the emulator running after zplayer exits or the timeout (ex: to take screenshots)',
)
args = parser.parse_args()


def log(msg):
    print(f'[3ds_emu] {msg}', file=sys.stderr, flush=True)


def edit_qt_ini(path: Path, settings: dict):
    """Sets (section, key) -> value in a Qt ini file (and marks each as not default)."""
    wanted = {}
    for (section, key), value in settings.items():
        wanted.setdefault(section, {})[key] = value
        wanted[section][key + '\\default'] = 'false'
    lines = path.read_text().splitlines() if path.exists() else []
    out = []
    section = None

    def flush_section():
        for key, value in wanted.get(section, {}).items():
            out.append(f'{key}={value}')
        wanted.pop(section, None)

    for line in lines:
        if line.startswith('[') and line.endswith(']'):
            if section is not None:
                # Keep a blank line between sections.
                trailing_blank = out and out[-1] == ''
                if trailing_blank:
                    out.pop()
                flush_section()
                if trailing_blank:
                    out.append('')
            section = line[1:-1]
            out.append(line)
            continue
        key = line.split('=', 1)[0]
        if section in wanted and key in wanted[section]:
            out.append(f'{key}={wanted[section].pop(key)}')
        else:
            out.append(line)
    if section is not None:
        flush_section()
    for section in list(wanted):
        out += ['', f'[{section}]']
        flush_section()
    path.write_text('\n'.join(out) + '\n')


def setup_home(home: Path):
    for rel in (CONFIG_REL, DATA_REL):
        if not (home / rel).exists():
            log(f'copying {SOURCE_HOME / rel} -> {home / rel}')
            shutil.copytree(SOURCE_HOME / rel, home / rel, symlinks=True)

    ini_path = home / CONFIG_REL / 'qt-config.ini'
    settings = {
        ('Debugging', 'use_gdbstub'): 'true',
        ('Debugging', 'gdbstub_port'): args.gdb_port,
        # Null audio sink: the emulated DSP still runs, nothing is played.
        ('Audio', 'output_type'): 1,
        ('Audio', 'enable_realtime_audio'): 'false',
        # 0 is unthrottled.
        ('Renderer', 'frame_limit'): 100 if args.throttle else 0,
        ('Renderer', 'use_vsync'): 'false',
        ('UI', 'confirmClose'): 'false',
        ('UI', 'pauseWhenInBackground'): 'false',
    }
    edit_qt_ini(ini_path, {k: str(v) for k, v in settings.items()})

    # The DSP service needs a firmware dump to start; zeros are enough for the emulator.
    dspfirm = home / DATA_REL / 'sdmc/3ds/dspfirm.cdc'
    if not dspfirm.exists():
        dspfirm.parent.mkdir(parents=True, exist_ok=True)
        dspfirm.write_bytes(bytes(64 * 1024))


def ensure_display(display: int):
    """Starts Xvfb on the display unless something already serves it. Returns the process."""
    lock = Path(f'/tmp/.X{display}-lock')
    try:
        os.kill(int(lock.read_text().strip()), 0)
        return None
    except (OSError, ValueError):
        # Not running; clear what a killed server left behind.
        lock.unlink(missing_ok=True)
        Path(f'/tmp/.X11-unix/X{display}').unlink(missing_ok=True)
    p = subprocess.Popen(
        ['Xvfb', f':{display}', '-screen', '0', '1280x800x24'],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    for _ in range(50):
        if Path(f'/tmp/.X11-unix/X{display}').exists():
            return p
        time.sleep(0.1)
    raise Exception(f'Xvfb :{display} did not start')


def unpause_with_gdb(deadline: float, emu: subprocess.Popen):
    # No symbol file: loading zplayer.elf takes gdb a long time, and isn't needed to detach.
    cmd = [str(GDB), '-q', '-batch', '-ex', 'set confirm off', '-ex', 'set tcp connect-timeout 5']
    cmd += ['-ex', f'target remote localhost:{args.gdb_port}', '-ex', 'detach']
    while time.time() < deadline and emu.poll() is None:
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            continue
        if 'detached' in (r.stdout + r.stderr).lower():
            return True
        time.sleep(1)
    return False


def read_text(path: Path):
    try:
        return path.read_text(errors='replace')
    except OSError:
        return ''


def kill(p: subprocess.Popen):
    if p and p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.wait()


def main():
    home = args.home.absolute()
    app = args.app.absolute()
    if not app.exists():
        log(f'not found: {app}')
        return 2
    setup_home(home)
    app_dir = home / DATA_REL / APP_DIR_REL
    app_dir.mkdir(parents=True, exist_ok=True)

    for spec in args.file:
        src, _, dest = spec.partition('=')
        src = Path(src)
        dest = app_dir / (dest or src.name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dest)

    # Fresh per run; zplayer (re)creates them.
    for name in ('stdout.txt', 'exit.txt', 'error.log'):
        (app_dir / name).unlink(missing_ok=True)
    (app_dir / 'args.txt').write_text(args.args + '\n')
    allegro_log = app_dir / 'allegro.log'
    log_start = allegro_log.stat().st_size if allegro_log.exists() else 0

    xvfb = ensure_display(args.display)
    env = {
        **os.environ,
        'HOME': str(home),
        'DISPLAY': f':{args.display}',
        'LIBGL_ALWAYS_SOFTWARE': '1',
        'QT_QPA_PLATFORM': 'xcb',
    }
    env.pop('XDG_CONFIG_HOME', None)
    env.pop('XDG_DATA_HOME', None)
    emu_log = open(home / 'azahar.out', 'w')
    # Watched for the end of the app (below), so it must not be a previous run's.
    (home / DATA_REL / 'log/azahar_log.txt').unlink(missing_ok=True)
    start = time.time()
    deadline = start + args.timeout
    log(f'starting Azahar on :{args.display} (gdb port {args.gdb_port}): {shlex.join([str(app)])}')
    emu = subprocess.Popen(
        [str(AZAHAR), str(app)],
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=emu_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )

    status = None
    try:
        if not unpause_with_gdb(deadline, emu):
            log('could not connect to the GDB stub')
        exit_file = app_dir / 'exit.txt'
        emu_log_path = home / DATA_REL / 'log/azahar_log.txt'
        while time.time() < deadline:
            if exit_file.exists() and exit_file.read_text().strip():
                status = int(exit_file.read_text().strip())
                break
            # The app ended some other way (abort, crash, out of memory): Azahar stays open.
            # (When emulation stops, Azahar logs StopGamemode; a clean exit also logs
            # Cleaning up process.)
            emu_text = read_text(emu_log_path)
            if 'Cleaning up process' in emu_text or 'StopGamemode' in emu_text:
                time.sleep(1)
                if exit_file.exists() and exit_file.read_text().strip():
                    status = int(exit_file.read_text().strip())
                else:
                    log('the app ended without exiting')
                break
            if emu.poll() is not None:
                log(f'Azahar exited ({emu.returncode})')
                break
            time.sleep(0.5)
    finally:
        elapsed = time.time() - start
        if not args.keep_running:
            kill(emu)
            kill(xvfb)
        emu_log.close()
        # So a later launch (ex: from the GUI, on the same HOME) gets a plain command line.
        if not args.keep_running:
            (app_dir / 'args.txt').unlink(missing_ok=True)

    stdout = app_dir / 'stdout.txt'
    print('==== stdout.txt ====')
    print(stdout.read_text(errors='replace') if stdout.exists() else '(missing)', end='')
    print(f'==== allegro.log (last {args.log_lines} new lines) ====')
    if allegro_log.exists():
        with open(allegro_log, 'rb') as f:
            f.seek(log_start)
            lines = f.read().decode(errors='replace').splitlines()
        print('\n'.join(lines[-args.log_lines :]))
    error_log = app_dir / 'error.log'
    if error_log.exists():
        print('==== error.log ====')
        print(error_log.read_text(errors='replace'), end='')
    print('====')

    if status is None:
        timed_out = time.time() >= deadline
        status = 124 if timed_out else 125
        log(('timed out' if timed_out else 'zplayer exited without an exit code') + f' after {elapsed:.1f}s (Azahar log: {home / "azahar.out"})')
    else:
        log(f'zplayer exit code {status} after {elapsed:.1f}s')
    return status


# So the emulator is cleaned up when this is killed.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
sys.exit(main())
