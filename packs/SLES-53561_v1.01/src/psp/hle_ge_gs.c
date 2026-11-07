/* hle_ge_gs.c (PSP only) - the GS sink of the PSP build: the GIF packets the game sends to the GS (PATH1 from the VU1
 * model hle_vu1.c, PATH2 VIF1 DIRECT, PATH3 the GIF channel; fed by hle_gfx_scan.c) are translated to GE display
 * lists. Same interface as the PC's software GS (src/host/hle_gs_lite.c), which is the reference to compare with.
 *
 * Model:
 *   - GS registers as on the GS (both contexts, PRMODECONT / PRMODE, XYOFFSET, SCISSOR, TEST, ALPHA, FRAME, ZBUF ...);
 *   - a 4 MB GS local memory (gs_mem.h swizzle): IMAGE uploads and local copies are written there as on the GS;
 *     a texture is converted from it to a PSP texture (8888 / T8 / T4 + an 8888 palette) on first use and cached;
 *     a cache entry is valid while the pages it reads have not been written (per-page write counters); after a write,
 *     a hash of the texture's own GS blocks (and its palette's) finds a copy made from the same data, so the textures
 *     the game re-uploads every frame are converted once;
 *   - only draws into the display buffers are rendered (FRAME FBP = a buffer the PCRTC shows); draws into other
 *     render targets (shadow / effect buffers) and draws that sample a render target are skipped for now;
 *   - drawing: GE through mode (screen coordinates, no transform), the PS2's 640 x 512 frame scaled to 480 x 272;
 *     16-bit depth from the GS Z (Z24 >> 8, Z32 >> 16), depth test / mask, alpha test, the blend equation mapped to
 *     the GE's blend factors (common cases; the rest approximated), texture functions modulate / decal, TCC;
 *     PS2 colour / alpha 128 = 1.0 is doubled for the GE.
 *   - Not yet: perspective-correct textures (through mode is affine), fog, DATE, render targets, mipmaps, filtering.
 * Display: two 480 x 272 8888 buffers + a 16-bit depth buffer in VRAM; one GE list per frame, swapped at the game's
 * frame edge (hle_gs_lite_flip). */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <malloc.h>
#include <pspkernel.h>
extern uint64_t hle_tprof[12];
#ifdef HLE_TPROF_FINE
#define TPROF(k, call) do { uint64_t t0_ = sceKernelGetSystemTimeWide(); call; hle_tprof[k] += sceKernelGetSystemTimeWide() - t0_; } while (0)
#define TNOW() sceKernelGetSystemTimeWide()
#else
#define TPROF(k, call) do { call; } while (0)
#define TNOW() 0
#endif
static uint64_t st_ge_wait_us;
#define GE_SYNC() do { uint64_t t_ = sceKernelGetSystemTimeWide(); sceGuSync(0, 0); st_ge_wait_us += sceKernelGetSystemTimeWide() - t_; } while (0)
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include "hle.h"
#include "hle_cfg.h"
#include "gs_mem.h"
#include "hle_mem_psp.h"

/* big blocks: the volatile partition when the runtime holds it (ps2rc.cfg mem_volatile=1), else free user memory
   (the runtime's newlib heap is only 768 KB). NULL when there is no room (the caller degrades). */
static uint64_t st_alloc_kb;
void *hle_ge_gs_alloc(size_t n, const char *what)
{
    void *p = hle_mem_vol_take(n, 512 * 1024);
    if (!p) {
        SceUID id = sceKernelAllocPartitionMemory(2, what, PSP_SMEM_High, n + 64, NULL);
        if (id >= 0) p = (void *)(((uintptr_t)sceKernelGetBlockHeadAddr(id) + 63u) & ~(uintptr_t)63u);
    }
    if (p) st_alloc_kb += n >> 10;
    else LOG("GE-GS: no memory for %s (%u KB; free %u KB)\n", what, (unsigned)(n >> 10), (unsigned)(sceKernelTotalFreeMemSize() >> 10));
    return p;
}

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512
#define LIST_BYTES (384 * 1024)
#define LIST_HIGH (LIST_BYTES - 48 * 1024)
/* the texture pool: 4 MB in the guest code hole when the runtime has it (the translated game never touches its old
   code region), else 2 MB from the volatile partition / user memory */
static uint32_t TEX_POOL = 2048 * 1024;
void *hle_psp_hole_take(size_t size, size_t align) __attribute__((weak));
unsigned hle_psp_hole_free_kb(void) __attribute__((weak));
#define TEX_MAX 2048

/* GS local memory, sparse: 8 KB pages allocated on the first write (on the PSP only textures, palettes and image
   transfers are written here - the frame and depth buffers are the GE's - so ~1.5 of the 4 MB are used) */
int hle_psp_screen_taken __attribute__((weak));   /* hle/psp/hle_psp.c (absent in the replayer) */
static uint8_t *gsm_pg[512];
static uint8_t gsm_zero[8192] __attribute__((aligned(64))), gsm_scratch[8192] __attribute__((aligned(64)));
static uint8_t *pg_blk; static int pg_left;
static uint64_t st_pages, st_page_fail;
static uint8_t *pg_new(uint32_t i)
{
    if (!pg_left) {
        pg_blk = (uint8_t *)hle_ge_gs_alloc(32 * 8192, "ge_gs_pages");
        if (!pg_blk) { st_page_fail++; return gsm_scratch; }
        pg_left = 32;
    }
    uint8_t *p = pg_blk; pg_blk += 8192; pg_left--;
    memset(p, 0, 8192);
    gsm_pg[i] = p; st_pages++;
    return p;
}
static inline const uint8_t *R8(uint32_t a) { const uint8_t *p = gsm_pg[(a >> 13) & 511]; return (p ? p : gsm_zero) + (a & 0x1FFF); }
static inline uint8_t *W8(uint32_t a) { uint32_t i = (a >> 13) & 511; uint8_t *p = gsm_pg[i]; if (!p) p = pg_new(i); return p + (a & 0x1FFF); }
static uint32_t page_gen[512];                         /* write counter per 8 KB GS page */
static uint64_t st_tex_sig, st_tex_resets, st_tex_bytes, st_tex_c[3], st_tex_rehit;
static unsigned int __attribute__((aligned(64))) *lists[2];
static int cur_list, in_frame, ge_ready;
static int state_dirty = 1;
static void *vfb[2], *vzb;
static int vfb_draw;

typedef struct { uint64_t tex0, clamp, xyoff, tex1, scissor, alpha, test, frame, zbuf, fba; } Ctx;
static Ctx cx[2];
static uint64_t r_prim, r_prmodecont = 1, r_prmode, r_rgbaq, r_st, r_uv, r_fog, r_fogcol, r_texa, r_colclamp = 1, r_pabe,
    r_bitblt, r_trxpos, r_trxreg;
static float q_cur = 1.0f;

typedef struct { float u, v; uint32_t c; float x, y, z; } GV;          /* GU_TEXTURE_32BITF|COLOR_8888|VERTEX_32BITF */
#define GV_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define GV_FMT3 (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
typedef struct { float x, y, s, t, q, u, v; uint32_t z; uint8_t r, g, b, a, f; } Vtx;
static Vtx vq[3];
static int nv;

/* batch of vertices with one state */
#define BATCH_MAX 4096
static GV batch[BATCH_MAX];
static int nb, batch_prim = -1;
/* perspective batches (GE 3D mode, input (sx w, sy w, w), w = 1/q): the GS depth of the batch is A q + B (16-bit
   units) and its fog F / 255 = fa w + fb, both fitted from the vertices; a vertex that does not fit starts a new batch */
static int b3d, b3d_fog;                 /* b3d: 1 fitted screen-space batch, 2 direct mesh (model matrix bM) */
static ScePspFMatrix4 bM; static uint64_t st_direct; static int model_set;
static float bA, bB, bFa, bFb;
static uint64_t st_prims3d, st_fit_fail;

static uint64_t st_prims, st_skipped_rt, st_skipped_tex, st_tex_conv, st_tex_hit, st_lists, st_frames, st_uploads, st_flush_full;
static int on = -1;
static uint32_t disp_fbp[2] = {0xFFFFFFFF, 0xFFFFFFFF};   /* the display buffers (blocks), learned at flips */
static uint32_t rt_fbp[16]; static int n_rt;              /* other render targets seen */

static int psm_bpp(int psm)
{
    switch (psm) {
    case 0x00: return 32; case 0x01: return 24; case 0x02: case 0x0A: return 16;
    case 0x13: return 8; case 0x14: return 4; case 0x1B: return 8; case 0x24: return 4; case 0x2C: return 4;
    case 0x30: return 32; case 0x31: return 24; case 0x32: case 0x3A: return 16;
    default: return 32;
    }
}
/* pages touched by a w x h rectangle at (x, y) of a buffer (bp blocks, bw) in format psm: bump their counters */
static void page_dims(int psm, int *pw, int *ph)
{
    int b = psm_bpp(psm);
    if (b >= 24) { *pw = 64; *ph = 32; } else if (b == 16) { *pw = 64; *ph = 64; } else if (b == 8) { *pw = 128; *ph = 64; } else { *pw = 128; *ph = 128; }
    if (psm == 0x1B || psm == 0x24 || psm == 0x2C) { *pw = 64; *ph = 32; }          /* stored in 32-bit pixels */
}
static uint32_t pages_sum(uint32_t bp, uint32_t bw, int psm, int x, int y, int w, int h, int bump)
{
    int pw, ph; page_dims(psm, &pw, &ph);
    int bwp = (int)bw * 64 / pw; if (bwp < 1) bwp = 1;
    uint32_t sum = 0;
    for (int py = y / ph; py <= (y + h - 1) / ph; py++)
        for (int px = x / pw; px <= (x + w - 1) / pw; px++) {
            uint32_t p = (bp / 32 + (uint32_t)(py * bwp + px)) & 511;
            if (bump) page_gen[p]++;
            sum += page_gen[p] * 2654435761u + p;
            if (bp % 32) { uint32_t p2 = (p + 1) & 511; if (bump) page_gen[p2]++; sum += page_gen[p2] * 2654435761u + p2; }
        }
    return sum;
}

/* ------------------------------------------------------------------ GS memory writes (same layouts as the GS) */
static int xfer_on, xfer_x, xfer_y, xfer_w, xfer_h, xfer_i, x_bp, x_bw, x_psm;
typedef struct { uint32_t bp, bw, frame; int psm, x, y, w, h; } UpRec;
static UpRec uprec[512]; static uint32_t uprec_n; static uint32_t frame_no;
static uint64_t st_up_match, st_up_match_b, st_up_part;
/* deferred uploads: an image upload from the game's memory is not written into GS memory at once; the 256-byte GS
   blocks it covers are marked as owned by it (own[]). A texture whose blocks all belong to one such upload is found
   in the cache by the upload's source (address + content sample): no copy, no hash of GS memory. Anything that
   reads GS memory first writes ("materializes") the uploads owning the blocks it reads; a materialization only
   writes the blocks the upload still owns (later uploads may have taken some). */
