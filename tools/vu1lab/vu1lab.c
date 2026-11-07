/* vu1lab.c - the VU1 model on captured batches (HLE_VU1_BATCHDUMP .in / .out files), on the PC.
   vu1lab CODE OUTDIR BATCH... : for each batch, runs the program at 0 (interpreter / AOT, native paths off) and
   writes OUTDIR/<name>.ref (kicked GS qwords); with LAB_NATIVE=1 the native version (nat_fol_try) instead
   (OUTDIR/<name>.nat). LAB_PCHIST=file: the executed instruction histogram. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
volatile int hle_zone;
uint64_t hle_frame;
void hle_path3_unmask(void) {}
void gsr_logf(const char *fmt, ...) { va_list a; va_start(a, fmt); if (getenv("LAB_LOG")) vfprintf(stderr, fmt, a); va_end(a); }
#include "hle_vu1.c"
static int load(const char *fn, void *p, size_t n) { FILE *f = fopen(fn, "rb"); if (!f) return -1; size_t r = fread(p, 1, n, f); fclose(f); return r == n ? 0 : -1; }
int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: vu1lab CODE OUTDIR BATCH...\n"); return 2; }
    static uint8_t code[16384];
    if (load(argv[1], code, sizeof code)) { fprintf(stderr, "no code %s\n", argv[1]); return 1; }
    int nat = getenv("LAB_NATIVE") && atoi(getenv("LAB_NATIVE"));
    uint32_t lab_pc = getenv("LAB_PC") ? (uint32_t)strtoul(getenv("LAB_PC"), 0, 16) : 0;   /* the program start */
    for (int i = 3; i < argc; i++) {
        static uint8_t d[16384 + 64 + 512 + 64];
        char fn[600]; snprintf(fn, sizeof fn, "%s.in", argv[i]);
        if (load(fn, d, sizeof d)) { fprintf(stderr, "bad %s\n", fn); continue; }
        memset(&vu, 0, sizeof vu);
        memcpy(vu.code, code, sizeof code);
        memcpy(vu.mem, d, 16384);
        uint32_t regs[16]; memcpy(regs, d + 16384, 64);
        v_top = regs[0]; v_itop = regs[1]; v_tops = regs[2]; v_base = regs[3]; v_ofst = regs[4]; v_dbf = regs[5];
        for (int r = 0; r < 32; r++) memcpy(vu.vf[r].u, d + 16384 + 64 + 16 * r, 16);
        for (int r = 0; r < 16; r++) { uint32_t x; memcpy(&x, d + 16384 + 64 + 512 + 4 * r, 4); vu.vi[r] = (uint16_t)x; }
        vu.vf[0].f[3] = 1.0f;
        const char *base = strrchr(argv[i], '/'); base = base ? base + 1 : argv[i];
        snprintf(fn, sizeof fn, "%s/%s.%s", argv[2], base, nat ? "nat" : "ref");
        hle_vu1_kickdump = fopen(fn, "wb");
        nat_code_ok = 0; nat_mode = 0;
#ifdef LAB_HAVE_NATIVE
        if (nat) { nat_fol_try(lab_pc); if (hle_vu1_kickdump) { fclose(hle_vu1_kickdump); hle_vu1_kickdump = 0; } continue; }
#endif
        vu1_run(lab_pc);
        if (hle_vu1_kickdump) { fclose(hle_vu1_kickdump); hle_vu1_kickdump = 0; }
        {   /* the state after the run: VU memory, VF, VI, ACC, Q, I, MAC, clip (OUTDIR/<name>.st) */
            char sn[600]; snprintf(sn, sizeof sn, "%s/%s.st", argv[2], base);
            FILE *f = fopen(sn, "wb");
            if (f) { fwrite(vu.mem, 1, 16384, f); fwrite(vu.vf, 1, sizeof vu.vf, f); fwrite(vu.vi, 1, sizeof vu.vi, f);
                     fwrite(&vu.acc, 1, sizeof vu.acc, f); flags_commit(); vu.now += 64; flags_commit();
                     if (vu.q_pend) { vu.Q = vu.q_new; vu.q_pend = 0; }
                     fwrite(&vu.Q, 4, 1, f); fwrite(&vu.I, 4, 1, f); fwrite(&vu.mac, 4, 1, f); fwrite(&vu.clip, 4, 1, f); fclose(f); }
        }
        {   /* LAB_REPEAT=n: the same batch n more times from its captured state (timing, no output) */
            static int rep = -1; if (rep < 0) rep = getenv("LAB_REPEAT") ? atoi(getenv("LAB_REPEAT")) : 0;
            static uint8_t m0[16384]; memcpy(m0, d, 16384);
            for (int k = 0; k < rep; k++) {
                memcpy(vu.mem, m0, 16384);
                for (int r = 0; r < 32; r++) memcpy(vu.vf[r].u, d + 16384 + 64 + 16 * r, 16);
                for (int r = 0; r < 16; r++) { uint32_t x; memcpy(&x, d + 16384 + 64 + 512 + 4 * r, 4); vu.vi[r] = (uint16_t)x; }
                vu.vf[0].f[3] = 1.0f;
                vu1_run(lab_pc);
            }
        }
    }
    if (getenv("LAB_PCHIST")) hle_vu1_dump_pchist(getenv("LAB_PCHIST"));
    return 0;
}
