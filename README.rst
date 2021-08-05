Gallium Nine Standalone |buildstate|
====================================

.. |buildstate| image:: https://github.com/iXit/wine-nine-standalone/actions/workflows/build.yml/badge.svg?branch=master
    :target: https://github.com/iXit/wine-nine-standalone/actions

.. image:: https://wiki.ixit.cz/_media/gallium-nine.png
    :target: https://wiki.ixit.cz/d3d9

About
-----
Gallium Nine allows to run any Direct3D 9 application with nearly no CPU overhead, which provides a smoother gaming experience and increased FPS.

Gallium Nine Standalone, as the name implies, is a standalone version of the `WINE <https://www.winehq.org/>`_ parts of `Gallium Nine <https://github.com/iXit/wine>`_.

This decouples Gallium Nine from the WINE tree, so that it can be used with any WINE version. There is no need for any WINE patches. A stable, development, or staging WINE release is sufficient.

Gallium Nine Standalone consists of two parts:

* ``d3d9-nine.dll``: Gallium Nine Direct3D 9 library
* ``ninewinecfg.exe``: GUI to enable/disable Gallium Nine with some additional info about the current state

Objective
---------
* Official distro packages

  Gallium Nine is a fork of the WINE tree, without any chances to be merged upstream. The decoupling of the WINE tree makes it its own upstream.

* Ease updates for the user

  WINE can be updated independently of Gallium Nine Standalone. Users can mix releases of both projects to their liking. Switching between staging and non-staging does not require a rebuild.

Requirements
------------
* A Gallium based graphics driver (`Mesa 3D <https://www.mesa3d.org/>`_)
* Mesa's Gallium Nine state tracker (d3dadapter9.so)

Packages
--------
Your distribution may provide a package, avoiding the need to compile the code yourself. The exact usage instructions may vary in this case so check your distribution for the details. The currently known packages are:

* Arch Linux - releases: `wine-nine <https://www.archlinux.org/packages/multilib/x86_64/wine-nine/>`_, snapshots: `gallium-nine-git (AUR) <https://aur.archlinux.org/packages/gallium-nine-git>`_
* Gentoo Linux - `app-emulation/gallium-nine-standalone <https://packages.gentoo.org/packages/app-emulation/gallium-nine-standalone>`_
* Slackware Linux - `wine-nine-standalone <https://slackbuilds.org/apps/wine-nine-standalone/>`_

We also provide distro independent release binaries, available as `GitHub releases <https://github.com/iXit/wine-nine-standalone/releases>`_. You can either download these yourself (see Usage_ below), or install them via `Winetricks <https://github.com/Winetricks/winetricks>`_.

Usage
-----
This part assumes that you downloaded a release binary or compiled using `release.sh` yourself.

* Extract the tarball in e.g. your home directory
* run the ``nine-install.sh`` script from the directory you extracted the tarball in

The latter symlinks the extracted binaries to your WINE prefix and enables Gallium Nine Standalone. To target another WINE prefix than the standard ``~/.wine``, just set ``WINEPREFIX`` accordingly before you run ``nine-install.sh``.

Gallium Nine Standalone comes with a GUI.

For the 32bit version run ``wine ninewinecfg`` and for 64bit ``wine64 ninewinecfg``.

Compiling
---------
Please see `our wiki <https://github.com/iXit/wine-nine-standalone/wiki/Compiling>`_,  which also includes distro specific help.

Backends
--------
The DRI3 backend is the preferred one and has the lowest CPU and memory overhead.

As fallback for legacy platforms the DRI2 backend can be used, which has more CPU overhead and a bigger memory footprint.
The DRI2 fallback relies on mesa's EGL which provides EGLImages.

Intel Drivers
-------------
Gallium Nine could be used with the new Crocus driver (included since Mesa 21.2) on older Shader model 3.0 aka feature level 9_3 compatible Intel gen4-7 graphics (GMA X3000, GMA 4500, HD 2000-5000; year 2007-2014).

Use the environment variable ``MESA_LOADER_DRIVER_OVERRIDE=crocus`` to force using Crocus instead of i965.

All newer Intel iGPU hardware (Broadwell+) is supported through the already working Iris driver.

Debugging
---------
You can use the environment variable ``D3D_BACKEND`` to force one of the supported backends:

* dri3
* dri2

If not specified it prefers DRI3 over DRI2 if available.

