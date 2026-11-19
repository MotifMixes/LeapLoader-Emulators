/* What the left-out parts (Mega CD, 32X, SVP, SMS, Pico, zip/zlib, mp3)
   would have defined. No headers on purpose: only the names and sizes matter. */
#include <stdint.h>
#include <stddef.h>

void *Pico_mcd, *carthw_chunks, *opll;
void (*PicoLoadStateHook)(void);
int (*PicoScan32xBegin)(unsigned int), (*PicoScan32xEnd)(unsigned int);
uint32_t pcd_base_address;
unsigned int SekCycleAimS68k, SekCycleCntS68k;
uint64_t Pico_msd[1024], PicoCpuFS68k[1024];
uintptr_t s68k_read8_map[256], s68k_read16_map[256], s68k_write8_map[256], s68k_write16_map[256];

void DmaSlowCell(void) {}
void Pico32xPrepare(void) {}
void PicoDoHighPal555SMS(void) {}
void PicoDraw2Init(void) {}
void PicoDraw2SetOutBuf(void) {}
void PicoDrawSetOutBuf32X(void) {}
void PicoDrawSetOutFormat32x(void) {}
void PicoDrawSetOutputSMS(void) {}
void PicoExitMCD(void) {}
void PicoFrameFull(void) {}
void PicoFrameMCD(void) {}
void PicoInitMCD(void) {}
void PicoInitPico(void) {}
void PicoMCDPrepare(void) {}
void PicoMemSetupCD(void) {}
void PicoMemSetupPico(void) {}
void PicoPicoIrqAck(void) {}
void PicoPicoPCMUpdate(void) {}
void PicoPowerMCD(void) {}
void PicoReratePico(void) {}
void PicoResetMCD(void) {}
void PicoSVPInit(void) {}
void PicoSVPStartup(void) {}
unsigned PicoRead16_32x(unsigned a) { return 0; }
unsigned PicoRead8_32x(unsigned a) { return 0; }
void PicoWrite16_32x(unsigned a, unsigned d) {}
void PicoWrite8_32x(unsigned a, unsigned d) {}
void pcd_pcm_update(void) {}
void mp3_start_play(void) {}
void mp3_update(void) {}
void *OPLL_new(void) { return 0; }
void OPLL_delete(void) {}
void OPLL_reset(void) {}
void OPLL_setChipType(void) {}
void OPLL_setRate(void) {}
short OPLL_calc(void) { return 0; }
void *resampler_new(void) { return 0; }
void resampler_free(void) {}
void resampler_update(void) {}
void *openzip(void) { return 0; }
int readzip(void) { return -1; }
void closezip(void) {}
int seekcompresszip(void) { return -1; }
int inflate(void) { return -2; }
int inflateEnd(void) { return 0; }
int inflateInit2_(void) { return -2; }
int inflateReset(void) { return -2; }
unsigned long crc32(unsigned long crc, const unsigned char *p, unsigned n)
{
    crc = ~crc & 0xffffffff;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc & 0xffffffff;
}
/* PicoReset starts the 68k at a rand() point of the frame; glibc's and
   newlib's rand() differ, so both builds use this one (same runs on both) */
static unsigned rand_state = 1;
int rand(void) { rand_state = rand_state * 1103515245 + 12345; return (rand_state >> 16) & 0x7fff; }
void srand(unsigned s) { rand_state = s; }
