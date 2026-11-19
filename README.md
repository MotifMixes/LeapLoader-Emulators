# LeapLoader-Emulators

NES, Game Boy / Game Boy Color, Game Boy Advance, Super Nintendo and Sega Genesis / Mega Drive emulators for the **LeapFrog Leapster 2**, as homebrew apps for
[LeapLoader](https://github.com/MotifMixes/LeapLoader). LeapLoader puts them on your SD card next to your
Leapster games; on the console they appear in LeapLoader's menu as blue buttons.

| App | Emulator core | Plays | Card folder |
|---|---|---|---|
| **NES** | [QuickNES](https://github.com/libretro/QuickNES_Core) (blargg; libretro) | `.nes` | `NES\` |
| **Game Boy** | [gnuboy](https://github.com/rofl0r/gnuboy) 1.0.4 (rofl0r's revival) | `.gb`, `.gbc` | `GB\`, `GBC\` |
| **GBA** (experimental) | [gpSP](https://github.com/libretro/gpsp) (libretro's), with a dynamic recompiler for the Leapster's ARC CPU | `.gba` | `GBA\` |
| **SNES** (experimental) | [Snes9x](https://www.snes9x.com) 1.39 ([snes9x2002](https://github.com/libretro/snes9x2002), libretro's), with 65816, SPC700 and renderer cores in ARC assembly and a Super FX translator | `.sfc`, `.smc` | `SNES\` |
| **Genesis** (experimental) | [PicoDrive](https://github.com/irixxxx/picodrive) 2.04, with 68000, Z80, renderer and sound cores in ARC assembly | `.md`, `.bin`, `.gen`, `.smd` | `GENESIS\` |

**Status:** tested in the [Ribbit](https://github.com/MotifMixes/ribbit) emulator (Leapster 2 BIOS 0.6):
both run at full speed there, with sound, saves, touch and the menus. On a real Leapster 2 the NES
menu and games work (2026-11-01, after the fixes under *Building*); Game Boy less tested there.
Use at your own risk.

No games come with them: bring your own `.nes`, `.gb` and `.gbc` files.

**GBA is experimental.** It runs in Ribbit, with sound, saves and the menus, but has not been tested
on a real Leapster 2 yet. Games are too heavy to draw every frame at the console's 96 MHz: measured
in gameplay on a model of the console's CPU and caches, every frame drawn, a frame takes from ~140%
of a frame's time (Wario Land 4) to ~320% (F-Zero, Mario Kart); Super Mario Advance 2 ~190%,
Metroid Fusion ~200%, Advance Wars ~215%, Sonic Advance ~235%. So it skips drawing frames to keep
the game at its real speed.

**SNES is experimental too**, and under its own licence: Snes9x's, which allows **non-commercial use
only** (`snes/LICENSE.txt`), not the GPL. It is a heavily modified Snes9x 1.39: the 65816 and SPC700
CPUs, the picture and the Super FX chip (Star Fox, Yoshi's Island) run as ARC code made for the
Leapster, with the same results as Snes9x's own code, frame by frame. It has been tried in an emulator
of the console, not yet on a real Leapster 2. Measured on the model of the console (every frame
drawn), simpler games are close to full speed (9 of 32 tested at or under a frame's time; Super Mario
World and Donkey Kong Country about 100%), Mario Kart ~190%, Yoshi's Island and Star Fox ~200-215%;
it skips drawing frames to keep the game at its real speed.

**Genesis is experimental and not tested on a real Leapster 2**, and also under its own licence:
PicoDrive's (the MAME licence), **non-commercial use only** (`genesis/LICENSE.txt`). It is PicoDrive
2.04 with the 68000 and Z80 CPUs, the line renderer and the YM2612/PSG sound loops as ARC code made
for the Leapster, with the same results as PicoDrive's own code, frame by frame, sound included. It
draws only the 160 lines the LCD shows, straight into the LCD. Drawn every frame, most games take
more than a frame's time at 96 MHz, so it skips drawing (never the game itself, the Z80 or the
sound): at most 2 frames in a row normally; while more than 2 frames behind, the limit rises a frame
at a time up to 8, and goes back to 2 once less than a frame behind. Measured in gameplay on a model
of the console's CPU and caches (12 games):

| Game | Every frame drawn | With skipping: time a frame | frames skipped | longest skip run | 60 Hz |
|---|---|---|---|---|---|
| Streets of Rage 2 | 79% | 79% | 0% | 0 | yes |
| Moonwalker | 92% | 81% | 18% | 2 | yes |
| Sonic the Hedgehog | 113% | 95% | 31% | 2 | yes |
| Castlevania: Bloodlines | 108% | 94% | 29% | 4 | yes |
| Earthworm Jim | 119% | 98% | 34% | 2 | yes |
| Aladdin | 123% | 97% | 40% | 2 | yes |
| Gunstar Heroes | 130% | 99% | 50% | 3 | yes |
| Shinobi III | 134% | 100% | 59% | 4 | yes |
| Sonic the Hedgehog 2 | 140% | 100% | 59% | 2 | yes |
| Lightening Force | 144% | 100% | 66% | 8 | yes |
| Comix Zone | 140% | 97% | 60% | 8 | no: up to ~5 frames behind in its heaviest scenes |
| Vectorman | 162% | 101% | 74% | 8 | almost: ~3 frames behind |

("60 Hz": the game keeps its real speed, never more than about 3 frames behind.)

## Installing

LeapLoader 0.2.4 and later come with both emulators: they are on LeapLoader's list already, so
**Build card** puts them on the card. Then copy your games into the card's `NES`, `GB` and `GBC`
folders (LeapLoader creates them).

With an older LeapLoader, or to try a build of your own: in LeapLoader, **Add a folder…** and pick
`package/NES`, `package/GB`, `package/GBA`, `package/SNES` or `package/GENESIS` (see *Building*). GBA, SNES
and Genesis aren't bundled with LeapLoader yet: add `package/GBA`, `package/SNES` or `package/GENESIS` that
way and copy your `.gba` games into the card's `GBA` folder, your `.sfc` / `.smc` games into its `SNES`
folder, your `.md` / `.bin` / `.gen` / `.smd` games into its `GENESIS` folder.

## Using them

Choose **NES**, **Game Boy**, **GBA**, **SNES** or **Genesis** in LeapLoader's menu on the console.

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

| Leapster | NES | Game Boy | GBA | SNES | Genesis |
|---|---|---|---|---|---|
| D-pad | D-pad | D-pad | D-pad | D-pad | D-pad |
| A | B | B | B | Y | C |
| B | A | A | A | B | B |
| PAUSE | Start | Start | Start | Start | Start |
| HINT | Select | Select | Select | A | A |
| HOME + B / HOME + A | | | L / R | Select / X | / A |
| HOME + Left / Right | | | | L / R | |
| Volume buttons | volume | volume | volume | volume | volume |

(Genesis: a 3-button pad; HOME + A gives Genesis A too, for games that need A with B or C.)

* **HINT + A + B**, held 2.5 s: back to the game list (a prompt shows a bar filling; let go to cancel).
* **HOME + PAUSE**, held 2 s: back to the game list too.
* Battery saves are written next to the game (`GAME.sav`; SNES and Genesis: `GAME.srm`) a few seconds
  after the game changes them, and when you leave it.

## How they fit the Leapster 2

* **Screen:** the Leapster 2's LCD is 160 x 160. NES pictures (256 x 240) are cropped to 240 x 224 and
  shrunk: every 3 pixels across become 2 (blended), every 7 lines become 5 (each line the average of
  two neighbours, so none is dropped). Game Boy pictures (160 x 144) fit as they are, pixel for pixel.
  GBA pictures (240 x 160) keep every line; every 3 pixels across become 2. SNES pictures (256 x 224)
  are sampled: 5 of every 8 columns, 160 of the lines. Genesis pictures (320 x 224) are sampled too:
  every other column, 160 of the lines.
* **Sound:** played through the Leapster 2's own PCM channel at 32 kHz from an interrupt handler;
  the BIOS's own menu sounds are mixed in, so the game lists sound like LeapLoader's menu.
* **Speed:** when a frame takes too long, the next ones are run without drawing (a few at most), so
  the game runs at its real speed.
* **GBA:** gpSP translates the game's ARM and Thumb code into ARC code as it runs (`gba/core/arc`). It
  keeps as much of the game in memory as fits (1 MB blocks) and reads the rest from the card in 32 KB
  pages. A line whose inputs (registers, palettes, video memory it reads) haven't changed since it
  was drawn is not drawn again. It uses the open-source GBA BIOS replacement that comes with gpSP
  (`gba/core/bios`), not Nintendo's.
* **SNES:** the 65816 and SPC700 cores and the renderer's inner loops are ARC assembly made by Python
  generators (`snes/kern`) from Snes9x's C, with the same cycle counts and results; the renderer draws
  only the 160 x 160 pixels shown, straight into the LCD. Super FX code is translated into ARC code
  as it runs (`snes/s9x/src/fxjit.c`). Games of up to 4 MB (32 Mbit).
* **Genesis:** the 68000 interpreter (`genesis/port/k68`) and the Z80 core are ARC assembly made by
  Python generators, with FAME's and Cz80's cycle counts and results; the YM2612 and PSG loops and
  the line renderer are hand-written ARC assembly; loops in which a game or its sound driver only
  waits are skipped as a whole when the result is the same. The code is laid out as measured on the
  model (the hottest parts together). Games of up to 4 MB (32 Mbit); Sega CD, 32X, SVP (Virtua
  Racing) and the Master System are not included.
* **Code:** each app is native ARC code (`code.bin`, linked at 0x3C81_0000), packaged with LeapLoader's
  homebrew format (`app.txt`, `code=`; see LeapLoader's `docs/homebrew.md`). LeapLoader wraps it into a
  Leapster 2 SD image when it builds the card. The BIOS owns the `gp` register, so everything is built
  with `-mno-sdata` and checked for `gp` use.

## Building

You need the **GNU toolchain for ARC** (`arc-elf32-gcc`, `-mcpu=arc600`), a POSIX shell (Linux,
macOS, WSL, or Git Bash on Windows) and, for SNES and Genesis, Python 3.

```sh
ARC=/path/to/arc-elf32-win/bin/arc-elf32- ./build.sh
```

`ARC` is the toolchain's prefix (default `arc-elf32-`), `PYTHON` the Python (default `python3`, else
`python`). The build writes `package/NES`, `package/GB`, `package/GBA`, `package/SNES` and `package/GENESIS` (each
`app.txt`, `code.bin`, `LICENSE.txt` and the `card/` folders to create) and keeps its objects in
`build/`. `VERSION` sets the packages' version.

`cc.sh` wraps the compiler: on the Leapster, the low word of a MUL64 product is read from MMID, not
MLO, so the generated assembly is fixed up before it is assembled.

What only showed on a real console (Ribbit runs both right):

* **No zero-overhead loops.** GCC's `lp` loops and newlib's/libgcc's hand-written ones broke the apps
  (names mixed, tiles scrambled, games crashed). `CF` has `-fno-branch-count-reg`, `common/llstring.h`
  and `common/lldiv.h` replace the library's string and division functions, and the build stops if
  anything linked still has an `lp`.
* **The picture goes to VRAM with `st.di`.** Drawn straight into VRAM through the data cache, it reached
  the LCD late or not at all; it is drawn in RAM and copied (`show()`). File loads keep clear of the
  cache too (`common/llcache.h`).

## Layout

```
build.sh, cc.sh, VERSION   the build
common/llmenu.h            the game list, search keyboard, touch, menu sounds and HINT + A + B prompt
common/llvol.h             the volume panel and level, shared with LeapLoader's menu
nes/core/                  QuickNES, with LeapLoader's changes (#ifdef LL)
nes/console/               the NES app: BIOS calls, LCD, buttons, sound, saves (console.cpp)
gb/core/                   gnuboy, with LeapLoader's changes (#ifdef LL)
gb/console/                the Game Boy app (console.c)
gba/core/                  gpSP, with LeapLoader's changes (#ifdef LL) and the ARC dynarec (arc/)
gba/console/               the GBA app (console.c)
snes/s9x/                  Snes9x 1.39 (snes9x2002), with LeapLoader's changes (#ifdef LL_*) and the
                           Super FX translator (src/fxjit.c); its unused ARM assembly is left out
snes/kern/                 the generators of the 65816, SPC700 and renderer assembly; code layout lists
snes/console/              the SNES app (console.c), soft-float and libc pieces without lp loops
snes/LICENSE.txt           the Snes9x licence
genesis/picodrive/         PicoDrive 2.04, unchanged: the files this build uses (and the generators read)
genesis/port/              LeapLoader's speed work: the 68000 (k68) and Z80 generators, the renderer
                           (arcdraw), the YM2612/PSG assembly (ym), PicoDrive files patched by generators
genesis/console/           the Genesis app (console.c), soft-float and libc pieces without lp loops,
                           the code layout (link.ld)
genesis/LICENSE.txt        PicoDrive's licence and its components'
```

## Licences

* `nes/core/`: QuickNES, GNU LGPL 2.1 or later (the file headers); the libretro repository ships the
  GNU GPL 2 text, `nes/LICENSE`. `emu2413` has its own permissive licence (its header).
* `gb/core/`: gnuboy, GNU GPL 2 (`gb/core/COPYING`).
* `gba/core/`: gpSP, GNU GPL 2 (`gba/core/COPYING`), including the ARC dynarec; the BIOS replacement in
  `gba/core/bios` is the open GBA BIOS by Normmatt and the VBA/VBA-M team, GPL 2 (see its README).
* `snes/`: Snes9x, under the **Snes9x licence: non-commercial use only** (`snes/LICENSE.txt`; the
  file headers). It covers everything in `snes/` that was made for it (the SNES app, the generators,
  the Super FX translator). The libretro-common files in `snes/s9x/libretro/libretro-common` keep
  their own licence (their headers).
* `genesis/`: PicoDrive, under **PicoDrive's licence (the MAME licence): non-commercial use only**
  (`genesis/LICENSE.txt`, `genesis/picodrive/COPYING`). It covers everything in `genesis/` made for it
  (the Genesis app, the generators, the assembly cores). Its components keep their own terms, also
  non-commercial: the FAME 68000 library by Oscar Orallo (`cpu/fame/fame.html`; credit: "FAME Motorola
  68000 Emulation Library by Oscar Orallo"), Stephane Dallongeville's CZ80 (`cpu/cz80/readme.txt`),
  and MAME's YM2612 (fm.c, Tatsuyuki Satoh, Jarek Burczynski) and SN76496 code (the MAME licence).
* Everything else (the other console apps, `common/`, the build scripts): zlib licence (`LICENSE`).

The packages are built from these sources, so each is distributed under its core's licence: the
Game Boy and GBA apps under the GPL 2, the NES app under the GPL 2 (the LGPL allows it), the SNES app
under the Snes9x licence (non-commercial), the Genesis app under PicoDrive's licence (non-commercial).
Changes made to the cores for the Leapster are marked `#ifdef LL` (`LL_*` in Snes9x); PicoDrive's
files are unchanged, its replaced or patched parts are in `genesis/port`.

LeapFrog and Leapster are trademarks of LeapFrog Enterprises. This project is not affiliated with
LeapFrog, Nintendo, Sega or the authors of the emulator cores.
