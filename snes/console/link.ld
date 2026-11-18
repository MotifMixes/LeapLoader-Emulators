/* The SNES app's native code: in game.bin at CODE_OFFSET, so at
   0x3C80_0000 + CODE_OFFSET once the BIOS has loaded the image.
   .bss and the heap follow the file's end; the stack is the top 64 KiB of
   the load area (RAM ends at 0x3D00_0000). .rodata and .data start on 16 KiB and .bss.z* goes
   last, as in the bench's layout (link-al.ld: a few bytes moved elsewhere changed the cache's
   behaviour by 1-2%). */
ENTRY(_start)
SECTIONS
{
    . = 0x3C810000;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(0x4000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x4000);
    .data : { *(.data .data.*) *(.sdata .sdata.*) . = ALIGN(16); }
    __file_end = .;
    .bss (NOLOAD) : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.[!z]* COMMON) *(.bss.z*) . = ALIGN(16); __bss_end = .; }
    end = .;
    __heap_end = 0x3CFF0000;
    __stack_top = 0x3CFFFF00;
    /DISCARD/ : { *(.comment) *(.note*) }
}
