#!/bin/sh
# build.sh: builds the NES, Game Boy, GBA, SNES and Genesis apps into package/NES, package/GB,
# package/GBA, package/SNES and package/GENESIS, homebrew packages for LeapLoader (app.txt + code.bin
# + card/ folders).
#
# Needs ARC GCC (the GNU toolchain for ARC, arc-elf32, -mcpu=arc600), a POSIX shell and, for
# SNES and Genesis, Python 3 (it generates parts of their cores, in assembly and in C). ARC=<prefix> picks the
# toolchain (default: arc-elf32-), PYTHON the Python (default: python3, else python).
#   ARC=/opt/arc/bin/arc-elf32- ./build.sh
set -e
R=$(cd "$(dirname "$0")" && pwd)
ARC=${ARC:-arc-elf32-}
export ARC
CC="$R/cc.sh"
# -fno-branch-count-reg: no zero-overhead (LP) loops (see link: they broke the apps on a real console)
CF="-mcpu=arc600 -fno-branch-count-reg -mmul64 -mbarrel-shifter -mnorm -mswap -mno-sdata -O2 -fomit-frame-pointer -DLL -w"
LINK="-mcpu=arc600 -mmul64 -mbarrel-shifter -mnorm -mswap -nostartfiles"

# link NAME DIR OBJS...: DIR/console's link.ld -> build/NAME.elf -> build/NAME/code.bin
link() {
  name=$1; dir=$2; shift 2
  "${ARC}gcc" $LINK -T "$R/$dir/console/link.ld" -o "$R/build/$name.elf" "$@" \
    -lm -lc -lgcc -Wl,--gc-sections -Wl,-Map="$R/build/$name.map"
  check "$name"
}

# check NAME: build/NAME.elf must use neither gp nor zero-overhead loops -> build/NAME.bin
check() {
  name=$1
  # gp belongs to the BIOS (it reaches every BIOS service): nothing linked may use it
  "${ARC}objdump" -d "$R/build/$name.elf" | awk '/^[0-9a-f]+ <.*>:$/{fn=$2}
    /[^a-z_](gp|r26)([^a-z0-9]|$)/ && !/mov(_s)?[ \t]+r[0-9]+,gp[ \t]*$/{print fn, $0}' > "$R/build/$name.gp"
  if [ -s "$R/build/$name.gp" ]; then echo "$name: gp used (see build/$name.gp)"; exit 1; fi
  # no zero-overhead loops: on a real Leapster 2 GCC's and newlib's/libgcc's broke the apps (fine in
  # Ribbit); CF has -fno-branch-count-reg, common/llstring.h and lldiv.h replace the library ones
  "${ARC}objdump" -d "$R/build/$name.elf" | awk '/^[0-9a-f]+ <.*>:$/{fn=$2}
    /\tlp(ne|eq|nz|z|hs|lo)?\t|lp_count/{print fn, $0}' > "$R/build/$name.lp"
  if [ -s "$R/build/$name.lp" ]; then echo "$name: zero-overhead loops (see build/$name.lp)"; exit 1; fi
  "${ARC}objcopy" -O binary -j .text -j .rodata -j .data "$R/build/$name.elf" "$R/build/$name.bin"
}

# console DIR OBJDIR SRC [flags]: the console side (entry, libc stand-ins, the app)
console() {
  dir=$1; o=$2; src=$3; shift 3
  "$CC" "$R/$dir/console/$src" "$o/console.o" $CF "$@"
  "$CC" "$R/$dir/console/stubs.c" "$o/stubs.o" $CF -fno-builtin
  "$CC" "$R/$dir/console/libc.c" "$o/libc.o" $CF -fno-builtin
  "${ARC}gcc" -mcpu=arc600 -c "$R/$dir/console/entry.S" -o "$o/entry.o"
}

# ---- NES: QuickNES (LGPL-2.1)
o=$R/build/nes; rm -rf "$o"; mkdir -p "$o"
NF="-fno-exceptions -fno-rtti -DNO_UNALIGNED_ACCESS -DNDEBUG -I$R/nes/core"
for f in abstract_file apu_state Blip_Buffer Effects_Buffer Multi_Buffer Nes_Apu Nes_Buffer Nes_Cart Nes_Core \
         Nes_Cpu nes_data Nes_Effects_Buffer Nes_Emu Nes_File Nes_Fme7_Apu Nes_Mapper Nes_Vrc7 emu2413 \
         emu2413_state Nes_Namco_Apu Nes_Oscs Nes_Ppu Nes_Ppu_Impl Nes_Ppu_Rendering Nes_State nes_util \
         Nes_Vrc6_Apu Data_Reader; do
  "$CC" "$R/nes/core/$f.cpp" "$o/core_$f.o" $CF $NF
