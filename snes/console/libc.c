/* The parts of the C library the console build needs that newlib would bring
   with gp-relative small data (malloc, stdio, errno, ...): on the console gp
   is the BIOS's. String and math functions still come from newlib (they use
   no gp; build.sh checks the linked program for any gp access). */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "../../common/llstring.h"  /* (not newlib's ARC string functions: lp loops; see there) */
#include "../../common/lldiv.h"    /* (not libgcc's ARC division: lp loop; see there) */

/* ---- memory: a bump allocator over .bss end .. __heap_end. free() takes
   back only the newest block (snes9x frees its temporary buffers so). */
extern char end[], __heap_end[];
static char *brk_ = end;
static char *last_;

void *malloc(size_t n)
{
    char *p = (char *)(((unsigned)brk_ + 15) & ~15u);
    n = (n + 15) & ~15u;
    if (p + 16 + n > __heap_end) return 0;
    *(size_t *)p = n;
    last_ = p;
    brk_ = p + 16 + n;
    return p + 16;
}

void free(void *q)
{
    if (q && (char *)q - 16 == last_) {
        brk_ = last_;
        last_ = 0;
    }
}

void *calloc(size_t a, size_t b)
{
    void *p = malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

void *realloc(void *q, size_t n)
{
    void *p;
    if (!q) return malloc(n);
    if (*(size_t *)((char *)q - 16) >= n) return q;
    p = malloc(n);
    if (p) memcpy(p, q, *(size_t *)((char *)q - 16));
    return p;
}

size_t ll_heap_free(void) { return __heap_end - brk_; }

/* ---- formatted text: %d %i %u %x %X %c %s %p, flags 0 and -, width */
int vsnprintf(char *buf, size_t size, const char *f, va_list ap)
{
    size_t o = 0;
#define PUT(c) do { if (o + 1 < size) buf[o] = (c); o++; } while (0)
    for (; *f; f++) {
        char tmp[16], *s, pad = ' ';
        int width = 0, left = 0, len, i;
        unsigned v, base = 10;
        if (*f != '%') { PUT(*f); continue; }
        f++;
        for (; *f == '-' || *f == '0'; f++) {
            if (*f == '-') left = 1;
            else pad = '0';
        }
        for (; *f >= '0' && *f <= '9'; f++) width = width * 10 + *f - '0';
        while (*f == 'l' || *f == 'h' || *f == 'z') f++;
        switch (*f) {
        case 's':
            s = va_arg(ap, char *);
            if (!s) s = "(null)";
            len = strlen(s);
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            s = tmp, len = 1;
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
            int neg = 0;
            v = va_arg(ap, unsigned);
            if (*f == 'x' || *f == 'X' || *f == 'p') base = 16;
            if ((*f == 'd' || *f == 'i') && (int)v < 0) neg = 1, v = -v;
            s = tmp + sizeof tmp;
            do {
                unsigned d = v % base;
                *--s = d < 10 ? '0' + d : (*f == 'X' ? 'A' : 'a') + d - 10;
                v /= base;
            } while (v);
            if (neg) *--s = '-';
            len = tmp + sizeof tmp - s;
            break;
        }
        case 0:
            f--;
            continue;
        default:
            tmp[0] = *f;
            s = tmp, len = 1;
        }
        if (!left) for (i = len; i < width; i++) PUT(pad);
        for (i = 0; i < len; i++) PUT(s[i]);
        if (left) for (i = len; i < width; i++) PUT(' ');
    }
    if (size) buf[o < size ? o : size - 1] = 0;
#undef PUT
    return o;
}

int snprintf(char *buf, size_t size, const char *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = vsnprintf(buf, size, f, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *f, va_list ap) { return vsnprintf(buf, 0x10000, f, ap); }

int sprintf(char *buf, const char *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = vsnprintf(buf, 0x10000, f, ap);
    va_end(ap);
    return n;
}

/* no console output: messages are dropped */
int printf(const char *f, ...) { return 0; }
int fprintf(void *fp, const char *f, ...) { return 0; }
int fiprintf(void *fp, const char *f, ...) { return 0; }
int vfprintf(void *fp, const char *f, va_list ap) { return 0; }
int puts(const char *s) { return 0; }
int putchar(int c) { return c; }
int fputs(const char *s, void *fp) { return 0; }
int fflush(void *fp) { return 0; }
int sscanf(const char *s, const char *f, ...) { return 0; }

static int errno_;
int *__errno(void) { return &errno_; }

long strtol(const char *s, char **e, int base)
{
    long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2, base = 16;
    if (base == 0) base = *s == '0' ? 8 : 10;
    for (;; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10
              : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (e) *e = (char *)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **e, int base) { return strtol(s, e, base); }
int atoi(const char *s) { return strtol(s, 0, 10); }

static unsigned seed_ = 1;
int rand(void) { return (seed_ = seed_ * 1103515245u + 12345u) >> 16 & 0x7fff; }
void srand(unsigned s) { seed_ = s; }
long time(long *t) { if (t) *t = 0; return 0; }

/* A fatal error in the emulator: stop here (the screen keeps its last frame). */
void abort(void) { for (;;) {} }
void exit(int code) { for (;;) {} }
void _exit(int code) { for (;;) {} }
void __assert_func(const char *file, int line, const char *fn, const char *e) { for (;;) {} }
int raise(int sig) { return 0; }

/* qsort (clip.c sorts a few window edges): an insertion sort, as newlib's ARC qsort has lp loops */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base, t[16];
    size_t i, j;
    if (size > sizeof t) return;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--) {
            memcpy(t, b + j * size, size);
            memcpy(b + j * size, b + (j - 1) * size, size);
            memcpy(b + (j - 1) * size, t, size);
        }
}
