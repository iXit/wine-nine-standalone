#!/bin/sh -e

BASE="`dirname "$(readlink -f "$0")"`"

die() {
	echo "$*"
	exit 1
}

wine --version >/dev/null 2>&1 || die "wine not found"
DST=`wine winepath -u 'c:\windows\system32'`

echo "installing 32bit binaries to $DST"
ln -sf "$BASE/lib32/d3d9-nine.dll.so" "$DST/d3d9-nine.dll"
ln -sf "$BASE/bin32/ninewinecfg.exe.so" "$DST/ninewinecfg.exe"

unset HAVE_WINE64
wine64 winepath >/dev/null 2>&1 && HAVE_WINE64=1

if test -z "$HAVE_WINE64"; then
	echo "wine64 not found, skipping 64bit"
	echo "enabling gallium nine"
	wine ninewinecfg.exe -e
	exit 0
fi

DST=`wine64 winepath -u 'c:\windows\system32'`

echo "installing 64bit binaries to $DST"
ln -sf "$BASE/lib64/d3d9-nine.dll.so" "$DST/d3d9-nine.dll"
ln -sf "$BASE/bin64/ninewinecfg.exe.so" "$DST/ninewinecfg.exe"

echo "enabling gallium nine"
wine64 ninewinecfg.exe -e

echo "done"
exit 0
