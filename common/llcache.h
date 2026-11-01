/* The data cache around the BIOS's file service. Included by a console after it defines u32.

   ll_cache_clean, before the file service reads or writes a buffer: every dirty line is pushed out
   by loading 512 KiB of other memory (16 loads per set of 4 ways, whatever the replacement order),
   so no line of the buffer can be written back over what the service puts in RAM, and the service
   reads what the app wrote. The cache's own line operations aren't used: the ports' dc_fldl
   write-back did nothing on the console (32 KiB, 4 ways, 16-byte lines, write-back; Ribbit's
   hardware-research.md).

   Not done any more (2026-11-01): reloading the buffer past the cache after a read (ld.di, then a
   store through the cache). That assumed the service writes RAM past the cache; when it copies
   through the cache instead, the newest ~32 KiB of the file were still in dirty lines, and the
   reload put RAM's old contents over them: 32 KiB Game Boy ROMs (Tetris) came up white or as junk
   tiles on the console. Ribbit doesn't model the caches. */
#define LL_CLEAN_FROM 0x3C800000u  /* the load area's first 512 KiB: the image, our code, .bss */
#define LL_CLEAN_SIZE 0x80000u

static void ll_cache_clean(void)
{
    const volatile u32 *p = (const volatile u32 *)LL_CLEAN_FROM, *e = p + LL_CLEAN_SIZE / 4;
    u32 x = 0;
    for (; p < e; p += 4) x += *p;  /* one load per line */
    (void)x;
}
