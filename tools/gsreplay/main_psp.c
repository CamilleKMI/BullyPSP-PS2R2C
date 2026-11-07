/* main_psp.c - the PSP replayer: plays cap.bin (from the EBOOT's folder) through the PSP renderer (the pack's
   src/psp/hle_ge_gs.c) in a loop; writes gsreplay.log next to the EBOOT. HOME quits. */
#include "hle.h"
#include <stdarg.h>
#include <pspkernel.h>
#include <psprtc.h>
PSP_MODULE_INFO("gsreplay", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(26 * 1024);

#include <pspdisplay.h>
#include <psppower.h>
static FILE *logf_;
void gsr_logf(const char *fmt, ...)
{
    char b[1024];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (logf_) { fputs(b, logf_); fflush(logf_); }
    sceIoDevctl("emulator:", 2, b, (int)strlen(b), 0, 0);            /* PPSSPP's headless output (ignored elsewhere) */
}
int gsr_load(uint8_t *data, uint32_t n);
int gsr_frame(void);
void hle_gs_lite_report(void);
void hle_vu1_report(void);

static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; sceKernelExitGame(); return 0; }
static int cb_thread(SceSize n, void *p)
{
    (void)n; (void)p;
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

int main(int argc, char **argv)
{
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    scePowerSetClockFrequency(333, 333, 166);       /* as the game's runtime does (timings comparable) */
    char dir[256] = "";
    if (argc > 0 && argv[0]) { strncpy(dir, argv[0], sizeof dir - 1); char *s = strrchr(dir, '/'); if (s) s[1] = 0; else dir[0] = 0; }
    char p[300];
    snprintf(p, sizeof p, "%sgsreplay.log", dir); logf_ = fopen(p, "w");
    snprintf(p, sizeof p, "%scap.bin", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { gsr_logf("no %s\n", p); sceKernelExitGame(); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d) { gsr_logf("no memory for %ld bytes\n", n); sceKernelExitGame(); return 0; }
    fread(d, 1, (size_t)n, f); fclose(f);
    gsr_logf("capture %ld bytes\n", n);
    if (gsr_load(d, (uint32_t)n)) { gsr_logf("not a capture\n"); sceKernelExitGame(); return 0; }
    /* shot.txt (optional): "N" = after frame N of the first pass, ask the emulator for a screenshot and quit */
    int shot = -1;
    snprintf(p, sizeof p, "%sshot.txt", dir);
    FILE *sf = fopen(p, "r"); if (sf) { if (fscanf(sf, "%d", &shot) != 1) shot = -1; fclose(sf); }
    if (shot >= 0) {
        for (int l = 0; l < shot / 100; l++) { while (gsr_frame()) ; gsr_load(d, (uint32_t)n); }   /* "L0N": L whole passes first */
        shot %= 100;
        int k = 0;
        while (k <= shot && gsr_frame()) k++;
        sceDisplayWaitVblankStart(); sceDisplayWaitVblankStart();
        hle_vu1_report(); hle_gs_lite_report();
        gsr_logf("screenshot after frame %d\n", k - 1);
        sceIoDevctl("emulator:", 0x20, 0, 0, 0, 0);
        sceKernelExitGame();
        return 0;
    }
    for (int loop = 0; loop < 1000; loop++) {
        u64 t0, t1; sceRtcGetCurrentTick(&t0);
        int k = 0; while (gsr_frame()) k++;
        sceRtcGetCurrentTick(&t1);
        gsr_logf("loop %d: %d frames in %llu us\n", loop, k, (unsigned long long)(t1 - t0));
        if (loop == 0 || loop == 3) { hle_vu1_report(); hle_gs_lite_report(); }
        /* reload the snapshots so every loop starts from the same state */
        gsr_load(d, (uint32_t)n);
    }
    sceKernelExitGame();
    return 0;
}
