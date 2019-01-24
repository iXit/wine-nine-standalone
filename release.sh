#!/bin/sh -e

SRC=`dirname $(readlink -f $0)`

$SRC/bootstrap.sh

TMP=`mktemp -d`

meson \
	--cross-file "$SRC/tools/cross-wine64" \
	--buildtype "release" \
	--prefix "$TMP/nine" \
	--bindir bin64 \
	--libdir lib64 \
	"$TMP/build64"

ninja -C "$TMP/build64" install

meson \
	--cross-file "$SRC/tools/cross-wine32" \
	--buildtype "release" \
	--prefix "$TMP/nine" \
	--bindir bin32 \
	--libdir lib32 \
	"$TMP/build32"

ninja -C "$TMP/build32" install

install -m 755 "$SRC/tools/nine-install.sh" "$TMP/nine/"
tar -C "$TMP" -czf "$TMP/nine.tar.gz" nine

printf "\nenjoy your release: $TMP/nine.tar.gz\n"

exit 0