#define NBLK 16384
#define NPEND 2048
static uint16_t own[NBLK];
static int wfilt;                                     /* materializing upload: writes outside its blocks are dropped */
static inline uint8_t *WX(uint32_t a)
{
    if (wfilt && own[(a >> 8) & (NBLK - 1)] != wfilt) return gsm_scratch + (a & 0x1FFF);
    return W8(a);
}
static void put_px(int bp, int bw, int psm, int x, int y, uint32_t v)
{
    int nbt;
    uint32_t a = gs_addr_px(psm, (uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y, &nbt);
    if (nbt == 4) { if (psm == 0x01 || psm == 0x31) memcpy(WX(a), &v, 3); else memcpy(WX(a), &v, 4); return; }
    if (nbt == 2) { uint16_t h = (uint16_t)v; memcpy(WX(a), &h, 2); return; }
    switch (psm) {
    case 0x13: *WX(gs_addr8((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y)) = (uint8_t)v; break;
    case 0x14: { uint32_t n = gs_addr4((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y); uint8_t *b = WX(n >> 1);
                 *b = (n & 1) ? (uint8_t)((*b & 0x0F) | (v << 4)) : (uint8_t)((*b & 0xF0) | (v & 15)); } break;
    case 0x1B: *WX(gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y) + 3) = (uint8_t)v; break;
    case 0x24: case 0x2C: { uint32_t a2 = gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y);
                 int sh = psm == 0x24 ? 24 : 28; uint32_t w; uint8_t *pw = WX(a2); memcpy(&w, pw, 4); w = (w & ~(15u << sh)) | ((v & 15) << sh); memcpy(pw, &w, 4); } break;
    default: break;
    }
}
static uint32_t get_px(int bp, int bw, int psm, int x, int y)
{
    int nbt;
    uint32_t a = gs_addr_px(psm, (uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y, &nbt), v = 0;
    if (nbt == 4) { memcpy(&v, R8(a), 4); return (psm == 0x01 || psm == 0x31) ? v & 0xFFFFFF : v; }
    if (nbt == 2) { uint16_t h; memcpy(&h, R8(a), 2); return h; }
    switch (psm) {
    case 0x13: return *R8(gs_addr8((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y));
    case 0x14: { uint32_t n = gs_addr4((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y); return (*R8(n >> 1) >> ((n & 1) * 4)) & 15; }
    case 0x1B: return *R8(gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y) + 3);
    case 0x24: case 0x2C: { uint32_t w; memcpy(&w, R8(gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y)), 4); return (w >> (psm == 0x24 ? 24 : 28)) & 15; }
    }
    return 0;
}
static void def_start(void); static void def_close(void); static void def_rw(int dir, int sbp, int sbw, int spsm, int ssax, int ssay);
static void xfer_start(int dir)
{
    def_close();
    int sbp = (int)(r_bitblt & 0x3FFF), sbw = (int)((r_bitblt >> 16) & 0x3F), spsm = (int)((r_bitblt >> 24) & 0x3F);
    x_bp = (int)((r_bitblt >> 32) & 0x3FFF); x_bw = (int)((r_bitblt >> 48) & 0x3F); x_psm = (int)((r_bitblt >> 56) & 0x3F);
    int ssax = (int)(r_trxpos & 0x7FF), ssay = (int)((r_trxpos >> 16) & 0x7FF);
    xfer_x = (int)((r_trxpos >> 32) & 0x7FF); xfer_y = (int)((r_trxpos >> 48) & 0x7FF);
    xfer_w = (int)(r_trxreg & 0xFFF); xfer_h = (int)((r_trxreg >> 32) & 0xFFF);
    xfer_i = 0;
    if (xfer_w <= 0 || xfer_h <= 0) return;
    pages_sum((uint32_t)x_bp, (uint32_t)x_bw, x_psm, xfer_x, xfer_y, xfer_w, xfer_h, 1);
    def_rw(dir, sbp, sbw, spsm, ssax, ssay);
    if (dir == 0) {
        xfer_on = 1; st_uploads++;
        def_start();
        UpRec *u = &uprec[uprec_n++ & 511];
        u->bp = (uint32_t)x_bp; u->bw = (uint32_t)x_bw; u->psm = x_psm; u->x = xfer_x; u->y = xfer_y; u->w = xfer_w; u->h = xfer_h; u->frame = frame_no;
        return;
    }
    if (dir == 2)
        for (int y = 0; y < xfer_h; y++)
            for (int x = 0; x < xfer_w; x++)
                put_px(x_bp, x_bw, x_psm, xfer_x + x, xfer_y + y, get_px(sbp, sbw, spsm, ssax + x, ssay + y));
}
static int xfer_cx, xfer_cy;                         /* the next pixel of the transfer */
/* the common formats without divisions / format switches per pixel */
static int xfer_fast(const uint8_t *q)
{
    int total = xfer_w * xfer_h, x1 = xfer_x + xfer_w;
    uint32_t bp = (uint32_t)x_bp, bw = (uint32_t)x_bw;
    if (xfer_i == 0) { xfer_cx = xfer_x; xfer_cy = xfer_y; }
#define XADV() do { if (++xfer_cx >= x1) { xfer_cx = xfer_x; xfer_cy++; } } while (0)
    /* a whole qword inside one block row (aligned, not crossing the rectangle's edge): one block address, then the
       column tables only */
    {
        int per = x_psm == 0x00 ? 4 : x_psm == 0x02 ? 8 : x_psm == 0x13 ? 16 : x_psm == 0x14 ? 32 : 0;
        if (per && (xfer_cx & (per - 1)) == 0 && xfer_cx + per <= x1 && xfer_i + per <= total) {
            uint32_t cx = (uint32_t)xfer_cx, cy = (uint32_t)xfer_cy;
            switch (x_psm) {
            case 0x00: { uint32_t a = gs_addr32(bp, bw, cx, cy); uint8_t *pb = WX(a & ~255u); const uint8_t *col = gs_col32[cy & 7];
                         for (int k = 0; k < 4; k++) memcpy(pb + col[(cx + k) & 7] * 4u, q + 4 * k, 4); } break;
            case 0x02: { uint32_t a = gs_addr16(bp, bw, cx, cy); uint8_t *pb = WX(a & ~255u); const uint8_t *col = gs_col16[cy & 7];
                         for (int k = 0; k < 8; k++) memcpy(pb + col[(cx + k) & 15] * 2u, q + 2 * k, 2); } break;
            case 0x13: { uint32_t a = gs_addr8(bp, bw, cx, cy); uint8_t *pb = WX(a & ~255u); const uint8_t *col = gs_col8[cy & 15];
                         for (int k = 0; k < 16; k++) pb[col[k]] = q[k]; } break;
            case 0x14: { uint32_t n = gs_addr4(bp, bw, cx, cy); uint8_t *pb = WX((n >> 1) & ~255u); const uint16_t *col = gs_col4[cy & 15];
                         for (int k = 0; k < 32; k++) {
                             uint32_t c = col[k]; uint8_t v = (uint8_t)((q[k >> 1] >> ((k & 1) * 4)) & 15), *b = pb + (c >> 1);
                             *b = (c & 1) ? (uint8_t)((*b & 0x0F) | (v << 4)) : (uint8_t)((*b & 0xF0) | v);
                         } } break;
            }
            xfer_i += per; xfer_cx += per;
            if (xfer_cx >= x1) { xfer_cx = xfer_x; xfer_cy++; }
            if (xfer_i >= total) xfer_on = 0;
            return 1;
        }
    }
    switch (x_psm) {
    case 0x00:
        for (int k = 0; k < 4 && xfer_i < total; k++, xfer_i++) { memcpy(WX(gs_addr32(bp, bw, (uint32_t)xfer_cx, (uint32_t)xfer_cy)), q + 4 * k, 4); XADV(); }
        break;
    case 0x02:
        for (int k = 0; k < 8 && xfer_i < total; k++, xfer_i++) { memcpy(WX(gs_addr16(bp, bw, (uint32_t)xfer_cx, (uint32_t)xfer_cy)), q + 2 * k, 2); XADV(); }
        break;
    case 0x13:
        for (int k = 0; k < 16 && xfer_i < total; k++, xfer_i++) { *WX(gs_addr8(bp, bw, (uint32_t)xfer_cx, (uint32_t)xfer_cy)) = q[k]; XADV(); }
        break;
    case 0x14:
        for (int k = 0; k < 32 && xfer_i < total; k++, xfer_i++) {
            uint32_t n = gs_addr4(bp, bw, (uint32_t)xfer_cx, (uint32_t)xfer_cy); uint8_t *b = WX(n >> 1);
            uint8_t v = (uint8_t)((q[k >> 1] >> ((k & 1) * 4)) & 15);
            *b = (n & 1) ? (uint8_t)((*b & 0x0F) | (v << 4)) : (uint8_t)((*b & 0xF0) | v);
            XADV();
        }
        break;
    default: return 0;
    }
#undef XADV
    if (xfer_i >= total) xfer_on = 0;
    return 1;
}
static uint64_t st_xq_raw, st_xq_fast, st_xq_def;          /* image qwords: written per pixel, by xfer_fast, deferred */
static void xfer_data_raw(const uint8_t *q)
{
    if (!xfer_on) return;
    if (xfer_fast(q)) { st_xq_fast++; return; }
    st_xq_raw++;
    int total = xfer_w * xfer_h, bpp = psm_bpp(x_psm);
    if (bpp == 24) {
        static uint8_t carry[3]; static int nc;
        for (int b = 0; b < 16; b++) {
            carry[nc++] = q[b];
            if (nc == 3) { nc = 0; if (xfer_i < total) put_px(x_bp, x_bw, x_psm, xfer_x + xfer_i % xfer_w, xfer_y + xfer_i / xfer_w, carry[0] | carry[1] << 8 | carry[2] << 16); xfer_i++; }
        }
        return;
    }
    int n = bpp == 32 ? 4 : bpp == 16 ? 8 : bpp == 8 ? 16 : 32;
    for (int k = 0; k < n && xfer_i < total; k++, xfer_i++) {
        uint32_t v;
        if (bpp == 32) memcpy(&v, q + 4 * k, 4);
        else if (bpp == 16) { uint16_t h; memcpy(&h, q + 2 * k, 2); v = h; }
        else if (bpp == 8) v = q[k];
        else v = (q[k >> 1] >> ((k & 1) * 4)) & 15;
        put_px(x_bp, x_bw, x_psm, xfer_x + xfer_i % xfer_w, xfer_y + xfer_i / xfer_w, v);
    }
    if (xfer_i >= total) xfer_on = 0;
}


/* ------------------------------------------------------------------ deferred uploads (see own[] above) */
typedef struct { const uint8_t *p; uint32_t n; } Run;
#define NRUN 16
typedef struct {
    Run run[NRUN]; int nrun;
    uint32_t nq, ck, bp, bw; int psm, x, y, w, h;
    int nown;                                         /* blocks it still owns (0: written or dead) */
    int nblk;                                         /* the blocks it took when made */
} Pend;
static Pend pend[NPEND]; static uint32_t pend_head;
static int d_on, region_why = 2;                                      /* the current upload is being collected */
static Pend d_cur; static uint32_t d_need;            /* qwords the upload needs */
static int defer_on = -1;
static uint64_t st_mat_c[6];
static uint64_t st_def_inc, st_def_new, st_def_up, st_def_eager, st_def_mat, st_def_mat_blk, st_def_dead, st_def_stale, st_def_flush, st_src_hit, st_src_miss, st_src_mixed;
typedef struct { const uint8_t *p; uint32_t ck, id; } Seen;
static uint64_t st_def_reuse;                         /* uploads identical to the pending one they repeat: kept as is */
static Seen seen_src[16384];
typedef struct { int on, x, y, w, h, i, bp, bw, psm, cx, cy; } XS;
static XS xs_save(void) { XS s = {xfer_on, xfer_x, xfer_y, xfer_w, xfer_h, xfer_i, x_bp, x_bw, x_psm, xfer_cx, xfer_cy}; return s; }
static void xs_load(const XS *s) { xfer_on = s->on; xfer_x = s->x; xfer_y = s->y; xfer_w = s->w; xfer_h = s->h; xfer_i = s->i; x_bp = s->bp; x_bw = s->bw; x_psm = s->psm; xfer_cx = s->cx; xfer_cy = s->cy; }
static inline int def_psm(int psm) { return psm == 0x00 || psm == 0x02 || psm == 0x13 || psm == 0x14; }
/* formats whose block layout blk_of knows (the others make every pending upload be written first) */
static inline int blk_psm(int psm) { return psm == 0x00 || psm == 0x01 || psm == 0x02 || psm == 0x0A || psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C; }
static inline void cell_dims(int psm, int *bx, int *by)
{
    switch (psm) {
    case 0x02: case 0x0A: *bx = 16; *by = 8; break;
    case 0x13: *bx = 16; *by = 16; break;
    case 0x14: *bx = 32; *by = 16; break;
    default: *bx = 8; *by = 8; break;
    }
}
static inline uint32_t blk_of(int psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t a;
    switch (psm) {
    case 0x02: a = gs_addr16(bp, bw, x, y); break;
    case 0x0A: a = gs_addr16t(gs_blk16s, bp, bw, x, y); break;
    case 0x13: a = gs_addr8(bp, bw, x, y); break;
    case 0x14: a = gs_addr4(bp, bw, x, y) >> 1; break;
    default: a = gs_addr32(bp, bw, x, y); break;
    }
    return (a >> 8) & (NBLK - 1);
}
static uint32_t def_ck(const Pend *p)                 /* a sample of the source: first / last and 14 qwords between */
{
    uint32_t h = 2166136261u ^ p->nq;
    for (int r = 0; r < p->nrun; r++) {
        const Run *u = &p->run[r];
        uint32_t step = u->n > 16 ? u->n / 15 : 1;
        for (uint32_t k = 0; k < u->n; k += step) { const uint32_t *w = (const uint32_t *)(u->p + 16 * k); h = (h ^ (w[0] + w[1] * 3u + w[2] * 5u + w[3] * 7u)) * 16777619u; }
        const uint32_t *w = (const uint32_t *)(u->p + 16 * (u->n - 1)); h = (h ^ (w[0] + w[1] * 3u + w[2] * 5u + w[3] * 7u)) * 16777619u;
    }
    return h;
}
static void def_write(const Pend *p, int filt)        /* the upload's data through the normal writers */
{
    XS sv = xs_save();
    x_bp = (int)p->bp; x_bw = (int)p->bw; x_psm = p->psm; xfer_x = p->x; xfer_y = p->y; xfer_w = p->w; xfer_h = p->h; xfer_i = 0; xfer_on = 1;
    wfilt = filt;
    for (int r = 0; r < p->nrun && xfer_on; r++) for (uint32_t k = 0; k < p->run[r].n && xfer_on; k++) xfer_data_raw(p->run[r].p + 16 * k);
    wfilt = 0;
    xs_load(&sv);
}
static uint64_t st_cell_fast;
static void cell_put(Pend *p, int id, uint32_t bw, int x, int y, uint32_t b);
static int cellput_on = -1;
static void pend_mat(int id)                          /* write upload id's blocks */
{
    Pend *p = &pend[id];
    if (p->nown <= 0) return;
    if (def_ck(p) != p->ck) st_def_stale++;           /* its source changed since (best effort) */
    st_def_mat++;
    int bx, by; cell_dims(p->psm, &bx, &by);
    uint32_t bw = p->bw ? p->bw : 1;
    if (cellput_on < 0) { const char *e = hle_cfg_get("x_cellput"); cellput_on = e ? atoi(e) : 1; }
    if (cellput_on) {                                 /* the cells it still owns, each straight into its block */
        for (int y = p->y & ~(by - 1); y < p->y + p->h && p->nown > 0; y += by)
            for (int x = p->x & ~(bx - 1); x < p->x + p->w && p->nown > 0; x += bx) { uint32_t b = blk_of(p->psm, p->bp, bw, (uint32_t)x, (uint32_t)y); if (own[b] == id) cell_put(p, id, bw, x, y, b); }
        p->nown = 0;
        return;
    }
    st_def_mat_blk += (uint64_t)p->nown;
    def_write(p, id);
    for (int y = p->y & ~(by - 1); y < p->y + p->h; y += by)
        for (int x = p->x & ~(bx - 1); x < p->x + p->w; x += bx) { uint32_t b = blk_of(p->psm, p->bp, bw, (uint32_t)x, (uint32_t)y); if (own[b] == id) own[b] = 0; }
    p->nown = 0;
}
/* write only block b of upload id (a later upload covers part of it): the pixels of the upload's cell there */
static uint32_t def_src_px(const Pend *p, uint32_t idx, int bpp)
{
    uint32_t bit = idx * (uint32_t)bpp, q = bit >> 7, byte = (bit >> 3) & 15;
    for (int r = 0; r < p->nrun; r++) {
        if (q < p->run[r].n) {
            const uint8_t *d = p->run[r].p + 16 * q + byte;
            switch (bpp) {
            case 32: { uint32_t v; memcpy(&v, d, 4); return v; }
            case 16: { uint16_t v; memcpy(&v, d, 2); return v; }
            case 8: return *d;
            default: return (*d >> (bit & 4)) & 15;
            }
        }
        q -= p->run[r].n;
    }
    return 0;
}
/* write cell (x, y) = block b of upload id. A cell the upload covers whole (deferred formats) goes straight into its
   256-byte block through the column table; the others pixel by pixel */
static void cell_put(Pend *p, int id, uint32_t bw, int x, int y, uint32_t b)
{
    int bx, by; cell_dims(p->psm, &bx, &by);
    int bpp = psm_bpp(p->psm);
    int x0 = x < p->x ? p->x : x, y0 = y < p->y ? p->y : y, x1 = x + bx > p->x + p->w ? p->x + p->w : x + bx, y1 = y + by > p->y + p->h ? p->y + p->h : y + by;
    uint8_t *blk = 0;
    if (x0 == x && y0 == y && x1 == x + bx && y1 == y + by && def_psm(p->psm)) {
        uint32_t a;
        switch (p->psm) {
        case 0x00: a = gs_addr32(p->bp, bw, (uint32_t)x, (uint32_t)y); break;
        case 0x02: a = gs_addr16(p->bp, bw, (uint32_t)x, (uint32_t)y); break;
        case 0x13: a = gs_addr8(p->bp, bw, (uint32_t)x, (uint32_t)y); break;
        default: a = gs_addr4(p->bp, bw, (uint32_t)x, (uint32_t)y) >> 1; break;
        }
        blk = W8(a & ~255u); st_cell_fast++;
    }
    for (int yy = y0; yy < y1; yy++) {
        /* the row's source pixels: one run lookup for the row when it lies in one run */
        uint32_t i0 = (uint32_t)((yy - p->y) * p->w + (x0 - p->x)), cnt = (uint32_t)(x1 - x0);
        uint32_t b0 = i0 * (uint32_t)bpp, b1 = (i0 + cnt) * (uint32_t)bpp - 1, q0 = b0 >> 7, q1 = b1 >> 7;
        const uint8_t *rowp = 0;
        for (int r = 0; r < p->nrun; r++) {
            if (q0 < p->run[r].n) { if (q1 < p->run[r].n) rowp = p->run[r].p; break; }
            q0 -= p->run[r].n; q1 -= p->run[r].n;
        }
        if (!rowp) {
            for (int xx = x0; xx < x1; xx++) put_px((int)p->bp, (int)bw, p->psm, xx, yy, def_src_px(p, (uint32_t)((yy - p->y) * p->w + (xx - p->x)), bpp));
            continue;
        }
        uint32_t bit = b0 - ((b0 >> 7) - q0) * 128;              /* (q0 is the qword inside the run) */
        if (blk) {
            int ry = yy - y;
            switch (p->psm) {
            case 0x00: { const uint8_t *c = gs_col32[ry]; const uint8_t *d = rowp + (bit >> 3);
                         for (int k = 0; k < 8; k++) memcpy(blk + 4 * c[k], d + 4 * k, 4); } break;
            case 0x02: { const uint8_t *c = gs_col16[ry]; const uint8_t *d = rowp + (bit >> 3);
                         for (int k = 0; k < 16; k++) memcpy(blk + 2 * c[k], d + 2 * k, 2); } break;
            case 0x13: { const uint8_t *c = gs_col8[ry]; const uint8_t *d = rowp + (bit >> 3);
                         for (int k = 0; k < 16; k++) blk[c[k]] = d[k]; } break;
            default: { const uint16_t *c = gs_col4[ry];
                       for (int k = 0; k < 32; k++) { uint32_t n = c[k], bk = bit + 4u * (uint32_t)k; uint8_t v = (uint8_t)((rowp[bk >> 3] >> (bk & 4)) & 15); uint8_t *o = blk + (n >> 1);
                                                      *o = (n & 1) ? (uint8_t)((*o & 0x0F) | (v << 4)) : (uint8_t)((*o & 0xF0) | v); } } break;
            }
            continue;
        }
        for (int xx = x0; xx < x1; xx++, bit += (uint32_t)bpp) {
            const uint8_t *d = rowp + (bit >> 3);
            uint32_t v;
            switch (bpp) {
            case 32: memcpy(&v, d, 4); break;
            case 16: { uint16_t h; memcpy(&h, d, 2); v = h; } break;
            case 8: v = *d; break;
            default: v = (*d >> (bit & 4)) & 15; break;
            }
            put_px((int)p->bp, (int)bw, p->psm, xx, yy, v);
        }
    }
    own[b] = 0; p->nown--; st_def_mat_blk++;
    (void)id;
}
/* hx, hy >= 0: the cell's corner when the caller knows it in the upload's own layout (same bp / bw / psm) */
static void pend_mat_cell_at(int id, uint32_t b, int hx, int hy)
{
    Pend *p = &pend[id];
    if (p->nown <= 0 || own[b] != id) return;
    int bx, by; cell_dims(p->psm, &bx, &by);
    uint32_t bw = p->bw ? p->bw : 1;
    int ys = p->y & ~(by - 1), xs = p->x & ~(bx - 1);
    if (hx >= 0 && hx >= xs && hy >= ys && hx < p->x + p->w && hy < p->y + p->h && blk_of(p->psm, p->bp, bw, (uint32_t)hx, (uint32_t)hy) == b) { ys = hy; xs = hx; }
    for (int y = ys; y < p->y + p->h; y += by)
        for (int x = (y == ys ? xs : p->x & ~(bx - 1)); x < p->x + p->w; x += bx)
            if (blk_of(p->psm, p->bp, bw, (uint32_t)x, (uint32_t)y) == b) { cell_put(p, id, bw, x, y, b); return; }
    own[b] = 0; p->nown--;                            /* (not found: cannot happen) */
}
static void pend_mat_cell(int id, uint32_t b) { pend_mat_cell_at(id, b, -1, -1); }
static void pend_flush_all(void)
{
    for (int i = 1; i < NPEND; i++) if (pend[i].nown > 0) pend_mat(i);
    st_def_flush++;
}
/* write the pending uploads owning blocks of a rectangle (before reading it) */
static void region_need(uint32_t bp, uint32_t bw, int psm, int x0, int y0, int w, int h)
{
    if (!blk_psm(psm)) { pend_flush_all(); return; }
    int bx, by; cell_dims(psm, &bx, &by);
    if (!bw) bw = 1;
    for (int y = y0 & ~(by - 1); y < y0 + h; y += by)
        for (int x = x0 & ~(bx - 1); x < x0 + w; x += bx) { int o = own[blk_of(psm, bp, bw, (uint32_t)x, (uint32_t)y)]; if (o) { if (pend[o].nown > 0) st_mat_c[2 + region_why]++; pend_mat(o); } }
}
/* -1: some block not pending / mixed owners / unknown layout; else the one pending upload owning every block */
static int region_owner(uint32_t bp, uint32_t bw, int psm, int w, int h)
{
    if (!blk_psm(psm)) return -1;
    int bx, by; cell_dims(psm, &bx, &by);
    if (!bw) bw = 1;
    int o = -2;
    for (int y = 0; y < h; y += by)
        for (int x = 0; x < w; x += bx) {
            int b = own[blk_of(psm, bp, bw, (uint32_t)x, (uint32_t)y)];
            if (!b || (o >= 0 && b != o)) return -1;
            o = b;
        }
    return o < 0 ? -1 : o;
}
/* a write into a rectangle (written now): blocks wholly covered stop belonging to pending uploads; pending uploads
   owning partly covered blocks are written first */
static uint64_t st_own_pages, st_own_bad;
static void own_take(uint32_t bp, uint32_t bw, int psm, int x0, int y0, int w, int h, int id)
{
    if (!blk_psm(psm)) { pend_flush_all(); return; }
    int bx, by; cell_dims(psm, &bx, &by);
    if (!bw) bw = 1;
    /* whole pages: their 32 blocks follow the block of the page's corner (no address per block) */
    int pw, ph; page_dims(psm, &pw, &ph);
    int fx0 = (x0 + pw - 1) / pw * pw, fy0 = (y0 + ph - 1) / ph * ph, fx1 = (x0 + w) / pw * pw, fy1 = (y0 + h) / ph * ph;
    if (fx1 <= fx0 || fy1 <= fy0) fx0 = fx1 = fy0 = fy1 = 0;
    for (int py = fy0; py < fy1; py += ph)
        for (int px = fx0; px < fx1; px += pw) {
            uint32_t pb = blk_of(psm, bp, bw, (uint32_t)px, (uint32_t)py);
            if (st_own_pages < 512) {                 /* (checked on the first pages: every cell inside pb .. pb + 31) */
                for (int yy = py; yy < py + ph; yy += by) for (int xx = px; xx < px + pw; xx += bx)
                    if (((blk_of(psm, bp, bw, (uint32_t)xx, (uint32_t)yy) - pb) & (NBLK - 1)) >= 32) st_own_bad++;
            }
            st_own_pages++;
            for (uint32_t j = 0; j < 32; j++) {
                uint32_t b = (pb + j) & (NBLK - 1);
                int o = own[b];
                if (o && o != id) { if (--pend[o].nown == 0) st_def_dead++; }
                if (id) { if (own[b] != id) { own[b] = (uint16_t)id; pend[id].nown++; } }
                else own[b] = 0;
            }
        }
    for (int y = y0 & ~(by - 1); y < y0 + h; y += by)
        for (int x = x0 & ~(bx - 1); x < x0 + w; x += bx) {
            if (x >= fx0 && x < fx1 && y >= fy0 && y < fy1) continue;   /* (done as a whole page) */
            uint32_t b = blk_of(psm, bp, bw, (uint32_t)x, (uint32_t)y);
            int full = x >= x0 && y >= y0 && x + bx <= x0 + w && y + by <= y0 + h;
            int o = own[b];
            if (o && o != id) {
                if (full) { if (--pend[o].nown == 0) st_def_dead++; }
                else {
                    if (pend[o].nown > 0) {
                        st_mat_c[1]++;
                    }
                    if (pend[o].psm == psm && pend[o].bp == bp && (pend[o].bw ? pend[o].bw : 1) == bw) pend_mat_cell_at(o, b, x, y);
                    else pend_mat_cell(o, b);
                }
            }
            if (id) { if (own[b] != id) { own[b] = (uint16_t)id; pend[id].nown++; } }
            else own[b] = 0;
        }
}
static void def_finish(void)                          /* the collected upload: pending, or written now */
{
    d_on = 0;
    if (!d_cur.nrun) return;
    d_cur.nq = 0; for (int r = 0; r < d_cur.nrun; r++) d_cur.nq += d_cur.run[r].n;
    d_cur.ck = def_ck(&d_cur);
    int complete = d_cur.nq >= d_need;
    /* only sources seen before with the same content are deferred (game data that stays, not buffers refilled) */
    uint32_t si = ((uint32_t)(((uintptr_t)d_cur.run[0].p >> 4) * 2654435761u) >> 18) & ~1u, sj = si;
    int again = 0;
    if (seen_src[si].p == d_cur.run[0].p) again = seen_src[si].ck == d_cur.ck;
    else if (seen_src[si + 1].p == d_cur.run[0].p) { sj = si + 1; again = seen_src[sj].ck == d_cur.ck; }
    else { seen_src[si + 1] = seen_src[si]; }         /* newest in the first way */
    uint32_t prev = seen_src[sj].p == d_cur.run[0].p ? seen_src[sj].id : 0;
    seen_src[sj].p = d_cur.run[0].p; seen_src[sj].ck = d_cur.ck; seen_src[sj].id = 0;
    if (!complete) st_def_inc++; else if (!again) st_def_new++;
    if (complete && again && prev && prev < NPEND) {
        /* the same source, content sample and rectangle as the pending upload it repeats, which still owns all its
           blocks: the GS memory it describes is already this one - keep it (no new entry, no block walk) */
        const Pend *o = &pend[prev];
        int same = o->nown > 0 && o->nown == o->nblk && o->ck == d_cur.ck && o->nq == d_cur.nq && o->nrun == d_cur.nrun && o->bp == d_cur.bp &&
                   o->bw == d_cur.bw && o->psm == d_cur.psm && o->x == d_cur.x && o->y == d_cur.y && o->w == d_cur.w && o->h == d_cur.h;
        for (int r = 0; same && r < d_cur.nrun; r++) same = o->run[r].p == d_cur.run[r].p && o->run[r].n == d_cur.run[r].n;
        if (same) { seen_src[sj].id = prev; st_def_reuse++; st_def_up++; xfer_on = 0; return; }
    }
    if (!complete || !again) {
        own_take(d_cur.bp, d_cur.bw, d_cur.psm, d_cur.x, d_cur.y, d_cur.w, d_cur.h, 0);
        def_write(&d_cur, 0); st_def_eager++;
        if (complete) xfer_on = 0;
        return;
    }
    uint32_t id = pend_head % (NPEND - 1) + 1; pend_head++;
    if (pend[id].nown > 0) { st_mat_c[0]++; pend_mat((int)id); }
    pend[id] = d_cur; pend[id].nown = 0;
    own_take(d_cur.bp, d_cur.bw, d_cur.psm, d_cur.x, d_cur.y, d_cur.w, d_cur.h, (int)id);
    pend[id].nblk = pend[id].nown;
    seen_src[sj].id = id;
    st_def_up++;
    xfer_on = 0;
}
static void def_close(void) { if (d_on) { d_need = 0xFFFFFFFFu; def_finish(); } }
static void def_rw(int dir, int sbp, int sbw, int spsm, int ssax, int ssay)
{
    if (defer_on < 0) { const char *e = hle_cfg_get("x_defer"); defer_on = !(e && e[0] == '0');
#ifdef GE_NODEFER
        defer_on = 0;
#endif
    }
    if (dir == 1) { pend_flush_all(); return; }
    if (dir == 2) region_need((uint32_t)sbp, (uint32_t)sbw, spsm, ssax, ssay, xfer_w, xfer_h);
    if (dir == 2 || !defer_on || !def_psm(x_psm)) own_take((uint32_t)x_bp, (uint32_t)x_bw, x_psm, xfer_x, xfer_y, xfer_w, xfer_h, 0);
}
static void def_start(void)
{
    if (!defer_on || !def_psm(x_psm)) return;
    d_on = 1; d_cur.nrun = 0; d_cur.nq = 0;
    d_cur.bp = (uint32_t)x_bp; d_cur.bw = (uint32_t)x_bw; d_cur.psm = x_psm; d_cur.x = xfer_x; d_cur.y = xfer_y; d_cur.w = xfer_w; d_cur.h = xfer_h;
    d_need = (uint32_t)(((uint64_t)xfer_w * xfer_h * (uint32_t)psm_bpp(x_psm) + 127) / 128);
}
static void xfer_data(const uint8_t *q)
{
    if (d_on) {
        Run *u = d_cur.nrun ? &d_cur.run[d_cur.nrun - 1] : 0;
        if (u && u->p + 16 * u->n == q) u->n++;
        else if (d_cur.nrun < NRUN && (!hle_spr || q < hle_spr || q >= hle_spr + 16384)) { d_cur.run[d_cur.nrun].p = q; d_cur.run[d_cur.nrun].n = 1; d_cur.nrun++; }
        else {                                        /* scattered / scratchpad: what came is written now, the rest directly */
            d_need = 0xFFFFFFFFu; def_finish();
            wfilt = -1;                               /* move the writer past the qwords def_finish wrote */
            for (int r = 0; r < d_cur.nrun; r++) for (uint32_t k = 0; k < d_cur.run[r].n; k++) xfer_data_raw(d_cur.run[r].p + 16 * k);
            wfilt = 0;
            xfer_data_raw(q);
            return;
        }
        if (++d_cur.nq >= d_need) def_finish();
        return;
    }
    xfer_data_raw(q);
}

static void xfer_span(const uint8_t *q, uint32_t m)       /* m contiguous qwords */
{
    while (m && d_on) {
        Run *u = d_cur.nrun ? &d_cur.run[d_cur.nrun - 1] : 0;
        uint32_t take = d_need - d_cur.nq; if (take > m) take = m;
        if (u && u->p + 16 * u->n == q && (!hle_spr || q < hle_spr || q >= hle_spr + 16384)) { st_xq_def += take; u->n += take; d_cur.nq += take; q += 16 * take; m -= take; if (d_cur.nq >= d_need) def_finish(); continue; }
        xfer_data(q); q += 16; m--;                   /* a new run (or the fallback) */
    }
    for (; m; m--, q += 16) xfer_data(q);
}
/* ------------------------------------------------------------------ texture cache */
typedef struct {
    uint64_t key0, key1;               /* tex0 fields that matter, texa for 16/24-bit */
    uint32_t gen, cgen, last, sig, csig;
    uint8_t *data; uint32_t *clut;
    int psp_psm, w, h, bufw, nclut;
    uint32_t off, size;                /* its block in the pool (palette + pixels) */
    int live, next;                    /* in use; next entry of its hash chain (-1 = none) */
    uint32_t uid;
} Tex;
typedef struct { uint64_t t, k1; uint32_t gen, cgen, uid; int ix; } TMemo;
static TMemo tmemo[64]; static uint32_t tex_uid;
/* the pool is a ring: a new copy goes after the previous one and evicts whatever it overlaps (oldest first); an
   entry used in the current frame is only evicted after the GE has finished the list (copies of earlier frames are
   free: every flip waits for the GE). Entries are found through a hash of (TEX0 key, palette key). */
static Tex texs[TEX_MAX];
static int ntex, tex_free = -1, tex_bucket[1024];
static uint8_t *tpool; static uint32_t tp_head;
static uint64_t st_tex_evict, st_tex_ring_sync, st_tex_again, st_tex_again_b, st_tex_full;
static uint32_t tsz_key[64]; static uint64_t tsz_n[64], tsz_b[64];
static inline int tex_hash(uint64_t k0, uint64_t k1) { uint64_t h = (k0 ^ (k1 * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull; return (int)(h >> 54) & 1023; }
static void tex_unlink(int i)
{
    int b = tex_hash(texs[i].key0, texs[i].key1), *pp = &tex_bucket[b];
    while (*pp >= 0 && *pp != i) pp = &texs[*pp].next;
    if (*pp == i) *pp = texs[i].next;
    texs[i].live = 0; texs[i].next = tex_free; tex_free = i;
}
static void ge_drain(void);
static int tex_ring_alloc(uint32_t n, uint32_t *off)
{
    n = (n + 63) & ~63u;
    if (n > TEX_POOL) return 0;
    /* second chance: copies used in this frame or the previous one are stepped over (the head moves past them);
       only cold copies are evicted. After a full turn without room, the oldest go anyway. */
    uint32_t a = tp_head, moved = 0;
    for (int tries = 0; tries < 256; tries++) {
        if (a + n > TEX_POOL) { moved += TEX_POOL - a; a = 0; }
        uint32_t b = a + n, hot_end = 0;
        for (int i = 0; i < ntex; i++)
            if (texs[i].live && texs[i].off < b && a < texs[i].off + texs[i].size && texs[i].last + 1 >= frame_no) {
                uint32_t e = texs[i].off + texs[i].size; if (e > hot_end) hot_end = e;
            }
        if (!hot_end) break;
        moved += hot_end - a; a = hot_end;
        if (moved >= TEX_POOL) { a = tp_head; if (a + n > TEX_POOL) a = 0; st_tex_full++; break; }
    }
    uint32_t b = a + n;
    int busy = 0;
    for (int i = 0; i < ntex; i++)
        if (texs[i].live && texs[i].off < b && a < texs[i].off + texs[i].size && texs[i].last == frame_no) busy = 1;
    if (busy) { ge_drain(); st_tex_ring_sync++; }
    for (int i = 0; i < ntex; i++)
        if (texs[i].live && texs[i].off < b && a < texs[i].off + texs[i].size) { tex_unlink(i); st_tex_evict++; }
    tp_head = b; *off = a;
    return 1;
}
static Tex *tex_slot(void)
{
    if (tex_free >= 0) { int i = tex_free; tex_free = texs[i].next; return &texs[i]; }
    if (ntex < TEX_MAX) return &texs[ntex++];
    /* every slot live: drop the oldest copy */
    int o = 0; for (int i = 1; i < ntex; i++) if (texs[i].last < texs[o].last) o = i;
    if (texs[o].last == frame_no) { ge_drain(); st_tex_ring_sync++; }
    tex_unlink(o); st_tex_evict++;
    int i = tex_free; tex_free = texs[i].next; return &texs[i];
}
static inline uint32_t dbl(uint32_t c)                 /* PS2 alpha 0..0x80 -> GE 0..0xFF */
{
    uint32_t a = c >> 24; a = a >= 0x80 ? 0xFF : a * 2;
    return (c & 0xFFFFFF) | a << 24;
}
static uint32_t c16(uint16_t c)
{
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3, a;
    int aem = (int)((r_texa >> 15) & 1);
    a = (c & 0x8000) ? (uint32_t)((r_texa >> 32) & 0xFF) : (aem && !(c & 0x7FFF)) ? 0 : (uint32_t)(r_texa & 0xFF);
    return r | g << 8 | b << 16 | a << 24;
}
static uint32_t clut_raw(uint32_t cbp, int cpsm, int p)
{
    if (cpsm == 0x00 || cpsm == 0x01) { uint32_t v; memcpy(&v, R8(gs_addr32(cbp, 1, (uint32_t)(p & 15), (uint32_t)(p >> 4))), 4); return v; }
    uint16_t h;
    if (cpsm == 0x0A) memcpy(&h, R8(gs_addr16t(gs_blk16s, cbp, 1, (uint32_t)(p & 15), (uint32_t)(p >> 4))), 2);
    else memcpy(&h, R8(gs_addr16(cbp, 1, (uint32_t)(p & 15), (uint32_t)(p >> 4))), 2);
    return c16(h);
}
static void cache_reset_synced(void);
/* convert the GS texture into x (data / clut already allocated) */
static void tex_fill_(Tex *x, uint32_t tbp, uint32_t tbw, int psm, uint32_t cbp, int cpsm, int csa);
static void tex_fill(Tex *x, uint32_t tbp, uint32_t tbw, int psm, uint32_t cbp, int cpsm, int csa)
{ tex_fill_(x, tbp, tbw, psm, cbp, cpsm, csa); }
static void tex_fill_(Tex *x, uint32_t tbp, uint32_t tbw, int psm, uint32_t cbp, int cpsm, int csa)
{
    int w = x->w, h = x->h, bufw = x->bufw;
    st_tex_conv++;
    if (x->psp_psm == GU_PSM_8888) {
        uint32_t *d = (uint32_t *)x->data;
        int aem = (int)((r_texa >> 15) & 1);
        for (int y = 0; y < h; y++)
            for (int xx = 0; xx < w; xx++) {
                uint32_t raw = get_px((int)tbp, (int)tbw, psm, xx, y), c;
                if (psm == 0x00 || psm == 0x30) c = raw;
                else if (psm == 0x01 || psm == 0x31) c = (raw & 0xFFFFFF) | ((aem && !(raw & 0xFFFFFF)) ? 0u : (uint32_t)(r_texa & 0xFF)) << 24;
                else c = c16((uint16_t)raw);
                d[y * bufw + xx] = dbl(c);
            }
        sceKernelDcacheWritebackRange(d, (unsigned)(bufw * h * 4));
        return;
    }
    uint8_t *d = x->data;
    if (x->psp_psm == GU_PSM_T8) {
        if (psm == 0x13 && (w & 15) == 0 && (h & 15) == 0) {   /* PSMT8 by 16x16 blocks */
            uint32_t bw_ = tbw ? tbw : 1;
            for (int by = 0; by < h; by += 16)
                for (int bx = 0; bx < w; bx += 16) {
                    const uint8_t *pb = R8(gs_addr8(tbp, bw_, (uint32_t)bx, (uint32_t)by) & ~255u);
                    for (int y = 0; y < 16; y++) { const uint8_t *col = gs_col8[y]; uint8_t *row = d + (by + y) * bufw + bx; for (int xx = 0; xx < 16; xx++) row[xx] = pb[col[xx]]; }
                }
        } else if (psm == 0x13) {                        /* PSMT8: straight from the swizzle tables */
            uint32_t bw_ = tbw ? tbw : 1;
            for (int y = 0; y < h; y++) { uint8_t *row = d + y * bufw; for (int xx = 0; xx < w; xx++) row[xx] = *R8(gs_addr8(tbp, bw_, (uint32_t)xx, (uint32_t)y)); }
        } else
        for (int y = 0; y < h; y++) for (int xx = 0; xx < w; xx++) d[y * bufw + xx] = (uint8_t)get_px((int)tbp, (int)tbw, psm, xx, y);
        for (int i = 0; i < 256; i++) { int p = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); x->clut[i] = dbl(clut_raw(cbp, cpsm, p)); }
        sceKernelDcacheWritebackRange(d, (unsigned)(bufw * h));
    } else {
        memset(d, 0, (size_t)bufw * h / 2);
        if (psm == 0x14 && (w & 31) == 0 && (h & 15) == 0) {   /* PSMT4 by 32x16 blocks: one address per block */
            uint32_t bw_ = tbw ? tbw : 1;
            for (int by = 0; by < h; by += 16)
                for (int bx = 0; bx < w; bx += 32) {
                    const uint8_t *pb = R8((gs_addr4(tbp, bw_, (uint32_t)bx, (uint32_t)by) >> 1) & ~255u);
                    for (int y = 0; y < 16; y++) {
                        const uint16_t *col = gs_col4[y];
                        uint8_t *row = d + (((by + y) * bufw + bx) >> 1);
                        for (int xx = 0; xx < 32; xx += 2) {
                            uint32_t n0 = col[xx], n1 = col[xx + 1];
                            uint32_t a = (pb[n0 >> 1] >> ((n0 & 1) * 4)) & 15, b = (pb[n1 >> 1] >> ((n1 & 1) * 4)) & 15;
                            row[xx >> 1] = (uint8_t)(a | b << 4);
                        }
                    }
                }
        } else if (psm == 0x14) {                        /* PSMT4: two pixels per output byte */
            uint32_t bw_ = tbw ? tbw : 1;
            for (int y = 0; y < h; y++) {
                uint8_t *row = d + (y * bufw >> 1);
                for (int xx = 0; xx + 1 < w; xx += 2) {
                    uint32_t n0 = gs_addr4(tbp, bw_, (uint32_t)xx, (uint32_t)y), n1 = gs_addr4(tbp, bw_, (uint32_t)xx + 1, (uint32_t)y);
                    uint32_t a = (*R8(n0 >> 1) >> ((n0 & 1) * 4)) & 15, b = (*R8(n1 >> 1) >> ((n1 & 1) * 4)) & 15;
                    row[xx >> 1] = (uint8_t)(a | b << 4);
                }
            }
        } else
        for (int y = 0; y < h; y++) for (int xx = 0; xx < w; xx++) {
            uint32_t v = get_px((int)tbp, (int)tbw, psm, xx, y);
            d[(y * bufw + xx) >> 1] |= (uint8_t)(v << ((xx & 1) * 4));
        }
        for (int i = 0; i < 16; i++) { int ii = i + csa * 16; int p = (ii & ~0x18) | ((ii & 0x08) << 1) | ((ii & 0x10) >> 1); x->clut[i] = dbl(clut_raw(cbp, cpsm, p)); }
        sceKernelDcacheWritebackRange(d, (unsigned)(bufw * h / 2));
    }
    sceKernelDcacheWritebackRange(x->clut, x->psp_psm == GU_PSM_T8 ? 1024 : 64);
}

/* hash of the GS blocks a w x h texture (or palette) at bp reads */
static uint32_t blocks_sig(uint32_t bp, uint32_t bw, int psm, int w, int h)
{
    int bx, by;
    switch (psm) {
    case 0x02: case 0x0A: bx = 16; by = 8; break;
    case 0x13: bx = 16; by = 16; break;
    case 0x14: bx = 32; by = 16; break;
    default: bx = 8; by = 8; break;
    }
    if (!bw) bw = 1;
    uint32_t h32 = 2166136261u;
    st_tex_sig++;
    for (int y = 0; y < h; y += by)
        for (int x = 0; x < w; x += bx) {
            uint32_t a;
            switch (psm) {
            case 0x02: a = gs_addr16(bp, bw, (uint32_t)x, (uint32_t)y); break;
            case 0x0A: a = gs_addr16t(gs_blk16s, bp, bw, (uint32_t)x, (uint32_t)y); break;
            case 0x13: a = gs_addr8(bp, bw, (uint32_t)x, (uint32_t)y); break;
            case 0x14: a = gs_addr4(bp, bw, (uint32_t)x, (uint32_t)y) >> 1; break;
            default: a = gs_addr32(bp, bw, (uint32_t)x, (uint32_t)y); break;
            }
            const uint32_t *q = (const uint32_t *)R8(a & ~255u);
            for (int i = 0; i < 64; i += 4) h32 = (h32 ^ (q[i] + q[i + 1] * 3u + q[i + 2] * 5u + q[i + 3] * 7u)) * 16777619u;
        }
    return h32;
}
/* the PSP texture for TEX0; 0 when it cannot be made. Entries are never rewritten in place (the GE may still be
   reading them): a stale entry gets new memory; a full pool is reset after the GE has finished. */
static Tex *texture_(uint64_t t, int *scale_u, int *scale_v);
extern volatile int hle_zone;
#define ZN(z, stmt) do { int pz_ = hle_zone; hle_zone = (z); stmt; hle_zone = pz_; } while (0)
static Tex *texture(uint64_t t, int *scale_u, int *scale_v)
{ Tex *r_; ZN(11, r_ = texture_(t, scale_u, scale_v)); return r_; }
static Tex *texture_(uint64_t t, int *scale_u, int *scale_v)
{
    uint32_t tbp = (uint32_t)(t & 0x3FFF), tbw = (uint32_t)((t >> 14) & 0x3F);
    int psm = (int)((t >> 20) & 0x3F);
    int tw = 1 << ((t >> 26) & 15), th = 1 << ((t >> 30) & 15);
    uint32_t cbp = (uint32_t)((t >> 37) & 0x3FFF); int cpsm = (int)((t >> 51) & 15), csa = (int)((t >> 56) & 31);
    int w = tw > 512 ? 512 : tw, h = th > 512 ? 512 : th;
    *scale_u = tw / w; *scale_v = th / h;
    int is4 = psm == 0x14 || psm == 0x24 || psm == 0x2C, is8 = psm == 0x13 || psm == 0x1B;
    uint64_t k0 = t & 0x3FFFFFFFFull;                                   /* TBP TBW PSM TW TH */
    uint64_t k1 = (is4 || is8) ? ((uint64_t)cbp | (uint64_t)cpsm << 14 | (uint64_t)(is4 ? csa : 0) << 18) : 0;
    if (psm == 0x01 || psm == 0x02 || psm == 0x0A || ((is4 || is8) && cpsm >= 2)) k1 |= (r_texa & 0xFF00FFFFull) << 24;
    uint64_t k1o = k1;
    uint32_t gen = pages_sum(tbp, tbw ? tbw : 1, psm, 0, 0, tw, th, 0);
    uint32_t cgen = (is4 || is8) ? pages_sum(cbp, 1, cpsm, 0, 0, 16, is4 ? 2 : 16, 0) : 0;
    Tex *x = 0;
    int hb = tex_hash(k0, k1);
    for (int i = tex_bucket[hb]; i >= 0; i = texs[i].next)
        if (texs[i].key0 == k0 && texs[i].key1 == k1 && texs[i].gen == gen && texs[i].cgen == cgen) {
            texs[i].last = frame_no; st_tex_hit++; return &texs[i];
        }
    /* memo of the last source-keyed answer for this TEX0 (GS pages unchanged since) */
    uint32_t mi = (uint32_t)((t ^ (t >> 29) ^ k1) * 0x9E3779B1u) >> 26;
    if (tmemo[mi].t == t && tmemo[mi].k1 == k1 && tmemo[mi].gen == gen && tmemo[mi].cgen == cgen) {
        Tex *m = &texs[tmemo[mi].ix];
        if (m->live && m->uid == tmemo[mi].uid) { m->last = frame_no; st_tex_hit++; return m; }
    }
    /* every block of the texture (and of its palette) from one pending upload: keyed by the uploads' sources */
    uint64_t sk0 = 0, sk1 = 0; int srck = 0;
    if (defer_on > 0) {
        int ow = region_owner(tbp, tbw ? tbw : 1, psm, w, h);
        if (ow > 0) {
            int cow = 0, cok = 1;
            if (is8) cow = region_owner(cbp, 1, cpsm, 16, 16);
            else if (is4) {
                int ii0 = csa * 16, ii1 = csa * 16 + 8;
                int p0 = (ii0 & ~0x18) | ((ii0 & 0x08) << 1) | ((ii0 & 0x10) >> 1), p1 = (ii1 & ~0x18) | ((ii1 & 0x08) << 1) | ((ii1 & 0x10) >> 1);
                if (!blk_psm(cpsm)) cok = 0;
                else {
                    int o0 = own[blk_of(cpsm, cbp, 1, (uint32_t)(p0 & 15), (uint32_t)(p0 >> 4))], o1 = own[blk_of(cpsm, cbp, 1, (uint32_t)(p1 & 15), (uint32_t)(p1 >> 4))];
                    cow = o0 == o1 ? o0 : -1;
                }
            }
            if (cow < 0 || !cok) st_src_mixed++;
            else {
                const Pend *P = &pend[ow];
                uint64_t h = (uint64_t)(uintptr_t)P->run[0].p * 0x9E3779B97F4A7C15ull;
                h ^= ((uint64_t)P->ck << 32 | P->nq) * 0xBF58476D1CE4E5B9ull;
                h ^= ((uint64_t)P->bw << 48 | (uint64_t)P->psm << 40 | (uint64_t)P->x << 20 | (uint64_t)P->y) * 0x94D049BB133111EBull;
                h ^= ((uint64_t)((tbp - P->bp) & 0x3FFF) << 34 | (k0 >> 14)) * 0xD6E8FEB86659FD93ull;
                sk0 = h | 1ull << 63;
                if (is4 || is8) {
                    if (cow > 0) {
                        const Pend *V = &pend[cow];
                        uint64_t c = (uint64_t)(uintptr_t)V->run[0].p * 0x9E3779B97F4A7C15ull;
                        c ^= ((uint64_t)V->ck << 32 | V->nq) * 0xBF58476D1CE4E5B9ull;
                        c ^= ((uint64_t)((cbp - V->bp) & 0x3FFF) << 32 | (uint64_t)V->x << 16 | (uint64_t)V->y) * 0x94D049BB133111EBull;
                        sk1 = c ^ ((uint64_t)cpsm << 8 | (uint64_t)(is4 ? csa : 0)) ^ (k1 & ~0x3FFFFull);
                    } else {                                  /* palette in GS memory: its used entries */
                        uint32_t cs = 2166136261u;
                        for (int i = 0; i < (is4 ? 16 : 256); i++) {
                            int ii = is4 ? i + csa * 16 : i, pi = (ii & ~0x18) | ((ii & 0x08) << 1) | ((ii & 0x10) >> 1);
                            cs = (cs ^ clut_raw(cbp, cpsm, pi)) * 16777619u;
                        }
                        sk1 = ((uint64_t)cs << 24 | (uint64_t)cpsm << 8 | (uint64_t)(is4 ? csa : 0)) ^ (k1 & ~0x3FFFFull) ^ 0x5555ull << 48;
                    }
                } else sk1 = k1;
                srck = 1;
                int sb = tex_hash(sk0, sk1);
                for (int i = tex_bucket[sb]; i >= 0; i = texs[i].next)
                    if (texs[i].key0 == sk0 && texs[i].key1 == sk1) {
                        texs[i].last = frame_no; st_src_hit++;
                        tmemo[mi].t = t; tmemo[mi].k1 = k1; tmemo[mi].gen = gen; tmemo[mi].cgen = cgen; tmemo[mi].ix = i; tmemo[mi].uid = texs[i].uid;
                        return &texs[i];
                    }
                st_src_miss++;
            }
        }
        /* GS memory is read from here on: write what is pending there */
        region_why = 0; region_need(tbp, tbw ? tbw : 1, psm, 0, 0, w, h);
        region_why = 1; if (is4 || is8) region_need(cbp, 1, cpsm, 0, 0, 16, 16);
        region_why = 2;
    }
    /* pages written since: the same data again? */
    uint32_t sig = blocks_sig(tbp, tbw, psm, w, h);
    uint32_t csig = 0;                                  /* the palette entries the texture uses (not its neighbours) */
    if (is4 || is8) {
        csig = 2166136261u;
        for (int i = 0; i < (is4 ? 16 : 256); i++) {
            int ii = is4 ? i + csa * 16 : i, pi = (ii & ~0x18) | ((ii & 0x08) << 1) | ((ii & 0x10) >> 1);
            csig = (csig ^ clut_raw(cbp, cpsm, pi)) * 16777619u;
        }
    }
    if (!srck)
    for (int i = tex_bucket[hb]; i >= 0; i = texs[i].next)
        if (texs[i].key0 == k0 && texs[i].key1 == k1 && texs[i].sig == sig && texs[i].csig == csig) {
            texs[i].gen = gen; texs[i].cgen = cgen; texs[i].last = frame_no; st_tex_rehit++; return &texs[i];
        }
    if (srck) { k0 = sk0; k1 = sk1; hb = tex_hash(k0, k1); }
    int pp = is4 ? GU_PSM_T4 : is8 ? GU_PSM_T8 : GU_PSM_8888;
    int bufw = pp == GU_PSM_T4 ? (w < 32 ? 32 : w) : pp == GU_PSM_T8 ? (w < 16 ? 16 : w) : (w < 4 ? 4 : w);
    uint32_t bytes = pp == GU_PSM_8888 ? (uint32_t)bufw * h * 4 : pp == GU_PSM_T8 ? (uint32_t)bufw * h : (uint32_t)bufw * h / 2;
    uint32_t cbytes = is8 ? 1024 : is4 ? 64 : 0, off;
    if (bytes + cbytes > TEX_POOL) return 0;
    x = tex_slot();
    if (!tex_ring_alloc(cbytes + bytes, &off)) { x->live = 0; x->next = tex_free; tex_free = (int)(x - texs); return 0; }
    uint32_t *cl = cbytes ? (uint32_t *)(tpool + off) : 0;
    uint8_t *data = tpool + off + cbytes;
    x->off = off; x->size = (cbytes + bytes + 63) & ~63u; x->live = 1;
    x->next = tex_bucket[hb]; tex_bucket[hb] = (int)(x - texs);
    x->uid = ++tex_uid; if (srck) { tmemo[mi].t = t; tmemo[mi].k1 = k1o; tmemo[mi].gen = gen; tmemo[mi].cgen = cgen; tmemo[mi].ix = (int)(x - texs); tmemo[mi].uid = x->uid; }
    x->key0 = k0; x->key1 = k1; x->gen = gen; x->cgen = cgen; x->last = frame_no; x->sig = sig; x->csig = csig;
    x->data = data; x->clut = cl; x->psp_psm = pp; x->w = w; x->h = h; x->bufw = bufw;
    st_tex_bytes += bytes; st_tex_c[pp == GU_PSM_8888 ? 0 : pp == GU_PSM_T8 ? 1 : 2]++;
    {   /* diagnostics: was this exact content converted before (evicted = a capacity miss) or is it new? */
        static uint32_t seen[16384];
        uint32_t h = (uint32_t)(k0 * 2654435761u) ^ (uint32_t)(k1 * 40503u) ^ sig ^ (csig * 31u); if (!h) h = 1;
        uint32_t i = h & 16383;
        int found = 0;
        for (int t = 0; t < 8; t++) { uint32_t j = (i + (uint32_t)t) & 16383; if (seen[j] == h) { found = 1; break; } if (!seen[j]) { seen[j] = h; break; } }
        if (found) { st_tex_again++; st_tex_again_b += bytes; }
        /* would a whole upload (same base, format, at 0,0, covering the texture) have given it? */
        {
            int m = 0;
            for (int t = 0; t < 512 && t < (int)uprec_n; t++) {
                const UpRec *u = &uprec[(uprec_n - 1 - (uint32_t)t) & 511];
                if (u->bp == tbp && u->psm == psm) { m = (u->x == 0 && u->y == 0 && u->w >= w && u->h >= h) ? 1 : 2; break; }
            }
            if (m == 1) { st_up_match++; st_up_match_b += bytes; } else if (m == 2) st_up_part++;
        }
        /* sizes converted (report: the 10 biggest by volume) */
        uint32_t key = (uint32_t)pp << 24 | (uint32_t)(w & 0xFFF) << 12 | (uint32_t)(h & 0xFFF);
        for (int t = 0; t < 64; t++) { if (tsz_key[t] == key || !tsz_key[t]) { tsz_key[t] = key; tsz_n[t]++; tsz_b[t] += bytes; break; } }
    }
    tex_fill(x, tbp, tbw, psm, cbp, cpsm, csa);
    return x;
}

/* ------------------------------------------------------------------ GE frame */
static void ge_init(void)
{
    lists[0] = (unsigned int *)hle_ge_gs_alloc(LIST_BYTES, "ge_list0");
    lists[1] = (unsigned int *)hle_ge_gs_alloc(LIST_BYTES, "ge_list1");
    tpool = 0;
    /* the guest code hole: 2.5 MB of textures (keyed by their uploads' sources, a frame's set fits), the rest for
       the converted meshes (hle_vu1_native.h) */
    if (hle_psp_hole_take && hle_psp_hole_free_kb && hle_psp_hole_free_kb() >= 2560) {
        tpool = (uint8_t *)hle_psp_hole_take(2560 * 1024, 64);
        if (tpool) { TEX_POOL = 2560 * 1024; LOG("GE-GS: texture pool of 2560 KB in the guest code hole\n"); }
    }
    if (!tpool) tpool = (uint8_t *)hle_ge_gs_alloc(TEX_POOL, "ge_textures");
    for (int i = 0; i < 1024; i++) tex_bucket[i] = -1;
    if (!lists[0] || !lists[1] || !tpool) { on = 0; return; }
    vfb[0] = (void *)0; vfb[1] = (void *)(BUF_W * SCR_H * 4); vzb = (void *)(2 * BUF_W * SCR_H * 4);
    sceGuInit();
    sceGuStart(GU_DIRECT, lists[0]);
    sceGuDrawBuffer(GU_PSM_8888, vfb[0], BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, vfb[1], BUF_W);
    sceGuDepthBuffer(vzb, BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(0, 65535);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuClearColor(0); sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    GE_SYNC();
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    vfb_draw = 0;
    ge_ready = 1;
    hle_psp_screen_taken = 1;                      /* the runtime's text console stays off the screen from now on */
}
/* what the GE holds already (set again only when it changes: ~1000 batches a frame); invalid at each list start */
static int sh_valid, sh_fog; static ScePspFMatrix4 sh_proj, sh_model; static float sh_near, sh_far; static unsigned sh_fogcol;
static void frame_begin(void)
{
    if (!ge_ready) ge_init();
    if (!ge_ready) return;
    sceGuStart(GU_DIRECT, lists[cur_list]);
    in_frame = 1; sh_valid = 0;
    st_lists++;
    {
        /* view: z negated (the GE's fog uses view-space z, negative in front); the projection undoes it */
        ScePspFMatrix4 id; memset(&id, 0, sizeof id); id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_MODEL, &id); model_set = 0;
        id.z.z = -1.0f;
        sceGuSetMatrix(GU_VIEW, &id);
        sceGuDisable(GU_CLIP_PLANES);
    }
}
static void batch_flush(void);
static void list_check(void)
{
    if (sceGuCheckList() > LIST_HIGH) {                 /* list nearly full: run it and continue in the other */
        batch_flush();
        sceGuFinish(); GE_SYNC();
        cur_list ^= 1;
        sceGuStart(GU_DIRECT, lists[cur_list]);
        st_flush_full++; st_lists++;
    }
}

static void ge_drain(void)                     /* run the list so far and wait: the GE is done with every texture */
{
    if (in_frame) {
        batch_flush();
        sceGuFinish(); GE_SYNC();
        cur_list ^= 1;
        sceGuStart(GU_DIRECT, lists[cur_list]);
        st_lists++;
        state_dirty = 1;
    }
}
static void cache_reset_synced(void)
{
    ge_drain();
    ntex = 0; tex_free = -1; tp_head = 0; st_tex_resets++;
    for (int i = 0; i < 1024; i++) tex_bucket[i] = -1;
}

/* ------------------------------------------------------------------ state -> GE */
static Ctx *Cc;
static int cur_tme, cur_fst, cur_abe, cur_iip, cur_tw, cur_th, cur_su, cur_sv;

static int is_rt(uint32_t bp)
{
    for (int i = 0; i < n_rt; i++) if (rt_fbp[i] == bp) return 1;
    return 0;
}
static void note_rt(uint32_t fbp_blocks)
{
    if (fbp_blocks == disp_fbp[0] || fbp_blocks == disp_fbp[1] || is_rt(fbp_blocks)) return;
    if (n_rt < 16) rt_fbp[n_rt++] = fbp_blocks;
}
static int blend_factor_src(int c, int inv, uint32_t *fix, int fixv)
{
    if (c == 0) return inv ? GU_ONE_MINUS_SRC_ALPHA : GU_SRC_ALPHA;
    if (c == 1) return inv ? GU_ONE_MINUS_DST_ALPHA : GU_DST_ALPHA;
    uint32_t f = (uint32_t)(fixv >= 0x80 ? 0xFF : fixv * 2); if (inv) f = 255 - f;
    *fix = f | f << 8 | f << 16;
    return GU_FIX;
}
/* returns 0 when the primitive is not to be drawn */
static int apply_state(void)
{
    uint64_t at = (r_prmodecont & 1) ? r_prim : r_prmode;
    Ctx *c = &cx[(at >> 9) & 1];
    uint32_t fbp = (uint32_t)(c->frame & 0x1FF) * 32;
    if (fbp != disp_fbp[0] && fbp != disp_fbp[1]) {
        if (disp_fbp[0] != 0xFFFFFFFF) { note_rt(fbp); st_skipped_rt++; return 0; }
    }
    int tme = (int)((at >> 4) & 1);
    Tex *tx = 0;
    if (tme) {
        uint32_t tbp = (uint32_t)(c->tex0 & 0x3FFF);
        if (tbp == disp_fbp[0] || tbp == disp_fbp[1] || is_rt(tbp)) { st_skipped_tex++; return 0; }
        tx = texture(c->tex0, &cur_su, &cur_sv);
        if (!tx) { st_skipped_tex++; return 0; }
    }
    Cc = c;
    cur_tme = tme; cur_fst = (int)((at >> 8) & 1); cur_abe = (int)((at >> 6) & 1); cur_iip = (int)((at >> 3) & 1);
    cur_tw = 1 << ((c->tex0 >> 26) & 15); cur_th = 1 << ((c->tex0 >> 30) & 15);
    /* scissor (PS2 frame -> PSP screen) */
    int sx0 = (int)(c->scissor & 0x7FF), sx1 = (int)((c->scissor >> 16) & 0x7FF);
    int sy0 = (int)((c->scissor >> 32) & 0x7FF), sy1 = (int)((c->scissor >> 48) & 0x7FF);
    int px0 = sx0 * SCR_W / 640, px1 = (sx1 + 1) * SCR_W / 640, py0 = sy0 * SCR_H / 512, py1 = (sy1 + 1) * SCR_H / 512;
    if (px1 > SCR_W) px1 = SCR_W;
    if (py1 > SCR_H) py1 = SCR_H;
    sceGuScissor(px0, py0, px1, py1);
    /* texture */
    if (tme) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(tx->psp_psm, 0, 0, 0);
        if (tx->psp_psm != GU_PSM_8888) {
            sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
            sceGuClutLoad(tx->psp_psm == GU_PSM_T8 ? 32 : 2, tx->clut);
        }
        sceGuTexImage(0, tx->w, tx->h, tx->bufw, tx->data);
        int tfx = (int)((c->tex0 >> 35) & 3), tcc = (int)((c->tex0 >> 34) & 1);
        sceGuTexFunc(tfx == 1 ? GU_TFX_REPLACE : GU_TFX_MODULATE, tcc ? GU_TCC_RGBA : GU_TCC_RGB);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        int wms = (int)(c->clamp & 3), wmt = (int)((c->clamp >> 2) & 3);
        sceGuTexWrap(wms == 1 || wms == 2 ? GU_CLAMP : GU_REPEAT, wmt == 1 || wmt == 2 ? GU_CLAMP : GU_REPEAT);
        sceGuTexScale(1.0f, 1.0f); sceGuTexOffset(0, 0);
        sceGuTexFlush();
    } else sceGuDisable(GU_TEXTURE_2D);
    /* alpha test */
    int ate = (int)(c->test & 1), atst = (int)((c->test >> 1) & 7), aref = (int)((c->test >> 4) & 0xFF), afail = (int)((c->test >> 12) & 3);
    if (ate && afail == 0) {
        static const int fn[8] = {GU_NEVER, GU_ALWAYS, GU_LESS, GU_LEQUAL, GU_EQUAL, GU_GEQUAL, GU_GREATER, GU_NOTEQUAL};
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(fn[atst], aref >= 0x80 ? 0xFF : aref * 2, 0xFF);
    } else if (ate && atst == 0 && afail != 0) {                 /* "never" + fail action: writes only part */
        sceGuDisable(GU_ALPHA_TEST);
    } else sceGuDisable(GU_ALPHA_TEST);
    /* depth */
    int zte = (int)((c->test >> 16) & 1), ztst = (int)((c->test >> 17) & 3), zmsk = (int)((c->zbuf >> 32) & 1);
    if (zte) {
        static const int zf[4] = {GU_NEVER, GU_ALWAYS, GU_GEQUAL, GU_GREATER};
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(zf[ztst]);
        sceGuDepthMask(zmsk || (ate && afail == 2) ? GU_TRUE : GU_FALSE);
    } else sceGuDisable(GU_DEPTH_TEST);
    if (ate && atst == 0 && afail == 1) sceGuDepthMask(GU_TRUE);      /* FB_ONLY */
    /* blend */
    if (cur_abe) {
        int A = (int)(c->alpha & 3), B = (int)((c->alpha >> 2) & 3), C = (int)((c->alpha >> 4) & 3), D = (int)((c->alpha >> 6) & 3);
        int fix = (int)((c->alpha >> 32) & 0xFF);
        uint32_t f1 = 0, f2 = 0;
        int op = GU_ADD, sf, df;
        if (A == 0 && B == 1 && D == 1) { sf = blend_factor_src(C, 0, &f1, fix); df = blend_factor_src(C, 1, &f2, fix); }
        else if (A == 0 && B == 2 && D == 1) { sf = blend_factor_src(C, 0, &f1, fix); df = GU_FIX; f2 = 0xFFFFFF; }
        else if (A == 1 && B == 0 && D == 0) { sf = blend_factor_src(C, 1, &f1, fix); df = blend_factor_src(C, 0, &f2, fix); }
        else if (A == 2 && B == 0 && D == 1) { op = GU_REVERSE_SUBTRACT; sf = blend_factor_src(C, 0, &f1, fix); df = GU_FIX; f2 = 0xFFFFFF; }
        else if (A == 0 && B == 2 && D == 2) { sf = blend_factor_src(C, 0, &f1, fix); df = GU_FIX; f2 = 0; }
        else if (A == 1 && B == 2 && D == 2) { sf = GU_FIX; f1 = 0; df = blend_factor_src(C, 0, &f2, fix); }
        else if (A == 1 && B == 2 && D == 0) { sf = GU_FIX; f1 = 0xFFFFFF; df = blend_factor_src(C, 0, &f2, fix); }
        else if (A == 2 && B == 2 && D == 1) { sf = GU_FIX; f1 = 0; df = GU_FIX; f2 = 0xFFFFFF; }
        else if (A == 0 && B == 1 && D == 2) { op = GU_SUBTRACT; sf = blend_factor_src(C, 0, &f1, fix); df = blend_factor_src(C, 0, &f2, fix); }
        else { sf = GU_SRC_ALPHA; df = GU_ONE_MINUS_SRC_ALPHA; }
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(op, sf, df, f1, f2);
    } else sceGuDisable(GU_BLEND);
    sceGuPixelMask((uint32_t)(c->frame >> 32));
    return 1;
}

/* the 3D state of a perspective batch: projection from the depth line (bA, bB), fog line (bFa, bFb), the model
   matrix (bM for direct meshes, else identity) */
/* GE commands written straight into the display list (the Gu calls send each word through a function call) */
typedef struct { unsigned int *start, *current; int parent_context; } GuDL_;
extern GuDL_ *gu_list;
static inline unsigned int ge_f24(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 8; }
static inline void ge_draw_array(int prim, unsigned int vtype, int n, const void *v)      /* = sceGuDrawArray, no indices */
{
    unsigned int *p = gu_list->current; uint32_t a = (uint32_t)(uintptr_t)v;
    p[0] = 18u << 24 | (vtype & 0xFFFFFF); p[1] = 16u << 24 | ((a >> 8) & 0xF0000); p[2] = 1u << 24 | (a & 0xFFFFFF);
    p[3] = 4u << 24 | (unsigned int)prim << 16 | ((unsigned int)n & 0xFFFF);
    gu_list->current = p + 4;
}
static inline void ge_model_matrix(const ScePspFMatrix4 *m)                              /* = sceGuSetMatrix(GU_MODEL) */
{
    unsigned int *p = gu_list->current; const float *f = (const float *)m;
    *p++ = 58u << 24;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) *p++ = 59u << 24 | ge_f24(f[4 * i + j]);
    gu_list->current = p;
}
static inline int m64ne(const void *a, const void *b)   /* two 4x4 matrices differ (word compare, no memcmp) */
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b; uint32_t d = 0;
    for (int i = 0; i < 16; i++) d |= x[i] ^ y[i];
    return d != 0;
}
static void set_3d_state(int direct)
{
    /* clip = M (X, Y, Z, 1): x = X/240 - Z, y = Z - Y/136, w = Z, z = depth fit (see the comment above) */
    const float c = 32767.5f;
    static float pA = -1.0f, pB = -1.0f; static int pvalid;         /* the projection only depends on bA / bB */
    if (!sh_valid || !pvalid || bA != pA || bB != pB) {
        ScePspFMatrix4 m;
        memset(&m, 0, sizeof m);
        m.x.x = 2.0f / SCR_W;
        m.y.y = -2.0f / SCR_H;
        m.z.x = 1.0f; m.z.y = -1.0f; m.z.z = -(bB - c) / c; m.z.w = -1.0f;   /* input z is -w after the view */
        m.w.z = bA / c;
        if (!sh_valid || m64ne(&m, &sh_proj)) { sceGuSetMatrix(GU_PROJECTION, &m); sh_proj = m; }
        pA = bA; pB = bB; pvalid = 1;
    }
    if (direct) {
        if (!sh_valid || model_set != 1 || m64ne(&bM, &sh_model)) { ge_model_matrix(&bM); sh_model = bM; }
        model_set = 1;
    } else if (model_set) {
        ScePspFMatrix4 id; memset(&id, 0, sizeof id); id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_MODEL, &id); model_set = 0;
    }
    if (b3d_fog) {
        /* GE fog factor = (far - w) / (far - near); the game's F / 255 = fa w + fb */
        float far_ = -bFb / bFa, near_ = far_ + 1.0f / bFa;
        unsigned col = (unsigned int)(r_fogcol & 0xFFFFFF);
        if (!sh_valid || sh_fog != 1 || near_ != sh_near || far_ != sh_far || col != sh_fogcol) {
            sceGuFog(near_, far_, col); sceGuEnable(GU_FOG); sh_near = near_; sh_far = far_; sh_fogcol = col; sh_fog = 1;
        }
    } else if (!sh_valid || sh_fog != 0) { sceGuDisable(GU_FOG); sh_fog = 0; }
    sh_valid = 1;
}
static void batch_flush(void)
{
    if (!nb) return;
    GV *v = (GV *)sceGuGetMemory(nb * (int)sizeof(GV));
    memcpy(v, batch, (size_t)nb * sizeof(GV));
    if (b3d) {
        set_3d_state(0);
        sceGuDrawArray(batch_prim, GV_FMT3, nb, 0, v);
        st_prims3d += (uint64_t)nb / 3;
    } else {
        if (!sh_valid || sh_fog != 0) { sceGuDisable(GU_FOG); sh_fog = 0; }
        sceGuDrawArray(batch_prim, GV_FMT, nb, 0, v);
    }
    nb = 0;
    list_check();
}