done
console nes "$o" console.cpp $NF -fno-threadsafe-statics
link nes nes "$o/entry.o" "$o/console.o" "$o/stubs.o" "$o/libc.o" "$o"/core_*.o

# ---- Game Boy / Color: gnuboy (GPL-2)
o=$R/build/gb; rm -rf "$o"; mkdir -p "$o"
GF="-DIS_LITTLE_ENDIAN -I$R/gb/core"
for f in lcd refresh lcdc palette cpu mem rtc hw sound loader save emu events keytable exports rccmds rckeys \
         rcvars rcfile split path inflate miniz_tinfl; do
  "$CC" "$R/gb/core/$f.c" "$o/core_$f.o" $CF $GF
done
console gb "$o" console.c $GF
link gb gb "$o/entry.o" "$o/console.o" "$o/stubs.o" "$o/libc.o" "$o"/core_*.o

# ---- Game Boy Advance: gpSP (libretro's gpsp, GPL-2) with an ARC600 dynarec (gba/core/arc), #ifdef LL
o=$R/build/gba; rm -rf "$o"; mkdir -p "$o"
AF="-DNDEBUG -DHAVE_DYNAREC -DARC_ARCH -DLL_ROM_CACHE_KB=1024 -DSOUND_BUFFER_BITS=12 -DLL_MIX_SHIFT=1 -DLL_FOLD_HBLANK
    -DARC_DC_FLUSH_ALL -DLL_VSKIP -ffunction-sections -I$R/gba/core -I$R/gba/core/libretro/libretro-common/include
    -I$R/gba/core/libretro -include retro_inline.h"
for f in main gba_memory savestate input sound cheats memmap serial gbp rfu serial_proto gba_cc_lut cpu_threaded; do
  "$CC" "$R/gba/core/$f.c" "$o/core_$f.o" $CF $AF &
done
for f in video cpu; do
  "$CC" "$R/gba/core/$f.cc" "$o/core_$f.o" $CF $AF -fno-exceptions -fno-rtti &
done
wait
"${ARC}gcc" -mcpu=arc600 -mmul64 $CF $AF -c "$R/gba/core/arc/arc_stub.S" -o "$o/core_arc_stub.o"
"${ARC}gcc" -mcpu=arc600 -I"$R/gba/core" -c "$R/gba/core/bios_data.S" -o "$o/core_bios.o"  # the open GBA BIOS (gba/core/bios)
console gba "$o" console.c $AF
link gba gba "$o/entry.o" "$o/console.o" "$o/stubs.o" "$o/libc.o" "$o"/core_*.o

# ---- SNES: Snes9x 1.39 (libretro's snes9x2002; the Snes9x licence: non-commercial use only) with
# LeapLoader's speed work (#ifdef LL_*): 65816 and SPC700 cores in assembly and the new renderer's
# kernels, generated by snes/kern/*.py, and a Super FX translator (snes/s9x/src/fxjit.c). Experimental.
PY=${PYTHON:-$(command -v python3 >/dev/null 2>&1 && echo python3 || echo python)}
S=$R/snes; o=$R/build/snes; rm -rf "$o"; mkdir -p "$o"
SF="-mcpu=arc600 -mmul64 -mbarrel-shifter -mnorm -mswap -mno-sdata -O2 -fomit-frame-pointer -fno-branch-count-reg
    -fsigned-char -DUSE_SA1 -DRIGHTSHIFT_IS_SAR -D__OLD_RASTER_FX__ -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H
    -D__LIBRETRO__ -I$S/s9x -I$S/s9x/libretro -I$S/s9x/libretro/libretro-common/include -I$S/s9x/src -I$o -w
    -ffunction-sections -DLL_CONSOLE"
