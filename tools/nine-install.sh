#!/bin/sh -e

BASE=$(dirname "$(readlink -f "$0")")

die() {
	echo "$*"
	exit 1
}

wine --version >/dev/null 2>&1 || die "wine not found"
DST=$(wine winepath -u 'c:\windows\system32')

echo "installing 32bit binaries to $DST"
pe_dir='i386-windows'
so_dir='i386-unix'
ln -sf "$BASE/wine/$so_dir/d3d9-nine.dll.so" "$DST/d3d9-nine.dll"
ln -sf "$BASE/wine/$so_dir/ninewinecfg.exe.so" "$DST/ninewinecfg.exe"

unset HAVE_WINE64
wine64 winepath >/dev/null 2>&1 && HAVE_WINE64=1

if test -z "$HAVE_WINE64"; then
	echo "wine64 not found, skipping 64bit"
	echo "enabling gallium nine"
	wine ninewinecfg.exe -e
	exit 0
fi

DST=$(wine64 winepath -u 'c:\windows\system32')

echo "installing 64bit binaries to $DST"
pe_dir='x86_64-windows'
so_dir='x86_64-unix'
ln -sf "$BASE/wine/$so_dir/d3d9-nine.dll.so" "$DST/d3d9-nine.dll"
ln -sf "$BASE/wine/$so_dir/ninewinecfg.exe.so" "$DST/ninewinecfg.exe"

echo "enabling gallium nine"
wine64 ninewinecfg.exe -e

echo "done"
exit 0
