/* hle_gs_lite.c (host only) - a software GS: the reference picture of what the game draws, for checking the PSP
 * renderer against. Input: every GIF packet (PATH1 = VU1 XGKICK from hle_vu1.c, PATH2 = VIF1 DIRECT, PATH3 = the GIF
 * channel; hle_gfx_scan.c feeds them in DMA order). Output: env HLE_GS_FRAMES=<dir> writes a PPM of the displayed
 * frame buffer (DISPFB of the enabled read circuit) every HLE_GS_EVERY flips (default 50), at most HLE_GS_MAX (40).
 *
 * Model (GS behaviour, simplified where noted):
 *   - one 4 MB local memory with the GS's swizzled layouts (gs_mem.h): colour PSMCT32/24/16/16S, depth PSMZ32/24/16/
 *     16S, textures also PSMT8/4/8H/4HL/4HH with CT32/CT16 palettes (CSM1). Frame, depth and textures all live in it,
 *     so render-to-texture and depth-as-texture work;
 *   - IMAGE transfers host -> local (BITBLTBUF / TRXPOS / TRXREG / TRXDIR); local -> local copies (TRXDIR 2);
 *   - both drawing contexts (PRIM CTXT), PRMODECONT / PRMODE, XYOFFSET, SCISSOR;
 *   - points, lines, line strips, triangles, strips, fans, sprites; flat / Gouraud; UV (FST) or STQ (perspective);
 *     nearest texel sampling; CLAMP modes (repeat, clamp, region clamp, region repeat); TEXA for 24/16-bit alpha;
 *     TFX modulate / decal / highlight / highlight2 with TCC; fog (FGE, FOGCOL);
 *   - alpha test (ATST, AREF, AFAIL), destination alpha test (DATE / DATM), depth test (ZTE, ZTST) and ZMSK, the
 *     blend equation ((A - B) * C >> 7) + D with Cs / Cd / 0 and As / Ad / FIX, PABE, COLCLAMP, FBA, FBMSK.
 *   Not modelled: bilinear filtering, mipmapping, dithering, anti-aliasing, the CLUT buffer (palettes are read from
 *   memory at draw time), local -> host transfers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "hle.h"
#include "gs_mem.h"

static uint8_t *gsm;                       /* GS local memory */

typedef struct { uint64_t tex0, clamp, xyoff, tex1, scissor, alpha, test, frame, zbuf, fba; } Ctx;
static Ctx cx[2];
static uint64_t r_prim, r_prmodecont = 1, r_prmode, r_rgbaq, r_st, r_uv, r_fog, r_fogcol, r_texa, r_colclamp = 1, r_pabe,
    r_bitblt, r_trxpos, r_trxreg, r_texclut;
static float q_cur = 1.0f;

typedef struct { float x, y, s, t, q, u, v; double z; float r, g, b, a, f; } Vtx;
static Vtx vq[3];
static int nv;

typedef struct { uint32_t fbp; uint64_t draws; } Frame;   /* draw counts per frame buffer (fallback for the dump) */
static Frame fbs[32];
static int nfb;

static uint64_t half_px[2], st_fge, st_rt_draws, st_rt_tex;               /* pixels drawn this frame in rows < 512 / >= 512 */
static uint64_t st_draws, st_tex_miss, st_uploads, st_frames, st_pixels, st_psm_miss[64];
static int on = -1, every = 50, maxdump = 40, dumped;
static const char *outdir;

/* the state of the current primitive, latched at the kick */
static struct {
    Ctx *c;
    int iip, tme, fge, abe, fst;
    uint32_t fbp, fbw; int fpsm; uint32_t fbmsk;
    uint32_t zbp; int zpsm, zmsk;
    int ate, atst, aref, afail, date, datm, zte, ztst;
    int ba, bb, bc, bd; int fix;
    int tfx, tcc, tpsm, tw, th; uint32_t tbp, tbw, cbp; int cpsm, csa;
    int wms, wmt, minu, maxu, minv, maxv;
    int sx0, sx1, sy0, sy1;
} D;

static int psm_bpp(int psm)
{
    switch (psm) {
    case 0x00: return 32; case 0x01: return 24; case 0x02: case 0x0A: return 16;
    case 0x13: return 8; case 0x14: return 4; case 0x1B: return 8; case 0x24: return 4; case 0x2C: return 4;
    case 0x30: return 32; case 0x31: return 24; case 0x32: case 0x3A: return 16;
    default: return 32;
    }
}