/* GS depth -> GE 16-bit depth. This game's depth is ~838000 Q (24-bit), so the useful range is the low ~1/16th:
   Z24 >> 4 (Z32 >> 12) keeps 16 times the precision of a plain 24 -> 16-bit cut; larger values saturate (order kept) */
#define ZSHIFT 4
static inline float z16_raw(const Vtx *s)                     /* unclamped, for the fit */
{
    uint32_t zb = Cc ? (uint32_t)((Cc->zbuf >> 24) & 15) : 0;
    return zb == 0 ? (float)s->z / (float)(1u << (ZSHIFT + 8)) : zb == 1 ? (float)s->z / (float)(1u << ZSHIFT) : (float)(s->z & 0xFFFF);
}
static inline float z16_of(const Vtx *s) { float z = z16_raw(s); return z > 65535.0f ? 65535.0f : z; }
static void gv_of(GV *o, const Vtx *s, const Vtx *colsrc, int mode3d)
{
    float sx = s->x * ((float)SCR_W / 640.0f), sy = s->y * ((float)SCR_H / 512.0f);
    float q = s->q ? s->q : 1.0f;
    if (mode3d) { float w = 1.0f / fabsf(q); o->x = sx * w; o->y = sy * w; o->z = w; }
    else { o->x = sx; o->y = sy; o->z = (float)(uint32_t)z16_of(s); }
    if (cur_tme) {
        if (cur_fst) { o->u = s->u / 16.0f / cur_su; o->v = s->v / 16.0f / cur_sv; }
        else if (mode3d) { o->u = s->s / q; o->v = s->t / q; }                 /* normalised, GE interpolates */
        else { o->u = s->s / q * cur_tw / cur_su; o->v = s->t / q * cur_th / cur_sv; }
    } else { o->u = 0; o->v = 0; }
    const Vtx *cs = cur_iip ? s : colsrc;
    uint32_t r = cs->r, g = cs->g, b = cs->b, a = cs->a;
    a = a >= 0x80 ? 0xFF : a * 2;
    if (cur_tme && !(((Cc->tex0 >> 35) & 3) == 1)) { r = r >= 0x80 ? 0xFF : r * 2; g = g >= 0x80 ? 0xFF : g * 2; b = b >= 0x80 ? 0xFF : b * 2; }
    o->c = r | g << 8 | b << 16 | a << 24;
}

