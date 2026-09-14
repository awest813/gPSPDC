#!/bin/sh
# Package a bootable CDI around dc/gdC.elf and one ROM, for Flycast or a burn.
# Runs inside the CI toolchain container, which has scramble, mkisofs and
# cdi4dc (dc/dc.sh needs mkdcdisc, which the container lacks):
#
#   MSYS_NO_PATHCONV=1 docker run --rm \
#     -v "<repo>:/src:ro" \
#     -v "<dir holding gba_bios.bin and gbaDC/<rom>>:/assets:ro" \
#     -v "<output dir>:/out" \
#     einsteinx2/dcdev-kos-toolchain:gcc-9__v2.0.0 \
#     sh /src/scripts/dc-disc.sh "<rom file name in gbaDC/>" <name>.cdi
#
# Build dc/gdC.elf first. The ROM is copied as gbaDC/game.gba and autoloaded.
# Each of these details cost a failed boot to find: the binary must be
# scrambled, the session offset must be 11702, and gba_bios.bin belongs at the
# disc root rather than under gbaDC/.
set -e

if [ "$#" -ne 2 ]; then
  echo "usage: dc-disc.sh <rom in /assets/gbaDC> <output name>.cdi" >&2
  exit 1
fi

ROM="$1"
OUT="$2"

rm -rf /tmp/d /tmp/d.iso /tmp/g.bin
mkdir -p /tmp/d/gbaDC
cp /assets/gba_bios.bin /tmp/d/
cp /src/dc/cd/gbaDC/game_config.txt /tmp/d/gbaDC/
cp "/assets/gbaDC/$ROM" /tmp/d/gbaDC/game.gba
printf 'game.gba\r\n' > /tmp/d/gbaDC/autoload.txt
sh-elf-objcopy -R .stack -O binary /src/dc/gdC.elf /tmp/g.bin
scramble /tmp/g.bin /tmp/d/1ST_READ.BIN
mkisofs -C 0,11702 -V GPSPDC -G /src/dc/cd/IP.BIN -joliet -rock -l \
  -o /tmp/d.iso /tmp/d >/dev/null 2>&1
/opt/toolchains/dc/kos/utils/img4dc/cdi4dc/cdi4dc /tmp/d.iso "/out/$OUT" >/dev/null
ls -la "/out/$OUT"
