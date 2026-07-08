# Flurry

3DS screen streaming over WiFi, built for [Chokistream](https://github.com/Eiim/Chokistream).

Flurry is based on [ChirunoMod](https://github.com/ChainSwordCS/ChirunoMod) by ChainSwordCS, itself a continuation of HzMod by Sono. It is a utility background process for the Nintendo 3DS, purpose-built for screen-streaming over WiFi.

`Flurry` is licensed under the `GNU GPLv3` license. See the [License](#license) section of this file for details, and see the file `LICENSE` for more details including the full terms of the license.

## Current features
* screen streaming from 3DS to PC using the Chokistream client
* Hold the Y button while launching the loader to boot HzMod instead

## Getting started

Get the latest stable release of [Chokistream](https://github.com/Eiim/Chokistream/releases) to run on PC.

Grab the latest `.cia` builds from this repo's [GitHub Actions](../../actions) — every commit produces a build artifact containing `ChirunoMod.cia`, `ChLoad.cia`, and `ChLoad_HIMEM.cia`.

On 3DS, use FBI to install the `.cia` files...

Install `ChLoad.cia`, which is the loader.

If you are using an original 3DS, 3DS XL, or 2DS, which are otherwise referred to as "Old 3DS" systems,
if you're going to use the streamer while playing games which use the High/Extended Memory Mode (such as Super Smash Bros. or Pokemon Sun/Moon,) then also install `ChLoad_HIMEM.cia`.

## RGB LED color codes

### Status Codes
- light-blue / teal = connected to wifi, waiting for Chokistream client to connect
- green = connected to client
- yellow = waiting to connect to wifi...
    - dev: sometimes this indicates when certain games switch the 3DS into Local Wireless (WLAN) mode.
- flashing yellow + magenta = stopping secondary thread, disconnecting...

### Error Codes
- flashing yellow + black = fatal error. if possible, hold Start and Select to shutdown the streamer.
    - dev: main() function encountered an issue and panicked (hangmacro). check HzLog.log for details.
- flashing red + green = stuck (or crashed) in netfuncTestFramebuffer() function.
    - dev: encountered an issue trying to obtain process ID of foreground process (?)
- flashing red + white = misc. error
    - dev: possibly C++ exception; CPPCrashHandler(). check HzLog.log for details.
- flashing red + other color = failed to start secondary thread (out of resources)
- red = encountered an error near the start of the main() function
- dark blue = trying to shut down (perhaps softlocked?)

## Credits

see [CREDITS.md](CREDITS.md)

## Build Instructions

Builds happen automatically in CI — see [.github/workflows/build.yml](.github/workflows/build.yml). Push a commit, download the `.cia` artifacts from the Actions run.

To build locally instead:

### Prerequisites

* git
* [makerom](https://github.com/3DSGuy/Project_CTR/releases?q=makerom) in PATH
* [bannertool](https://github.com/Epicpkmn11/bannertool) in PATH
* Legacy versions of devkitARM and libctru (detailed below)
* Libraries: 3ds-libjpeg-turbo and 3ds-zlib

Currently, all active branches need to be compiled with a legacy version of the libctru library and the devkitARM toolchain. Specifically:
* devkitARM r46
* libctru 1.2.1

An archive containing these can be downloaded here (Windows only): <https://chainswordcs.com/dl/hzmod_dependencies_2017_v1.zip>

Alternatively, archives of libctru, devkitARM, and related things can be found here: <https://wii.leseratte10.de/devkitPro/>

Installation (tailored to Windows users):
1. Contents of the `devkitarm-r46` folder should be moved to `C:/devkitPro/devkitARM` (Please copy or rename the up-to-date devkitARM folder so it can be restored if you need)
2. Contents of the `libctru-1.2.1` folder should be moved to `C:/devkitPro/libctru` (Same precaution as above)
3. The `portlibs` folder can be copied over and merged with the existing folder `C:/devkitPro/portlibs`. 2017 libctru/devkitARM uses "armv6k", whereas current libctru/devkitARM uses "3ds".

### Building

The streamer module

1. Clone the repository.
2. If desired, change `#define` build flags at the top of `/soos/main.cpp` to enable verbose debug logging or disable debug logging altogether.
3. Run `make`.
4. The compiled file is `ChirunoMod.cia`. Copy that to the 3DS and install it using FBI or another cia installer homebrew.

ChLoad (HzLoad)

1. Navigate to the `/HzLoad/` directory. Run `make` to compile `ChLoad.cia`, or `make HIMEM=1` to compile `ChLoad_HIMEM.cia`.

# License

`Flurry` is licensed under the `GNU GPLv3` license. See `LICENSE` for details.

`Flurry` is based on `ChirunoMod`.

`ChirunoMod` is Copyright (c) 2022-2024 ChainSwordCS (https://chainswordcs.com), (chainswordcs@gmail.com), and is licensed under the `GNU GPLv3` license.

`ChirunoMod` is based on `HzMod`.

`HzMod` code and project are licensed under `GNU GPLv3` license.

`HzMod` code and project are Copyright (c) 2017 Sono (https://github.com/SonoSooS), (https://gbatemp.net/members/sono.373734/).

The original `HzMod` project is sometimes otherwise referred to as `HorizonM`, `HzModHax`, `HorizonMod`, or `HorizonModule`. The `HzMod` project includes `HorizonScreen` and `HzLoad`.

**See `LICENSE` for details.**