/* ------------------------------------------------------------------ IMAGE transfers */
static int xfer_on, xfer_x, xfer_y, xfer_w, xfer_h, xfer_i, x_bp, x_bw, x_psm;
static void put_px(int bp, int bw, int psm, int x, int y, uint32_t v)
{
    int nb;
    uint32_t a = gs_addr_px(psm, (uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y, &nb);
    if (nb == 4) { if (psm == 0x01 || psm == 0x31) memcpy(gsm + a, &v, 3); else memcpy(gsm + a, &v, 4); return; }
    if (nb == 2) { uint16_t h = (uint16_t)v; memcpy(gsm + a, &h, 2); return; }
    switch (psm) {
    case 0x13: gsm[gs_addr8((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y)] = (uint8_t)v; break;
    case 0x14: { uint32_t n = gs_addr4((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y); uint8_t *b = gsm + (n >> 1);
                 *b = (n & 1) ? (uint8_t)((*b & 0x0F) | (v << 4)) : (uint8_t)((*b & 0xF0) | (v & 15)); } break;
    case 0x1B: gsm[gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y) + 3] = (uint8_t)v; break;
    case 0x24: case 0x2C: { uint32_t a2 = gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y);
                 int sh = psm == 0x24 ? 24 : 28; uint32_t w; memcpy(&w, gsm + a2, 4); w = (w & ~(15u << sh)) | ((v & 15) << sh); memcpy(gsm + a2, &w, 4); } break;
    default: break;
    }
}
static uint32_t get_px(int bp, int bw, int psm, int x, int y)          /* raw value in its format */
{
    int nb;
    uint32_t a = gs_addr_px(psm, (uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y, &nb), v = 0;
    if (nb == 4) { memcpy(&v, gsm + a, 4); return (psm == 0x01 || psm == 0x31) ? v & 0xFFFFFF : v; }
    if (nb == 2) { uint16_t h; memcpy(&h, gsm + a, 2); return h; }
    switch (psm) {
    case 0x13: return gsm[gs_addr8((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y)];
    case 0x14: { uint32_t n = gs_addr4((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y); return (gsm[n >> 1] >> ((n & 1) * 4)) & 15; }
    case 0x1B: return gsm[gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y) + 3];
    case 0x24: case 0x2C: { uint32_t w; memcpy(&w, gsm + gs_addr32((uint32_t)bp, (uint32_t)bw, (uint32_t)x, (uint32_t)y), 4); return (w >> (psm == 0x24 ? 24 : 28)) & 15; }
    }
    return 0;
}
static void xfer_start(int dir)
{
    int sbp = (int)(r_bitblt & 0x3FFF), sbw = (int)((r_bitblt >> 16) & 0x3F), spsm = (int)((r_bitblt >> 24) & 0x3F);
    x_bp = (int)((r_bitblt >> 32) & 0x3FFF); x_bw = (int)((r_bitblt >> 48) & 0x3F); x_psm = (int)((r_bitblt >> 56) & 0x3F);
    int ssax = (int)(r_trxpos & 0x7FF), ssay = (int)((r_trxpos >> 16) & 0x7FF);
    xfer_x = (int)((r_trxpos >> 32) & 0x7FF); xfer_y = (int)((r_trxpos >> 48) & 0x7FF);
    xfer_w = (int)(r_trxreg & 0xFFF); xfer_h = (int)((r_trxreg >> 32) & 0xFFF);
    xfer_i = 0;
    if (dir == 0) { xfer_on = 1; st_uploads++; return; }
    if (dir == 2) {                                                    /* local -> local */
        for (int y = 0; y < xfer_h; y++)
            for (int x = 0; x < xfer_w; x++)
                put_px(x_bp, x_bw, x_psm, xfer_x + x, xfer_y + y, get_px(sbp, sbw, spsm, ssax + x, ssay + y));
    }
}
static void xfer_data(const uint8_t *q)                                /* one qword of IMAGE data */
{
    if (!xfer_on) return;
    int total = xfer_w * xfer_h;
    int bpp = psm_bpp(x_psm);
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

/* ------------------------------------------------------------------ texels */
static uint32_t c16to32(uint16_t c, int use_texa)
{
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3, a;
    if (use_texa) {
        int aem = (int)((r_texa >> 15) & 1);
        a = (c & 0x8000) ? (uint32_t)((r_texa >> 32) & 0xFF) : (aem && !(c & 0x7FFF)) ? 0 : (uint32_t)(r_texa & 0xFF);
    } else a = c & 0x8000 ? 0x80 : 0;
    return r | g << 8 | b << 16 | a << 24;
}
static uint32_t clut_entry(int idx, int is4)
{
    int i = is4 ? idx + D.csa * 16 : idx;
    int p = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);      /* CSM1: bits 3 and 4 swapped */
    if (D.cpsm == 0x00 || D.cpsm == 0x01) {
        uint32_t v; memcpy(&v, gsm + gs_addr32(D.cbp, 1, (uint32_t)(p & 15), (uint32_t)(p >> 4)), 4);
        return v;
    }
    uint16_t h;
    if (D.cpsm == 0x0A) memcpy(&h, gsm + gs_addr16t(gs_blk16s, D.cbp, 1, (uint32_t)(p & 15), (uint32_t)(p >> 4)), 2);
    else memcpy(&h, gsm + gs_addr16(D.cbp, 1, (uint32_t)(p & 15), (uint32_t)(p >> 4)), 2);
    return c16to32(h, 1);
}
static int wrap(int u, int mode, int size, int mn, int mx)
{
    switch (mode) {
    case 0: return u & (size - 1);
    case 1: return u < 0 ? 0 : u >= size ? size - 1 : u;
    case 2: return u < mn ? mn : u > mx ? mx : u;
    default: return (u & mn) | mx;
    }
}
static uint32_t texel(int u, int v)
{
    u = wrap(u, D.wms, D.tw, D.minu, D.maxu);
    v = wrap(v, D.wmt, D.th, D.minv, D.maxv);
    uint32_t x = (uint32_t)u & 2047, y = (uint32_t)v & 2047;
    uint32_t raw = get_px((int)D.tbp, (int)D.tbw, D.tpsm, (int)x, (int)y);
    switch (D.tpsm) {
    case 0x00: case 0x30: return raw;
    case 0x01: case 0x31: {
        int aem = (int)((r_texa >> 15) & 1);
        uint32_t a = (aem && !(raw & 0xFFFFFF)) ? 0 : (uint32_t)(r_texa & 0xFF);
        return (raw & 0xFFFFFF) | a << 24;
    }
    case 0x02: case 0x0A: case 0x32: case 0x3A: return c16to32((uint16_t)raw, 1);
    case 0x13: case 0x1B: return clut_entry((int)raw, 0);
    case 0x14: case 0x24: case 0x2C: return clut_entry((int)raw, 1);
    default: st_tex_miss++; st_psm_miss[D.tpsm & 63]++; return 0x80808080u;
    }
}

/* ------------------------------------------------------------------ pixels */
static inline int clampc(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void pixel(int x, int y, double zf, int r, int g, int b, int a, float s, float t, float q, float u, float v, float fog)
{
    if (x < D.sx0 || x > D.sx1 || y < D.sy0 || y > D.sy1) return;
    st_pixels++;
    half_px[y >= 512]++;
    if (D.tme) {
        int tu, tv;
        if (D.fst) { tu = (int)floorf(u / 16.0f); tv = (int)floorf(v / 16.0f); }
        else { float qq = q ? q : 1.0f; tu = (int)floorf(s / qq * D.tw); tv = (int)floorf(t / qq * D.th); }
        uint32_t tx = texel(tu, tv);
        int tr = (int)(tx & 255), tg = (int)((tx >> 8) & 255), tb = (int)((tx >> 16) & 255), ta = (int)(tx >> 24);
        switch (D.tfx) {
        case 0: r = tr * r >> 7; g = tg * g >> 7; b = tb * b >> 7; a = D.tcc ? ta * a >> 7 : a; break;
        case 1: r = tr; g = tg; b = tb; a = D.tcc ? ta : a; break;
        case 2: r = (tr * r >> 7) + a; g = (tg * g >> 7) + a; b = (tb * b >> 7) + a; a = D.tcc ? ta + a : a; break;
        default: r = (tr * r >> 7) + a; g = (tg * g >> 7) + a; b = (tb * b >> 7) + a; a = D.tcc ? ta : a; break;
        }
        r = clampc(r); g = clampc(g); b = clampc(b); a = clampc(a);
    }
    if (D.fge) {
        int f = (int)fog, fr = (int)(r_fogcol & 255), fg = (int)((r_fogcol >> 8) & 255), fb = (int)((r_fogcol >> 16) & 255);
        r = (f * r + (255 - f) * fr) >> 8; g = (f * g + (255 - f) * fg) >> 8; b = (f * b + (255 - f) * fb) >> 8;
    }
    int wfb = 1, wz = !D.zmsk, wa = 1;
    if (D.ate) {
        int pass;
        switch (D.atst) {
        case 0: pass = 0; break; case 1: pass = 1; break; case 2: pass = a < D.aref; break; case 3: pass = a <= D.aref; break;
        case 4: pass = a == D.aref; break; case 5: pass = a >= D.aref; break; case 6: pass = a > D.aref; break;
        default: pass = a != D.aref; break;
        }
        if (!pass) {
            switch (D.afail) {
            case 0: return;
            case 1: wz = 0; break;
            case 2: wfb = 0; break;
            default: wz = 0; wa = 0; break;
            }
        }
    }
    /* depth */
    int zb = 0;
    uint32_t za = 0, zmax = D.zpsm == 0x30 ? 0xFFFFFFFFu : D.zpsm == 0x31 ? 0xFFFFFFu : 0xFFFFu;
    uint32_t z = zf <= 0 ? 0 : zf >= (double)zmax ? zmax : (uint32_t)zf;
    if (D.zte) {
        za = gs_addr_px(D.zpsm, D.zbp, D.fbw, (uint32_t)x, (uint32_t)y, &zb);
        uint32_t zo = 0;
        if (zb == 4) { memcpy(&zo, gsm + za, 4); if (D.zpsm == 0x31) zo &= 0xFFFFFF; }
        else if (zb == 2) { uint16_t h; memcpy(&h, gsm + za, 2); zo = h; }
        switch (D.ztst) {
        case 0: return;
        case 1: break;
        case 2: if (z < zo) return; break;
        default: if (z <= zo) return; break;
        }
    } else wz = 0;
    /* frame */
    int fb;
    uint32_t fa = gs_addr_px(D.fpsm, D.fbp, D.fbw, (uint32_t)x, (uint32_t)y, &fb), o = 0;
    if (!fb || D.fpsm >= 0x30) wfb = 0;
    if (fb == 4) memcpy(&o, gsm + fa, 4);
    else if (fb == 2) { uint16_t h; memcpy(&h, gsm + fa, 2); o = c16to32(h, 0); }
    if (D.date && fb) {
        int abit = fb == 4 ? (int)(o >> 31) : (o >> 24) ? 1 : 0;
        if (D.fpsm != 0x01 && abit != D.datm) return;
    }
    if (wfb) {
        int dr = (int)(o & 255), dg = (int)((o >> 8) & 255), db = (int)((o >> 16) & 255), da = (int)(o >> 24);
        if (D.fpsm == 0x01) da = 0x80;
        if (D.abe && !(r_pabe && !(a & 0x80))) {
            int A[3] = {r, g, b}, Dd[3] = {dr, dg, db}, res[3];
            int cv = D.bc == 0 ? a : D.bc == 1 ? da : D.fix;
            for (int k = 0; k < 3; k++) {
                int va = D.ba == 0 ? A[k] : D.ba == 1 ? Dd[k] : 0;
                int vb = D.bb == 0 ? A[k] : D.bb == 1 ? Dd[k] : 0;
                int vd = D.bd == 0 ? A[k] : D.bd == 1 ? Dd[k] : 0;
                res[k] = ((va - vb) * cv >> 7) + vd;
            }
            r = res[0]; g = res[1]; b = res[2];
        }
        if (r_colclamp & 1) { r = clampc(r); g = clampc(g); b = clampc(b); } else { r &= 255; g &= 255; b &= 255; }
        if (D.c->fba & 1) a |= 0x80;
        uint32_t c = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)(a & 255) << 24;
        uint32_t msk = D.fbmsk | (wa ? 0 : 0xFF000000u);
        if (fb == 4) {
            uint32_t oraw; memcpy(&oraw, gsm + fa, 4);
            uint32_t nw = (c & ~msk) | (oraw & msk);
            if (D.fpsm == 0x01) memcpy(gsm + fa, &nw, 3); else memcpy(gsm + fa, &nw, 4);
        } else {
            uint16_t h = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15));
            uint16_t oh; memcpy(&oh, gsm + fa, 2);
            uint16_t m16 = (uint16_t)(((msk >> 3) & 0x1F) | (((msk >> 11) & 0x1F) << 5) | (((msk >> 19) & 0x1F) << 10) | ((msk >> 31) << 15));
            h = (uint16_t)((h & ~m16) | (oh & m16));
            memcpy(gsm + fa, &h, 2);
        }
    }
    if (wz && D.zte) {
        if (zb == 4) { if (D.zpsm == 0x31) memcpy(gsm + za, &z, 3); else memcpy(gsm + za, &z, 4); }
        else if (zb == 2) { uint16_t h = (uint16_t)z; memcpy(gsm + za, &h, 2); }
    }
}

/* ------------------------------------------------------------------ primitives */
static void latch(void)
{
    uint64_t at = (r_prmodecont & 1) ? r_prim : r_prmode;
    int ctxt = (int)((at >> 9) & 1);
    Ctx *c = &cx[ctxt];
    D.c = c;
    D.iip = (int)((at >> 3) & 1); D.tme = (int)((at >> 4) & 1); D.fge = (int)((at >> 5) & 1);
    D.abe = (int)((at >> 6) & 1); D.fst = (int)((at >> 8) & 1);
    D.fbp = (uint32_t)(c->frame & 0x1FF) * 32; D.fbw = (uint32_t)((c->frame >> 16) & 0x3F); D.fpsm = (int)((c->frame >> 24) & 0x3F);
    D.fbmsk = (uint32_t)(c->frame >> 32);
    D.zbp = (uint32_t)(c->zbuf & 0x1FF) * 32; D.zpsm = (int)((c->zbuf >> 24) & 15) | 0x30; D.zmsk = (int)((c->zbuf >> 32) & 1);
    D.ate = (int)(c->test & 1); D.atst = (int)((c->test >> 1) & 7); D.aref = (int)((c->test >> 4) & 0xFF);
    D.afail = (int)((c->test >> 12) & 3); D.date = (int)((c->test >> 14) & 1); D.datm = (int)((c->test >> 15) & 1);
    D.zte = (int)((c->test >> 16) & 1); D.ztst = (int)((c->test >> 17) & 3);
    D.ba = (int)(c->alpha & 3); D.bb = (int)((c->alpha >> 2) & 3); D.bc = (int)((c->alpha >> 4) & 3); D.bd = (int)((c->alpha >> 6) & 3);
    D.fix = (int)((c->alpha >> 32) & 0xFF);
    uint64_t t = c->tex0;
    D.tbp = (uint32_t)(t & 0x3FFF); D.tbw = (uint32_t)((t >> 14) & 0x3F); D.tpsm = (int)((t >> 20) & 0x3F);
    D.tw = 1 << ((t >> 26) & 15); D.th = 1 << ((t >> 30) & 15);
    if (D.tw > 1024) D.tw = 1024;
    if (D.th > 1024) D.th = 1024;
    D.tcc = (int)((t >> 34) & 1); D.tfx = (int)((t >> 35) & 3);
    D.cbp = (uint32_t)((t >> 37) & 0x3FFF); D.cpsm = (int)((t >> 51) & 15); D.csa = (int)((t >> 56) & 31);
    uint64_t cl = c->clamp;
    D.wms = (int)(cl & 3); D.wmt = (int)((cl >> 2) & 3);
    D.minu = (int)((cl >> 4) & 0x3FF); D.maxu = (int)((cl >> 14) & 0x3FF); D.minv = (int)((cl >> 24) & 0x3FF); D.maxv = (int)((cl >> 34) & 0x3FF);
    D.sx0 = (int)(c->scissor & 0x7FF); D.sx1 = (int)((c->scissor >> 16) & 0x7FF);
    D.sy0 = (int)((c->scissor >> 32) & 0x7FF); D.sy1 = (int)((c->scissor >> 48) & 0x7FF);
    int k;
    for (k = 0; k < nfb && fbs[k].fbp != D.fbp; k++) ;
    if (k == nfb && nfb < 32) { fbs[nfb].fbp = D.fbp; fbs[nfb].draws = 0; nfb++; }
    if (k < 32) fbs[k].draws++;
    st_draws++;
    if (D.fge) st_fge++;
    {
        static int rd = -1; if (rd < 0) rd = getenv("GSL_RTDUMP") != 0;
        int rtw = D.fbp != 0 && D.fbp != 0x1400, rtt = D.tme && (D.tbp < 0x2800 || (D.tbp >= 0x2800 && D.tpsm >= 0x30));
        if (rd && st_frames == 3 && (rtw || rtt))
            LOG("RT prim %d fbp %04x fbw %u fpsm %02x zte %d ztst %d | tme %d tbp %04x tbw %u tpsm %02x tw %d th %d | abe %d alpha %d%d%d%d fix %d | sc %d,%d-%d,%d fbmsk %08x\n",
                (int)(r_prim & 7), D.fbp, D.fbw, D.fpsm, D.zte, D.ztst, D.tme, D.tbp, D.tbw, D.tpsm, D.tw, D.th, D.abe, D.ba, D.bb, D.bc, D.bd, D.fix,
                D.sx0, D.sy0, D.sx1, D.sy1, D.fbmsk);
    }
    if (D.fbp != 0 && D.fbp != 0x1400) st_rt_draws++;
    if (D.tme && (D.tbp == 0 || D.tbp == 0x1400)) st_rt_tex++;
}

static void sprite(const Vtx *a, const Vtx *b)
{
    float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y;
    float u0 = a->u, v0 = a->v, u1 = b->u, v1 = b->v, s0 = a->s, t0 = a->t, s1 = b->s, t1 = b->t;
    if (x1 < x0) { float tt = x0; x0 = x1; x1 = tt; tt = u0; u0 = u1; u1 = tt; tt = s0; s0 = s1; s1 = tt; }
    if (y1 < y0) { float tt = y0; y0 = y1; y1 = tt; tt = v0; v0 = v1; v1 = tt; tt = t0; t0 = t1; t1 = tt; }
    int ix0 = (int)ceilf(x0 - 0.5f), ix1 = (int)ceilf(x1 - 0.5f), iy0 = (int)ceilf(y0 - 0.5f), iy1 = (int)ceilf(y1 - 0.5f);
    if (ix0 < D.sx0) ix0 = D.sx0;
    if (iy0 < D.sy0) iy0 = D.sy0;
    if (ix1 > D.sx1 + 1) ix1 = D.sx1 + 1;
    if (iy1 > D.sy1 + 1) iy1 = D.sy1 + 1;
    float dx = x1 - x0, dy = y1 - y0;
    for (int y = iy0; y < iy1; y++) {
        float fy = dy > 0 ? (y + 0.5f - y0) / dy : 0;
        for (int x = ix0; x < ix1; x++) {
            float fx = dx > 0 ? (x + 0.5f - x0) / dx : 0;
            pixel(x, y, b->z, (int)b->r, (int)b->g, (int)b->b, (int)b->a, s0 + (s1 - s0) * fx, t0 + (t1 - t0) * fy, b->q,
                  u0 + (u1 - u0) * fx, v0 + (v1 - v0) * fy, b->f);
        }
    }
}

static void tri(const Vtx *a, const Vtx *b, const Vtx *c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    int ix0 = (int)ceilf(minx - 0.5f), ix1 = (int)floorf(maxx - 0.5f), iy0 = (int)ceilf(miny - 0.5f), iy1 = (int)floorf(maxy - 0.5f);
    if (ix0 < D.sx0) ix0 = D.sx0;
    if (iy0 < D.sy0) iy0 = D.sy0;
    if (ix1 > D.sx1) ix1 = D.sx1;
    if (iy1 > D.sy1) iy1 = D.sy1;
    if (ix1 - ix0 > 2048 || iy1 - iy0 > 2048) return;
    float inv = 1.0f / area;
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double z = a->z * w0 + b->z * w1 + c->z * w2;
            int r, g, bb, al;
            if (D.iip) { r = (int)(a->r * w0 + b->r * w1 + c->r * w2); g = (int)(a->g * w0 + b->g * w1 + c->g * w2);
                         bb = (int)(a->b * w0 + b->b * w1 + c->b * w2); al = (int)(a->a * w0 + b->a * w1 + c->a * w2); }
            else { r = (int)c->r; g = (int)c->g; bb = (int)c->b; al = (int)c->a; }
            pixel(x, y, z, r, g, bb, al,
                  a->s * w0 + b->s * w1 + c->s * w2, a->t * w0 + b->t * w1 + c->t * w2, a->q * w0 + b->q * w1 + c->q * w2,
                  a->u * w0 + b->u * w1 + c->u * w2, a->v * w0 + b->v * w1 + c->v * w2, a->f * w0 + b->f * w1 + c->f * w2);
        }
}

