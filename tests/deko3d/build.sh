#!/bin/sh
# Builds triangle_test.nro: libdol's GPU layer (src/platform/gpu), UAM and the
# test, with devkitA64 - nothing else of libdol, and no Aurora.
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
mkdir -p "$OUT"
rm -f "$OUT"/*.o  # (only this build's objects are linked)

FLAGS="-std=gnu++20 -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -D__SWITCH__ -I$DKP/libnx/include"
for f in "$root"/src/platform/gpu/dk_*_switch.cpp "$here/triangle_test.cpp"; do
    $BIN/aarch64-none-elf-g++ $FLAGS -c "$f" -o "$OUT/$(basename "$f" .cpp).o"
done
"$root/tools/wiinx-uam-object" "$BIN/aarch64-none-elf-g++" "$BIN/aarch64-none-elf-ld" "$BIN/aarch64-none-elf-nm" \
    "$BIN/aarch64-none-elf-objcopy" "$UAM_SOURCE" "$UAM_BUILD" "$OUT/uam_compiler.o"
$BIN/aarch64-none-elf-g++ -specs=$DKP/libnx/switch.specs -march=armv8-a+crc+crypto -mtp=soft -fPIE \
    -Wl,--gc-sections -o "$OUT/triangle_test.elf" "$OUT"/*.o -L$DKP/libnx/lib -ldeko3d -lnx -lm
$DKP/tools/bin/nacptool --create "libdol deko3d triangle" "nx-mod" "1.0" "$OUT/triangle_test.nacp"
$DKP/tools/bin/elf2nro "$OUT/triangle_test.elf" "$OUT/triangle_test.nro" --nacp="$OUT/triangle_test.nacp"
echo "$OUT/triangle_test.nro"
