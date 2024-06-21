#!/bin/sh -e
# SPDX-License-Identifier: LGPL-2.1-or-later

SRC=$(dirname "$(readlink -f "$0")")
OUT=$PWD/gallium-nine-standalone.tar.gz

while getopts "o:h" opt; do
	case "$opt" in
		o)
			OUT=$OPTARG
			;;
		h|\?)
			printf "%s [OPTION] [-- MESONARGS]\n" "$0"
			printf "\t-o FILE\t\tcreate release as FILE\n"
			printf "\t-h\t\tprint this help\n"
			printf "\t-- MESONARGS\tpass MESONARGS to meson\n"
			exit 1
			;;
	esac
done

shift $((OPTIND - 1))

echo "creating $OUT"
echo "additional meson args: $*"

"$SRC"/bootstrap.sh

TMP=$(mktemp -d)
PREFIX="$TMP/gallium-nine-standalone"

meson \
	--cross-file "$SRC/tools/cross-wine64" \
	--buildtype "release" \
	--prefix "$PREFIX" \
	--libdir '' \
	"$@" \
	"$TMP/build64"

ninja -C "$TMP/build64" install

meson \
	--cross-file "$SRC/tools/cross-wine32" \
	--buildtype "release" \
	--prefix "$PREFIX" \
	--libdir '' \
	"$@" \
	"$TMP/build32"

ninja -C "$TMP/build32" install

# winetricks backwards compatibility
# Hard links should be safe here, and avoid bloating up the archive size.
install -d "$PREFIX/lib64"
ln "$PREFIX/wine/x86_64-unix/d3d9-nine.dll.so" "$PREFIX/lib64/d3d9-nine.dll.so"
install -d "$PREFIX/bin64"
ln "$PREFIX/wine/x86_64-unix/ninewinecfg.exe.so" "$PREFIX/bin64/ninewinecfg.exe.so"
install -d "$PREFIX/lib32"
ln "$PREFIX/wine/i386-unix/d3d9-nine.dll.so" "$PREFIX/lib32/d3d9-nine.dll.so"
install -d "$PREFIX/bin32"
ln "$PREFIX/wine/i386-unix/ninewinecfg.exe.so" "$PREFIX/bin32/ninewinecfg.exe.so"

install -m 644 "$SRC/LICENSE" "$PREFIX/"
install -m 644 "$SRC/README.rst" "$PREFIX/"
install -m 755 "$SRC/tools/nine-install.sh" "$PREFIX/"
tar --owner=nine:1000 --group=nine:1000 -C "$TMP" -czf "$OUT" gallium-nine-standalone

printf "\nenjoy your release: %s\n" "$OUT"

exit 0