# the speed options (all exact: the same emulation as Snes9x's C code, frame by frame)
SF="$SF -DLL_SPEEDHACKS -DSPC700_SHUTDOWN -DLL_APUBATCH -DLL_SLEEP -DLL_CORE2 -DLL_APUASM -DLLR_K2 -DLLR_K3
    -DLLR_OBJASM -DLL_LINELOG -DLL_DMAFAST -DLL_MAPPAD -DLL_STRIPS=8 -DLL_FASTTILE -DLL_LCD -DLL_MEMSET32 -DLL_LCD12
    -DLL_BACKLINE -DLL_HIDESKIP -DLL_NIB -DLL_PALIDX -DLL_MIX -DLL_SLEEP3 -DLL_M7SHOWN -DLL_NEWR -DLLR_FIXSWAP
    -DLLR_OUT2 -DLLR_OUTZ -DLLR_M7 -DLL_VRAMW -DLLR_NIBONLY -DLLR_M7ASM -DLL_DMA2180 -DLL_DMAFILL -DLL_ECHOHI
    -DLL_SPCASM -DLL_PORTFAST -DLLR_M7B -DLLR_OBJCH -DLL_VRAMB -DLL_BRLAZY -DLLR_M7OUT -DLL_HDMAFAST -DLL_SPCDEFER
    -DLLR_T16 -DLL_SPCMULTI -DLLR_SRCW -DLLR_M7FILL -DLLR_M7OUT2 -DLL_SPCDISP2 -DLL_SPCSHORT0 -DLL_OSC -DLL_ECHOLOC
    -DLL_OSCHOT -DLL_FASTFP -DLL_IRQSKIP -DLL_IRQFAST -DLL_FXJIT -DLLR_MODE2"
SA="-mcpu=arc600 -mmul64 -mbarrel-shifter -mnorm -mswap -I$o -I$S/s9x/src"
# the generated parts: structure offsets for the assembly, the 65816 core (hot handlers grouped),
# the renderer's kernels, the SPC700 core, the Super FX translator's opcode table
"${ARC}gcc" $SF -S "$S/kern/offs65.c" -o - | grep '\.equ' > "$o/lloffs65.inc"
LLOFFS65="$o/lloffs65.inc" G65_IRQSKIP=1 G65_IRQFAST=1 "$PY" -I "$S/kern/gen65.py" "$o/ll65.S" > /dev/null
"$PY" -I "$S/kern/hotsec.py" "$o/ll65.S" "$S/kern/ll65hot.txt" > /dev/null
"$PY" -I "$S/kern/genllr.py" "$o/llrk.S" > /dev/null
SPCSHORT=0 "$PY" -I "$S/kern/genspc.py" "$o/llspc.S" > /dev/null
"$PY" -I "$S/kern/genfxtab.py" "$S/s9x/src/fxinst.c" "$o/fxjit_tab.h" > /dev/null
"${ARC}gcc" $SA -DLL_VRAMW -DLL_VRAMB -DLL_SPCDEFER -c "$o/ll65.S" -o "$o/core_ll65.o"
"${ARC}gcc" $SA -c "$o/llrk.S" -o "$o/core_llrk.o"
"${ARC}gcc" $SA -DLL_SPCDISP2 -c "$o/llspc.S" -o "$o/core_llapu.o"
# -DLL_OSC: the per-line/per-frame C outside the renderer at -Os (a smaller i-cache footprint)
for f in src/apu src/apuaux src/c4 src/c4emu src/cheats src/cheats2 src/clip src/data src/dsp1 src/fxemu src/fxinst \
         src/globals src/dma src/memmap src/cpu src/cpuexec src/cpuops src/sa1 src/sa1cpu src/sdd1 src/sdd1emu \
         src/snapshot src/soundux src/spc700 src/srtc src/ppu_ src/gfx src/tile libretro/libretro \
         libretro/libretro-common/streams/memory_stream; do
  b=${f##*/}; F=$SF
  case $b in cpuexec|dma|ppu_|apu|dsp1|cpuops|soundux) F=$(echo "$SF" | sed 's/-O2/-Os/');; esac
  "$CC" "$S/s9x/$f.c" "$o/core_$b.o" $F &
done
wait
# -DLL_FASTFP: single-precision soft float (and __divdf3) without libgcc's slow fp-bit and its lp
# loops; libgcc's fp-bit.o/dp-bit.o give the rest, with their own definitions made weak
"$CC" "$S/console/fastfp.c" "$o/fastfp.o" $SF
LIBGCC=$("${ARC}gcc" -mcpu=arc600 -mmul64 -mbarrel-shifter -mnorm -mswap -print-libgcc-file-name)
for m in fp dp; do
  (cd "$o" && "${ARC}ar" x "$LIBGCC" $m-bit.o &&
   "${ARC}objcopy" $("${ARC}nm" -g --defined-only $m-bit.o | awk '{printf " --weaken-symbol=%s", $3}') $m-bit.o ${m}bitw.o &&
   rm -f $m-bit.o)