static void line(const Vtx *a, const Vtx *b)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy)) + 1;
    if (n > 4096) return;
    for (int i = 0; i < n; i++) {
        float f = n > 1 ? (float)i / (n - 1) : 0;
        const Vtx *cc = D.iip ? 0 : b;
        pixel((int)(a->x + dx * f), (int)(a->y + dy * f), a->z + (b->z - a->z) * f,
              (int)(cc ? cc->r : a->r + (b->r - a->r) * f), (int)(cc ? cc->g : a->g + (b->g - a->g) * f),
              (int)(cc ? cc->b : a->b + (b->b - a->b) * f), (int)(cc ? cc->a : a->a + (b->a - a->a) * f),
              a->s + (b->s - a->s) * f, a->t + (b->t - a->t) * f, a->q + (b->q - a->q) * f,
              a->u + (b->u - a->u) * f, a->v + (b->v - a->v) * f, a->f + (b->f - a->f) * f);
    }
}

/* XYZ2 / XYZF2 / XYZ3 / XYZF3: x, y in bits 0-31; z (32 or 24 bits) and f above */
static void vertex(uint64_t xyz, int has_f, int kick)
{
    Ctx *c = &cx[(int)((((r_prmodecont & 1) ? r_prim : r_prmode) >> 9) & 1)];
    Vtx v;
    int ox = (int)(c->xyoff & 0xFFFF), oy = (int)((c->xyoff >> 32) & 0xFFFF);
    v.x = (float)((int)(xyz & 0xFFFF) - ox) / 16.0f;
    v.y = (float)((int)((xyz >> 16) & 0xFFFF) - oy) / 16.0f;
    if (has_f) { v.z = (double)((xyz >> 32) & 0xFFFFFF); v.f = (float)((xyz >> 56) & 0xFF); }
    else { v.z = (double)(uint32_t)(xyz >> 32); v.f = (float)((r_fog >> 56) & 0xFF); }
    memcpy(&v.s, &r_st, 4);
    uint32_t tt = (uint32_t)(r_st >> 32); memcpy(&v.t, &tt, 4);
    v.q = q_cur;
    v.u = (float)(r_uv & 0x3FFF); v.v = (float)((r_uv >> 16) & 0x3FFF);
    v.r = (float)(uint8_t)r_rgbaq; v.g = (float)(uint8_t)(r_rgbaq >> 8); v.b = (float)(uint8_t)(r_rgbaq >> 16); v.a = (float)(uint8_t)(r_rgbaq >> 24);
    int type = (int)(r_prim & 7);
    {
        static int vd = -1, vn; if (vd < 0) vd = getenv("GSL_VDUMP") != 0;
        if (vd && vn < 20000 && ((((r_prmodecont & 1) ? r_prim : r_prmode) >> 5) & 1) && st_frames == 3) {
            vn++; LOG("VD q %.6f w %.3f z %.0f f %.0f x %.1f y %.1f\n", v.q, v.q ? 1.0 / v.q : 0, v.z, v.f, v.x, v.y);
        }
    }
    switch (type) {
    case 0: if (kick) { latch(); pixel((int)v.x, (int)v.y, v.z, (int)v.r, (int)v.g, (int)v.b, (int)v.a, v.s, v.t, v.q, v.u, v.v, v.f); } break;
    case 1: vq[nv++] = v; if (nv == 2) { if (kick) { latch(); line(&vq[0], &vq[1]); } nv = 0; } break;
    case 2: if (nv < 2) vq[nv++] = v; else { vq[0] = vq[1]; vq[1] = v; } if (nv == 2 && kick) { latch(); line(&vq[0], &vq[1]); } break;
    case 3: vq[nv++] = v; if (nv == 3) { if (kick) { latch(); tri(&vq[0], &vq[1], &vq[2]); } nv = 0; } break;
    case 4: if (nv < 3) vq[nv++] = v; else { vq[0] = vq[1]; vq[1] = vq[2]; vq[2] = v; }
            if (nv == 3 && kick) { latch(); tri(&vq[0], &vq[1], &vq[2]); } break;
    case 5: if (nv < 3) vq[nv++] = v; else { vq[1] = vq[2]; vq[2] = v; }
            if (nv == 3 && kick) { latch(); tri(&vq[0], &vq[1], &vq[2]); } break;
    case 6: vq[nv++] = v; if (nv == 2) { if (kick) { latch(); sprite(&vq[0], &vq[1]); } nv = 0; } break;
    default: break;
    }
}

