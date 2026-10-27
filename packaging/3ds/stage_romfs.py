# Stages the 3DS RomFS: the read-only subset of resources/ that zplayer loads
# at runtime. Configs are preprocessed like scripts/package.py does (the
# default of every `#?` directive is kept), and resources/3ds/zc.cfg is applied
# over base_config/zc.cfg.
#
#   python stage_romfs.py <resources dir> <romfs dir>

import re
import shutil
import sys
from pathlib import Path

INCLUDE = [
	'allegro5.cfg',
	'assets/*.bmp',
	'assets/*.mid',
	'assets/zc/*',
	'base_config/zc.cfg',
	'base_config/zcl.cfg',
	'base_config/zquest.cfg',
	'base_config/zscript.cfg',
	'*.nsf',
	'music/*.nsf',
	'modules/**/*',
	'sfx.dat',
	'themes/**/*',
	'zc.png',
]
EXCLUDE = [
	'assets/zc/ZC_Icon_Medium_Editor.png',
	'assets/zc/ZC_Icon_Medium_Launcher.png',
]


def strip_directives(text: str):
	return '\n'.join(line.split('#?')[0].rstrip() for line in text.splitlines()) + '\n'


def read_overrides(path: Path):
	overrides: dict[str, dict[str, str]] = {}
	section = ''
	for line in path.read_text().splitlines():
		line = line.strip()
		if not line or line.startswith('#'):
			continue
		if m := re.fullmatch(r'\[(.+)\]', line):
			section = m.group(1)
			overrides.setdefault(section, {})
		else:
			key, value = [x.strip() for x in line.split('=', 1)]
			overrides.setdefault(section, {})[key] = value
	return overrides


def apply_overrides(text: str, overrides: dict[str, dict[str, str]]):
	pending = {s: dict(kv) for s, kv in overrides.items()}
	out: list[str] = []
	section = ''

	def flush(section: str):
		for key, value in pending.pop(section, {}).items():
			out.append(f'{key} = {value}')

	for line in text.splitlines():
		if m := re.fullmatch(r'\s*\[(.+)\]\s*', line):
			flush(section)
			section = m.group(1)
		elif m := re.fullmatch(r'\s*([^#=\s][^=]*?)\s*=.*', line):
			key = m.group(1)
			if key in pending.get(section, {}):
				line = f'{key} = {pending[section].pop(key)}'
		out.append(line)
	flush(section)
	for section in list(pending):
		out.append(f'\n[{section}]')
		flush(section)
	return '\n'.join(out) + '\n'


def main():
	resources_dir = Path(sys.argv[1]).resolve()
	romfs_dir = Path(sys.argv[2]).resolve()
	overrides = read_overrides(resources_dir / '3ds/zc.cfg')

	if romfs_dir.exists():
		shutil.rmtree(romfs_dir)
	for pattern in INCLUDE:
		matches = [p for p in resources_dir.glob(pattern) if p.is_file()]
		if not matches:
			raise Exception(f'nothing matched: {pattern}')
		for path in matches:
			rel = path.relative_to(resources_dir).as_posix()
			if rel in EXCLUDE:
				continue
			dest = romfs_dir / rel
			dest.parent.mkdir(parents=True, exist_ok=True)
			if path.suffix == '.cfg':
				text = strip_directives(path.read_text())
				if rel == 'base_config/zc.cfg':
					text = apply_overrides(text, overrides)
				dest.write_text(text)
			else:
				shutil.copy2(path, dest)


if __name__ == '__main__':
	main()