/* depth / fog fit of a triangle for the perspective path: 1 = fits (when fresh: sets the batch's A, B, fog) */
static int fog_known; static float kFa, kFb;                   /* the scene's fog line (last good fit) */
static int fog_ok(const float *ws, const float *fs, float fa, float fb)
{
    for (int i = 0; i < 3; i++) {
        float f = fa * ws[i] + fb; if (f > 1) f = 1; if (f < 0) f = 0;
        float e = f - fs[i]; if (e > 0.03f || e < -0.03f) return 0;
    }
    return 1;
}
static int fit_tri(const Vtx *const *vs, int fresh, int fge)
{
    float qs[3], zs[3], ws[3], fs[3];
    int neg = vs[0]->q < 0;
    for (int i = 0; i < 3; i++) {
        float q = vs[i]->q;
        if (q == 0 || (q < 0) != neg) return 0;
        qs[i] = fabsf(q); zs[i] = z16_raw(vs[i]); ws[i] = 1.0f / qs[i]; fs[i] = (float)vs[i]->f / 255.0f;
        if (zs[i] > 65535.0f || zs[i] < 0) return 0;
    }
    if (fresh) {
        int a = 0, b = 0;
        for (int i = 1; i < 3; i++) { if (qs[i] < qs[a]) a = i; if (qs[i] > qs[b]) b = i; }
        if (qs[b] - qs[a] > qs[b] * 1e-4f) { bA = (zs[b] - zs[a]) / (qs[b] - qs[a]); bB = zs[a] - bA * qs[a]; }
        else { bA = 0; bB = (zs[0] + zs[1] + zs[2]) / 3.0f; }
        b3d_fog = 0;
        if (fge) {
            if (fog_known && fog_ok(ws, fs, kFa, kFb)) { b3d_fog = 1; bFa = kFa; bFb = kFb; }
            else {
                int c = 0, d = 0;
                for (int i = 1; i < 3; i++) { if (ws[i] < ws[c]) c = i; if (ws[i] > ws[d]) d = i; }
                int unclamped = fs[c] > 0.01f && fs[c] < 0.99f && fs[d] > 0.01f && fs[d] < 0.99f;
                if (unclamped && ws[d] - ws[c] > ws[d] * 1e-3f && fs[d] < fs[c]) {
                    float fa = (fs[d] - fs[c]) / (ws[d] - ws[c]), fb = fs[c] - fa * ws[c];
                    if (fog_ok(ws, fs, fa, fb)) { b3d_fog = 1; bFa = kFa = fa; bFb = kFb = fb; fog_known = 1; }
                }
                if (!b3d_fog && !(fs[0] > 0.99f && fs[1] > 0.99f && fs[2] > 0.99f)) return 0;   /* fog needed, no line */
            }
        }
    }
    for (int i = 0; i < 3; i++) { float e = bA * qs[i] + bB - zs[i]; if (e > 2.0f || e < -2.0f) return 0; }
    if (fge) {
        if (b3d_fog) { if (!fog_ok(ws, fs, bFa, bFb)) return 0; }
        else if (!(fs[0] > 0.99f && fs[1] > 0.99f && fs[2] > 0.99f)) return 0;
    }
    return 1;
}

