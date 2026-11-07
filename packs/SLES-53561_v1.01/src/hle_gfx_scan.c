/* hle_gfx_scan.c - analysis of how this game feeds the GS (the first step towards its render boundary): every VIF1
 * and GIF DMA the core would drop is walked here (core hook hle_pack_gfx_dma) and classified - VIF commands (UNPACK
 * data to VU1, MPG microcode uploads, MSCAL program starts, DIRECT = GIF packets through PATH2), GIF packets (PATH2
 * via VIF1 DIRECT, PATH3 via the GIF channel): their formats and the primitive types they draw, IMAGE transfers
 * (texture uploads). Counts only; the data is still dropped. A report at the end of the run (host + PSP log). */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hle.h"

static uint64_t n_dma[3], n_qw[3], n_tags, n_vif[128], n_vu1_starts[64], n_gif_pk[2], n_gif_fmt[2][4],
    n_prim[2][8], n_img_qw[2], n_vu1_mpg_bytes;
static uint32_t vu1_start_addr[64];
static int n_vu1_addr;

static const uint8_t *qw(uint32_t a)
{
    if (a & 0x80000000u) return hle_spr + (a & (HLE_SPR_SIZE - 16));
    a &= 0x01FFFFF0u;
    if (a + 16 > hle_ram_end) return 0;
    return (const uint8_t *)gptr(a, 16);
}
static uint32_t w32(const uint8_t *p, int k) { uint32_t v; memcpy(&v, p + 4 * k, 4); return v; }

/* the host's software GS (src/host/hle_gs_lite.c; absent on the PSP) */
void hle_gs_lite_packets(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n) __attribute__((weak));
void hle_gs_lite_flip(void) __attribute__((weak));
void hle_gs_lite_report(void) __attribute__((weak));
int hle_vif1_stream(const uint32_t *w, uint32_t n) __attribute__((weak));   /* host VU1 model */
void hle_vu1_report(void) __attribute__((weak));
static int vif_model;                       /* the VU1 model draws PATH2 itself */
void hle_path3_release(void);
void hle_path3_unmask(void);