done
"$CC" "$S/console/console.c" "$o/console.o" $SF
"$CC" "$S/console/libc.c" "$o/libc.o" $SF -fno-builtin
"$CC" "$S/console/llfloat.c" "$o/llfloat.o" $SF -fno-builtin
"${ARC}gcc" -mcpu=arc600 -c "$S/console/entry.S" -o "$o/entry.o"
# the bench's code layout (kern/order.txt: hot code first, as measured)
"${ARC}gcc" $LINK -T "$S/console/link.ld" -Wl,--section-ordering-file="$S/kern/order.txt" -o "$R/build/snes.elf" \
  "$o/entry.o" "$o/console.o" "$o/libc.o" "$o/llfloat.o" "$o/fastfp.o" "$o/fpbitw.o" "$o/dpbitw.o" "$o"/core_*.o \
  -lm -lc -lgcc -Wl,--gc-sections -Wl,-Map="$R/build/snes.map"
check snes

# ---- Genesis / Mega Drive: PicoDrive 2.04 (the MAME licence: non-commercial use only) with LeapLoader's
# speed work (genesis/port): a 68000 interpreter (k68) and a Z80 core in assembly, an assembly line
# renderer writing the LCD, the YM2612/PSG loops in assembly, wait-loop skipping. All exact: the same
# emulation as PicoDrive's C code, frame by frame. Experimental.
G=$R/genesis; o=$R/build/genesis; rm -rf "$o"; mkdir -p "$o/core"
cp -r "$G/picodrive" "$o/picodrive"; cp -r "$G/port" "$o/port"
# the generated parts (in build/genesis/port): the 68000 interpreter (hot handlers first), the patched
# YM2612/PSG, Cz80 with its idle-loop skips, the Z80 core in assembly, the VDP port with the renderer's
# change stamps
(cd "$o/port" &&
 K68HOT=1 K68HOTF=hot5.txt K68SPLIT=100 K68POLL=1 K68IDLE2=1 K68TAIL=40 "$PY" -I k68/gen68.py k68/k68i.S > /dev/null &&
 "$PY" -I ym/mkym.py && "$PY" -I z80/mkz80.py && "$PY" -I z80/mkz80x.py > /dev/null && "$PY" -I vp/mkvp.py)
GD="-DEMU_F68K -D_USE_CZ80 -DNO_32X -DNO_SMS -I../picodrive -I../picodrive/pico -I../picodrive/pico/sound
    -I../picodrive/zlib -I. -include port.h -w"
GF="-mcpu=arc600 -mmul64 -mbarrel-shifter -mnorm -mswap -mno-sdata -O2 -fomit-frame-pointer -fno-branch-count-reg
    -fno-common -fsigned-char -ffunction-sections -Wno-error=implicit-function-declaration -Wno-error=int-conversion
    -Wno-error=incompatible-pointer-types $GD -DDIRECT -DARCDRAW -DK68"
# the speed options (all exact; as the bench's best build, genesis/port and its NOTES: t6)
GF="$GF -DYMX -DYMLOOP -DYMASM -DSNX -DZ80IDLE -DK68HOT -DOUTQ -DSNQ -DZ80POLL -DK68TAIL -DZ80ASM -DYMEG3 -DYMSCHED
    -DSNASM -DLREUSE -DYMSETE -DZ80EI -DPLBW -DPSPR -DLRK2 -DPSPR2 -DVDPFAST -DK68IDLE2 -DPLPIPE -DK68POLL -DK68HOT5
    -DFIXLD -DSHFAST -DK68IDLEJ -DNTMEMO -DZ80IDLE3 -DZ80DIA -DZ80IDLE4 -DFSKIP=2"
(cd "$o/port" &&
 for f in pico/pico pico/cart pico/memory pico/sek pico/z80if pico/misc pico/eeprom pico/carthw/carthw \
          pico/carthw/eeprom_spi pico/sound/sound pico/sound/mix; do
   "$CC" "../picodrive/$f.c" "$o/core/${f##*/}.o" $GF &
 done
 for f in vp/videoport arcdraw/drawx ym/ym_patched ym/sn_patched z80/cz80 k68/k68c z80/ymwait stubs; do
   "$CC" "$f.c" "$o/core/${f##*/}.o" $GF &
 done
 for f in ym/ymx z80/z80x ym/sn k68/k68i arcdraw/arcdraw arcdraw/shfast k68/k68ij; do
   "${ARC}gcc" $GF -c "$f.S" -o "$o/core/${f##*/}.o" &
 done
 wait
 "$CC" "$G/console/console.c" "$o/console.o" $GF
 "$CC" "$G/console/libc.c" "$o/libc.o" $GF -fno-builtin
 "$CC" "$G/console/llfloat.c" "$o/llfloat.o" $GF -fno-builtin)