static int skip_state, use_persp = 1;
static void emit_(int prim, const Vtx *const *vs, int n);
static void emit(int prim, const Vtx *const *vs, int n) { emit_(prim, vs, n); }
static void emit_(int prim, const Vtx *const *vs, int n)
{
    if (state_dirty) {
        batch_flush();
        if (!in_frame) { frame_begin(); if (!in_frame) { skip_state = 1; state_dirty = 0; return; } }
        skip_state = !apply_state();
        state_dirty = 0;
        batch_prim = -1;
    }
    if (skip_state) return;
    int fge = (int)((((r_prmodecont & 1) ? r_prim : r_prmode) >> 5) & 1);
    int want3d = use_persp && prim == GU_TRIANGLES && ((cur_tme && !cur_fst) || fge);
    if (want3d) {
        int same = b3d == 1 && batch_prim == prim && nb + n <= BATCH_MAX;
        if (!(same && fit_tri(vs, 0, fge))) {
            batch_flush();
            if (fit_tri(vs, 1, fge)) { b3d = 1; batch_prim = prim; }
            else { st_fit_fail++; want3d = 0; }
        }
    }
    if (!want3d && (b3d || prim != batch_prim || nb + n > BATCH_MAX)) {
        batch_flush();
        b3d = 0;
        batch_prim = prim;
    }
    for (int i = 0; i < n; i++) gv_of(&batch[nb++], vs[i], vs[n - 1], want3d);
    st_prims++;
}

