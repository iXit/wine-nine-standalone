#!/bin/sh -e
# SPDX-License-Identifier: LGPL-2.1-or-later

SRC=`dirname $(readlink -f $0)`

PKG_CONFIG_32=
PKG_CONFIG_64=
test -z "$WINE32_LIBDIR" && WINE32_LIBDIR=/nonexistant
test -z "$WINE64_LIBDIR" && WINE64_LIBDIR=/nonexistant

. /etc/os-release

for i in $ID $ID_LIKE; do
	case $i in
		debian|ubuntu)
			PKG_CONFIG_32=i686-linux-gnu-pkg-config
			PKG_CONFIG_64=x86_64-linux-gnu-pkg-config
			;;
		gentoo|arch|archlinux)
			PKG_CONFIG_32=i686-pc-linux-gnu-pkg-config
			PKG_CONFIG_64=x86_64-pc-linux-gnu-pkg-config
			;;
		opensuse|suse)
			PKG_CONFIG_32=i586-suse-linux-gnu-pkg-config
			PKG_CONFIG_64=x86_64-suse-linux-gnu-pkg-config
			;;
		fedora|rhel)
			PKG_CONFIG_32=i686-redhat-linux-gnu-pkg-config
			PKG_CONFIG_64=x86_64-redhat-linux-gnu-pkg-config
			;;
		slackware)
			PKG_CONFIG_32=i586-slackware-linux-gnu-pkg-config
			PKG_CONFIG_64=x86_64-slackware-linux-gnu-pkg-config
			;;
		solus)
			PKG_CONFIG_32=pkg-config
			PKG_CONFIG_64=x86_64-solus-linux-gnu-pkg-config
			;;
		*)
			continue
			;;
	esac

	break
done

if test -z "$PKG_CONFIG_32" -o -z "$PKG_CONFIG_64"; then
	printf '%s\n' "unknown distro (\"$ID\", like \"$ID_LIKE\")" \
		"please add support to this script and open a pull request!"
	exit 1
fi

printf '%s\n' "found $i compatible distro"

sed -e "s|@PKG_CONFIG@|$PKG_CONFIG_32|" \
	-e "s|@WINE32_LIBDIR@|$WINE32_LIBDIR|" \
	< $SRC/tools/cross-wine32.in \
	> $SRC/tools/cross-wine32

sed -e "s|@PKG_CONFIG@|$PKG_CONFIG_64|" \
	-e "s|@WINE64_LIBDIR@|$WINE64_LIBDIR|" \
	< $SRC/tools/cross-wine64.in \
	> $SRC/tools/cross-wine64

exit 0
