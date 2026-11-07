/* hle.h stand-in for the graphics replayer (tools/gsreplay): the few runtime symbols the pack's graphics units use. */
#ifndef HLE_H
#define HLE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void gsr_logf(const char *fmt, ...);
#define LOG(...) gsr_logf(__VA_ARGS__)
extern uint64_t hle_frame, hle_vtime;
#define EE_HZ 294912000ull
extern uint32_t hle_ram_end;
extern uint8_t *hle_spr;
#define HLE_SPR_SIZE 16384
static inline void *gptr(uint32_t a, uint32_t n) { (void)a; (void)n; return 0; }
int hle_path3_masked(void);
uint64_t hle_gs_priv(uint32_t off);
#endif
