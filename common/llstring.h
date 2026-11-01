/* String and memory functions in C, included by each console's libc.c, so that newlib's
   hand-written ARC versions aren't linked.

   On the real console (2026-11-01) the ROM list's names came out mixed ("Basebal" + "ros":
   llmenu's llm_name took them with strncpy into a buffer reused per row, and the copy stopped
   early) while a memory check found memory, the cache, MUL64 and strcpy all fine. newlib's
   arc600 libc.a has an ARC600 version of strcpy only (strcpy-bs-arc600); strncpy, memcpy,
   memset and strchr are its -bs versions for later cores, which set LP_COUNT in the
   instruction before the zero-overhead loop (strncpy: lsr.f lp_count; lpne). Ribbit doesn't
   model the pipeline, so they worked there. GCC's own loops keep to the ARC600's rules. */

/* (GCC turns copy and fill loops back into memcpy/memset calls without this) */
#define LL_STR __attribute__((optimize("no-tree-loop-distribute-patterns")))

LL_STR void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (!(((size_t)d | (size_t)s) & 3)) {
        for (; n >= 16; n -= 16, d += 16, s += 16) {
            unsigned *dw = (unsigned *)d;
            const unsigned *sw = (const unsigned *)s;
            unsigned a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a, dw[1] = b, dw[2] = c, dw[3] = e;
        }
        for (; n >= 4; n -= 4, d += 4, s += 4) *(unsigned *)d = *(const unsigned *)s;
    }
    while (n--) *d++ = *s++;
    return dst;
}

LL_STR void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d <= s || d >= s + n) return memcpy(dst, src, n);
    while (n--) d[n] = s[n];
    return dst;
}

LL_STR void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned w = (unsigned char)c * 0x01010101u;
    for (; n && ((size_t)d & 3); n--) *d++ = (unsigned char)c;
    for (; n >= 16; n -= 16, d += 16) {
        unsigned *dw = (unsigned *)d;
        dw[0] = w, dw[1] = w, dw[2] = w, dw[3] = w;
    }
    for (; n >= 4; n -= 4, d += 4) *(unsigned *)d = w;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

LL_STR int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

LL_STR size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return p - s;
}

LL_STR char *stpcpy(char *d, const char *s)
{
    while ((*d = *s++)) d++;
    return d;
}

LL_STR char *strcpy(char *d, const char *s)
{
    stpcpy(d, s);
    return d;
}

LL_STR char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

LL_STR char *strcat(char *d, const char *s)
{
    stpcpy(d + strlen(d), s);
    return d;
}

LL_STR int strcmp(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++) {}
    return (unsigned char)*a - (unsigned char)*b;
}

LL_STR int strncmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && *a == *b; n--, a++, b++) {}
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

LL_STR char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

LL_STR char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

LL_STR char *strpbrk(const char *s, const char *set)
{
    for (; *s; s++)
        if (strchr(set, *s)) return (char *)s;
    return 0;
}