/* ------------------------------------------------------------------ direct meshes
 * The native VU1 programs (hle_vu1_native.inc) hand their triangles here instead of GS packets: vertices in the
 * object's space, C = Mo (x, y, z, 1) the PS2 clip space (screen = (offx, offy) + C / Cw, GS Z = z0 + zK / Cw, fog
 * F / 255 = fa Cw + fb), so the GE does the transform. hle_ge_mesh_begin returns 1 (go on: hle_ge_mesh_draw),
 * 0 (cannot: send GS packets instead) or -1 (the GS path would not draw it either). */
static void init_once(void);
static void gs_reg(int reg, uint64_t v);
static int mesh_begin_(uint64_t prim, const float Mo[4][4], float offx, float offy, float zK, float z0, int fog, float fa, float fb, int *dbl_rgb);
int hle_ge_mesh_begin(uint64_t prim, const float Mo[4][4], float offx, float offy, float zK, float z0, int fog, float fa, float fb, int *dbl_rgb)
{ int r_; ZN(12, r_ = mesh_begin_(prim, Mo, offx, offy, zK, z0, fog, fa, fb, dbl_rgb)); return r_; }
static int mesh_begin_(uint64_t prim, const float Mo[4][4], float offx, float offy, float zK, float z0, int fog, float fa, float fb, int *dbl_rgb)
{
    init_once();
    if (!on) return 0;
    if (prim != r_prim || state_dirty) gs_reg(0x00, prim); else nv = 0;    /* the same PRIM again: nothing changed */
    if (state_dirty) {
        batch_flush();
        if (!in_frame) { frame_begin(); if (!in_frame) { skip_state = 1; state_dirty = 0; return -1; } }
        skip_state = !apply_state();
        state_dirty = 0;
        batch_prim = -1;
    }
    if (skip_state) return -1;
    int fge = (int)((((r_prmodecont & 1) ? r_prim : r_prmode) >> 5) & 1);
    if (fge && !fog) return 0;
    if (cur_fst) return 0;
    uint32_t zb = (uint32_t)((Cc->zbuf >> 24) & 15);
    float zs = zb == 0 ? (float)(1u << (ZSHIFT + 8)) : zb == 1 ? (float)(1u << ZSHIFT) : 1.0f;
    float ox = (float)(Cc->xyoff & 0xFFFF) / 16.0f, oy = (float)((Cc->xyoff >> 32) & 0xFFFF) / 16.0f;
    float sxs = (float)SCR_W / 640.0f, sys = (float)SCR_H / 512.0f;
    batch_flush();
    b3d = 0; batch_prim = -1;
    bA = zK / zs; bB = z0 / zs;
    b3d_fog = fge; bFa = fa; bFb = fb;
    /* X = sxs ((offx - XYOFFSET.x) Cw + Cx), Y = sys ((offy - XYOFFSET.y) Cw + Cy), Z = Cw: the screen-space input of
       the fitted batches (sx w, sy w, w) as a linear function of the object's coordinates */
    float *col[4] = {&bM.x.x, &bM.y.x, &bM.z.x, &bM.w.x};
    for (int r = 0; r < 4; r++) {
        float *o = col[r];
        o[0] = sxs * (offx - ox) * Mo[r][3] + sxs * Mo[r][0];
        o[1] = sys * (offy - oy) * Mo[r][3] + sys * Mo[r][1];
        o[2] = Mo[r][3];
        o[3] = r == 3 ? 1.0f : 0.0f;
    }
    *dbl_rgb = cur_tme && !(((Cc->tex0 >> 35) & 3) == 1);
    set_3d_state(1);
    return 1;
}
/* vertex memory in the display list for n vertices (the caller writes them there: no copy), then draws of ranges of it */
void *hle_ge_mesh_alloc(int n)                       /* = sceGuGetMemory: the data skipped by a jump in the list */
{
    unsigned int size = ((unsigned int)n * (unsigned int)sizeof(GV) + 3) & ~3u;
    unsigned int *p = gu_list->current, *next = p + 2 + size / 4;
    uint32_t a = (uint32_t)(uintptr_t)next;
    p[0] = 16u << 24 | ((a >> 8) & 0xF0000); p[1] = 8u << 24 | (a & 0xFFFFFF);
    gu_list->current = next;
    return p + 2;
}
void hle_ge_mesh_draw_at(int prim, const void *v, int n)
{
    if (n < 3) return;
    ge_draw_array(prim, GV_FMT3, n, v);
    st_direct += (uint64_t)(n - 2); st_prims++;
}
void hle_ge_mesh_end(void) { list_check(); }
/* draws n vertices (GU_TRIANGLE_STRIP / GU_TRIANGLE_FAN / GU_TRIANGLES) of the current direct mesh */
void hle_ge_mesh_draw(int prim, const void *v, int n)
{
    if (n < 3) return;
    void *m = sceGuGetMemory(n * (int)sizeof(GV));
    memcpy(m, v, (size_t)n * sizeof(GV));
    sceGuDrawArray(prim, GV_FMT3, n, 0, m);
    st_direct += (uint64_t)(n - 2); st_prims++;
    list_check();
}

