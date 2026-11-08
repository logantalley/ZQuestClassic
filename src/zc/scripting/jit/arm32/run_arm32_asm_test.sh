#!/usr/bin/env bash
# Builds and runs the arm32 JIT assembler's host test (needs devkitARM's binutils; set
# ARM_AS / ARM_OBJCOPY to use others). Also compile-checks the assembler with devkitARM.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/arm32_asm_test_build.$$"
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -o "$out/arm32_asm_test" \
	"$here/arm32_asm_test.cpp" "$here/arm32_asm.cpp"
DKA="${DEVKITARM:-/opt/devkitpro/devkitARM}"
if [ -x "$DKA/bin/arm-none-eabi-g++" ]; then
	"$DKA/bin/arm-none-eabi-g++" -std=c++20 -O2 -Wall -Wextra -march=armv6k -marm \
		-c "$here/arm32_asm.cpp" -o "$out/arm32_asm.o"
	echo "devkitARM compile: ok"
fi
"$out/arm32_asm_test"
