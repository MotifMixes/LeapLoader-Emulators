/* The SNES app's native code: in game.bin at CODE_OFFSET, so at
   0x3C80_0000 + CODE_OFFSET once the BIOS has loaded the image.
   .bss and the heap follow the file's end; the stack is the top 64 KiB of
   the load area (RAM ends at 0x3D00_0000). */
ENTRY(_start)
SECTIONS
{
    . = 0x3C810000;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) . = ALIGN(4); __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array .ctors)) __init_array_end = .; }
    .data : { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(16); }
    __file_end = .;
    .bss (NOLOAD) : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.* COMMON) . = ALIGN(16); __bss_end = .; }
    end = .;
    __heap_end = 0x3CFF0000;
    __stack_top = 0x3CFFFF00;
    /DISCARD/ : { *(.comment) *(.note*) }
}
