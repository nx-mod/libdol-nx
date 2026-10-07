#!/bin/sh
# Builds the GPU tests - triangle_test.nro (the layer and UAM alone) and
# gx_test.nro (GX display lists through the command processor and the
# renderer) - from libdol's GPU layer (src/platform/gpu), its portable GX
# library (src/format/gx) and UAM, with devkitA64. Nothing else of libdol, and
# no Aurora.
#
#   tests/deko3d/build.sh [out-folder] [uam-source] [uam-build]
#
# UAM's folders default to Halo's build beside this checkout, as the game
# build's do (docs/deko3d.md).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
DKP=${DEVKITPRO:-/opt/devkitpro}
BIN=$DKP/devkitA64/bin
OUT=${1:-$here/build}
UAM_SOURCE=${2:-$root/../haloce-nx/build/switch/third_party/uam}
UAM_BUILD=${3:-$root/../haloce-nx/build/switch/uam}
mkdir -p "$OUT/common"
rm -f "$OUT"/*.o "$OUT"/common/*.o  # (only this build's objects are linked)

FLAGS="-I$root/include -std=gnu++20 -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -D__SWITCH__ -I$DKP/libnx/include"
for f in "$root"/src/platform/gpu/dk_*_switch.cpp "$root"/src/format/gx/*.cpp; do
    $BIN/aarch64-none-elf-g++ $FLAGS -c "$f" -o "$OUT/common/$(basename "$f" .cpp).o"
done
"$root/tools/wiinx-uam-object" "$BIN/aarch64-none-elf-g++" "$BIN/aarch64-none-elf-ld" "$BIN/aarch64-none-elf-nm" \
    "$BIN/aarch64-none-elf-objcopy" "$UAM_SOURCE" "$UAM_BUILD" "$OUT/common/uam_compiler.o"

for test in triangle_test gx_test; do
    $BIN/aarch64-none-elf-g++ $FLAGS -c "$here/$test.cpp" -o "$OUT/$test.o"
    $BIN/aarch64-none-elf-g++ -specs=$DKP/libnx/switch.specs -march=armv8-a+crc+crypto -mtp=soft -fPIE \
        -Wl,--gc-sections -o "$OUT/$test.elf" "$OUT/$test.o" "$OUT"/common/*.o -L$DKP/libnx/lib -ldeko3d -lnx -lm
    $DKP/tools/bin/nacptool --create "libdol $test" "nx-mod" "1.0" "$OUT/$test.nacp"
    $DKP/tools/bin/elf2nro "$OUT/$test.elf" "$OUT/$test.nro" --nacp="$OUT/$test.nacp"
    echo "$OUT/$test.nro"
done
