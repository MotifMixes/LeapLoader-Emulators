#!/bin/sh
# build.sh: builds the NES, Game Boy and GBA apps into package/NES, package/GB and package/GBA,
# homebrew packages for LeapLoader (app.txt + code.bin + card/ folders).
#
# Needs ARC GCC (the GNU toolchain for ARC, arc-elf32, -mcpu=arc600) and a
# POSIX shell; ARC=<prefix> picks the toolchain (default: arc-elf32-).
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
    -DARC_DC_FLUSH_ALL -ffunction-sections -I$R/gba/core -I$R/gba/core/libretro/libretro-common/include
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

ls -l "$R/package/NES/code.bin" "$R/package/GB/code.bin" "$R/package/GBA/code.bin" | awk '{print $NF, $5}'
