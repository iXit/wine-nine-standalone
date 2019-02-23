# spec file for package wine-nine
#
# Copyright (c) 2017-2019 siro@das-labor.org
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Depends on stable Wine API. No need to rebuild with every minor release
# Official nine bugtracker https://github.com/iXit/Mesa-3D/issues
# Depends on libdl to install redirects to d3d9-nine.dll

# define the following at build-time
# patchlevel
# commithash
# mychangelog

%define pkgname wine-nine

Name:             %{pkgname}
Version:          4.0
Release:          %{?patchlevel}%{?dist}
Summary:          Wine D3D9 interface library for Mesa's Gallium Nine statetracker. Tag stable releases.
License:          LGPL-2.0
URL:              https://github.com/iXit/wine-nine-standalone/
Source0:          https://github.com/iXit/wine-nine-standalone/archive/%{commithash}.tar.gz
Group:            Applications/Emulators
Requires(post):   /sbin/ldconfig
Requires(postun): /sbin/ldconfig

ExclusiveArch:  %{ix86} x86_64

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  libX11-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  mesa-libd3d-devel
BuildRequires:  libXext-devel
BuildRequires:  libxcb-devel
BuildRequires:  xorg-x11-proto-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  mesa-libEGL-devel
BuildRequires:  libdrm-devel
BuildRequires:  llvm-devel
BuildRequires:  wine-devel

Conflicts:      wine-nine
Requires:       wine-common >= %{version}
Enhances:       wine

%ifarch %{ix86}
Requires:       mesa-dri-drivers(x86-32)
Requires:       mesa-libd3d(x86-32)
Requires:       libxcb(x86-32)
Requires:       libX11(x86-32)
Requires:       libXext(x86-32)
Provides:       wine-nine(x86-32) = %{version}-%{release}
Obsoletes:      wine-nine(x86-32) < %{version}-%{release}

Provides: ninewinecfg.exe.so(x86-32) = %{version}
Provides: d3d9-nine.dll.so(x86-32) = %{version}
%endif

%ifarch x86_64
Requires:       mesa-dri-drivers(x86-64)
Requires:       mesa-libd3d(x86-64)
Requires:       libxcb(x86-64)
Requires:       libX11(x86-64)
Requires:       libXext(x86-64)
Provides:       wine-nine(x86-64) = %{version}-%{release}
Obsoletes:      wine-nine(x86-64) < %{version}-%{release}
Requires:       wine-nine(x86-32) = %{version}-%{release}

Provides: ninewinecfg.exe.so(x86-64) = %{version}
Provides: d3d9-nine.dll.so(x86-64) = %{version}
%endif

%define desc Wine sub package that contains the D3D9 library as well as the tool to configure it. \
Installs d3d9-nine.dll that interfaces Mesa's gallium nine statetracker. \
Installs ninewinecfg.exe that allows to configure nine and to provide debugging information. \
Offical bugtracker is at: https://github.com/iXit/Mesa-3D/issues \
Build from master branch, commit %{commithash}.

%description
%desc

%global debug_package %{nil}
%prep
%autosetup -n %{pkgname}-standalone-%{commithash}

%build

export PKG_CONFIG_PATH=%{_libdir}/pkgconfig
./bootstrap.sh
mkdir -p ./tmp

meson \
%ifarch x86_64
        --cross-file "./tools/cross-wine64" \
%else
        --cross-file "./tools/cross-wine32" \
%endif
        --buildtype "release" \
        --bindir bin \
        --libdir lib \
        $MESONARGS \
        "./tmp/build"

ninja -C "./tmp/build"
find -L .

%install
install -m 755 -d %{buildroot}/%{_libdir}/wine
install -m 755 -d %{buildroot}/%{_libdir}/wine/fakedlls

install -m 755 ./tmp/build/ninewinecfg/ninewinecfg.exe.so %{buildroot}/%{_libdir}/wine/ninewinecfg.exe.so
install -m 755 ./tmp/build/ninewinecfg/ninewinecfg.exe.fake %{buildroot}/%{_libdir}/wine/fakedlls/ninewinecfg.exe

install -m 755 ./tmp/build/d3d9-nine/d3d9-nine.dll.so %{buildroot}/%{_libdir}/wine/d3d9-nine.dll.so
install -m 755 ./tmp/build/d3d9-nine/d3d9-nine.dll.fake %{buildroot}/%{_libdir}/wine/fakedlls/d3d9-nine.dll

%files
%dir %{_libdir}/wine
%dir %{_libdir}/wine/fakedlls
%{_libdir}/wine/*.so
%{_libdir}/wine/fakedlls/*

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%changelog
%{mychangelog}
