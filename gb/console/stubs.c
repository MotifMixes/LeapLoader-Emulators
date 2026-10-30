/* The GB console build: gnuboy's own files (rc scripts, its saves) are not
   used, so no stdio (newlib's uses gp); these stand in for it. No headers. */
void *fopen(const char *p, const char *m) { return 0; }
int fclose(void *f) { return 0; }
unsigned fread(void *b, unsigned s, unsigned n, void *f) { return 0; }
unsigned fwrite(const void *b, unsigned s, unsigned n, void *f) { return 0; }
int fseek(void *f, long o, int w) { return -1; }
long ftell(void *f) { return 0; }
char *fgets(char *s, int n, void *f) { return 0; }
int feof(void *f) { return 1; }
int fgetc(void *f) { return -1; }
int fputc(int c, void *f) { return c; }
void rewind(void *f) {}
int remove(const char *p) { return -1; }
int rename(const char *a, const char *b) { return -1; }
char *getenv(const char *n) { return 0; }
int unlink(const char *p) { return -1; }
void *malloc(unsigned);
unsigned strlen(const char *);
void *memcpy(void *, const void *, unsigned);
char *strdup(const char *s)
{
    unsigned n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
int fscanf(void *f, const char *fmt, ...) { return -1; }
int atexit(void (*fn)(void)) { return 0; }
double atof(const char *s) { return 0; }
int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y || !x) return x - y;
    }
}
/* newlib's stderr and ctype tables (only gnuboy's unused rc / error paths read them) */
static char impure[0x400] __attribute__((aligned(4)));
void *_impure_ptr = impure;
const char _ctype_[257];
