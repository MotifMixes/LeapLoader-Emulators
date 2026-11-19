/* stand-in for the emu2413 submodule (SMS FM, never used here) */
#include <stdint.h>
typedef struct OPLL OPLL;
OPLL *OPLL_new(uint32_t clk, uint32_t rate);
void OPLL_delete(OPLL *);
void OPLL_reset(OPLL *);
void OPLL_setChipType(OPLL *, uint8_t);
void OPLL_setRate(OPLL *, uint32_t);
int16_t OPLL_calc(OPLL *);
void OPLL_writeIO(OPLL *, uint32_t, uint8_t);
void OPLL_writeReg(OPLL *, uint32_t, uint8_t);