# libgcc's fp-bit.o/dp-bit.o with their symbols weakened: llfloat.c's divisions (no lp loops) replace theirs
for m in fp dp; do
  (cd "$o" && "${ARC}ar" x "$LIBGCC" $m-bit.o &&
   "${ARC}objcopy" $("${ARC}nm" -g --defined-only $m-bit.o | awk '{printf " --weaken-symbol=%s", $3}') $m-bit.o ${m}bitw.o &&
   rm -f $m-bit.o)
done
"${ARC}gcc" -mcpu=arc600 -c "$G/console/entry.S" -o "$o/entry.o"
"${ARC}gcc" $LINK -T "$G/console/link.ld" -o "$R/build/genesis.elf" "$o/entry.o" "$o/console.o" "$o/libc.o" \
  "$o/llfloat.o" "$o/fpbitw.o" "$o/dpbitw.o" "$o"/core/*.o \
  -lm -lc -lgcc -Wl,--gc-sections -Wl,-Map="$R/build/genesis.map"
check genesis

# ---- the packages
VERSION=$(cat "$R/VERSION")
p=$R/package/NES; rm -rf "$p"; mkdir -p "$p/card/NES"
cp "$R/build/nes.bin" "$p/code.bin"
cp "$R/nes/LICENSE" "$p/LICENSE.txt"
printf 'name=NES\nid=NES\ncode=code.bin\nauthor=LeapLoader-Emulators (QuickNES, LGPL-2.1)\nversion=%s\n' "$VERSION" > "$p/app.txt"
printf 'Put NES games (.nes) here.\n' > "$p/card/NES/Put NES games here.txt"

p=$R/package/GB; rm -rf "$p"; mkdir -p "$p/card/GB" "$p/card/GBC"
cp "$R/build/gb.bin" "$p/code.bin"
cp "$R/gb/core/COPYING" "$p/LICENSE.txt"
printf 'name=Game Boy\nid=GB\ncode=code.bin\nauthor=LeapLoader-Emulators (gnuboy, GPL-2)\nversion=%s\n' "$VERSION" > "$p/app.txt"
printf 'Put Game Boy games (.gb) here.\n' > "$p/card/GB/Put GB games here.txt"
printf 'Put Game Boy Color games (.gbc) here.\n' > "$p/card/GBC/Put GBC games here.txt"

p=$R/package/GBA; rm -rf "$p"; mkdir -p "$p/card/GBA"
cp "$R/build/gba.bin" "$p/code.bin"
cp "$R/gba/core/COPYING" "$p/LICENSE.txt"
printf 'name=GBA
id=GBA
code=code.bin
author=LeapLoader-Emulators (gpSP, GPL-2) - experimental
version=%s
' "$VERSION" > "$p/app.txt"
printf 'Put Game Boy Advance games (.gba) here.
' > "$p/card/GBA/Put GBA games here.txt"

p=$R/package/SNES; rm -rf "$p"; mkdir -p "$p/card/SNES"
cp "$R/build/snes.bin" "$p/code.bin"
cp "$R/snes/LICENSE.txt" "$p/LICENSE.txt"
printf 'name=SNES
id=SNES
code=code.bin
author=LeapLoader-Emulators (Snes9x, non-commercial) - experimental
version=%s
' "$VERSION" > "$p/app.txt"
printf 'Put Super Nintendo games (.sfc, .smc) here.
' > "$p/card/SNES/Put SNES games here.txt"

p=$R/package/GENESIS; rm -rf "$p"; mkdir -p "$p/card/GENESIS"
cp "$R/build/genesis.bin" "$p/code.bin"
cp "$R/genesis/LICENSE.txt" "$p/LICENSE.txt"
printf 'name=Genesis\nid=GENESIS\ncode=code.bin\nauthor=LeapLoader-Emulators (PicoDrive, non-commercial) - experimental\nversion=%s\n' "$VERSION" > "$p/app.txt"
printf 'Put Genesis / Mega Drive games (.md, .bin, .gen, .smd) here.\n' > "$p/card/GENESIS/Put Genesis games here.txt"

ls -l "$R/package/NES/code.bin" "$R/package/GB/code.bin" "$R/package/GBA/code.bin" "$R/package/SNES/code.bin" "$R/package/GENESIS/code.bin" | awk '{print $NF, $5}'
