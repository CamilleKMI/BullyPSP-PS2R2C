/* gsreplay_core.c - plays a graphics capture (written by the pack's hle_gfx_scan.c on the PC build) through the
 * pack's graphics units: hle_gfx_scan.c (PATH3 masking), hle_vu1.c (VIF1 + VU1) and a GS sink (the software GS on
 * the PC, the GE translator on the PSP). Shared by the PC and PSP replayers. */
#include "hle.h"
#include <stdarg.h>

uint64_t hle_frame, hle_vtime;
#ifdef HLE_PSP
#include <psprtc.h>
#include <pspkernel.h>
double hle_host_seconds(void) { return (double)sceKernelGetSystemTimeWide() / 1e6; }
#else
#include <time.h>
double hle_host_seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
#endif
uint32_t hle_ram_end = 0x2000000;
uint8_t *hle_spr;
static uint64_t priv[5];
int hle_path3_masked(void) { return 0; }
uint64_t hle_gs_priv(uint32_t off)
{
    switch (off) { case 0x00: return priv[0]; case 0x70: return priv[1]; case 0x80: return priv[2]; case 0x90: return priv[3]; case 0xA0: return priv[4]; }
    return 0;
}
void hle_gfx_replay_dma(int ch, int masked, const uint32_t *w, uint32_t n);
void hle_gfx_replay_held(const uint32_t *w, uint32_t n);
void hle_gfx_replay_flip(void);
void hle_vu1_state_load(const uint8_t *in);
void hle_gs_lite_load_mem(const uint8_t *m);
void hle_gs_lite_packets(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n);

static uint8_t *buf;
static uint32_t len, pos, start;
static const uint8_t *qget(void *u, uint32_t k) { return (const uint8_t *)u + 16 * k; }

/* the whole capture in memory; returns 0 when it looks valid */
int gsr_load(uint8_t *data, uint32_t n)
{
    if (n < 8 || memcmp(data, "BGCAP1", 6)) return -1;
    buf = data; len = n; pos = 8;
    /* snapshots */
    while (pos + 8 <= len) {
        uint32_t t, l; memcpy(&t, buf + pos, 4); memcpy(&l, buf + pos + 4, 4);
        if (t == 4 || t == 5 || t == 0) break;
        const uint8_t *d = buf + pos + 8;
        if (t == 1) hle_gs_lite_load_mem(d);
        else if (t == 2) hle_vu1_state_load(d);
        else if (t == 3) hle_gs_lite_packets(qget, (void *)d, l / 16);
        else if (t == 6) hle_gfx_replay_held((const uint32_t *)d, l / 4);
        pos += 8 + l;
    }
    start = pos;
    return 0;
}
/* plays records up to and including the next flip; returns 0 at the end of the capture (then rewinds) */
int gsr_frame(void)
{
    while (pos + 8 <= len) {
        uint32_t t, l; memcpy(&t, buf + pos, 4); memcpy(&l, buf + pos + 4, 4);
        const uint8_t *d = buf + pos + 8;
        pos += 8 + l;
        if (t == 0) break;
        if (t == 4) { uint32_t ch, m; memcpy(&ch, d, 4); memcpy(&m, d + 4, 4); hle_gfx_replay_dma((int)ch, (int)m, (const uint32_t *)(d + 8), (l - 8) / 4); }
        else if (t == 5) { memcpy(priv, d, sizeof priv); hle_frame++; hle_gfx_replay_flip(); return 1; }
    }
    pos = start;
    return 0;
}
