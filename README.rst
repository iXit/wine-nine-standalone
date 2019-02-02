Gallium Nine Standalone
=======================

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

  WINE can be updated independent of Gallium Nine Standalone. Users can mix releases of both projects to their liking. Switching between staging and non-staging does not require a rebuild.

Requirements
------------
* A Gallium based graphics driver (`Mesa 3D <https://www.mesa3d.org/>`_)
* Mesa's Gallium Nine state tracker (d3dadapter9.so)

Distro packages
---------------
Your distribution may provide a package, avoiding the need to compile the code yourself. The exact usage instructions may vary in this case so check your distribution for the details. The currently known packages are:

* Arch Linux - releases: `gallium-nine (AUR) <https://aur.archlinux.org/packages/gallium-nine>`_, snapshots: `gallium-nine-git (AUR) <https://aur.archlinux.org/packages/gallium-nine-git>`_
* Gentoo Linux - `app-emulation/gallium-nine-standalone <https://packages.gentoo.org/packages/app-emulation/gallium-nine-standalone>`_

Usage
-----
This part assumes that you used a release binary or the ``release.sh`` script as described below.

* Extract the tarball in e.g. your home directory
* run the ``nine-install.sh`` script from the directory you extracted the tarball in

The latter symlinks the extracted binaries to your WINE prefix and enables Gallium Nine Standalone. To target another WINE prefix than the standard ``~/.wine``, just set ``WINEPREFIX`` accordingly before you run ``nine-install.sh``.

Gallium Nine Standalone comes with a GUI.

For the 32bit version run ``wine ninewinecfg`` and for 64bit ``wine64 ninewinecfg``.

Compiling
---------
Gallium Nine Standalone requires the `Meson Build system <https://mesonbuild.com/>`_ and ``winegcc`` to build binaries usable by WINE.

The build dependencies are:

* WINE tools (``winegcc``, ``wrc``, ``winebuild``)
* libwine
* d3dadapter9
* libx11
* libxext
* libx11-xcb
* libxcb
* libxcb-dri3
* libxcb-present
* libxcb-xfixes

And optionally, for the DRI2 fallback:

* libgl
* libegl

Most DirectX 9 games are 32bit, for which you require 32bit binaries. For the few 64bit DirectX 9 games 64bit binaries are required.

For distro specific help, see `our wiki <https://github.com/iXit/wine-nine-standalone/wiki>`_.

To get started, it is recommended to use the script ``release.sh``, which will build for both architectures (so the build dependencies for both are required). It creates a tarball of the binaries as well as the runtime script ``nine-install.sh`` to set up a WINE prefix (see Usage_ above).
