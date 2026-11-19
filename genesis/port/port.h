/* forced into every core file: what NO_32X hides but draw.c still uses */
#ifndef __ASSEMBLER__
extern int (*PicoScan32xBegin)(unsigned int num);
extern int (*PicoScan32xEnd)(unsigned int num);
#endif
