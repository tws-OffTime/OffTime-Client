<a href="https://repology.org/metapackage/teeworlds/versions">
    <img src="https://repology.org/badge/vertical-allrepos/teeworlds.svg" alt="Packaging status" align="right">
</a>

Teeworlds [![CircleCI](https://circleci.com/gh/teeworlds/teeworlds.svg?style=svg)](https://circleci.com/gh/teeworlds/teeworlds) ![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/github/teeworlds/teeworlds?branch=master&svg=true)
=========

A retro multiplayer shooter
---------------------------

Teeworlds is a free online multiplayer game, available for all major
operating systems. Battle with up to 16 players in a variety of game
modes, including Team Deathmatch and Capture The Flag. You can even
design your own maps!

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software. See license.txt for full license
text including copyright information.

Please visit https://www.teeworlds.com/ for up-to-date information about
the game, including new versions, custom maps and much more.

Originally written by Magnus Auvinen.


Building on Linux
=================

Installing dependencies
-----------------------

    # Debian/Ubuntu
    sudo apt install build-essential cmake git libfreetype6-dev libsdl2-dev libpnglite-dev libwavpack-dev python3

    # Fedora
    sudo dnf install @development-tools cmake gcc-c++ git freetype-devel mesa-libGLU-devel pnglite-devel python3 SDL2-devel wavpack-devel

    # Arch Linux (doesn't have pnglite in its repositories)
    sudo pacman -S --needed base-devel cmake freetype2 git glu python sdl2 wavpack


Downloading repository
----------------------

    git clone https://github.com/teeworlds/teeworlds --recurse-submodules
    cd teeworlds

    # If you already cloned the repository before, use:
    # git submodule update --init


Building
--------

    mkdir -p build
    cd build
    cmake ..
    make

On subsequent builds, you only have to repeat the `make` step.

You can then run the client with `./teeworlds` and the server with
`./teeworlds_srv`.


Build options
-------------

The following options can be passed to the `cmake ..` command line (between the
`cmake` and `..`) in the "Building" step above.

`-GNinja`: Use the Ninja build system instead of Make. This automatically
parallizes the build and is generally **faster**. (Needs `sudo apt install
ninja-build` on Debian, `sudo dnf install ninja-build` on Fedora, and `sudo
pacman -S --needed ninja` on Arch Linux.)

`-DDEV=ON`: Enable debug mode and disable some release mechanics. This leads to
**faster** builds.

`-DCLIENT=OFF`: Disable generation of the client target. Can be useful on
headless servers which don't have graphics libraries like SDL2 installed.
