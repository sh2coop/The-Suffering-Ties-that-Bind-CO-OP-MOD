# The Suffering: Ties That Bind - PC port with co-op

## [⬇ Download the PC port builder + co-op mod (you need your own Xbox ISO)](https://github.com/sh2coop/The-Suffering-Ties-that-Bind-CO-OP-MOD/archive/refs/heads/main.zip)

The green **Code** button, then "Download ZIP", gives the same thing.

**This is not the game.** It is the tool that turns your own Xbox disc image of *The Suffering: Ties That Bind* into a
PC version with the co-op mod, on your PC. No game data is in here, and every player needs his own Xbox image.

An unofficial native PC port of the original Xbox version of *The Suffering: Ties That Bind*, with split-screen and
online co-op for two players. It is made with the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool:
the game's own code is translated for the PC, on your PC, from your own copy of the game.

> [!NOTE]
> This project contains no game data. You need your own Xbox disc image of the game (USA / Europe release).

This is a test build. Things will be rough in places.

## Playing

1. Download the builder with the link at the top of this page and unpack the zip anywhere.
2. Put your Xbox disc image (the `.iso` file) into the folder `drop xbox iso here`.
3. Double-click `Build Recomp`. It builds the game for your PC, which takes a few minutes and happens once.
   When it says it is ready, press Enter.
4. From then on, double-click `The Suffering 2` to play.

`Update` fetches the newest version when there is one. Every update is signed, and one that is not signed by this
project's release key is thrown away.

If something goes wrong, zip the `Logs` folder and send it. It has one file for every time the game was started, and
the crash reports.

## Co-op

In the game: Options, then "Display and Co-op".

- **Split screen**: two controllers on one PC. Controller 1 is Player 1.
- **Online**: one player picks "Host online", the other "Join online" and types the host's address. The host's PC runs
  the game and sends Player 2 his picture and sound. Over the internet the host has to forward UDP port 27025.

Player 1 opens doors, uses things, picks up story items, triggers scenes and saves. Player 2 fights alongside, comes
back beside Player 1 after scenes and level changes, and has a button (BACK) that brings him to Player 1.

## Also in this port

- 60 or 120 frames a second at the game's correct speed, widescreen and ultrawide, a resolution list, window modes
  and VSync in the game's own Options menu.
- Xbox, PlayStation and Switch controllers, and the keyboard: arrows = d-pad, Enter = START, Backspace = BACK,
  Space or J = A, K = B, U = X, I = Y, O = white, P = black, W A S D = move, Q and E = triggers, numpad 8 2 4 6 = look.

## Building from source

The source is in the `source` folder. Visual Studio 2022 with "Desktop development with C++", "C++ Clang tools for
Windows" and "C++ CMake tools for Windows". Put the disc's files (with `default.xbe`) or the image in `source\assets`,
then run `source\build.bat`. `source\tools\package.ps1` makes the folder you see on this page.

## Credits and license

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482 (MIT), the recompiler and runtime in `sdk`.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [Zydis](https://github.com/zyantific/zydis),
  [toml++](https://github.com/marzer/tomlplusplus), [Dear ImGui](https://github.com/ocornut/imgui) and
  [SDL](https://github.com/libsdl-org/SDL); see `source\sdk\THIRD_PARTY.txt`.
- *The Suffering: Ties That Bind* is the property of its owners. This project is not affiliated with them.

The port's own code (`source\src`, `source\tools`, `source\config`) is under the GNU General Public License v3
(`LICENSE`). The `source\sdk` folder keeps its own MIT license. The programs in `Game Files\compiler` are LLVM's
(`LLVM-LICENSE.txt` beside them).
