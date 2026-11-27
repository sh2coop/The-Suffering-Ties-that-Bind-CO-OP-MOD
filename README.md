# The Suffering: Ties That Bind - PC port with split-screen co-op

## [⬇ Download the PC port builder + co-op mod (you need your own Xbox ISO)](https://github.com/sh2coop/The-Suffering-Ties-that-Bind-CO-OP-MOD/archive/refs/heads/main.zip)

The green **Code** button, then "Download ZIP", gives the same thing.

**This is not the game.** It is the tool that turns your own Xbox disc image of *The Suffering: Ties That Bind* into a
PC version with the co-op mod, on your PC. No game data is in here, and every player needs his own Xbox image.

An unofficial native PC port of the original Xbox version of *The Suffering: Ties That Bind*, with split-screen
co-op for two players on one PC. It is made with the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool:
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

If something goes wrong, zip the `Logs` folder and send it. For every time the game was started it has a log file,
and for the three newest starts also a packed record of every frame (`..._detail.log.gz`) and a folder of small
pictures of the game's window, one every three seconds (`..._pictures`): about 60 to 100 MB for an hour of play.
Press **F9** when something looks wrong on the screen: the game saves a full picture and what it was doing at that
moment. Only the game's own window is ever pictured, and the keyboard is not recorded. The crash reports are in the
same folder.

## Co-op (split screen)

Two players on one PC, their views side by side. Controller 1 is Player 1, controller 2 is Player 2.

- Turn it on with **Co-op** on the main menu, or in a game: pause, then **Co-op**. The same page has each player's
  own look: *Player 1 look* and *Player 2 look* (Normal or Inverted) and *Player 2 look speed*. In Player 2's own
  pause menu, **Co-op** opens his settings.
- The picture fills the whole screen in split screen, whatever *Picture shape* is set to, so both players get the
  full width of a wide monitor.
- Player 1 opens doors, uses things, picks up story items, sets off scenes and saves. Player 2 fights alongside,
  picks up ammunition and guns (whatever one player picks up of those, the other gets too), has his own health
  and ammunition on his half of the screen, and comes back beside Player 1 after scenes and level changes.
- Player 2 stuck or left behind? He holds A+B+X+Y for five seconds and is beside Player 1.
- Either player can pause; the pause menu says who did, and that player uses it.
- The game's own difficulty: enemies are as tough as with one player, and the players cannot hurt each other.

There is no online play: it was tried and taken out again, split screen is the co-op of this port.

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