/* XYZ2 / XYZF2 / XYZ3 / XYZF3 */
static void vertex(uint64_t xyz, int has_f, int kick)
{
    Ctx *c = &cx[(int)((((r_prmodecont & 1) ? r_prim : r_prmode) >> 9) & 1)];
    Vtx v;
    int ox = (int)(c->xyoff & 0xFFFF), oy = (int)((c->xyoff >> 32) & 0xFFFF);
    v.x = (float)((int)(xyz & 0xFFFF) - ox) / 16.0f;
    v.y = (float)((int)((xyz >> 16) & 0xFFFF) - oy) / 16.0f;
    v.z = has_f ? (uint32_t)((xyz >> 32) & 0xFFFFFF) : (uint32_t)(xyz >> 32);
    v.f = has_f ? (uint8_t)(xyz >> 56) : (uint8_t)(r_fog >> 56);
    memcpy(&v.s, &r_st, 4);
    uint32_t tt = (uint32_t)(r_st >> 32); memcpy(&v.t, &tt, 4);
    v.q = q_cur;
    v.u = (float)(r_uv & 0x3FFF); v.v = (float)((r_uv >> 16) & 0x3FFF);
    v.r = (uint8_t)r_rgbaq; v.g = (uint8_t)(r_rgbaq >> 8); v.b = (uint8_t)(r_rgbaq >> 16); v.a = (uint8_t)(r_rgbaq >> 24);
    int type = (int)(r_prim & 7);
    const Vtx *p[3];
    switch (type) {
    case 0: if (kick) { p[0] = &v; emit(GU_POINTS, p, 1); } break;
    case 1: vq[nv++] = v; if (nv == 2) { if (kick) { p[0] = &vq[0]; p[1] = &vq[1]; emit(GU_LINES, p, 2); } nv = 0; } break;
    case 2: if (nv < 2) vq[nv++] = v; else { vq[0] = vq[1]; vq[1] = v; }
            if (nv == 2 && kick) { p[0] = &vq[0]; p[1] = &vq[1]; emit(GU_LINES, p, 2); } break;
    case 3: vq[nv++] = v; if (nv == 3) { if (kick) { p[0] = &vq[0]; p[1] = &vq[1]; p[2] = &vq[2]; emit(GU_TRIANGLES, p, 3); } nv = 0; } break;
    case 4: if (nv < 3) vq[nv++] = v; else { vq[0] = vq[1]; vq[1] = vq[2]; vq[2] = v; }
            if (nv == 3 && kick) { p[0] = &vq[0]; p[1] = &vq[1]; p[2] = &vq[2]; emit(GU_TRIANGLES, p, 3); } break;
    case 5: if (nv < 3) vq[nv++] = v; else { vq[1] = vq[2]; vq[2] = v; }
            if (nv == 3 && kick) { p[0] = &vq[0]; p[1] = &vq[1]; p[2] = &vq[2]; emit(GU_TRIANGLES, p, 3); } break;
    case 6: vq[nv++] = v; if (nv == 2) { if (kick) { p[0] = &vq[0]; p[1] = &vq[1]; emit(GU_SPRITES, p, 2); } nv = 0; } break;
    default: break;
    }
}

