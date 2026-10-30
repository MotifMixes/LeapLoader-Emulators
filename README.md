# LeapLoader-Emulators

NES and Game Boy / Game Boy Color emulators for the **LeapFrog Leapster 2**, as homebrew apps for
[LeapLoader](https://github.com/MotifMixes/LeapLoader). LeapLoader puts them on your SD card next to your
Leapster games; on the console they appear in LeapLoader's menu as blue buttons.

| App | Emulator core | Plays | Card folder |
|---|---|---|---|
| **NES** | [QuickNES](https://github.com/libretro/QuickNES_Core) (blargg; libretro) | `.nes` | `NES\` |
| **Game Boy** | [gnuboy](https://github.com/rofl0r/gnuboy) 1.0.4 (rofl0r's revival) | `.gb`, `.gbc` | `GB\`, `GBC\` |

**Status:** tested in the [Ribbit](https://github.com/MotifMixes/ribbit) emulator (Leapster 2 BIOS 0.6):
both run at full speed there, with sound, saves, touch and the menus. **Not yet tested on a real
Leapster 2.** Use at your own risk.

No games come with them: bring your own `.nes`, `.gb` and `.gbc` files.

## Installing

LeapLoader 0.2.4 and later come with both emulators: they are on LeapLoader's list already, so
**Build card** puts them on the card. Then copy your games into the card's `NES`, `GB` and `GBC`
folders (LeapLoader creates them).

With an older LeapLoader, or to try a build of your own: in LeapLoader, **Add a folder…** and pick
`package/NES` or `package/GB` (see *Building*).

## Using them

Choose **NES** or **Game Boy** in LeapLoader's menu on the console.

**Game list** (drawn like LeapLoader's menu, with its sounds):

| Button | Does |
|---|---|
| Up / Down | choose a game |
| Left / Right | previous / next page |
| A, or touch a game | start it |
| PAUSE, or touch the magnifying glass | search (an on-screen keyboard; touch works there too) |
| B | clear the search |
| HOME | back to the LeapLoader menu |

**In a game:**

| Leapster | NES | Game Boy |
|---|---|---|
| D-pad | D-pad | D-pad |
| A | B | B |
| B | A | A |
| PAUSE | Start | Start |
| HINT | Select | Select |
| Volume buttons | volume | volume |

* **HINT + A + B**, held 2.5 s: back to the game list (a prompt shows a bar filling; let go to cancel).
* **HOME + PAUSE**, held 2 s: back to the game list too.
* Battery saves are written next to the game (`GAME.sav`) a few seconds after the game changes them,
  and when you leave it.

## How they fit the Leapster 2

* **Screen:** the Leapster 2's LCD is 160 x 160. NES pictures (256 x 240) are cropped to 240 x 224 and
  shrunk: every 3 pixels across become 2 (blended), every 7 lines become 5 (each line the average of
  two neighbours, so none is dropped). Game Boy pictures (160 x 144) fit as they are, pixel for pixel.
* **Sound:** played through the Leapster 2's own PCM channel at 32 kHz from an interrupt handler;
  the BIOS's own menu sounds are mixed in, so the game lists sound like LeapLoader's menu.
* **Speed:** when a frame takes too long, the next ones are run without drawing (a few at most), so
  the game runs at its real speed.
* **Code:** each app is native ARC code (`code.bin`, linked at 0x3C81_0000), packaged with LeapLoader's
  homebrew format (`app.txt`, `code=`; see LeapLoader's `docs/homebrew.md`). LeapLoader wraps it into a
  Leapster 2 SD image when it builds the card. The BIOS owns the `gp` register, so everything is built
  with `-mno-sdata` and checked for `gp` use.

## Building

You need the **GNU toolchain for ARC** (`arc-elf32-gcc`, `-mcpu=arc600`) and a POSIX shell (Linux,
macOS, WSL, or Git Bash on Windows).

```sh
ARC=/path/to/arc-elf32-win/bin/arc-elf32- ./build.sh
```

`ARC` is the toolchain's prefix (default `arc-elf32-`). The build writes `package/NES` and
`package/GB` (each `app.txt`, `code.bin`, `LICENSE.txt` and the `card/` folders to create) and keeps
its objects in `build/`. `VERSION` sets the packages' version.

`cc.sh` wraps the compiler: on the Leapster, the low word of a MUL64 product is read from MMID, not
MLO, so the generated assembly is fixed up before it is assembled.

## Layout

```
build.sh, cc.sh, VERSION   the build
common/llmenu.h            the game list, search keyboard, touch, menu sounds and HINT + A + B prompt
nes/core/                  QuickNES, with LeapLoader's changes (#ifdef LL)
nes/console/               the NES app: BIOS calls, LCD, buttons, sound, saves (console.cpp)
gb/core/                   gnuboy, with LeapLoader's changes (#ifdef LL)
gb/console/                the Game Boy app (console.c)
```

## Licences

* `nes/core/`: QuickNES, GNU LGPL 2.1 or later (the file headers); the libretro repository ships the
  GNU GPL 2 text, `nes/LICENSE`. `emu2413` has its own permissive licence (its header).
* `gb/core/`: gnuboy, GNU GPL 2 (`gb/core/COPYING`).
* Everything else (the console apps, `common/`, the build scripts): zlib licence (`LICENSE`).

The packages are built from these sources, so each is distributed under its core's licence: the
Game Boy app under the GPL 2, the NES app under the GPL 2 (the LGPL allows it). Changes made to the
cores for the Leapster are marked `#ifdef LL`.

LeapFrog and Leapster are trademarks of LeapFrog Enterprises. This project is not affiliated with
LeapFrog, Nintendo or the authors of the emulator cores.
