#!/bin/sh -e
# SPDX-License-Identifier: LGPL-2.1-or-later

SRC=`dirname $(readlink -f $0)`
OUT=$PWD/gallium-nine-standalone.tar.gz

while getopts "o:h" opt; do
	case "$opt" in
		o)
			OUT=$OPTARG
			;;
		h|\?)
			printf "$0 [OPTION] [-- MESONARGS]\n"
			printf "\t-o FILE\t\tcreate release as FILE\n"
			printf "\t-h\t\tprint this help\n"
			printf "\t-- MESONARGS\tpass MESONARGS to meson\n"
			exit 1
			;;
	esac
done

shift $(($OPTIND - 1))
MESONARGS="$@"

echo "creating $OUT"
echo "additional meson args: $MESONARGS"

$SRC/bootstrap.sh

mkdir out_temp
PREFIX="./out_temp/gallium-nine-standalone"

meson \
	--cross-file "$SRC/tools/cross-wine64" \
	--buildtype "release" \
	--prefix "$PREFIX" \
	--bindir bin64 \
	--libdir lib64 \
	$MESONARGS \
	"./out_temp/build64"

ninja -C "./out_temp/build64" install

meson \
	--cross-file "$SRC/tools/cross-wine32" \
	--buildtype "release" \
	--prefix "$PREFIX" \
	--bindir bin32 \
	--libdir lib32 \
	$MESONARGS \
	"./out_temp/build32"

ninja -C "./out_temp/build32" install

install -m 644 "$SRC/LICENSE" "$PREFIX/"
install -m 644 "$SRC/README.rst" "$PREFIX/"
install -m 755 "$SRC/tools/nine-install.sh" "$PREFIX/"
tar --owner=nine:1000 --group=nine:1000 -C "./out_temp" -czf "$OUT" gallium-nine-standalone

printf "\nenjoy your release: $OUT\n"

exit 0