static void gs_reg(int reg, uint64_t v)
{
    switch (reg) {
    case 0x00: r_prim = v; nv = 0; state_dirty = 1; break;
    case 0x01: r_rgbaq = v; { uint32_t qb = (uint32_t)(v >> 32); memcpy(&q_cur, &qb, 4); } break;
    case 0x02: r_st = v; break;
    case 0x03: r_uv = v; break;
    case 0x04: vertex(v, 1, 1); break;
    case 0x05: vertex(v, 0, 1); break;
    case 0x0C: vertex(v, 1, 0); break;
    case 0x0D: vertex(v, 0, 0); break;
    case 0x06: case 0x07: cx[reg - 6].tex0 = v; state_dirty = 1; break;
    case 0x08: case 0x09: cx[reg - 8].clamp = v; state_dirty = 1; break;
    case 0x0A: r_fog = v; break;
    case 0x14: case 0x15: cx[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: { uint64_t m = (0x3Full << 20) | (0x7FFFFFFull << 37); cx[reg - 0x16].tex0 = (cx[reg - 0x16].tex0 & ~m) | (v & m); state_dirty = 1; } break;
    case 0x18: case 0x19: cx[reg - 0x18].xyoff = v; break;
    case 0x1A: r_prmodecont = v; state_dirty = 1; break;
    case 0x1B: r_prmode = v; state_dirty = 1; break;
    case 0x3B: r_texa = v; state_dirty = 1; break;
    case 0x3D: r_fogcol = v; break;
    case 0x40: case 0x41: cx[reg - 0x40].scissor = v; state_dirty = 1; break;
    case 0x42: case 0x43: cx[reg - 0x42].alpha = v; state_dirty = 1; break;
    case 0x46: r_colclamp = v; break;
    case 0x47: case 0x48: cx[reg - 0x47].test = v; state_dirty = 1; break;
    case 0x49: r_pabe = v & 1; break;
    case 0x4A: case 0x4B: cx[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: cx[reg - 0x4C].frame = v; state_dirty = 1; break;
    case 0x4E: case 0x4F: cx[reg - 0x4E].zbuf = v; state_dirty = 1; break;
    case 0x50: r_bitblt = v; break;
    case 0x51: r_trxpos = v; break;
    case 0x52: r_trxreg = v; break;
    case 0x53: batch_flush(); xfer_start((int)(v & 3)); state_dirty = 1; break;
    default: break;
    }
}

static void init_once(void)
{
    if (on >= 0) return;
    { const char *e = hle_cfg_get("x_ge"); if (e && e[0] == '0') { on = 0; LOG("GE-GS: off (x_ge=0)\n"); return; } }
    on = 1;

}

static const uint8_t *(*gs_span)(void *, uint32_t, uint32_t, uint32_t *);
static const uint8_t *(*gs_span_get)(void *, uint32_t);      /* the reader gs_span goes with (PATH3) */
void hle_gs_lite_set_span(const uint8_t *(*span)(void *, uint32_t, uint32_t, uint32_t *)) { gs_span = span; }
void hle_gs_lite_set_span_get(const uint8_t *(*get)(void *, uint32_t)) { gs_span_get = get; }
static void gs_packets_(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n);
/* the GS state a native batch leaves (its last two vertices, no drawing): kept and sent only when another packet
   comes (the next native batch replaces it; nothing between them reads it) */
static uint8_t tail_buf[16 * 8]; static uint32_t tail_n;
static const uint8_t *tail_get(void *u, uint32_t k) { (void)u; return k < tail_n ? tail_buf + 16 * k : 0; }
void hle_gs_lite_tail(const uint8_t *pk, uint32_t n) { if (n > 8) n = 8; memcpy(tail_buf, pk, 16 * n); tail_n = n; }
static void tail_send(void) { if (tail_n) { uint32_t m = tail_n; gs_packets_(tail_get, 0, m); tail_n = 0; } }
void hle_gs_lite_packets(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n)
{ ZN(9, tail_send(); TPROF(2, gs_packets_(get, u, n))); }
static void gs_packets_(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n)
{
    init_once();
    if (!on) return;
    uint32_t i = 0;
    while (i < n) {
        const uint8_t *t = get(u, i++);
        if (!t) return;
        uint64_t lo, hi;
        memcpy(&lo, t, 8); memcpy(&hi, t + 8, 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        if ((lo >> 46) & 1) gs_reg(0x00, (lo >> 47) & 0x7FF);
        if (flg == 0) {
            /* PATH3: the packet's qwords by contiguous runs (gs_span) instead of one get per qword */
            const uint8_t *rq = 0; uint32_t rleft = 0, need = nloop * nreg;
            int runs = gs_span && get == gs_span_get;
            for (uint32_t k = 0; k < nloop; k++)
                for (uint32_t r = 0; r < nreg; r++) {
                    const uint8_t *d;
                    if (rleft) { d = rq; rq += 16; rleft--; i++; }
                    else if (runs) { uint32_t m; d = gs_span(u, i, need, &m); if (!d || !m) return; rq = d + 16; rleft = m - 1; i++; }
                    else d = get(u, i++);
                    need--;
                    if (!d) return;
                    uint64_t a, b;
                    memcpy(&a, d, 8); memcpy(&b, d + 8, 8);
                    uint32_t reg = (uint32_t)(hi >> (4 * r)) & 15;
                    switch (reg) {
                    case 0x0: gs_reg(0x00, a & 0x7FF); break;
                    case 0x1: { uint32_t qb; memcpy(&qb, &q_cur, 4);
                                r_rgbaq = (a & 0xFF) | ((a >> 32) & 0xFF) << 8 | (b & 0xFF) << 16 | ((b >> 32) & 0xFF) << 24 | (uint64_t)qb << 32; } break;
                    case 0x2: { uint32_t qv = (uint32_t)b; r_st = a; memcpy(&q_cur, &qv, 4); } break;
                    case 0x3: r_uv = (a & 0x3FFF) | ((a >> 32) & 0x3FFF) << 16; break;
                    case 0x4: { uint64_t x = (a & 0xFFFF) | ((a >> 32) & 0xFFFF) << 16 | ((b >> 4) & 0xFFFFFF) << 32 | ((b >> 36) & 0xFF) << 56;
                                gs_reg((b >> 47) & 1 ? 0x0C : 0x04, x); } break;
                    case 0x5: { uint64_t x = (a & 0xFFFF) | ((a >> 32) & 0xFFFF) << 16 | (b & 0xFFFFFFFFu) << 32;
                                gs_reg((b >> 47) & 1 ? 0x0D : 0x05, x); } break;
                    case 0xA: r_fog = ((b >> 36) & 0xFF) << 56; break;
                    case 0xE: gs_reg((int)(b & 0xFF), a); break;
                    case 0xF: break;
                    default: gs_reg((int)reg, a); break;
                    }
                }
        } else if (flg == 1) {
            uint32_t words = nloop * nreg;
            for (uint32_t k = 0; k < words; k++) {
                const uint8_t *d = get(u, i + k / 2);
                if (!d) return;
                uint64_t a; memcpy(&a, d + 8 * (k & 1), 8);
                uint32_t reg = (uint32_t)(hi >> (4 * (k % nreg))) & 15;
                if (reg == 0xE || reg == 0xF) continue;
                gs_reg((int)reg, a);
            }
            i += (words + 1) / 2;
        } else {
            uint64_t t0_ = TNOW(); hle_zone = 10;
            const uint8_t *(*sp)(void *, uint32_t, uint32_t, uint32_t *) = gs_span;
            for (uint32_t k = 0; k < nloop; ) {
                if (sp && d_on) {                         /* collected by runs: nothing is read */
                    uint32_t m; const uint8_t *d = sp(u, i, nloop - k, &m);
                    if (!d || !m) { hle_tprof[9] += TNOW() - t0_; return; }
                    xfer_span(d, m); i += m; k += m; continue;
                }
                const uint8_t *d = get(u, i++); if (!d) { hle_tprof[9] += TNOW() - t0_; return; } xfer_data(d); k++;
            }
            hle_tprof[9] += TNOW() - t0_; hle_zone = 9;
        }
    }
}

uint32_t hle_gs_lite_frame(void) { return frame_no; }
void hle_gs_lite_flip(void)
{
    tail_send();
    if (on != 1) return;
    st_frames++;
    {   /* in PPSSPP only: a screenshot every 120 frames (its headless tester keeps the latest); nothing on a PSP */
        static int emu = -1;
        if (emu < 0) { unsigned int r = 0; emu = sceIoDevctl("emulator:", 3, 0, 0, &r, 4) == 0 && r == 1; }
        static int shot_at = -1; if (shot_at < 0) { const char *e = hle_cfg_get("shot_frame"); shot_at = e ? atoi(e) : 0; }   /* cfg shot_frame=N: only then */
        if (emu && ge_ready && (shot_at ? st_frames == (uint64_t)shot_at : st_frames % 120 == 0)) sceIoDevctl("emulator:", 0x20, 0, 0, 0, 0);
    }
    frame_no++;
    if (frame_no % 60 == 0) {
        static uint64_t a, b, c2, dd, ee;
        LOG("GE-GS image qwords per frame: deferred %llu, written by xfer_fast %llu, per pixel %llu; partly covered blocks %llu, deferred uploads %llu\n",
            (unsigned long long)(st_xq_def - a) / 60, (unsigned long long)(st_xq_fast - b) / 60, (unsigned long long)(st_xq_raw - c2) / 60,
            (unsigned long long)(st_def_mat_blk - dd) / 60, (unsigned long long)(st_def_up - ee) / 60);
        { static uint64_t cf; LOG("GE-GS cells written whole into their block: %llu per frame\n", (unsigned long long)(st_cell_fast - cf) / 60); cf = st_cell_fast; }
        { static uint64_t pg; LOG("GE-GS ownership by whole pages: %llu per frame (layout check: %llu bad)\n", (unsigned long long)(st_own_pages - pg) / 60, (unsigned long long)st_own_bad); pg = st_own_pages; }
        a = st_xq_def; b = st_xq_fast; c2 = st_xq_raw; dd = st_def_mat_blk; ee = st_def_up;
    }
    /* the display buffers, from the PCRTC registers */
    uint64_t pmode = hle_gs_priv(0x00);
    uint64_t d1 = hle_gs_priv(0x70), d2 = hle_gs_priv(0x90);
    uint32_t f = (uint32_t)(((pmode & 2) ? d2 : d1) & 0x1FF) * 32;
    if (pmode & 3) {
        if (disp_fbp[0] == 0xFFFFFFFF) disp_fbp[0] = f;
        else if (f != disp_fbp[0] && disp_fbp[1] == 0xFFFFFFFF) disp_fbp[1] = f;
    }
    if (!in_frame) return;
    batch_flush();
    sceGuFinish();
    GE_SYNC();
    sceGuSwapBuffers();
    in_frame = 0;
    state_dirty = 1;
    cur_list ^= 1;
}

void hle_gs_lite_load_mem(const uint8_t *m)
{
    init_once();
    for (uint32_t i = 0; i < 512; i++) {
        const uint8_t *src = m + i * 8192u; int nz = 0;
        for (int k = 0; k < 8192; k += 4) { uint32_t w; memcpy(&w, src + k, 4); if (w) { nz = 1; break; } }
        if (nz || gsm_pg[i]) memcpy(W8(i * 8192u), src, 8192);
    }
    for (int i = 0; i < 512; i++) page_gen[i]++;
    memset(own, 0, sizeof own); for (int i = 0; i < NPEND; i++) pend[i].nown = 0; d_on = 0;
}

void hle_gs_lite_report(void)
{
    LOG("GE-GS: deferred uploads %llu (written at once %llu), written later %llu (%llu blocks), never written %llu, source changed %llu, flushes %llu (written later because: ring %llu, partly covered %llu, texture %llu, palette %llu, copy %llu), incomplete %llu, first seen %llu; textures by source: %llu found, %llu made, %llu mixed palettes\n",
        (unsigned long long)st_def_up, (unsigned long long)st_def_eager, (unsigned long long)st_def_mat, (unsigned long long)st_def_mat_blk, (unsigned long long)st_def_dead,
        (unsigned long long)st_def_stale, (unsigned long long)st_def_flush, (unsigned long long)st_mat_c[0], (unsigned long long)st_mat_c[1], (unsigned long long)st_mat_c[2], (unsigned long long)st_mat_c[3], (unsigned long long)st_mat_c[4], (unsigned long long)st_def_inc, (unsigned long long)st_def_new, (unsigned long long)st_src_hit, (unsigned long long)st_src_miss, (unsigned long long)st_src_mixed);
    LOG("GE-GS: conversions a whole upload covers: %llu (%llu KB); %llu with a smaller / offset upload at that base\n",
        (unsigned long long)st_up_match, (unsigned long long)(st_up_match_b >> 10), (unsigned long long)st_up_part);
    for (int r = 0; r < 10; r++) {
        int best = -1; for (int t = 0; t < 64; t++) if (tsz_key[t] && (best < 0 || tsz_b[t] > tsz_b[best])) best = t;
        if (best < 0) break;
        LOG("GE-GS: converted %s %ux%u: %llu times, %llu KB\n", (tsz_key[best] >> 24) == GU_PSM_T4 ? "T4" : (tsz_key[best] >> 24) == GU_PSM_T8 ? "T8" : "8888",
            (tsz_key[best] >> 12) & 0xFFF, tsz_key[best] & 0xFFF, (unsigned long long)tsz_n[best], (unsigned long long)(tsz_b[best] >> 10));
        tsz_b[best] = 0; tsz_key[best] |= 0x80000000u;
    }
    LOG("GE-GS: %llu KB of buffers (GS pages %llu, %llu writes lost); %llu frames, %llu GE lists (%llu split when full), %llu primitives drawn, %llu skipped (render "
        "targets), %llu skipped (texture from a render target / unsupported), textures: %llu converted, %llu cache hits, "
        "%llu uploads; display buffers %x %x, %d other render targets; perspective path: %llu triangles, %llu fit failures\n", (unsigned long long)st_alloc_kb, (unsigned long long)st_pages, (unsigned long long)st_page_fail, (unsigned long long)st_frames,
        (unsigned long long)st_lists, (unsigned long long)st_flush_full, (unsigned long long)st_prims,
        (unsigned long long)st_skipped_rt, (unsigned long long)st_skipped_tex, (unsigned long long)st_tex_conv,
        (unsigned long long)st_tex_hit, (unsigned long long)st_uploads, disp_fbp[0], disp_fbp[1], n_rt,
        (unsigned long long)st_prims3d, (unsigned long long)st_fit_fail);
    LOG("GE-GS: %llu direct mesh triangles; waited %llu ms for the GE in total; texture cache: %llu content hashes, %llu found again by content, %llu resets, %llu evicted (%llu GE drains, %llu times all hot), %llu KB converted (8888 %llu, T8 %llu, T4 %llu; %llu conversions / %llu KB of content converted before)\n", (unsigned long long)st_direct, (unsigned long long)(st_ge_wait_us / 1000), (unsigned long long)st_tex_sig, (unsigned long long)st_tex_rehit, (unsigned long long)st_tex_resets, (unsigned long long)st_tex_evict, (unsigned long long)st_tex_ring_sync, (unsigned long long)st_tex_full,
        (unsigned long long)(st_tex_bytes >> 10), (unsigned long long)st_tex_c[0], (unsigned long long)st_tex_c[1], (unsigned long long)st_tex_c[2], (unsigned long long)st_tex_again, (unsigned long long)(st_tex_again_b >> 10));
}
