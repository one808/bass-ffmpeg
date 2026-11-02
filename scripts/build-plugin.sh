#!/usr/bin/env bash
# build-plugin.sh - compile and link bass-ffmpeg.dll (static FFmpeg, host=bass.dll).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
CROSS=$PWD/sandbox/cross_compilers/mingw-w64-i686
TRIPLE=i686-w64-mingw32
PREFIX=$CROSS/$TRIPLE
CFLAGS_XP="-O2 -march=pentium3 -mtune=athlon-xp -mfpmath=sse -msse"

export PATH=$CROSS/bin:$PATH
mkdir -p build

echo "== building bass.dll import library =="
$TRIPLE-dlltool -d bass-sdk/bass.def -l build/libbass.a -D bass.dll -k && $TRIPLE-nm build/libbass.a | grep -c BASS_GetVersion

echo "== compiling bass-ffmpeg.cpp =="
$TRIPLE-g++ $CFLAGS_XP -Os -fno-exceptions -fno-rtti \
	-Ibass-sdk -I"$PREFIX/include" \
	-c src/bass-ffmpeg.cpp -o build/bass-ffmpeg.o

echo "== linking bass-ffmpeg.dll (static FFmpeg) =="
# pkg-config resolves the FULL static dependency set (incl. Windows system libs).
# g++ driver + -static-libstdc++/-static-libgcc: no libstdc++-6.dll/libgcc_s DLL
# may be imported - everything except bass.dll (the host) goes into this DLL.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR=
FFLIBS=$(pkg-config --static --libs libavformat libavcodec libswresample libavutil)
echo "link libs: $FFLIBS"
$TRIPLE-g++ -shared -o build/bass-ffmpeg.dll build/bass-ffmpeg.o \
	-Wl,--kill-at -static-libstdc++ -static-libgcc \
	-Lbuild -lbass $FFLIBS \
	-lssp -latomic

# verify the exact export name BASS looks for
if ! $TRIPLE-objdump -p build/bass-ffmpeg.dll | grep -qw "BASSplugin"; then
	echo "ERROR: export 'BASSplugin' missing:"; $TRIPLE-objdump -p build/bass-ffmpeg.dll | sed -n '/Export Address Table/,/PE File Base Relocations/p'; exit 1
fi
echo "export check: BASSplugin OK"

$TRIPLE-strip --strip-unneeded build/bass-ffmpeg.dll
ls -la build/bass-ffmpeg.dll
echo "done."