static void gs_reg(int reg, uint64_t v)
{
    switch (reg) {
    case 0x00: r_prim = v; nv = 0; break;
    case 0x01: r_rgbaq = v; { uint32_t qb = (uint32_t)(v >> 32); memcpy(&q_cur, &qb, 4); } break;
    case 0x02: r_st = v; break;
    case 0x03: r_uv = v; break;
    case 0x04: vertex(v, 1, 1); break;
    case 0x05: vertex(v, 0, 1); break;
    case 0x0C: vertex(v, 1, 0); break;
    case 0x0D: vertex(v, 0, 0); break;
    case 0x06: case 0x07: cx[reg - 6].tex0 = v; break;
    case 0x08: case 0x09: cx[reg - 8].clamp = v; break;
    case 0x0A: r_fog = v; break;
    case 0x14: case 0x15: cx[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {                                       /* TEX2: PSM and palette fields of TEX0 */
        uint64_t m = (0x3Full << 20) | (0x7FFFFFFull << 37);
        cx[reg - 0x16].tex0 = (cx[reg - 0x16].tex0 & ~m) | (v & m);
    } break;
    case 0x18: case 0x19: cx[reg - 0x18].xyoff = v; break;
    case 0x1A: r_prmodecont = v; break;
    case 0x1B: r_prmode = v; break;
    case 0x1C: r_texclut = v; break;
    case 0x3B: r_texa = v; break;
    case 0x3D: r_fogcol = v; break;
    case 0x40: case 0x41: cx[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: cx[reg - 0x42].alpha = v; break;
    case 0x46: r_colclamp = v; break;
    case 0x47: case 0x48: cx[reg - 0x47].test = v; break;
    case 0x49: r_pabe = v & 1; break;
    case 0x4A: case 0x4B: cx[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: cx[reg - 0x4C].frame = v; break;
    case 0x4E: case 0x4F: cx[reg - 0x4E].zbuf = v; break;
    case 0x50: r_bitblt = v; break;
    case 0x51: r_trxpos = v; break;
    case 0x52: r_trxreg = v; break;
    case 0x53: xfer_start((int)(v & 3)); break;
    default: break;
    }
}

/* one GIF packet stream */
void hle_gs_lite_packets(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n)
{
    if (on < 0) {
        outdir = getenv("HLE_GS_FRAMES");
        on = outdir && *outdir;
        const char *e = getenv("HLE_GS_EVERY"); if (e) every = atoi(e);
        e = getenv("HLE_GS_MAX"); if (e) maxdump = atoi(e);
    }
    if (!on) return;
    if (!gsm) {
        gsm = (uint8_t *)calloc(1, GS_MEM_SIZE);
        if (gs_mem_selftest() != 0) LOG("GS-lite: swizzle table self-test FAILED\n");
    }
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
            for (uint32_t k = 0; k < nloop; k++)
                for (uint32_t r = 0; r < nreg; r++) {
                    const uint8_t *d = get(u, i++);
                    if (!d) return;
                    uint64_t a, b;
                    memcpy(&a, d, 8); memcpy(&b, d + 8, 8);
                    uint32_t reg = (uint32_t)(hi >> (4 * r)) & 15;
                    switch (reg) {
                    case 0x0: gs_reg(0x00, a & 0x7FF); break;
                    case 0x1: {                                                               /* RGBA; Q stays */
                        uint32_t qb; memcpy(&qb, &q_cur, 4);
                        r_rgbaq = (a & 0xFF) | ((a >> 32) & 0xFF) << 8 | (b & 0xFF) << 16 | ((b >> 32) & 0xFF) << 24 | (uint64_t)qb << 32;
                    } break;
                    case 0x2: { uint32_t qv = (uint32_t)b; gs_reg(0x02, a); memcpy(&q_cur, &qv, 4); } break;
                    case 0x3: gs_reg(0x03, (a & 0x3FFF) | ((a >> 32) & 0x3FFF) << 16); break;
                    case 0x4: {                                                               /* XYZF2: z 24 bits, f 8 */
                        uint64_t x = (a & 0xFFFF) | ((a >> 32) & 0xFFFF) << 16 | ((b >> 4) & 0xFFFFFF) << 32 | ((b >> 36) & 0xFF) << 56;
                        gs_reg((b >> 47) & 1 ? 0x0C : 0x04, x);
                    } break;
                    case 0x5: {                                                               /* XYZ2: z 32 bits */
                        uint64_t x = (a & 0xFFFF) | ((a >> 32) & 0xFFFF) << 16 | (b & 0xFFFFFFFFu) << 32;
                        gs_reg((b >> 47) & 1 ? 0x0D : 0x05, x);
                    } break;
                    case 0xA: r_fog = ((b >> 36) & 0xFF) << 56; break;                       /* FOG */
                    case 0xE: gs_reg((int)(b & 0xFF), a); break;                              /* A+D */
                    case 0xF: break;                                                          /* NOP */
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
            for (uint32_t k = 0; k < nloop; k++) { const uint8_t *d = get(u, i++); if (!d) return; xfer_data(d); }
        }
    }
}

/* ------------------------------------------------------------------ output */
static void dump_rect(uint32_t fbp, uint32_t fbw, int psm, int w, int y0, int h, const char *tag)
{
    char p[512];
    snprintf(p, sizeof p, "%s/frame_%04d_%s%04x.ppm", outdir, dumped, tag, fbp);
    FILE *o = fopen(p, "wb");
    if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int y = y0; y < y0 + h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = get_px((int)fbp, (int)fbw, psm, x, y);
            if (psm == 0x02 || psm == 0x0A) c = c16to32((uint16_t)c, 0);
            uint8_t rgb[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)};
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    dumped++;
}
static void dump(uint32_t fbp, uint32_t fbw, int psm, int w, int h, const char *tag)
{
    char p[512];
    snprintf(p, sizeof p, "%s/frame_%04d_%s%04x.ppm", outdir, dumped, tag, fbp);
    FILE *o = fopen(p, "wb");
    if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = get_px((int)fbp, (int)fbw, psm, x, y);
            if (psm == 0x02 || psm == 0x0A) c = c16to32((uint16_t)c, 0);
            uint8_t rgb[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)};
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    dumped++;
}
void hle_gs_lite_flip(void)
{
    if (on != 1) return;
    st_frames++;
    if (every > 0 && !(st_frames % (uint64_t)every) && dumped < maxdump && gsm) {
        uint64_t pmode = hle_gs_priv(0x00);
        uint64_t dfb = (pmode & 2) ? hle_gs_priv(0x90) : hle_gs_priv(0x70);
        uint64_t dsp = (pmode & 2) ? hle_gs_priv(0xA0) : hle_gs_priv(0x80);
        uint32_t fbw = (uint32_t)((dfb >> 9) & 0x3F);
        if ((pmode & 3) && fbw) {
            int w = (int)fbw * 64; if (w > 1024) w = 1024;
            int y0 = half_px[1] > half_px[0] ? 512 : 0;                        /* the buffer drawn this frame */
            char tag[16]; snprintf(tag, sizeof tag, "y%d_", y0);
            dump_rect((uint32_t)(dfb & 0x1FF) * 32, fbw, (int)((dfb >> 15) & 31), w, y0, 512, tag);
            (void)dsp;
        } else {
            Frame *best = 0;
            for (int i = 0; i < nfb; i++) if (!best || fbs[i].draws > best->draws) best = &fbs[i];
            if (best && best->draws) dump(best->fbp, 10, 0, 640, 448, "fbp");
        }
    }
    for (int i = 0; i < nfb; i++) fbs[i].draws = 0;
    half_px[0] = half_px[1] = 0;
}

void hle_gs_lite_report(void)
{
    if (on != 1) return;
    for (int k = 0; k < 64; k++) if (st_psm_miss[k]) LOG("GS-lite: unsupported psm 0x%02x: %llu texels\n", k, (unsigned long long)st_psm_miss[k]);
    LOG("GS-lite: %llu primitives, %llu pixels, %llu texture uploads, %llu flips, %d frames written to %s; display PMODE %llx "
        "DISPFB1 %llx DISPFB2 %llx DISPLAY2 %llx\n", (unsigned long long)st_draws, (unsigned long long)st_pixels,
        (unsigned long long)st_uploads, (unsigned long long)st_frames, dumped, outdir,
        (unsigned long long)hle_gs_priv(0), (unsigned long long)hle_gs_priv(0x70), (unsigned long long)hle_gs_priv(0x90),
        (unsigned long long)hle_gs_priv(0xA0));
    (void)st_tex_miss; (void)r_texclut;
    LOG("GS-lite: %llu primitives with fog (FGE), %llu into other render targets, %llu sampling a display buffer\n",
        (unsigned long long)st_fge, (unsigned long long)st_rt_draws, (unsigned long long)st_rt_tex);
}

/* ------------------------------------------------------------------ captures (see hle_gfx_scan.c) */
const uint8_t *hle_gs_lite_mem(void) { return gsm; }
/* the register state as one GIF A+D packet (qwords: tag + n), so a replay starts from the same state */
uint32_t hle_gs_lite_regs_packet(uint64_t *out /* 2 * 64 */)
{
    static const int regs[] = {0x06, 0x07, 0x08, 0x09, 0x14, 0x15, 0x18, 0x19, 0x1A, 0x1B, 0x3B, 0x3D, 0x40, 0x41, 0x42,
                               0x43, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x00, 0x01, 0x02, 0x03, 0x0A};
    int n = (int)(sizeof regs / sizeof regs[0]);
    uint64_t v[64];
    for (int k = 0; k < n; k++) {
        int r = regs[k];
        switch (r) {
        case 0x06: case 0x07: v[k] = cx[r - 6].tex0; break;
        case 0x08: case 0x09: v[k] = cx[r - 8].clamp; break;
        case 0x14: case 0x15: v[k] = cx[r - 0x14].tex1; break;
        case 0x18: case 0x19: v[k] = cx[r - 0x18].xyoff; break;
        case 0x40: case 0x41: v[k] = cx[r - 0x40].scissor; break;
        case 0x42: case 0x43: v[k] = cx[r - 0x42].alpha; break;
        case 0x47: case 0x48: v[k] = cx[r - 0x47].test; break;
        case 0x4A: case 0x4B: v[k] = cx[r - 0x4A].fba; break;
        case 0x4C: case 0x4D: v[k] = cx[r - 0x4C].frame; break;
        case 0x4E: case 0x4F: v[k] = cx[r - 0x4E].zbuf; break;
        case 0x1A: v[k] = r_prmodecont; break; case 0x1B: v[k] = r_prmode; break; case 0x3B: v[k] = r_texa; break;
        case 0x3D: v[k] = r_fogcol; break; case 0x46: v[k] = r_colclamp; break; case 0x49: v[k] = r_pabe; break;
        case 0x00: v[k] = r_prim; break; case 0x01: v[k] = r_rgbaq; break; case 0x02: v[k] = r_st; break;
        case 0x03: v[k] = r_uv; break; case 0x0A: v[k] = r_fog; break;
        }
    }
    out[0] = (uint64_t)n | (1ull << 15) | (0ull << 58) | (1ull << 60);     /* NLOOP n, EOP, PACKED, NREG 1 */
    out[1] = 0xE;                                                          /* A+D */
    for (int k = 0; k < n; k++) { out[2 + 2 * k] = v[k]; out[3 + 2 * k] = (uint64_t)regs[k]; }
    return (uint32_t)(1 + n);
}
void hle_gs_lite_load_mem(const uint8_t *m)
{
    if (on < 0) { hle_gs_lite_packets(0, 0, 0); }
    if (!gsm) gsm = (uint8_t *)calloc(1, GS_MEM_SIZE);
    memcpy(gsm, m, GS_MEM_SIZE);
}