/* GIF packets in a buffer of n qwords (path: 0 = PATH2 via VIF DIRECT, 1 = PATH3 via the GIF channel) */
static void gif_scan(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n, int path)
{
    if (hle_gs_lite_packets && !(path == 0 && vif_model)) hle_gs_lite_packets(get, u, n);
    uint32_t i = 0;
    int guard = 0;
    while (i < n && guard++ < 1000000) {
        const uint8_t *t = get(u, i++);
        if (!t) return;
        uint64_t lo, hi;
        memcpy(&lo, t, 8);
        memcpy(&hi, t + 8, 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        n_gif_pk[path]++;
        n_gif_fmt[path][flg]++;
        if ((lo >> 46) & 1) n_prim[path][(lo >> 47) & 7]++;                  /* PRE: PRIM from the tag */
        uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        if (flg == 0) {                                                         /* PACKED: A+D writes of PRIM */
            for (uint32_t k = 0; k < nloop && k < 64; k++)
                for (uint32_t r = 0; r < nreg; r++) {
                    uint32_t reg = (uint32_t)(hi >> (4 * r)) & 15;
                    if (reg != 0xE) continue;                                   /* A+D */
                    const uint8_t *d = get(u, i + k * nreg + r);
                    if (d && d[8] == 0x00) n_prim[path][d[0] & 7]++;            /* GS register 0x00 = PRIM */
                }
        }
        if (flg == 2) n_img_qw[path] += nloop;
        i += body;
    }
}

/* PATH3 data held back while the VIF1 side masks PATH3 (MSKPATH3): released at the unmask inside the VIF1 stream
   (hle_vu1.c calls hle_path3_release) or at the end of the VIF1 transfer, as on the GS */
static uint32_t *p3_buf;
static uint32_t p3_n, p3_cap;
static uint64_t n_p3_held, n_vif_irq;
#ifdef HLE_PSP
/* PSP: fixed blocks from user memory (the runtime's malloc heap is 768 KB) */
void *hle_ge_gs_alloc(size_t n, const char *what) __attribute__((weak));
#define VS_CAP_PSP (1536u * 1024u / 4u)
#define P3_CAP_PSP (512u * 1024u / 4u)
static uint64_t n_overflow;
static uint32_t *psp_buf(uint32_t words, const char *what)
{
    return hle_ge_gs_alloc ? (uint32_t *)hle_ge_gs_alloc(words * 4u, what) : 0;
}
#endif
static const uint8_t *p3_get(void *u, uint32_t k)
{
    uint32_t i = (uint32_t)(uintptr_t)u + 4 * k;
    return i + 4 <= p3_n ? (const uint8_t *)(p3_buf + i) : 0;
}
static void p3_hold(const uint32_t *w, uint32_t n)
{
#ifdef HLE_PSP
    { static int tried; if (!tried) { tried = 1; p3_buf = psp_buf(P3_CAP_PSP, "gfx_path3"); p3_cap = p3_buf ? P3_CAP_PSP : 0; } }
    if (p3_n + n > p3_cap) { n_overflow++; return; }
#endif
    if (p3_n + n > p3_cap) {
        uint32_t nc = p3_cap ? p3_cap : 65536;
        while (nc < p3_n + n) nc *= 2;
        uint32_t *nb = (uint32_t *)realloc(p3_buf, nc * 4);
        if (!nb) return;
        p3_buf = nb; p3_cap = nc;
    }
    memcpy(p3_buf + p3_n, w, n * 4);
    p3_n += n;
    n_p3_held++;
    {   /* diagnostics: GIF packets / EOP packets in the held data */
        static int dbg = -1; if (dbg < 0) dbg = getenv("HLE_P3_DBG") != 0;
        if (dbg) {
            uint32_t i = 0, pk = 0, eop = 0, img = 0;
            while (i + 4 <= n) {
                uint64_t lo; memcpy(&lo, w + i, 8);
                uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
                if (!nreg) nreg = 16;
                uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
                pk++; if ((lo >> 15) & 1) eop++; if (flg >= 2) img++;
                i += 4 * (1 + body);
            }
            LOG("P3DBG frame %llu held %u qw: %u packets, %u EOP, %u image\n", (unsigned long long)hle_frame, n / 4, pk, eop, img);
        }
    }
}
static void gif_scan(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n, int path);
static uint32_t p3_r;                       /* read position (words) */
static void p3_run(int one)                 /* one: up to and including the next packet with EOP */
{
    uint32_t start = p3_r, i = p3_r;
    while (i + 4 <= p3_n) {
        uint64_t lo; memcpy(&lo, p3_buf + i, 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        i += 4 * (1 + body);
        if (one && ((lo >> 15) & 1)) break;
    }
    if (i > p3_n) i = p3_n;
    if (i > start) gif_scan(p3_get, (void *)(uintptr_t)start, (i - start) / 4, 1);
    p3_r = i;
    if (p3_r >= p3_n) p3_n = p3_r = 0;
}
#ifndef HLE_PSP
void hle_path3_release(void) { if (p3_n) p3_run(0); }          /* everything (end of the VIF1 transfer) */
void hle_path3_unmask(void) { if (p3_n) p3_run(1); }           /* MSKPATH3(0) inside the VIF1 stream */
#endif

/* one VIF1 transfer as a continuous word stream (with TTE the chain tags' upper halves are part of it) */
static uint32_t *vs_buf;
static uint32_t vs_n, vs_cap;
static void vs_push_qw(const uint8_t *q, int tag_only_hi)
{
#ifdef HLE_PSP
    { static int tried; if (!tried) { tried = 1; vs_buf = psp_buf(VS_CAP_PSP, "gfx_stream"); vs_cap = vs_buf ? VS_CAP_PSP : 0; } }
    if (vs_n + 4 > vs_cap) { n_overflow++; return; }
#endif
    if (vs_n + 4 > vs_cap) {
        uint32_t nc = vs_cap ? vs_cap * 2 : 65536;
        if (nc > (1u << 24)) return;
        uint32_t *nb = (uint32_t *)realloc(vs_buf, nc * 4);
        if (!nb) return;
        vs_buf = nb; vs_cap = nc;
    }
    vs_buf[vs_n++] = tag_only_hi ? 0 : w32(q, 0);
    vs_buf[vs_n++] = tag_only_hi ? 0 : w32(q, 1);
    vs_buf[vs_n++] = w32(q, 2);
    vs_buf[vs_n++] = w32(q, 3);
}
static const uint8_t *vs_get(void *u, uint32_t k)
{
    uint32_t i = (uint32_t)(uintptr_t)u + 4 * k;
    return i + 4 <= vs_n ? (const uint8_t *)(vs_buf + i) : 0;
}
static void vs_parse(void)
{
    uint32_t p = 0;
    vif_model = hle_vif1_stream ? hle_vif1_stream(vs_buf, vs_n) : 0;
    hle_path3_release();                                   /* the end of the VIF1 transfer unmasks PATH3 */
    while (p < vs_n) {
        uint32_t code = vs_buf[p];
        uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
        n_vif[cmd]++;
        if (code >> 31) n_vif_irq++;
        p++;
        if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) {
            uint32_t addr = imm * 8;
            int k;
            for (k = 0; k < n_vu1_addr && vu1_start_addr[k] != addr; k++) ;
            if (k == n_vu1_addr && n_vu1_addr < 64) vu1_start_addr[n_vu1_addr++] = addr;
            if (k < 64) n_vu1_starts[k]++;
        } else if (cmd == 0x20) p += 1;
        else if (cmd == 0x30 || cmd == 0x31) p += 4;
        else if (cmd == 0x4A) { uint32_t w = (num ? num : 256) * 2; n_vu1_mpg_bytes += 4 * w; p += w; }
        else if (cmd == 0x50 || cmd == 0x51) {
            uint32_t qws = imm ? imm : 65536;
            p = (p + 3) & ~3u;
            gif_scan(vs_get, (void *)(uintptr_t)p, qws, 0);
            p += 4 * qws;
        } else if (cmd >= 0x60) {
            uint32_t vl = cmd & 3, vn = (cmd >> 2) & 3, cnt = num ? num : 256;
            p += (cnt * (vn + 1) * (32u >> vl) + 31) / 32;
        }
    }
}

/* ------------------------------------------------------------------ graphics captures
   PC: env HLE_GFX_CAPTURE=<file> with HLE_GFX_CAPTURE_AT=<frame> (counted like the software GS's flips) and
   HLE_GFX_CAPTURE_N=<frames> records, at that frame boundary, the GS memory, the GS register state and the VIF1 / VU1
   state, then every VIF1 / GIF transfer (as the word streams below, with the PATH3 mask) and every flip (with the
   display registers). tools/gsreplay plays such a file through the PSP renderer. The file holds the game's own
   graphics data: it stays in work/ on the user's machines (never shared). */
#ifndef HLE_PSP
#include <stdio.h>
const uint8_t *hle_gs_lite_mem(void) __attribute__((weak));
uint32_t hle_gs_lite_regs_packet(uint64_t *out) __attribute__((weak));
#endif
int hle_vu1_state_size(void) __attribute__((weak));
void hle_vu1_state_save(uint8_t *out) __attribute__((weak));
static uint64_t n_flips;
/* timing (both targets): every 60 game frames (one VIF1 chain per game frame) the host time per frame, the part
   spent in the graphics path (VIF1 / VU1 / GS sink) and how fast virtual time advanced against real time */
#include "hle_plat.h"
static double perf_t0, perf_c0, perf_gfx, perf_gfx_total; static uint64_t perf_vt0, perf_frames, perf_vif1;
#ifndef HLE_PSP
#include <time.h>
static double cpu_s(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
#else
static double cpu_s(void) { return hle_host_seconds(); }
#endif
uint64_t hle_vu1_vertices(void) __attribute__((weak));
/* where the game thread is (PSP): the graphics path sets hle_zone (a plain store, no system call); a high-priority
   thread samples it ~1000 times a second (cfg zprof=0: off). Printed per PERF window as ms per frame. */
volatile int hle_zone;
#ifdef HLE_PSP
#include <pspkernel.h>
#include "hle_cfg.h"
static volatile uint32_t zone_hist[32];
static const char *zone_name[32] = {"game code", "DMA walk + copy", "VIF parse", "unpacks", "mesh key", "mesh draw", "native other",
    "skin", "VU interpreter", "GS packets", "image data", "texture lookup", "mesh begin", "GE wait (flip)", "mesh convert", "clip polygons",
    "PATH3 walk", "gfx other", "foliage", 0};
static int zone_thread(SceSize a, void *p) { (void)a; (void)p; for (;;) { sceKernelDelayThread(997); zone_hist[hle_zone & 31]++; } return 0; }
static void zone_start(void)
{
    static int done; if (done) return; done = 1;
    const char *e = hle_cfg_get("zprof"); if (!e || e[0] != '1') return;   /* cfg zprof=1: on (it costs ~6 % of the CPU in PPSSPP) */
    SceUID t = sceKernelCreateThread("hle_zprof", zone_thread, 0x12, 0x1000, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
static void zone_report(double ms_per_frame)
{
    uint32_t tot = 0, h[32]; for (int i = 0; i < 32; i++) { h[i] = zone_hist[i]; zone_hist[i] = 0; tot += h[i]; }
    if (!tot) return;
    char line[600]; int o = 0;
    for (int i = 0; i < 32 && o < 560; i++) if (h[i]) o += snprintf(line + o, sizeof line - (size_t)o, "%s %.1f, ", zone_name[i] ? zone_name[i] : "?", ms_per_frame * h[i] / tot);
    LOG("ZONES ms/frame: %s(%u samples)\n", line, tot);
}
#endif
uint64_t hle_vif1_frames(void);
uint64_t hle_vif1_frames(void) { return perf_vif1; }
static void perf_frame(void)
{
    double now = hle_host_seconds(), cnow = cpu_s();
    if (perf_frames == 0) { perf_t0 = now; perf_c0 = cnow; perf_vt0 = hle_vtime; perf_gfx = 0; }
    if (++perf_frames <= 60) return;
    double dt = now - perf_t0, dc = cnow - perf_c0, vt = (double)(hle_vtime - perf_vt0) / (double)EE_HZ;
    static uint64_t v0; uint64_t v1 = hle_vu1_vertices ? hle_vu1_vertices() : 0;
    LOG("PERF %llu: 60 game frames in %.2f s = %.1f ms/frame (%.1f fps); cpu %.1f ms/frame; graphics path %.1f ms/frame; "
        "virtual time %.2f s (%.0f%% of real time); %llu VU1 vertices/frame\n", (unsigned long long)perf_vif1, dt, dt * 1000.0 / 60, 60.0 / dt,
        dc * 1000.0 / 60, perf_gfx * 1000.0 / 60, vt, dt > 0 ? 100.0 * vt / dt : 0, (unsigned long long)((v1 - v0) / 60));
    v0 = v1;
#ifdef HLE_PSP
    zone_report(dt * 1000.0 / 60);
#endif
    { void hle_vu1_tprof_window(uint32_t frames) __attribute__((weak)); if (hle_vu1_tprof_window) hle_vu1_tprof_window(60); }
    perf_frames = 1; perf_t0 = now; perf_c0 = cnow; perf_vt0 = hle_vtime; perf_gfx = 0;
}
#ifndef HLE_PSP
static FILE *cap;
static int cap_state;                       /* 0 off / waiting, 1 recording, 2 done */
static uint64_t cap_at, cap_left;
static void cap_rec(uint32_t type, const void *d, uint32_t n)
{
    fwrite(&type, 4, 1, cap); fwrite(&n, 4, 1, cap); if (n) fwrite(d, 1, n, cap);
}
static void cap_flip(void)
{
    static int init;
    if (!init) {
        init = 1;
        const char *f = getenv("HLE_GFX_CAPTURE");
        if (!f || !*f) { cap_state = 2; return; }
        const char *e = getenv("HLE_GFX_CAPTURE_AT"); cap_at = e ? strtoull(e, 0, 10) : 1000;
        e = getenv("HLE_GFX_CAPTURE_N"); cap_left = e ? strtoull(e, 0, 10) : 4;
        cap = fopen(f, "wb");
        if (!cap) { cap_state = 2; return; }
        fwrite("BGCAP1\0\0", 1, 8, cap);
    }
    if (cap_state == 2) return;
    if (cap_state == 1) {
        uint64_t d[5] = {hle_gs_priv(0x00), hle_gs_priv(0x70), hle_gs_priv(0x80), hle_gs_priv(0x90), hle_gs_priv(0xA0)};
        cap_rec(5, d, sizeof d);
        if (--cap_left == 0) { cap_rec(0, 0, 0); fclose(cap); cap = 0; cap_state = 2; LOG("graphics capture written\n"); }
        return;
    }
    if (n_flips < cap_at) return;
    if (!hle_gs_lite_mem || !hle_gs_lite_mem() || !hle_vu1_state_size) {
        LOG("graphics capture: needs the software GS (HLE_GS_FRAMES) and the VU1 model\n"); cap_state = 2; fclose(cap); return;
    }
    cap_rec(1, hle_gs_lite_mem(), 4u << 20);
    uint32_t vn = (uint32_t)hle_vu1_state_size();
    uint8_t *vb = (uint8_t *)malloc(vn);
    hle_vu1_state_save(vb); cap_rec(2, vb, vn); free(vb);
    uint64_t rp[130]; uint32_t q = hle_gs_lite_regs_packet(rp); cap_rec(3, rp, q * 16);
    if (p3_n > p3_r) cap_rec(6, p3_buf + p3_r, (p3_n - p3_r) * 4);     /* PATH3 data still held */
    cap_state = 1;
    LOG("graphics capture started at flip %llu\n", (unsigned long long)n_flips);
}
#endif

/* one gathered transfer: ch 1 = VIF1 word stream, ch 2 = GIF packet stream; masked = PATH3 masked at the kick */
static void deliver(int ch, int masked)
{
#ifndef HLE_PSP
    if (cap_state == 1) { uint32_t hdr[2] = {(uint32_t)ch, (uint32_t)masked}; uint32_t t = 4, n = 8 + vs_n * 4;
                          fwrite(&t, 4, 1, cap); fwrite(&n, 4, 1, cap); fwrite(hdr, 4, 2, cap); fwrite(vs_buf, 4, vs_n, cap); }
#endif
    if (ch == 1) vs_parse();
    else if (masked) p3_hold(vs_buf, vs_n);
    else { hle_path3_release(); gif_scan(vs_get, (void *)(uintptr_t)0, vs_n / 4, 1); }
}
static void frame_edge(void)
{
    n_flips++;
#ifndef HLE_PSP
    cap_flip();
#endif
    if (hle_gs_lite_flip) hle_gs_lite_flip();
}


#ifdef HLE_PSP
/* ------------------------------------------------------------------ PSP: no stream copies
   The PSP has ~10 MB free for everything; the per-frame streams are up to ~2 MB (VIF1) and ~1.4 MB (textures).
   VIF1: fed to the VU1 model in 256 KB chunks (a command cut at the chunk end waits for the next chunk).
   PATH3 held while masked: kept as references (the game's DMA chain, or a capture buffer in the replayer) and
   walked with a cursor when released. */
uint32_t hle_vif1_chunk(const uint32_t *w, uint32_t n, uint32_t start, int final);
int hle_vu1_on(void);
#define CK_WORDS (64u * 1024u)
static uint32_t *ck;
static uint32_t ck_n, ck_start;
/* where the chunk's words were copied from (blocks of the game's RAM): unpacked data keeps a stable address for the
   mesh cache keys (hle_vif_stable_ptr) */
static struct { uint32_t off, n; const uint32_t *src; } ckmap[256];
static int nckmap;
const uint32_t *hle_vif_stable_ptr(const uint32_t *p, uint32_t nw)
{
    if (!ck || p < ck || p >= ck + CK_WORDS) return 0;
    uint32_t o = (uint32_t)(p - ck);
    for (int i = nckmap - 1; i >= 0; i--)
        if (o >= ckmap[i].off && o + nw <= ckmap[i].off + ckmap[i].n) return ckmap[i].src + (o - ckmap[i].off);
    return 0;
}
static void ck_run(int final)
{
    if (!ck_n) return;
    uint32_t r = hle_vif1_chunk(ck, ck_n, ck_start, final);
    if (final || r >= ck_n) { ck_n = ck_start = 0; nckmap = 0; return; }
    uint32_t base = r & ~3u;
    memmove(ck, ck + base, (ck_n - base) * 4);
    ck_n -= base; ck_start = r - base;
    int k = 0;
    for (int i = 0; i < nckmap; i++) {
        if (ckmap[i].off + ckmap[i].n <= base) continue;
        uint32_t cut = ckmap[i].off < base ? base - ckmap[i].off : 0;
        ckmap[k].src = ckmap[i].src + cut; ckmap[k].n = ckmap[i].n - cut; ckmap[k].off = ckmap[i].off + cut - base; k++;
    }
    nckmap = k;
}
static void ck_push(const uint8_t *q, int tag_only_hi);
/* n contiguous qwords (a chain tag's data in RAM): copied by blocks */
static void ck_push_n(const uint8_t *q, uint32_t n)
{
    while (n) {
        if (!ck || ck_n + 4 > CK_WORDS) { ck_push(q, 0); q += 16; n--; continue; }   /* (allocates / runs the chunk) */
        uint32_t room = (CK_WORDS - ck_n) / 4, m = n < room ? n : room;
        memcpy(ck + ck_n, q, 16 * m);
        if (nckmap < 256) { ckmap[nckmap].off = ck_n; ckmap[nckmap].n = 4 * m; ckmap[nckmap].src = (const uint32_t *)q; nckmap++; }
        ck_n += 4 * m; q += 16 * m; n -= m;
    }
}
/* a large block in RAM: parsed where it is (no copy) once the chunk holds nothing pending; only a command it cuts
   at its end goes to the chunk (the next block or the final run completes it) */
static uint64_t n_direct_qw;
static void ck_push_direct(const uint8_t *q, uint32_t n)
{
    if (n >= 32 && ck) {
        ck_run(0);
        if (!ck_n) {
            extern int hle_vif_src_stable;
            hle_vif_src_stable = 1;
            uint32_t r = hle_vif1_chunk((const uint32_t *)q, 4 * n, 0, 0);
            hle_vif_src_stable = 0;
            n_direct_qw += n;
            if (r < 4 * n) { ck_push_n(q + 16 * (r / 4), n - r / 4); ck_start = r & 3; }
            return;
        }
    }
    ck_push_n(q, n);
}
static void ck_push(const uint8_t *q, int tag_only_hi)
{
    static int tried;
    if (!tried) { tried = 1; ck = psp_buf(CK_WORDS, "gfx_vif1_chunk"); }
    if (!ck) { n_overflow++; return; }
    if (ck_n + 4 > CK_WORDS) ck_run(0);
    if (ck_n + 4 > CK_WORDS) { n_overflow++; ck_run(1); }          /* one command larger than the chunk */
    ck[ck_n++] = tag_only_hi ? 0 : w32(q, 0);
    ck[ck_n++] = tag_only_hi ? 0 : w32(q, 1);
    ck[ck_n++] = w32(q, 2);
    ck[ck_n++] = w32(q, 3);
}

typedef struct {
    int kind;                    /* 0 normal (madr, qwc), 1 source chain (tadr), 2 linear host buffer */
    uint32_t madr, qwc, tadr, chcr;
    const uint8_t *lin;
    /* cursor */
    uint32_t data, left, ret[2]; int asp, end, started;
} P3Src;
#define P3Q 32
static P3Src p3q[P3Q];
static int p3q_n, p3q_r;
static const uint8_t *src_next(P3Src *c)          /* next qword of the transfer, 0 at its end */
{
    if (c->kind == 2) { if (c->left == 0) return 0; c->left--; const uint8_t *q = c->lin; c->lin += 16; return q; }
    if (c->kind == 0) { if (!c->started) { c->started = 1; c->data = c->madr; c->left = c->qwc; }
                        if (!c->left) return 0; c->left--; const uint8_t *q = qw(c->data); c->data += 16; return q; }
    for (int guard = 0; c->left == 0; guard++) {
        if (c->end || guard > 1000) return 0;
        const uint8_t *t = qw(c->tadr);
        if (!t) { c->end = 1; return 0; }
        uint64_t tag; memcpy(&tag, t, 8);
        uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)(tag >> 28) & 7, addr = (uint32_t)(tag >> 32);
        switch (id) {
        case 0: c->data = addr; c->end = 1; break;
        case 1: c->data = c->tadr + 16; c->tadr = c->data + 16 * n; break;
        case 2: c->data = c->tadr + 16; c->tadr = addr; break;
        case 3: case 4: c->data = addr; c->tadr += 16; break;
        case 5: c->data = c->tadr + 16; if (c->asp < 2) c->ret[c->asp++] = c->data + 16 * n; c->tadr = addr; break;
        case 6: c->data = c->tadr + 16; if (c->asp) c->tadr = c->ret[--c->asp]; else c->end = 1; break;
        default: c->data = c->tadr + 16; c->end = 1; break;
        }
        if ((tag >> 31) & 1 && (c->chcr & 0x80)) c->end = 1;
        c->left = n;
        if (!n && c->end) return 0;
    }
    c->left--;
    const uint8_t *q = qw(c->data); c->data += 16;
    return q;
}
/* the sink reads qwords in order (an index may be asked twice in a row) */
typedef struct { P3Src *c; uint32_t k; const uint8_t *last; const uint8_t *tag; } PkGet;
static const uint8_t *pk_get(void *u, uint32_t k)
{
    PkGet *g = (PkGet *)u;
    if (k == 0) return g->tag;
    if (k == g->k) return g->last;
    if (k == g->k + 1 && g->k && g->last && g->c->left && g->c->started | (g->c->kind != 0)) {   /* the same block: contiguous */
        P3Src *c = g->c; c->left--;
        if (c->kind == 2) { g->last = c->lin; c->lin += 16; }
        else { const uint8_t *q = qw(c->data); if (!q) return 0; g->last = q; c->data += 16; }
        g->k++;
        return g->last;
    }
    while (g->k < k) { g->last = src_next(g->c); g->k++; if (!g->last) return 0; }
    return g->last;
}
/* the qwords k, k+1, ... contiguous in memory from where the reader stands (up to want): the GS sink takes image
   data by runs instead of one qword at a time */
static const uint8_t *pk_span(void *u, uint32_t k, uint32_t want, uint32_t *n)
{
    PkGet *g = (PkGet *)u;
    const uint8_t *q = pk_get(u, k);
    *n = 0;
    if (!q || k == 0) { if (q) *n = 1; return q; }
    P3Src *c = g->c;
    uint32_t more = c->left;                                   /* qwords after q in its block (contiguous) */
    if (c->kind != 2 && more && qw(c->data) != q + 16) more = 0;
    if (more > want - 1) more = want - 1;
    if (c->kind == 2) c->lin += 16 * more; else c->data += 16 * more;
    c->left -= more;
    g->k = k + more; g->last = q + 16 * more;
    *n = 1 + more;
    return q;
}
void hle_gs_lite_set_span(const uint8_t *(*span)(void *, uint32_t, uint32_t, uint32_t *)) __attribute__((weak));
void hle_gs_lite_set_span_get(const uint8_t *(*get)(void *, uint32_t)) __attribute__((weak));
/* sends whole GIF packets of the queue to the GS; one = stop after a packet with EOP */
uint64_t hle_p3_qw;                                           /* qwords of PATH3 packets sent to the GS path */
static void p3_send(int one)
{
    while (p3q_r < p3q_n) {
        P3Src *c = &p3q[p3q_r];
        const uint8_t *t = src_next(c);
        if (!t) { p3q_r++; continue; }
        uint64_t lo; memcpy(&lo, t, 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        PkGet g = {c, 0, t, t};
        if (hle_gs_lite_set_span) hle_gs_lite_set_span((flg == 2 || flg == 0) ? pk_span : 0);   /* runs: image data, PACKED */
        if (hle_gs_lite_set_span_get) hle_gs_lite_set_span_get(pk_get);
        hle_p3_qw += 1 + body;
        if (hle_gs_lite_packets) hle_gs_lite_packets(pk_get, &g, 1 + body);
        if (hle_gs_lite_set_span) hle_gs_lite_set_span(0);
        while (g.k < body) { if (!src_next(c)) break; g.k++; }         /* skip what the sink did not read */
        n_gif_pk[1]++;
        if (one && ((lo >> 15) & 1)) return;
    }
    p3q_n = p3q_r = 0;
}
void hle_path3_release(void) { int pz_ = hle_zone; hle_zone = 16; p3_send(0); hle_zone = pz_; }
void hle_path3_unmask(void) { int pz_ = hle_zone; hle_zone = 16; p3_send(1); hle_zone = pz_; }
static void p3_add(P3Src s2)
{
    if (p3q_n >= P3Q) { n_overflow++; return; }
    memset(&p3q[p3q_n], 0, sizeof p3q[0]);
    p3q[p3q_n] = s2;
    p3q[p3q_n].data = 0; p3q[p3q_n].left = s2.kind == 2 ? s2.qwc : 0; p3q[p3q_n].asp = 0; p3q[p3q_n].end = 0; p3q[p3q_n].started = 0;
    p3q_n++;
    n_p3_held++;
}
static void gif_transfer(P3Src s2, int masked)
{
    p3_add(s2);
    if (!masked) p3_send(0);
}

static void psp_dma(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr);
void hle_pack_gfx_dma(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    static uint64_t last_frame;
    if (hle_frame != last_frame) {
        last_frame = hle_frame; n_flips++;
        double ft0 = hle_host_seconds();
        int pz_ = hle_zone; hle_zone = 13;
        if (hle_gs_lite_flip) hle_gs_lite_flip();
        hle_zone = pz_;
        double fd = hle_host_seconds() - ft0; perf_gfx += fd; perf_gfx_total += fd;
    }
    if (ch < 1 || ch > 2) return;
    zone_start();
    n_dma[ch]++;
    double pt0 = hle_host_seconds();
    if (ch == 1) { perf_vif1++; perf_frame(); }
    int pz_ = hle_zone; hle_zone = ch == 1 ? 1 : 16;
    psp_dma(ch, chcr, madr, qwc, tadr);
    hle_zone = pz_;
    double pd = hle_host_seconds() - pt0;
    perf_gfx += pd; perf_gfx_total += pd;
}
static void psp_dma(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    uint32_t mode = (chcr >> 2) & 3;
    if (ch == 2) {
        P3Src s2; memset(&s2, 0, sizeof s2);
        s2.kind = mode == 0 ? 0 : 1; s2.madr = madr; s2.qwc = qwc; s2.tadr = tadr; s2.chcr = chcr;
        gif_transfer(s2, hle_path3_masked());
        return;
    }
    if (!hle_vu1_on()) return;
    vif_model = 1;
    extern uint64_t hle_tprof[12];
    uint64_t t0_ = sceKernelGetSystemTimeWide();                    /* slot 5: the VIF1 path, per DMA */
    if (mode == 0) {
        for (uint32_t k = 0; k < qwc; k++) { const uint8_t *d = qw(madr + 16 * k); if (d) ck_push(d, 0); }
    } else {
        P3Src c; memset(&c, 0, sizeof c); c.kind = 1; c.tadr = tadr; c.chcr = chcr;
        /* the same walk as src_next, plus the TTE tag words */
        int guard = 0;
        while (!c.end && guard++ < 20000) {
            const uint8_t *t = qw(c.tadr);
            if (!t) break;
            if (chcr & 0x40) ck_push(t, 1);
            uint64_t tag; memcpy(&tag, t, 8);
            uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)(tag >> 28) & 7, addr = (uint32_t)(tag >> 32), data;
            switch (id) {
            case 0: data = addr; c.end = 1; break;
            case 1: data = c.tadr + 16; c.tadr = data + 16 * n; break;
            case 2: data = c.tadr + 16; c.tadr = addr; break;
            case 3: case 4: data = addr; c.tadr += 16; break;
            case 5: data = c.tadr + 16; if (c.asp < 2) c.ret[c.asp++] = data + 16 * n; c.tadr = addr; break;
            case 6: data = c.tadr + 16; if (c.asp) c.tadr = c.ret[--c.asp]; else c.end = 1; break;
            default: data = c.tadr + 16; c.end = 1; break;
            }
            n_tags++; n_qw[1] += n;
            {   /* RAM (not the scratchpad) and inside it: one pointer for the whole block */
                const uint8_t *d0 = (n && !(data & 0x80000000u) && (data & 0x01FFFFF0u) + 16 * n <= hle_ram_end) ? qw(data) : 0;
                if (d0 && qw(data + 16 * (n - 1)) == d0 + 16 * (n - 1)) ck_push_direct(d0, n);
                else for (uint32_t k = 0; k < n; k++) { const uint8_t *d = qw(data + 16 * k); if (d) ck_push(d, 0); }
            }
            if ((tag >> 31) & 1 && (chcr & 0x80)) c.end = 1;
        }
    }
    ck_run(1);
    hle_tprof[5] += sceKernelGetSystemTimeWide() - t0_;
    hle_path3_release();
}

/* replay (tools/gsreplay on the PSP): capture buffers stay in memory, so they are referenced, not copied */
void hle_gfx_replay_held(const uint32_t *w, uint32_t n)
{
    P3Src s2; memset(&s2, 0, sizeof s2); s2.kind = 2; s2.lin = (const uint8_t *)w; s2.qwc = n / 4;
    p3_add(s2);
}
void hle_gfx_replay_dma(int ch, int masked, const uint32_t *w, uint32_t n)
{
    if (ch == 2) { P3Src s2; memset(&s2, 0, sizeof s2); s2.kind = 2; s2.lin = (const uint8_t *)w; s2.qwc = n / 4; gif_transfer(s2, masked); return; }
    if (!hle_vu1_on()) return;
    for (uint32_t i = 0; i + 4 <= n; i += 4) ck_push((const uint8_t *)(w + i), 0);
    ck_run(1);
    hle_path3_release();
}
void hle_gfx_replay_flip(void) { n_flips++; if (hle_gs_lite_flip) hle_gs_lite_flip(); }

#else /* the PC: whole streams in buffers (captures, statistics) */
/* replay (tools/gsreplay): the same paths as the game's transfers */
void hle_gfx_replay_held(const uint32_t *w, uint32_t n) { p3_hold(w, n); }
void hle_gfx_replay_dma(int ch, int masked, const uint32_t *w, uint32_t n)
{
    vs_n = 0;
    for (uint32_t i = 0; i + 4 <= n; i += 4) vs_push_qw((const uint8_t *)(w + i), 0);
    deliver(ch, masked);
}
void hle_gfx_replay_flip(void) { frame_edge(); }

void hle_pack_gfx_dma(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    static uint64_t last_frame;
    if (hle_frame != last_frame) { last_frame = hle_frame; frame_edge(); }
    if (ch < 1 || ch > 2) return;
    n_dma[ch]++;
    if (ch == 1) { perf_vif1++; perf_frame(); }
    {
        static int seq_on = -1; static uint64_t seq_n;
        if (seq_on < 0) { const char *e = getenv("HLE_GFX_SEQ"); seq_on = e ? atoi(e) : 0; }
        if (seq_on && hle_frame >= (uint64_t)seq_on && seq_n < 300000) {
            seq_n++;
            LOG("GFXSEQ frame %llu ch %d mode %u qwc %u tadr %08x madr %08x masked %d\n", (unsigned long long)hle_frame, ch,
                (chcr >> 2) & 3, qwc, ch == 1 || ((chcr >> 2) & 3) ? tadr : 0, madr, hle_path3_masked());
        }
    }

    uint32_t mode = (chcr >> 2) & 3;
    if (mode == 0) {
        n_qw[ch] += qwc;
        vs_n = 0;
        for (uint32_t k = 0; k < qwc; k++) { const uint8_t *d = qw(madr + 16 * k); if (d) vs_push_qw(d, 0); }
        deliver(ch, ch == 2 && hle_path3_masked());
        return;
    }
    /* source chain */
    vs_n = 0;
    uint32_t retstk[2];
    int asp = 0, end = 0, guard = 0;
    while (!end && guard++ < 20000) {
        const uint8_t *t = qw(tadr);
        if (!t) return;
        uint64_t tag;
        memcpy(&tag, t, 8);
        uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)(tag >> 28) & 7, addr = (uint32_t)(tag >> 32);
        uint32_t data;
        n_tags++;
        if (ch == 1 && (chcr & 0x40)) vs_push_qw(t, 1);                    /* TTE: the tag's upper 64 bits = 2 VIF codes */
        switch (id) {
        case 0: data = addr; end = 1; break;
        case 1: data = tadr + 16; tadr = data + 16 * n; break;
        case 2: data = tadr + 16; tadr = addr; break;
        case 3: case 4: data = addr; tadr += 16; break;
        case 5: data = tadr + 16; if (asp < 2) retstk[asp++] = data + 16 * n; tadr = addr; break;
        case 6: data = tadr + 16; if (asp) tadr = retstk[--asp]; else end = 1; break;
        default: data = tadr + 16; end = 1; break;
        }
        n_qw[ch] += n;
        for (uint32_t k = 0; k < n; k++) { const uint8_t *d = qw(data + 16 * k); if (d) vs_push_qw(d, 0); }
        if ((tag >> 31) & 1 && (chcr & 0x80)) end = 1;
    }
    deliver(ch, ch == 2 && hle_path3_masked());
}

#endif /* HLE_PSP */

void hle_sampler_dump(void) __attribute__((weak));
void hle_wrapcount_dump(void) __attribute__((weak));       /* measurement builds only */
void hle_gfx_scan_report(void)
{
    if (hle_sampler_dump) hle_sampler_dump();
    if (hle_wrapcount_dump) hle_wrapcount_dump();
    static const char *pn[8] = {"point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"};
    static const char *fn[4] = {"packed", "reglist", "image", "image2"};
    LOG("GS feed analysis: %llu VIF codes with the interrupt bit\n", (unsigned long long)n_vif_irq);
#ifdef HLE_PSP
    LOG("GS feed analysis: %llu qwords dropped (stream buffers full)\n", (unsigned long long)n_overflow);
#endif
    LOG("GS feed analysis: %llu PATH3 transfers held until the VIF1 side unmasked them\n", (unsigned long long)n_p3_held);
    LOG("GS feed analysis: VIF1 DMAs %llu (%llu qw), GIF DMAs %llu (%llu qw), chain tags %llu\n",
        (unsigned long long)n_dma[1], (unsigned long long)n_qw[1], (unsigned long long)n_dma[2],
        (unsigned long long)n_qw[2], (unsigned long long)n_tags);
    uint64_t unp = 0;
    for (int k = 0x60; k < 0x80; k++) unp += n_vif[k];
    LOG("  VIF codes: UNPACK %llu, MSCAL %llu, MSCALF %llu, MSCNT %llu, MPG %llu (%llu bytes of VU1 code), DIRECT %llu, "
        "DIRECTHL %llu, FLUSH* %llu, STCYCL %llu, MSKPATH3 %llu\n", (unsigned long long)unp, (unsigned long long)n_vif[0x14],
        (unsigned long long)n_vif[0x15], (unsigned long long)n_vif[0x17], (unsigned long long)n_vif[0x4A],
        (unsigned long long)n_vu1_mpg_bytes, (unsigned long long)n_vif[0x50], (unsigned long long)n_vif[0x51],
        (unsigned long long)(n_vif[0x10] + n_vif[0x11] + n_vif[0x13]), (unsigned long long)n_vif[1],
        (unsigned long long)n_vif[6]);
    for (int k = 0; k < n_vu1_addr; k++)
        LOG("  VU1 program at %04x started %llu times\n", vu1_start_addr[k], (unsigned long long)n_vu1_starts[k]);
    for (int p = 0; p < 2; p++) {
        LOG("  %s: GIF packets %llu (packed %llu, reglist %llu, image %llu: %llu qw of texture/image data)\n",
            p ? "PATH3 (GIF channel)" : "PATH2 (VIF1 DIRECT)", (unsigned long long)n_gif_pk[p],
            (unsigned long long)n_gif_fmt[p][0], (unsigned long long)n_gif_fmt[p][1],
            (unsigned long long)(n_gif_fmt[p][2] + n_gif_fmt[p][3]), (unsigned long long)n_img_qw[p]);
        for (int k = 0; k < 7; k++)
            if (n_prim[p][k]) LOG("    PRIM %s: %llu\n", pn[k], (unsigned long long)n_prim[p][k]);
    }
    (void)fn;
    if (hle_vu1_report) hle_vu1_report();
    if (hle_gs_lite_report) hle_gs_lite_report();
}
