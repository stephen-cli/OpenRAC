#!/bin/sh
# The boot test, inside the openrac-be container (README.md): the game of BUILD (a build folder under /src/port)
# runs headless on the extracted data in /data for FRAMES frames, straight into Veldin, and writes its state and
# the functions it reached beside the data. The extractor runs first when /data has no decompiler_out (the x86-64
# build's, from the disc image in /iso). An emulator for the game, qemu-ppc64 for the big-endian build, goes after
# FRAMES.
#   boot.sh be-x86 1800
#   boot.sh be-ppc 1800 qemu-ppc64 -L /usr/powerpc64-linux-gnu
set -u
BUILD=$1
FRAMES=$2
shift 2
cd /src/port || exit 1
if [ ! -d /data/decompiler_out/rac1 ]; then
    echo "== extracting"
    ./build/be-x86/openrac-extractor /iso/SCES_509.16.iso --game rac1 --proj-path /data --extract --decompile || exit 1
fi
echo "== booting with $BUILD for $FRAMES frames"
export OPENRAC_DIRECT=1
export OPENRAC_PRESS=100:4000:5
export OPENRAC_STATE_DUMP=/data/$BUILD-state.json
export OPENRAC_REACH=/data/$BUILD-reach.json
"$@" "./build/$BUILD/openrac-rac1-pal" --data /data --no-card --level 0 --skip-movies --stop-on-missing \
    --frames "$FRAMES"
echo "== exit $?"
echo "== state"
cat "/data/$BUILD-state.json" 2>/dev/null
echo
