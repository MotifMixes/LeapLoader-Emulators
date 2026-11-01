/* The data cache around the BIOS's file service. Included by a console after it defines u32.

   On the real console (2026-11-01) ROMs loaded with b_fread came out wrong in 16-byte pieces (one
   cache line): NES tiles scrambled, then the game crashed to a black screen. The file service
   puts the data in RAM past the data cache (32 KiB, 4 ways, 16-byte lines, write-back; Ribbit's
   hardware-research.md), so lines of the buffer the cache already held (the menu's picture was
   drawn in the same heap) were read instead of the file, or written back over it. Ribbit doesn't
   model the caches, so this only showed on the console. The cache's own line operations aren't
   used: the NES/GB ports' dc_fldl write-back did nothing on the console.

   ll_cache_clean: before the file service reads or writes a buffer, push every dirty line out by
   loading 512 KiB of other memory (16 loads per set of 4 ways, whatever the replacement order).
   ll_cache_reload: after it filled one, load every word past the cache (ld.di) and store it
   through the cache, so no line still holds what was there before. */
#define LL_CLEAN_FROM 0x3C800000u  /* the load area's first 512 KiB: the image, our code, .bss */
#define LL_CLEAN_SIZE 0x80000u

static void ll_cache_clean(void)
{
    const volatile u32 *p = (const volatile u32 *)LL_CLEAN_FROM, *e = p + LL_CLEAN_SIZE / 4;
    u32 x = 0;
    for (; p < e; p += 4) x += *p;  /* one load per line */
    (void)x;
}

static void ll_cache_reload(unsigned char *b, int n)
{
    unsigned char *e = b + n;
    u32 v;
    for (; b < e && ((u32)b & 3); b++) {
        __asm__ volatile("ldb.di %0, [%1]" : "=r"(v) : "r"(b));
        *b = (unsigned char)v;
    }
    for (; b + 4 <= e; b += 4) {
        __asm__ volatile("ld.di %0, [%1]" : "=r"(v) : "r"(b));
        *(u32 *)b = v;
    }
    for (; b < e; b++) {
        __asm__ volatile("ldb.di %0, [%1]" : "=r"(v) : "r"(b));
        *b = (unsigned char)v;
    }
}
