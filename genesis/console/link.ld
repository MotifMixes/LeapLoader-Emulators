/* The Genesis app's native code: in game.bin at CODE_OFFSET, so at 0x3C80_0000 + CODE_OFFSET once
   the BIOS has loaded the image. .text keeps the bench's order (the hottest functions first, then the
   assembly cores, then the rest; the code that is not run every frame (.xtext) last), as measured: the
   i-cache's behaviour depends on it. .bss and the heap (the ROM buffer, PicoDrive's allocations)
   follow the file's end; the stack is the top 64 KiB of the load area (RAM ends at 0x3D00_0000). */
ENTRY(_start)
SECTIONS
{
    . = 0x3C810000;
    .text : {
        *(.text.start)
        . = ALIGN(0x2000) + 0x18;  /* the bench's i-cache sets (its start-up code is 0x18 bytes) */
        *arcdraw.o(.text) *ymx.o(.text) *sn.o(.text)
        *(.text.PicoVideoRead8CtlL)
        *(.text.scan_begin)
        *(.text.SekSyncM68k.constprop.0.isra.0)
        *(.text.do_timing_hacks_start.constprop.0)
        *(.text.mix_32_to_16_mono)
        *(.text.plane_asm)
        *(.text.PicoVideoFIFOSync)
        *(.text.do_timing_hacks_end.constprop.0)
        *(.text.PicoVideoFIFOHint)
        *(.text.pal_update)
        *(.text.PicoRead8_vdp)
        *(.text.fm68k_emulate)
        *(.text.spr_one)
        *(.text.ParseSprites)
        *(.text.PicoDoHighPal555)
        *(.text.z80_md_ym2612_write)
        *(.text.OP_0x6601_idle)
        *(.text.spr_shi)
        *(.text.PicoDrawSync)
        *(.text.ym2612_write_local.isra.0)
        *(.text.PsndDoDAC)
        *(.text.PicoSyncZ80)
        *(.text.PicoVideoFIFOMode)
        *(.text.ym2612_read_local_z80)
        *(.text.PicoWrite16_vdp)
        . = . + ((0x1EBC - .) & 0x1FFF);  /* k68i at the bench's i-cache sets (its offset mod 8 KB) */
        *k68i.o(.text)
        *z80x.o(.text)
        *(.text .text.*)
        *(.xtext .xtext.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(16); }
    __file_end = .;
    .bss (NOLOAD) : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.* COMMON) . = ALIGN(16); __bss_end = .; }
    end = .;
    __heap_end = 0x3CFF0000;
    __stack_top = 0x3CFFFF00;
    /DISCARD/ : { *(.comment) *(.note*) }
}
