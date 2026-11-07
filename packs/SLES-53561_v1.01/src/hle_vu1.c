/* hle_vu1.c (both targets) - the VIF1 unit and the VU1 (micro mode): it turns the game's VIF1 command stream (mesh
 * data, VU1 microprograms, program starts) into the GIF packets the VU1 sends to the GS (XGKICK). Its output goes to
 * the GS sink hle_gs_lite_packets: the software GS on the PC (src/host/hle_gs_lite.c), the GE translator on the PSP
 * (src/psp/hle_ge_gs.c). Env / cfg HLE_VU1=0 switches it off (2D only).
 *
 * VIF1: STCYCL / OFFSET / BASE / ITOP / STMOD / STMASK / STROW / STCOL / MPG / MSCAL / MSCALF / MSCNT / DIRECT /
 * DIRECTHL and every UNPACK format (S/V2/V3/V4 x 32/16/8, V4-5), masking, offset / difference modes, the write
 * cycle (skipping and filling), the TOPS double buffer. VU programs run to their E bit when started (the VIF waits
 * anyway: FLUSH*, the next MSCAL, or the next UNPACK into the same buffer).
 *
 * VU1: all upper and lower instructions; a simple timing model: one instruction per cycle, FMAC results usable after
 * 4 cycles (readers stall; ACC is forwarded to the next instruction), MAC / status / clip flags visible 4 cycles after the op that set them, Q after 7 (DIV,
 * SQRT) or 13 (RSQRT) cycles, P after the EFU latency, the branch-after-integer-op rule (the branch sees the old VI).
 * Float: operands with exponent 255 read as +-FLT_MAX, denormals as 0, overflow saturates.
 * XGKICK sends the GIF packet at VI*16 to the software GS. */
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hle.h"
#ifdef HLE_PSP
#include "hle_cfg.h"
#include <pspkernel.h>
uint64_t hle_tprof[12];                      /* time (us): 0 VU1 interpreter, 1 native batches, 2 GS packets, 3 textures, 4 GE emit, 5 VIF1 total */
/* fine timers (per batch / per command) cost a system call each: only with -DHLE_TPROF_FINE; slot 5 always */
#define TPROF_ON(k, call) do { uint64_t t0_ = sceKernelGetSystemTimeWide(); call; hle_tprof[k] += sceKernelGetSystemTimeWide() - t0_; } while (0)
#ifdef HLE_TPROF_FINE
#define TPROF(k, call) TPROF_ON(k, call)
#else
#define TPROF(k, call) do { call; } while (0)
#endif
#else
#define TPROF(k, call) do { call; } while (0)
#define TPROF_ON(k, call) do { call; } while (0)
#endif

void hle_gs_lite_packets(const uint8_t *(*get)(void *, uint32_t), void *u, uint32_t n) __attribute__((weak));
extern volatile int hle_zone;                /* hle_gfx_scan.c: the sampled zone */
#define ZN(z, stmt) do { int pz_ = hle_zone; hle_zone = (z); stmt; hle_zone = pz_; } while (0)
void hle_path3_unmask(void);

typedef union { float f[4]; uint32_t u[4]; int32_t i[4]; } V4;

static struct {
    V4 vf[32], acc;
    uint16_t vi[16];
    float I, Q, P;
    uint32_t mac, status, clip, R;
    uint8_t mem[16384] __attribute__((aligned(16))), code[16384];   /* mem: 16-byte rows for the VFPU loads */
    uint32_t tpc;                                   /* where MSCNT continues */
    uint32_t now;                                    /* VU cycles (32 bits: compared as differences) */
    uint32_t vf_rdy[32], acc_rdy;
    /* pending results */
    uint32_t q_rdy; float q_new; int q_pend;
    uint32_t p_rdy; float p_new; int p_pend;
    struct { uint32_t rdy; uint32_t v; } macq[8], stq[8], clq[8];   /* FIFOs (ready times only grow): ring buffers */
    int nmac, nst, ncl, hmac, hst, hcl;
} vu;

/* VIF1 state */
static uint32_t v_cl = 1, v_wl = 1, v_mode, v_mask, v_row[4], v_col[4], v_base, v_ofst, v_tops, v_top, v_itops, v_itop, v_dbf;

static uint64_t st_kicks, st_runs, st_steps, st_unpack, st_mpg, st_overrun, st_badop;
uint32_t hle_vu1_batches(void);
uint32_t hle_vu1_batches(void) { return (uint32_t)st_runs; }

/* ------------------------------------------------------------------ floats */
/* exponent 255 -> +-0x7F7FFFFF, exponent 0 -> +-0; branchless (min / movz on the Allegrex) */
static inline uint32_t uclamp(uint32_t u)
{
    int32_t m = (int32_t)(u & 0x7FFFFFFFu);
    m = m < 0x7F7FFFFF ? m : 0x7F7FFFFF;
    m = (m >> 23) == 0 ? 0 : m;
    return (u & 0x80000000u) | (uint32_t)m;
}
static inline float fclamp(uint32_t u) { u = uclamp(u); float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t fres(float f)                 /* result: saturate, flush denormals */
{
    uint32_t u; memcpy(&u, &f, 4);
    return uclamp(u);
}
#define LANE(m, i) ((m) & (8 >> (i)))
#define TDUE(t) ((int32_t)((t) - vu.now) <= 0)                 /* ready time t reached */
#define TLATE(t) ((int32_t)((t) - vu.now) > 0)                /* t still ahead: stall until it */

/* ------------------------------------------------------------------ flags with latency */
static inline __attribute__((always_inline)) void flags_commit(void)
{
    while (vu.nmac && TDUE(vu.macq[vu.hmac].rdy)) { vu.mac = vu.macq[vu.hmac].v; vu.hmac = (vu.hmac + 1) & 7; vu.nmac--; }
    while (vu.nst && TDUE(vu.stq[vu.hst].rdy)) { uint32_t v = vu.stq[vu.hst].v; vu.status = (vu.status & ~0xFu) | v | ((v & 0xF) << 6); vu.hst = (vu.hst + 1) & 7; vu.nst--; }
    while (vu.ncl && TDUE(vu.clq[vu.hcl].rdy)) { vu.clip = vu.clq[vu.hcl].v; vu.hcl = (vu.hcl + 1) & 7; vu.ncl--; }
    if (vu.q_pend && TDUE(vu.q_rdy)) { vu.Q = vu.q_new; vu.q_pend = 0; }
    if (vu.p_pend && TDUE(vu.p_rdy)) { vu.P = vu.p_new; vu.p_pend = 0; }
}
static uint32_t pending_clip(void) { return vu.ncl ? vu.clq[(vu.hcl + vu.ncl - 1) & 7].v : vu.clip; }   /* CLIP shifts the newest value */
static inline __attribute__((always_inline)) void push_mac(uint32_t mac)
{
    uint32_t st = 0;
    if (mac & 0x000F) st |= 1;
    if (mac & 0x00F0) st |= 2;
    if (mac & 0x0F00) st |= 4;
    if (mac & 0xF000) st |= 8;
    if (vu.nmac == 8 || vu.nst == 8) flags_commit();                  /* (commits may be skipped between readers) */
    if (vu.nmac < 8) { int i = (vu.hmac + vu.nmac++) & 7; vu.macq[i].rdy = vu.now + 4; vu.macq[i].v = mac; }
    if (vu.nst < 8) { int i = (vu.hst + vu.nst++) & 7; vu.stq[i].rdy = vu.now + 4; vu.stq[i].v = st; }
}
static void push_clip(uint32_t c) { if (vu.ncl == 8) flags_commit(); if (vu.ncl < 8) { int i = (vu.hcl + vu.ncl++) & 7; vu.clq[i].rdy = vu.now + 4; vu.clq[i].v = c & 0xFFFFFF; } }

static inline __attribute__((always_inline)) void stall_vf(int r) { if (r && TLATE(vu.vf_rdy[r])) vu.now = vu.vf_rdy[r]; }

/* ------------------------------------------------------------------ upper */
typedef struct { int wr; int reg; int toacc; V4 v; int mask; int set_mac; uint32_t mac; } UpRes;

static inline __attribute__((always_inline)) void upper_k(uint32_t w, UpRes *r)
{
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, f = w & 63;
    memset(r, 0, sizeof *r);
    r->mask = dest;
    const V4 *S = &vu.vf[fs], *T = &vu.vf[ft];
    int base = -1, src2 = 0, bc = -1, toacc = 0, outreg = fd;   /* src2: 0 vf, 1 i, 2 q */
    /* base: 0 ADD 1 SUB 2 MADD 3 MSUB 4 MAX 5 MINI 6 MUL */
    if (f < 28) { base = f >> 2; bc = f & 3; }
    else if (f < 60) {
        switch (f) {
        case 28: base = 6; src2 = 2; break; case 29: base = 4; src2 = 1; break; case 30: base = 6; src2 = 1; break;
        case 31: base = 5; src2 = 1; break; case 32: base = 0; src2 = 2; break; case 33: base = 2; src2 = 2; break;
        case 34: base = 0; src2 = 1; break; case 35: base = 2; src2 = 1; break; case 36: base = 1; src2 = 2; break;
        case 37: base = 3; src2 = 2; break; case 38: base = 1; src2 = 1; break; case 39: base = 3; src2 = 1; break;
        case 40: base = 0; break; case 41: base = 2; break; case 42: base = 6; break; case 43: base = 4; break;
        case 44: base = 1; break; case 45: base = 3; break; case 47: base = 5; break;
        case 46: {                                                   /* OPMSUB */
            stall_vf(fs); stall_vf(ft); if (TLATE(vu.acc_rdy)) vu.now = vu.acc_rdy;
            float a[3] = {fclamp(S->u[1]) * fclamp(T->u[2]), fclamp(S->u[2]) * fclamp(T->u[0]), fclamp(S->u[0]) * fclamp(T->u[1])};
            uint32_t mac = 0;
            r->v = vu.vf[fd];
            for (int i = 0; i < 3; i++) {
                if (!LANE(dest, i)) continue;
                r->v.u[i] = fres(fclamp(vu.acc.u[i]) - a[i]);
                if (!(r->v.u[i] & 0x7FFFFFFF)) mac |= 8u >> i;
                if (r->v.u[i] >> 31) mac |= 0x80u >> i;
            }
            r->wr = 1; r->reg = fd; r->mask = dest & 0xE; r->set_mac = 1; r->mac = mac;
            return;
        }
        default: st_badop++; return;
        }
    } else {
        int code = ((w >> 6) & 31) << 2 | (w & 3);
        toacc = 1;
        if (code < 28 && (code >> 2) != 4 && (code >> 2) != 5) {
            static const int m[7] = {0, 1, 2, 3, -1, -1, 6};
            base = m[code >> 2]; bc = code & 3;
        } else switch (code) {
        case 16: case 17: case 18: case 19: {                        /* ITOF */
            static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
            stall_vf(fs);
            r->v = vu.vf[ft];
            for (int i = 0; i < 4; i++) if (LANE(dest, i)) r->v.u[i] = fres((float)S->i[i] * sc[code - 16]);
            r->wr = 1; r->reg = ft; return;
        }
        case 20: case 21: case 22: case 23: {                        /* FTOI */
            static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            stall_vf(fs);
            r->v = vu.vf[ft];
            for (int i = 0; i < 4; i++) if (LANE(dest, i)) {
                float d = fclamp(S->u[i]) * sc[code - 20];          /* exact (power of two); float only for the PSP */
                r->v.i[i] = d >= 2147483648.0f ? 0x7FFFFFFF : d <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)d;
            }
            r->wr = 1; r->reg = ft; return;
        }
        case 29:                                                     /* ABS */
            stall_vf(fs);
            r->v = vu.vf[ft];
            for (int i = 0; i < 4; i++) if (LANE(dest, i)) r->v.u[i] = S->u[i] & 0x7FFFFFFF;
            r->wr = 1; r->reg = ft; return;
        case 31: {                                                   /* CLIP */
            stall_vf(fs); stall_vf(ft);
            float lim = fabsf(fclamp(T->u[3]));
            uint32_t c = pending_clip() << 6;
            for (int i = 0; i < 3; i++) {
                float x = fclamp(S->u[i]);
                if (x > lim) c |= 1u << (2 * i);
                if (x < -lim) c |= 2u << (2 * i);
            }
            push_clip(c);
            return;
        }
        case 46: {                                                   /* OPMULA */
            stall_vf(fs); stall_vf(ft);
            V4 a = vu.acc;
            a.u[0] = fres(fclamp(S->u[1]) * fclamp(T->u[2]));
            a.u[1] = fres(fclamp(S->u[2]) * fclamp(T->u[0]));
            a.u[2] = fres(fclamp(S->u[0]) * fclamp(T->u[1]));
            r->wr = 1; r->toacc = 1; r->v = a; r->mask = dest & 0xE;
            uint32_t mac = 0;
            for (int i = 0; i < 3; i++) if (LANE(dest, i)) { if (!(a.u[i] & 0x7FFFFFFF)) mac |= 8u >> i; if (a.u[i] >> 31) mac |= 0x80u >> i; }
            r->set_mac = 1; r->mac = mac;
            return;
        }
        case 47: return;                                             /* NOP */
        case 28: base = 6; src2 = 2; break; case 30: base = 6; src2 = 1; break;
        case 32: base = 0; src2 = 2; break; case 33: base = 2; src2 = 2; break; case 34: base = 0; src2 = 1; break;
        case 35: base = 2; src2 = 1; break; case 36: base = 1; src2 = 2; break; case 37: base = 3; src2 = 2; break;
        case 38: base = 1; src2 = 1; break; case 39: base = 3; src2 = 1; break; case 40: base = 0; break;
        case 41: base = 2; break; case 42: base = 6; break; case 44: base = 1; break; case 45: base = 3; break;
        default: st_badop++; return;
        }
    }
    if (base < 0) { st_badop++; return; }
    stall_vf(fs);
    if (src2 == 0) stall_vf(ft);
    if ((base == 2 || base == 3) && TLATE(vu.acc_rdy)) vu.now = vu.acc_rdy;
    V4 out = toacc ? vu.acc : vu.vf[outreg];
    uint32_t mac = 0;
    for (int i = 0; i < 4; i++) {
        if (!LANE(dest, i)) continue;
        float a = fclamp(S->u[i]);
        float b = src2 == 1 ? fclamp(fres(vu.I)) : src2 == 2 ? vu.Q : fclamp(T->u[bc >= 0 ? bc : i]);
        float v;
        switch (base) {
        case 0: v = a + b; break;
        case 1: v = a - b; break;
        case 2: v = fclamp(vu.acc.u[i]) + a * b; break;
        case 3: v = fclamp(vu.acc.u[i]) - a * b; break;
        case 4: { int32_t x = S->i[i], y; float bb = b; memcpy(&y, &bb, 4); if (src2 == 0) y = T->i[bc >= 0 ? bc : i];
                  /* MAX/MINI compare as signed-magnitude integers */
                  int32_t kx = x < 0 ? (int32_t)(0x80000000u - (uint32_t)x) : x, ky = y < 0 ? (int32_t)(0x80000000u - (uint32_t)y) : y;
                  out.i[i] = kx > ky ? x : y; continue; }
        case 5: { int32_t x = S->i[i], y; float bb = b; memcpy(&y, &bb, 4); if (src2 == 0) y = T->i[bc >= 0 ? bc : i];
                  int32_t kx = x < 0 ? (int32_t)(0x80000000u - (uint32_t)x) : x, ky = y < 0 ? (int32_t)(0x80000000u - (uint32_t)y) : y;
                  out.i[i] = kx < ky ? x : y; continue; }
        default: v = a * b; break;
        }
        out.u[i] = fres(v);
        if (!(out.u[i] & 0x7FFFFFFF)) mac |= 8u >> i;
        if (out.u[i] >> 31) mac |= 0x80u >> i;
    }
    r->wr = 1; r->toacc = toacc; r->reg = outreg; r->v = out;
    if (base != 4 && base != 5) { r->set_mac = 1; r->mac = mac; }
}

static void upper_slow(uint32_t w, UpRes *r) { upper_k(w, r); }

/* the common upper instructions predecoded once per code address (checked against the word: no invalidation);
   the float ops (ADD / SUB / MADD / MSUB / MUL with VF, broadcast, I or Q) run here without the decode switch and the
   per-lane operator switch; everything else goes to upper_slow (same results) */
typedef struct { uint32_t w; uint8_t ok, kind, base, src2, toacc, dest, fs, ft, outreg; int8_t bc; } UDec;
static UDec udt[2048];
static void udecode(UDec *d, uint32_t w)
{
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, f = w & 63;
    int base = -1, src2 = 0, bc = -1, toacc = 0;
    d->w = w; d->ok = 1; d->kind = 0;                                 /* 0 slow, 1 nop, 2 float op */
    if (f < 28) { base = f >> 2; bc = f & 3; }
    else if (f < 60) {
        switch (f) {
        case 28: base = 6; src2 = 2; break; case 30: base = 6; src2 = 1; break;
        case 32: base = 0; src2 = 2; break; case 33: base = 2; src2 = 2; break;
        case 34: base = 0; src2 = 1; break; case 35: base = 2; src2 = 1; break; case 36: base = 1; src2 = 2; break;
        case 37: base = 3; src2 = 2; break; case 38: base = 1; src2 = 1; break; case 39: base = 3; src2 = 1; break;
        case 40: base = 0; break; case 41: base = 2; break; case 42: base = 6; break;
        case 44: base = 1; break; case 45: base = 3; break;
        default: return;
        }
    } else {
        int code = ((w >> 6) & 31) << 2 | (w & 3);
        toacc = 1;
        if (code == 47) { d->kind = 1; return; }
        if (code < 28 && (code >> 2) != 4 && (code >> 2) != 5) { static const int m[7] = {0, 1, 2, 3, -1, -1, 6}; base = m[code >> 2]; bc = code & 3; }
        else switch (code) {
        case 28: base = 6; src2 = 2; break; case 30: base = 6; src2 = 1; break;
        case 32: base = 0; src2 = 2; break; case 33: base = 2; src2 = 2; break; case 34: base = 0; src2 = 1; break;
        case 35: base = 2; src2 = 1; break; case 36: base = 1; src2 = 2; break; case 37: base = 3; src2 = 2; break;
        case 38: base = 1; src2 = 1; break; case 39: base = 3; src2 = 1; break; case 40: base = 0; break;
        case 41: base = 2; break; case 42: base = 6; break; case 44: base = 1; break; case 45: base = 3; break;
        default: return;
        }
    }
    if (base < 0 || base == 4 || base == 5) return;
    d->kind = 2; d->base = (uint8_t)base; d->src2 = (uint8_t)src2; d->bc = (int8_t)bc; d->toacc = (uint8_t)toacc;
    d->dest = (uint8_t)dest; d->fs = (uint8_t)fs; d->ft = (uint8_t)ft; d->outreg = (uint8_t)fd;
}
static void upper(uint32_t pc, uint32_t w, UpRes *r)
{
    UDec *d = &udt[pc & 2047];
    if (!d->ok || d->w != w) udecode(d, w);
    if (d->kind == 1) { r->wr = 0; r->set_mac = 0; return; }
    if (d->kind != 2) { upper_slow(w, r); return; }
    const int dest = d->dest, base = d->base, src2 = d->src2, bc = d->bc;
    const V4 *S = &vu.vf[d->fs], *T = &vu.vf[d->ft];
    stall_vf(d->fs);
    if (src2 == 0) stall_vf(d->ft);
    if ((base == 2 || base == 3) && TLATE(vu.acc_rdy)) vu.now = vu.acc_rdy;
    V4 out = d->toacc ? vu.acc : vu.vf[d->outreg];
    float bq = src2 == 1 ? fclamp(fres(vu.I)) : vu.Q;
    uint32_t mac = 0;
    for (int i = 0; i < 4; i++) {
        if (!LANE(dest, i)) continue;
        float a = fclamp(S->u[i]);
        float b = src2 ? bq : fclamp(T->u[bc >= 0 ? bc : i]);
        float v;
        if (base == 6) v = a * b;
        else if (base == 0) v = a + b;
        else if (base == 1) v = a - b;
        else if (base == 2) v = fclamp(vu.acc.u[i]) + a * b;
        else v = fclamp(vu.acc.u[i]) - a * b;
        uint32_t u = fres(v);
        out.u[i] = u;
        if (!(u & 0x7FFFFFFF)) mac |= 8u >> i;
        if (u >> 31) mac |= 0x80u >> i;
    }
    r->wr = 1; r->toacc = d->toacc; r->reg = d->outreg; r->v = out; r->mask = dest; r->set_mac = 1; r->mac = mac;
}

/* ------------------------------------------------------------------ lower */
static inline uint8_t *vmem(uint32_t qwaddr) { return vu.mem + ((qwaddr & 0x3FF) << 4); }
#ifndef HLE_PSP
FILE *hle_vu1_kickdump;
#endif
static void ncap_kick(uint32_t qa);
static int ncap_on;
#ifndef HLE_PSP
static uint64_t kick_hash = 1469598103934665603ull;      /* host check: every kicked packet's bytes (report) */
#endif
static uint64_t st_kick_qw;                                   /* qwords the XGKICKs send to the GS path */
static void xgkick(uint32_t qa)
{
    st_kicks++;
#ifndef HLE_PSP
    {
        uint32_t a = qa;
        for (int guard = 0; guard < 64; guard++) {
            uint64_t lo; memcpy(&lo, vmem(a), 8);
            uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
            if (!nreg) nreg = 16;
            uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            for (uint32_t k = 0; k <= body; k++) { const uint8_t *q = vmem(a + k); for (int b = 0; b < 16; b++) kick_hash = (kick_hash ^ q[b]) * 1099511628211ull; }
            a += 1 + body;
            if ((lo >> 15) & 1) break;
        }
    }
    if (ncap_on == 2) ncap_kick(qa);
    if (hle_vu1_kickdump) {                    /* the packet (to EOP), as qwords */
        uint32_t a = qa;
        for (int guard = 0; guard < 64; guard++) {
            uint64_t lo; memcpy(&lo, vmem(a), 8);
            uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
            if (!nreg) nreg = 16;
            uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            for (uint32_t k = 0; k <= body; k++) fwrite(vmem(a + k), 1, 16, hle_vu1_kickdump);
            a += 1 + body;
            if ((lo >> 15) & 1) break;
        }
    }
#endif
    if (!hle_gs_lite_packets) return;
    /* count the packet's qwords up to EOP (wrapping in VU memory) */
    uint32_t n = 0, a = qa;
    for (int guard = 0; guard < 2048; guard++) {
        uint64_t lo; memcpy(&lo, vmem(a), 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        n += 1 + body; a += 1 + body;
        if ((lo >> 15) & 1) break;
        if (n > 1024) break;
    }
    struct K { uint32_t a; } k = {qa};
    st_kick_qw += n;
    extern const uint8_t *hle_vu1_kick_get(void *, uint32_t);
    hle_gs_lite_packets(hle_vu1_kick_get, &k, n);
}
const uint8_t *hle_vu1_kick_get(void *u, uint32_t k) { return vmem(*(uint32_t *)u + k); }

static float efu(int op, const V4 *S, int fsf, int *lat)
{
    float x = fclamp(S->u[0]), y = fclamp(S->u[1]), z = fclamp(S->u[2]), w = fclamp(S->u[3]), e = fclamp(S->u[fsf]);
    switch (op) {
    case 112: *lat = 11; return x * x + y * y + z * z;
    case 113: *lat = 18; { float s = x * x + y * y + z * z; return s ? 1.0f / s : FLT_MAX; }
    case 114: *lat = 18; return sqrtf(x * x + y * y + z * z);
    case 115: *lat = 24; { float s = sqrtf(x * x + y * y + z * z); return s ? 1.0f / s : FLT_MAX; }
    case 116: *lat = 54; return x ? atanf(y / x) : 0.0f;
    case 117: *lat = 54; return x ? atanf(z / x) : 0.0f;
    case 118: *lat = 12; return x + y + z + w;
    case 120: *lat = 12; return sqrtf(fabsf(e));
    case 121: *lat = 18; return e ? 1.0f / sqrtf(fabsf(e)) : FLT_MAX;
    case 122: *lat = 12; return e ? 1.0f / e : FLT_MAX;
    case 124: *lat = 29; return sinf(e);
    case 125: *lat = 54; return atanf(e);
    case 126: *lat = 44; return expf(-e);
    }
    *lat = 1; return 0;
}

/* returns: 0 continue, 1 branch taken (target in *tgt) */
static inline __attribute__((always_inline)) int lower(uint32_t w, uint32_t pc, uint32_t *tgt, int prev_vi_reg, uint16_t prev_vi_old, int *vi_written, uint16_t *vi_old)
{
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, id = (w >> 6) & 31;
    int it = ft & 15, is = fs & 15, idd = id & 15;
    int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
#define VI(r) ((r) == prev_vi_reg ? prev_vi_old : vu.vi[r])          /* branches: old value after an integer op */
#define SETVI(r, v) do { if (r) { *vi_written = (r); *vi_old = vu.vi[r]; vu.vi[r] = (uint16_t)(v); } } while (0)
    if (w >> 31) {
        if ((w >> 25) != 0x40) { st_badop++; return 0; }
        int f = w & 63;
        if (f < 60) {
            switch (f) {
            case 48: SETVI(idd, vu.vi[is] + vu.vi[it]); break;
            case 49: SETVI(idd, vu.vi[is] - vu.vi[it]); break;
            case 50: { int v = (w >> 6) & 31; if (v & 16) v -= 32; SETVI(it, vu.vi[is] + v); } break;
            case 52: SETVI(idd, vu.vi[is] & vu.vi[it]); break;
            case 53: SETVI(idd, vu.vi[is] | vu.vi[it]); break;
            default: st_badop++;
            }
            return 0;
        }
        int code = ((w >> 6) & 31) << 2 | (w & 3);
        V4 *T = &vu.vf[ft]; const V4 *S = &vu.vf[fs];
        switch (code) {
        case 48: stall_vf(fs); if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) T->u[i] = S->u[i]; break;          /* MOVE */
        case 49: { stall_vf(fs); V4 s = *S; if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) T->u[i] = s.u[(i + 1) & 3]; } break;  /* MR32 */
        case 52: { uint8_t *m = vmem(vu.vi[is]); if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(&T->u[i], m + 4 * i, 4);
                   SETVI(is, vu.vi[is] + 1); } break;                                                                      /* LQI */
        case 53: { stall_vf(fs); uint8_t *m = vmem(vu.vi[it]); for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(m + 4 * i, &S->u[i], 4);
                   SETVI(it, vu.vi[it] + 1); } break;                                                                      /* SQI */
        case 54: { SETVI(is, vu.vi[is] - 1); uint8_t *m = vmem(vu.vi[is]); if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(&T->u[i], m + 4 * i, 4); } break;  /* LQD */
        case 55: { stall_vf(fs); SETVI(it, vu.vi[it] - 1); uint8_t *m = vmem(vu.vi[it]); for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(m + 4 * i, &S->u[i], 4); } break;  /* SQD */
        case 56: case 57: case 58: {                                                                                       /* DIV SQRT RSQRT */
            stall_vf(fs); stall_vf(ft);
            if (vu.q_pend) { if (TLATE(vu.q_rdy)) vu.now = vu.q_rdy; vu.Q = vu.q_new; vu.q_pend = 0; }
            float a = fclamp(S->u[fsf]), b = fclamp(T->u[ftf]), q;
            vu.status &= ~0x30u;
            if (code == 56) { if (b == 0) { q = (a == 0) ? 0 : ((S->u[fsf] ^ T->u[ftf]) >> 31 ? -FLT_MAX : FLT_MAX); vu.status |= a == 0 ? 0x10 : 0x20; } else q = a / b; }
            else if (code == 57) q = sqrtf(fabsf(b));
            else { float s = sqrtf(fabsf(b)); if (s == 0) { q = (S->u[fsf] >> 31) ? -FLT_MAX : FLT_MAX; vu.status |= 0x20; } else q = a / s; }
            memcpy(&q, (uint32_t[]){fres(q)}, 4);
            vu.q_new = q; vu.q_pend = 1; vu.q_rdy = vu.now + (code == 58 ? 13 : 7);
        } break;
        case 59: if (vu.q_pend) { if (TLATE(vu.q_rdy)) vu.now = vu.q_rdy; vu.Q = vu.q_new; vu.q_pend = 0; } break;          /* WAITQ */
        case 60: stall_vf(fs); SETVI(it, S->u[fsf] & 0xFFFF); break;                                                       /* MTIR */
        case 61: if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) T->i[i] = (int16_t)vu.vi[is]; break;              /* MFIR */
        case 62: { uint8_t *m = vmem(vu.vi[is]); for (int i = 0; i < 4; i++) if (LANE(dest, i)) { uint32_t x; memcpy(&x, m + 4 * i, 4); SETVI(it, x & 0xFFFF); break; } } break;  /* ILWR */
        case 63: { uint8_t *m = vmem(vu.vi[is]); for (int i = 0; i < 4; i++) if (LANE(dest, i)) { uint32_t x = vu.vi[it]; memcpy(m + 4 * i, &x, 4); } } break;  /* ISWR */
        case 64: {                                                                                                          /* RNEXT */
            uint32_t x = vu.R; int b = ((x >> 4) ^ (x >> 22)) & 1; x = ((x << 1) | (uint32_t)b) & 0x7FFFFF; vu.R = x;
            if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) T->u[i] = 0x3F800000u | x;
        } break;
        case 65: if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) T->u[i] = 0x3F800000u | vu.R; break;               /* RGET */
        case 66: vu.R = S->u[fsf] & 0x7FFFFF; break;                                                                        /* RINIT */
        case 67: vu.R = (vu.R ^ S->u[fsf]) & 0x7FFFFF; break;                                                               /* RXOR */
        case 100: if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(&T->u[i], &vu.P, 4); break;               /* MFP */
        case 104: SETVI(it, v_top); break;                                                                                  /* XTOP */
        case 105: SETVI(it, v_itop); break;                                                                                 /* XITOP */
        case 108: xgkick(vu.vi[is]); break;                                                                                 /* XGKICK */
        case 123: if (vu.p_pend) { if (TLATE(vu.p_rdy)) vu.now = vu.p_rdy; vu.P = vu.p_new; vu.p_pend = 0; } break;      /* WAITP */
        case 112: case 113: case 114: case 115: case 116: case 117: case 118: case 120: case 121: case 122: case 124: case 125: case 126: {
            stall_vf(fs);
            if (vu.p_pend) { if (TLATE(vu.p_rdy)) vu.now = vu.p_rdy; vu.P = vu.p_new; vu.p_pend = 0; }
            int lat; float p = efu(code, S, fsf, &lat);
            uint32_t pu = fres(p); memcpy(&p, &pu, 4);
            vu.p_new = p; vu.p_pend = 1; vu.p_rdy = vu.now + (uint32_t)lat;
        } break;
        default: st_badop++;
        }
        return 0;
    }
    int op = (int)(w >> 25);
    int imm11 = (int)(w & 0x7FF); if (imm11 & 0x400) imm11 -= 0x800;
    switch (op) {
    case 0x00: { uint8_t *m = vmem((uint32_t)(vu.vi[is] + imm11)); if (ft) for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(&vu.vf[ft].u[i], m + 4 * i, 4); } break;   /* LQ */
    case 0x01: { stall_vf(fs); uint8_t *m = vmem((uint32_t)(vu.vi[it] + imm11)); for (int i = 0; i < 4; i++) if (LANE(dest, i)) memcpy(m + 4 * i, &vu.vf[fs].u[i], 4); } break;  /* SQ */
    case 0x04: { uint8_t *m = vmem((uint32_t)(vu.vi[is] + imm11)); for (int i = 0; i < 4; i++) if (LANE(dest, i)) { uint32_t x; memcpy(&x, m + 4 * i, 4); SETVI(it, x & 0xFFFF); break; } } break;  /* ILW */
    case 0x05: { uint8_t *m = vmem((uint32_t)(vu.vi[is] + imm11)); for (int i = 0; i < 4; i++) if (LANE(dest, i)) { uint32_t x = vu.vi[it]; memcpy(m + 4 * i, &x, 4); } } break;  /* ISW */
    case 0x08: SETVI(it, vu.vi[is] + (((w >> 10) & 0x7800) | (w & 0x7FF))); break;            /* IADDIU */
    case 0x09: SETVI(it, vu.vi[is] - (((w >> 10) & 0x7800) | (w & 0x7FF))); break;            /* ISUBIU */
    case 0x10: flags_commit(); vu.vi[1] = (vu.clip & 0xFFFFFF) == (w & 0xFFFFFF); break;      /* FCEQ */
    case 0x11: vu.clip = w & 0xFFFFFF; vu.ncl = 0; break;                                     /* FCSET */
    case 0x12: vu.vi[1] = (vu.clip & w & 0xFFFFFF) != 0; break;                               /* FCAND */
    case 0x13: vu.vi[1] = ((vu.clip | w) & 0xFFFFFF) == 0xFFFFFF; break;                      /* FCOR */
    case 0x14: SETVI(it, (vu.status & 0xFFF) == ((((w >> 10) & 0x800) | (w & 0x7FF)))); break; /* FSEQ */
    case 0x15: vu.status = (vu.status & 0x3F) | ((((w >> 10) & 0x800) | (w & 0x7FF)) & 0xFC0); break;  /* FSSET */
    case 0x16: SETVI(it, vu.status & (((w >> 10) & 0x800) | (w & 0x7FF))); break;              /* FSAND */
    case 0x17: SETVI(it, vu.status | (((w >> 10) & 0x800) | (w & 0x7FF))); break;              /* FSOR */
    case 0x18: SETVI(it, vu.vi[is] == (vu.mac & 0xFFFF)); break;                              /* FMEQ */
    case 0x1A: SETVI(it, vu.vi[is] & vu.mac); break;                                          /* FMAND */
    case 0x1B: SETVI(it, vu.vi[is] | vu.mac); break;                                          /* FMOR */
    case 0x1C: SETVI(it, vu.clip & 0xFFF); break;                                             /* FCGET */
    case 0x20: *tgt = (uint32_t)((int)pc + 1 + imm11); return 1;                               /* B */
    case 0x21: { uint32_t t = (uint32_t)((int)pc + 1 + imm11); SETVI(it, pc + 2); *tgt = t; return 1; }  /* BAL */
    case 0x24: *tgt = VI(is); return 1;                                                        /* JR (8-byte units) */
    case 0x25: { uint32_t t = VI(is); SETVI(it, pc + 2); *tgt = t; return 1; }                 /* JALR */
    case 0x28: if (VI(it) == VI(is)) { *tgt = (uint32_t)((int)pc + 1 + imm11); return 1; } break;
    case 0x29: if (VI(it) != VI(is)) { *tgt = (uint32_t)((int)pc + 1 + imm11); return 1; } break;
    case 0x2C: if ((int16_t)VI(is) < 0) { *tgt = (uint32_t)((int)pc + 1 + imm11); return 1; } break;
    case 0x2D: if ((int16_t)VI(is) > 0) { *tgt = (uint32_t)((int)pc + 1 + imm11); return 1; } break;
    case 0x2E: if ((int16_t)VI(is) <= 0) { *tgt = (uint32_t)((int)pc + 1 + imm11); return 1; } break;
    case 0x2F: if ((int16_t)VI(is) >= 0) { *tgt = (uint32_t)((int)pc + 1 + imm11); return 1; } break;
    default: st_badop++;
    }
    return 0;
#undef VI
#undef SETVI
}

/* profile (env HLE_VU1_PROF): per program start + code hash: runs, instructions, XGKICKs */
typedef struct { uint32_t pc, hash; uint64_t runs, steps, kicks; } Prof;
static Prof prof[256];
static int nprof, prof_on = -1;
static uint64_t kicks_before;
static uint32_t code_hash(uint32_t pc)
{
    uint32_t h = 2166136261u;
    for (uint32_t k = pc; k < pc + 64 && k < 2048; k++) { uint32_t w; memcpy(&w, vu.code + k * 8 + 4, 4); h = (h ^ w) * 16777619u; memcpy(&w, vu.code + k * 8, 4); h = (h ^ w) * 16777619u; }
    return h;
}
static Prof *prof_get(uint32_t pc)
{
    uint32_t h = code_hash(pc);
    for (int i = 0; i < nprof; i++) if (prof[i].pc == pc && prof[i].hash == h) return &prof[i];
    if (nprof >= 256) return 0;
    prof[nprof].pc = pc; prof[nprof].hash = h;
    return &prof[nprof++];
}
void hle_vu1_dump_program(uint32_t pc, uint32_t hash, const char *path);
/* deferred UNPACKs (PSP mesh cache): a batch's vertex UNPACKs (TOPS-relative, no mask / mode) are only recorded;
   at the kick the native mesh path keys its converted copy by their data and skips them when the copy exists.
   Anything else that could read VU memory writes them first (pu_flush). The data stays in the VIF1 chunk buffer:
   every return of hle_vif1_chunk flushes. */
typedef struct { const uint32_t *src; uint32_t cmd, imm, num, nw, tops, cl, wl, stable; } PU;
int hle_vif_src_stable;                       /* hle_gfx_scan.c: the words being parsed are the game's RAM (not a copy) */
#define NPU 8
static PU pu[NPU]; static int npu;
static int mc_on = -1;
static void unpack(uint32_t cmd, uint32_t imm, uint32_t num, const uint32_t *src, uint32_t nsrc_words);
static void pu_flush(void)
{
    if (!npu) return;
    uint32_t cl = v_cl, wl = v_wl, tops = v_tops;
    for (int i = 0; i < npu; i++) { v_cl = pu[i].cl; v_wl = pu[i].wl; v_tops = pu[i].tops; unpack(pu[i].cmd, pu[i].imm, pu[i].num, pu[i].src, pu[i].nw); }
    v_cl = cl; v_wl = wl; v_tops = tops;
    npu = 0;
}
#include "hle_vu1_native.h"

/* one VU1 instruction pair (the interpreter's step; the ahead-of-time code inlines it with constant words, so the
   decoders fold to the one instruction) */
typedef struct { int prev_vi_reg; uint16_t prev_vi_old; int branch_pending; uint32_t branch_tgt; int ebit_pending; } VStep;
#define VU_END 0xFFFFFFFFu
#define VU_NOAOT 0xFFFFFFFEu
/* the parts every instruction shares stay out of line (code size of the ahead-of-time programs) */
static __attribute__((noinline)) void push_mac_call(uint32_t mac) { push_mac(mac); }
static inline __attribute__((always_inline)) uint32_t vu_tail(uint32_t pc, uint32_t uw, const UpRes *ur, int br, uint32_t tgt, int vi_written, uint16_t vi_old, VStep *s)
{
    if (ur->wr) {                                                       /* upper commits after lower (and wins) */
        V4 *d = ur->toacc ? &vu.acc : &vu.vf[ur->reg];
        if (ur->toacc || ur->reg) {
            for (int i = 0; i < 4; i++) if (LANE(ur->mask, i)) d->u[i] = ur->v.u[i];
            if (ur->toacc) vu.acc_rdy = vu.now + 1; /* ACC is forwarded: MULA/MADDA/MADD chains issue back to back */ else vu.vf_rdy[ur->reg] = vu.now + 4;
        }
    }
    if (ur->set_mac) push_mac_call(ur->mac);
    vu.vi[0] = 0;
    vu.vf[0].u[0] = 0; vu.vf[0].u[1] = 0; vu.vf[0].u[2] = 0; vu.vf[0].u[3] = 0x3F800000u;
    s->prev_vi_reg = vi_written; s->prev_vi_old = vi_old;
    vu.now++;
    if (s->ebit_pending) { vu.tpc = pc + 1; return VU_END; }          /* this was the E-bit delay slot */
    if (s->branch_pending) { s->branch_pending = 0; if ((uw >> 30) & 1) s->ebit_pending = 1; return s->branch_tgt & 0x7FF; }
    if ((uw >> 30) & 1) s->ebit_pending = 1;
    if (br) { s->branch_pending = 1; s->branch_tgt = tgt; }
    return (pc + 1) & 0x7FF;
}
/* instructions that read what the flag / Q / P queues hand over (the ahead-of-time code commits the queues only
   before these: the values a reader sees are the same; DIV / WAITQ / EFU / WAITP handle their own pending result) */
static inline __attribute__((always_inline)) int vu_reader(uint32_t lw, uint32_t uw)
{
    int f = uw & 63, code = ((uw >> 6) & 31) << 2 | (uw & 3);
    if (f < 60 ? (f == 28 || f == 32 || f == 33 || f == 36 || f == 37) : (code == 28 || code == 32 || code == 33 || code == 36 || code == 37)) return 1;   /* Q operand */
    if (uw >> 31) return 0;                                            /* LOI */
    if (lw >> 31) return (((lw >> 6) & 31) << 2 | (lw & 3)) == 100;   /* MFP */
    uint32_t op = lw >> 25;
    return op >= 0x12 && op <= 0x1C && op != 0x19;                     /* FCAND..FCGET, FS*, FM* */
}
static inline __attribute__((always_inline)) uint32_t vu_step(uint32_t pc, uint32_t lw, uint32_t uw, VStep *s)
{
    if (!__builtin_constant_p(uw) || !__builtin_constant_p(lw) || vu_reader(lw, uw)) flags_commit();
    st_steps++;
    UpRes ur;
    if (__builtin_constant_p(uw)) upper_k(uw, &ur); else upper(pc, uw, &ur);
    int vi_written = -1; uint16_t vi_old = 0;
    uint32_t tgt = 0; int br = 0;
    if (uw >> 31) memcpy(&vu.I, &lw, 4);                                /* LOI (I read by upper above: old I) */
    else br = lower(lw, pc, &tgt, s->prev_vi_reg, s->prev_vi_old, &vi_written, &vi_old);
    return vu_tail(pc, uw, &ur, br, tgt, vi_written, vi_old, s);
}
#if defined(__has_include) && !defined(VU1_NOFAST)
#if __has_include("hle_vu1_fast.h")
#define VU1_FAST 1
#include "hle_vu1_fast.h"
#endif
#endif
#if defined(__has_include) && !defined(VU1_NOAOT)
#if __has_include("hle_vu1_aot.h")
#define VU1_AOT 1
#include "hle_vu1_aot.h"
#endif
#endif

static uint32_t st_pcrun[0x800], st_pcsteps[0x800], st_pcat; static uint64_t st_pcstep0;
static uint64_t st_vcmd, st_uq[3], st_unum[3]; static uint32_t st_vc[128];   /* (st_vcmd: summed from st_vc per window) */
static uint64_t st_sku[5];
                                    /* skin unpacks: no FLG, masked, mode, wl > cl, deferrable */               /* VIF codes; unpacked qwords / unpacks by code image */
static void vu1_run_(uint32_t pc);
static void vu1_run(uint32_t pc) { ZN(6, TPROF(0, vu1_run_(pc))); }
/* run from instruction index pc until the E bit (+ its delay slot) */
static void vu1_run_(uint32_t pc)
{
    st_runs++;
    if (prof_on < 0) { const char *e = getenv("HLE_VU1_PROF"); prof_on = (e && *e) || getenv("HLE_VU1_TRACE"); }
    uint32_t start_pc = pc;
    Prof *pf = 0; uint64_t s0 = st_steps; kicks_before = st_kicks;
    { int done_; TPROF(1, done_ = nat_try(pc)); if (done_) return; }
    pu_flush();
#ifdef HLE_PSP
    {   /* test switch: cfg x_vu1skip=<entry address, hex> drops the interpreted runs from there (what they draw) */
        static int sk = -2;
        if (sk == -2) { const char *e = hle_cfg_get("x_vu1skip"); sk = e ? (int)strtol(e, 0, 16) : -1; }
        if (sk >= 0 && (uint32_t)sk == pc && code_hash(pc < 0x555 ? 0 : 0x555) == (pc < 0x555 ? 0xc3651f62u : 0x3d8ad419u)) return;
    }
#endif
    st_pcrun[pc & 0x7FF]++; st_pcstep0 = st_steps; st_pcat = pc & 0x7FF;
    hle_zone = 8;
#ifndef HLE_PSP
    {   /* statistics of the batch variants (env HLE_VU1_VARSTAT): 1023.y jump target, flags */
        static int vs = -1; if (vs < 0) vs = getenv("HLE_VU1_VARSTAT") != 0;
        if (vs && pc == 0x424) {
            uint32_t y, x, uvo, cm; memcpy(&x, vu.mem + 16 * 1023, 4); memcpy(&y, vu.mem + 16 * 1023 + 4, 4);
            memcpy(&uvo, vu.mem + 16 * 973 + 8, 4); memcpy(&cm, vu.mem + 16 * 976 + 12, 4);
            LOG("VARSTAT %x %x %x %x itop %u code %08x\n", y, x, uvo, cm, v_itop, code_hash(0));
        }
    }
    {
        static int bd = -1, nbd; static const char *bdir; static uint32_t bpc;
        if (bd < 0) { bdir = getenv("HLE_VU1_BATCHDUMP"); bd = bdir != 0; const char *e = getenv("HLE_VU1_BATCHPC"); bpc = e ? (uint32_t)strtoul(e, 0, 16) : 0x424; }
        static long bh = -2; if (bh == -2) { const char *e = getenv("HLE_VU1_BATCHHASH"); bh = e ? (long)strtoul(e, 0, 16) : -1; }
        static int bmax = -1; if (bmax < 0) { const char *e = getenv("HLE_VU1_BATCHMAX"); bmax = e ? atoi(e) : 40; }
        if (bd && pc == bpc && nbd < bmax && (bh < 0 || code_hash(0) == (uint32_t)bh)) {
            char fn[512]; snprintf(fn, sizeof fn, "%s/batch_%03d.in", bdir, nbd);
            FILE *f = fopen(fn, "wb");
            if (f) {
                fwrite(vu.mem, 1, 16384, f);
                uint32_t regs[16] = {v_top, v_itop, v_tops, v_base, v_ofst, v_dbf};
                fwrite(regs, 4, 16, f);
                for (int r = 0; r < 32; r++) fwrite(vu.vf[r].u, 4, 4, f);
                for (int r = 0; r < 16; r++) { uint32_t x = vu.vi[r]; fwrite(&x, 4, 1, f); }
                fclose(f);
            }
            snprintf(fn, sizeof fn, "%s/batch_%03d.out", bdir, nbd);
            extern FILE *hle_vu1_kickdump; hle_vu1_kickdump = fopen(fn, "wb");
            nbd++;
        }
    }
#endif
    if (prof_on) {
        pf = prof_get(pc);
#ifndef HLE_PSP
        if (pf && pf->runs == 0) {                      /* first run of this program: dump code + VU memory */
            const char *dir = getenv("HLE_VU1_PROF");
            if (dir && dir[0] == '/') {
                char fn[512];
                snprintf(fn, sizeof fn, "%s/prog_%03x_%08x.code", dir, pc, pf->hash);
                FILE *f = fopen(fn, "wb"); if (f) { fwrite(vu.code, 1, 16384, f); fclose(f); }
                snprintf(fn, sizeof fn, "%s/prog_%03x_%08x.mem", dir, pc, pf->hash);
                f = fopen(fn, "wb");
                if (f) {
                    fwrite(vu.mem, 1, 16384, f);
                    uint32_t regs[16] = {v_top, v_itop, v_tops, v_base, v_ofst, v_dbf};
                    fwrite(regs, 4, 16, f);
                    for (int r = 0; r < 32; r++) fwrite(vu.vf[r].u, 4, 4, f);
                    for (int r = 0; r < 16; r++) { uint32_t x = vu.vi[r]; fwrite(&x, 4, 1, f); }
                    fclose(f);
                }
            }
        }
#endif
    }
    VStep st = {-1, 0, 0, 0, 0};
    uint32_t steps = 0;
#ifdef VU1_FAST
    {   /* a program translated without the per-instruction timing model (hle_vu1_fast.h, tools/vu1_fast.py) */
        static int fon = -1;
#ifdef HLE_PSP
        if (fon < 0) { const char *e = hle_cfg_get("x_vu1fast"); fon = e ? atoi(e) : 1; }
#else
        if (fon < 0) { const char *e = getenv("HLE_VU1_FAST"); fon = e ? atoi(e) : 1; }
#endif
        if (fon) {
            uint32_t r = fast_run(pc);
            if (r == VU_END) goto end;
            if (r != VU_NOAOT) pc = r & 0x7FF;
        }
    }
#endif
#ifdef VU1_AOT
    {   /* a program translated ahead of time (hle_vu1_aot.h): runs until the E bit, or hands back an address it
           does not cover and the interpreter goes on from there */
        uint32_t r = aot_run(pc, &st);
        if (r == VU_END) goto end;
        if (r != VU_NOAOT) pc = r & 0x7FF;
    }
#endif
    for (; steps < 4000000; steps++) {
        pc &= 0x7FF;
        uint32_t lw, uw;
#ifndef HLE_PSP
        { extern uint64_t hle_vu1_pchist[2048]; hle_vu1_pchist[pc]++; }
#endif
        memcpy(&lw, vu.code + pc * 8, 4);
        memcpy(&uw, vu.code + pc * 8 + 4, 4);
#ifndef HLE_PSP
        { static int nt = -1; static long left; if (nt < 0) { const char *e = getenv("HLE_VU1_NOWTRACE"); nt = e ? (int)strtol(e, 0, 16) : 0; left = 20000; }
          if (nt && start_pc == (uint32_t)nt && left > 0) { left--; LOG("NOW pc %03x now %llu Q %g\n", pc, (unsigned long long)vu.now, vu.Q); } }
#endif
        uint32_t npc = vu_step(pc, lw, uw, &st);
        if (npc == VU_END) goto end;
        pc = npc;
    }
    st_overrun++;
    return;
end:
#ifndef HLE_PSP
    if (ncap_on == 2) { nat_compare_done(); ncap_on = 0; }
    { extern FILE *hle_vu1_kickdump; if (hle_vu1_kickdump) { fclose(hle_vu1_kickdump); hle_vu1_kickdump = 0; } }
#endif
    st_pcsteps[st_pcat] += (uint32_t)(st_steps - st_pcstep0);
    if (pf) { pf->runs++; pf->steps += st_steps - s0; pf->kicks += st_kicks - kicks_before; }
    {
        static int tr = -1; static uint64_t ntr; if (tr < 0) { const char *e = getenv("HLE_VU1_TRACE"); tr = e ? atoi(e) : 0; }
        if (tr && ntr < (uint64_t)tr) { ntr++; LOG("VUTRACE run %03x -> E %03x: %llu steps, %llu kicks, top %03x itop %03x\n", start_pc, vu.tpc - 1, (unsigned long long)(st_steps - s0), (unsigned long long)(st_kicks - kicks_before), v_top, v_itop); }
    }
}

/* ------------------------------------------------------------------ VIF1 */
static void unpack_(uint32_t cmd, uint32_t imm, uint32_t num, const uint32_t *src, uint32_t nsrc_words);
static void unpack(uint32_t cmd, uint32_t imm, uint32_t num, const uint32_t *src, uint32_t nsrc_words)
{ ZN(3, TPROF(8, unpack_(cmd, imm, num, src, nsrc_words))); }
static void unpack_(uint32_t cmd, uint32_t imm, uint32_t num, const uint32_t *src, uint32_t nsrc_words)
{
    st_unpack++;
    { int ci = nat_code_ok == 1 ? 0 : nat_code_ok == 2 ? 1 : 2; st_uq[ci] += num ? num : 256; st_unum[ci]++; }
#ifndef HLE_PSP
    {   static int us = -1; static uint64_t h[4096], q[4096]; static int at;
        if (us < 0) { us = getenv("HLE_VIF_USTAT") != 0; }
        if (us) {
            int key = (int)(cmd & 0x1F) | ((v_mode & 3) << 5) | ((v_cl == v_wl) << 7) | ((v_cl > v_wl) << 8);
            h[key]++; q[key] += num ? num : 256;
            if (!at) { at = 1; }
            if ((st_unpack & 0xFFFF) == 0 || st_unpack == 44000)
                for (int k = 0; k < 4096; k++) if (h[k]) LOG("USTAT vl %d vn %d msk %d mode %d cl==wl %d cl>wl %d: %llu unpacks, %llu qwords\n", k & 3, (k >> 2) & 3, (k >> 4) & 1, (k >> 5) & 3, (k >> 7) & 1, (k >> 8) & 1, (unsigned long long)h[k], (unsigned long long)q[k]);
        }
    }
#endif
    int vl = (int)(cmd & 3), vn = (int)((cmd >> 2) & 3), msk = (int)((cmd >> 4) & 1);
    int usn = (int)((imm >> 14) & 1), flg = (int)((imm >> 15) & 1);
    uint32_t addr = (imm & 0x3FF) + (flg ? v_tops : 0);
    uint32_t n = num ? num : 256;
    int bits = 32 >> vl;
    int elem = vn + 1;                       /* components per vector in the data */
    int v45 = (vn == 3 && vl == 3);          /* V4-5 */
    uint32_t bitpos = 0;
    uint32_t cl = v_cl ? v_cl : 1, wl = v_wl ? v_wl : 1;
    uint32_t cyc = 0;
    const uint8_t *b8 = (const uint8_t *)src;
    /* fast path: no mask, no offset / difference mode, no filling (WL <= CL: contiguous or skipping), enough data */
    if (!msk && !v_mode && wl <= cl && (uint64_t)n * (v45 ? 16 : (uint32_t)(elem * bits)) <= (uint64_t)nsrc_words * 32) {
        uint32_t qa = addr, j = 0;
        if (bits == 32 && elem == 4) {
            if (wl == cl && (qa & 0x3FF) + n <= 0x400) { memcpy(vmem(qa), b8, 16 * n); return; }
            for (uint32_t k = 0; k < n; k++) { memcpy(vmem(qa), b8 + 16 * k, 16); if (++j == wl) { j = 0; qa += cl - wl + 1; } else qa++; }
            return;
        }
        /* one loop per format (element count E, element type T): the data is word aligned, 16-bit elements are
           halfword aligned (no unaligned loads); V2 = (x, y, x, y), S = (x, x, x, x), V3 w = 0 as below */
#define UNP_ADV() do { if (++j == wl) { j = 0; qa += cl - wl + 1; } else qa++; } while (0)
#define UNP(E, T) do { const T *s_ = (const T *)src; \
            for (uint32_t k = 0; k < n; k++, s_ += (E)) { \
                int32_t d0 = s_[0], d1 = (E) > 1 ? (int32_t)s_[(E) > 1 ? 1 : 0] : d0; \
                int32_t d2 = (E) > 2 ? (int32_t)s_[(E) > 2 ? 2 : 0] : d0; \
                int32_t d3 = (E) > 3 ? (int32_t)s_[(E) > 3 ? 3 : 0] : (E) == 2 ? d1 : (E) == 1 ? d0 : 0; \
                int32_t o_[4] = {d0, d1, d2, d3}; memcpy(vmem(qa), o_, 16); UNP_ADV(); } \
            return; } while (0)
        if (!v45) switch (vl << 2 | vn) {
        case 0 << 2 | 2: UNP(3, int32_t);
        case 0 << 2 | 1: UNP(2, int32_t);
        case 0 << 2 | 0: UNP(1, int32_t);
        case 1 << 2 | 3: if (usn) UNP(4, uint16_t); else UNP(4, int16_t);
        case 1 << 2 | 2: if (usn) UNP(3, uint16_t); else UNP(3, int16_t);
        case 1 << 2 | 1: if (usn) UNP(2, uint16_t); else UNP(2, int16_t);
        case 1 << 2 | 0: if (usn) UNP(1, uint16_t); else UNP(1, int16_t);
        case 2 << 2 | 3: if (usn) UNP(4, uint8_t); else UNP(4, int8_t);
        case 2 << 2 | 2: if (usn) UNP(3, uint8_t); else UNP(3, int8_t);
        case 2 << 2 | 1: if (usn) UNP(2, uint8_t); else UNP(2, int8_t);
        case 2 << 2 | 0: if (usn) UNP(1, uint8_t); else UNP(1, int8_t);
        default: break;
        }
#undef UNP
#undef UNP_ADV
        for (uint32_t k = 0; k < n; k++) {
            int32_t d[4];
            if (v45) {
                uint16_t h; memcpy(&h, b8 + 2 * k, 2);
                d[0] = (h & 0x1F) << 3; d[1] = ((h >> 5) & 0x1F) << 3; d[2] = ((h >> 10) & 0x1F) << 3; d[3] = (h >> 15) << 7;
            } else {
                int32_t c[4];
                for (int e = 0; e < elem; e++, bitpos += (uint32_t)bits) {
                    if (bits == 32) memcpy(&c[e], b8 + bitpos / 8, 4);
                    else if (bits == 16) { uint16_t h; memcpy(&h, b8 + bitpos / 8, 2); c[e] = usn ? (int32_t)h : (int32_t)(int16_t)h; }
                    else { uint8_t c8 = b8[bitpos / 8]; c[e] = usn ? (int32_t)c8 : (int32_t)(int8_t)c8; }
                }
                if (elem == 1) d[0] = d[1] = d[2] = d[3] = c[0];
                else if (elem == 2) { d[0] = c[0]; d[1] = c[1]; d[2] = c[0]; d[3] = c[1]; }
                else if (elem == 3) { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 0; }
                else { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3]; }
            }
            memcpy(vmem(qa), d, 16);
            if (++j == wl) { j = 0; qa += cl - wl + 1; } else qa++;
        }
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        /* destination qword for the k-th written vector */
        uint32_t qa;
        int fill = 0;
        if (wl <= cl) qa = addr + (k / wl) * cl + (k % wl);
        else { qa = addr + k; fill = (k % wl) >= cl; }
        uint32_t row = cyc < 3 ? cyc : 3;
        cyc = (cyc + 1) % wl;
        int32_t d[4] = {0, 0, 0, 0};
        if (!fill) {
            if (v45) {
                if (bitpos / 8 + 2 > nsrc_words * 4) break;
                uint16_t h; memcpy(&h, b8 + bitpos / 8, 2); bitpos += 16;
                d[0] = (h & 0x1F) << 3; d[1] = ((h >> 5) & 0x1F) << 3; d[2] = ((h >> 10) & 0x1F) << 3; d[3] = (h >> 15) << 7;
            } else {
                int32_t c[4] = {0, 0, 0, 0};
                for (int e = 0; e < elem; e++) {
                    if ((bitpos + (uint32_t)bits) / 8 > nsrc_words * 4) { k = n; break; }
                    uint32_t v;
                    if (bits == 32) memcpy(&v, b8 + bitpos / 8, 4);
                    else if (bits == 16) { uint16_t h; memcpy(&h, b8 + bitpos / 8, 2); v = usn ? h : (uint32_t)(int32_t)(int16_t)h; }
                    else { uint8_t c8 = b8[bitpos / 8]; v = usn ? c8 : (uint32_t)(int32_t)(int8_t)c8; }
                    c[e] = (int32_t)v; bitpos += (uint32_t)bits;
                }
                if (k >= n) break;
                if (elem == 1) d[0] = d[1] = d[2] = d[3] = c[0];
                else if (elem == 2) { d[0] = c[0]; d[1] = c[1]; d[2] = c[0]; d[3] = c[1]; }
                else if (elem == 3) { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 0; }
                else { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3]; }
            }
        }
        uint8_t *m = vmem(qa);
        for (int i = 0; i < 4; i++) {
            int mm = msk ? (int)((v_mask >> (row * 8 + i * 2)) & 3) : 0;
            if (fill && !msk) mm = 3;                      /* filling write without a mask: leave as is (approximation) */
            uint32_t out;
            if (mm == 3) continue;
            if (mm == 1) out = v_row[i];
            else if (mm == 2) out = v_col[row];
            else {
                out = (uint32_t)d[i];
                if (v_mode == 1) out += v_row[i];
                else if (v_mode == 2) { v_row[i] += out; out = v_row[i]; }
            }
            memcpy(m + 4 * i, &out, 4);
        }
    }
}

static void mscal_start(void)
{
    v_itop = v_itops;
    v_top = v_tops;
    v_dbf ^= 1;
    v_tops = v_base + (v_dbf ? v_ofst : 0);
}

static const uint32_t *ds_w;
static const uint8_t *ds_get(void *u, uint32_t k) { return (const uint8_t *)(ds_w + (uint32_t)(uintptr_t)u + 4 * k); }

/* VIF1 words w[start..n): runs every complete command. final = 0: stops before a command whose data is not all in
   the buffer yet and returns its position (the caller keeps w[ret..n) and calls again with more); final = 1: runs
   everything (a truncated command gets what there is) and returns n. The buffer must start at a qword boundary of
   the stream (DIRECT aligns to qwords). */
static uint32_t vif1_chunk_(const uint32_t *w, uint32_t n, uint32_t start, int final);
uint32_t hle_vif1_chunk(const uint32_t *w, uint32_t n, uint32_t start, int final)
{ uint32_t r_; ZN(2, r_ = vif1_chunk_(w, n, start, final)); return r_; }   /* (timed per DMA: hle_gfx_scan.c) */
static uint32_t vif1_chunk_(const uint32_t *w, uint32_t n, uint32_t start, int final)
{
    uint32_t p = start;
    if (mc_on < 0) mc_init();
    while (p < n) {
        if (!w[p]) {                                                   /* NOP padding (2/3 of the codes): skip the run */
            uint32_t p0 = p;
            do p++; while (p < n && !w[p]);
            st_vc[0] += p - p0;
            continue;
        }
        uint32_t at = p;
        uint32_t code = w[p++];
        st_vc[(code >> 24) & 0x7F]++;
        uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
        uint32_t need = 0;                                             /* data words after the code */
        if (cmd >= 0x60) {
            uint32_t vl = cmd & 3, vn = (cmd >> 2) & 3, cnt = num ? num : 256;
            uint32_t cl = v_cl ? v_cl : 1, wl = v_wl ? v_wl : 1;
            uint32_t dcnt = wl <= cl ? cnt : cl * (cnt / wl) + ((cnt % wl) < cl ? cnt % wl : cl);
            need = (dcnt * (vn + 1) * (32u >> vl) + 31) / 32;
        } else if (cmd == 0x20) need = 1;
        else if (cmd == 0x30 || cmd == 0x31) need = 4;
        else if (cmd == 0x4A) need = (num ? num : 256) * 2;
        else if (cmd == 0x50 || cmd == 0x51) need = ((p + 3) & ~3u) - p + 4 * (imm ? imm : 65536u);
        if (p + need > n) {
            if (!final) { pu_flush(); return at; }
            need = n - p;
        }
#ifndef HLE_PSP
        { static long kl = -2; if (kl == -2) { const char *e = getenv("HLE_KICKLOG"); kl = e ? atol(e) : -1; }
          if (kl > 0) { kl--; LOG("VIFCMD %02x imm %04x num %u need %u top %u tops %u cl %u wl %u mode %u msk %d\n", cmd, imm, num, need, v_top, v_tops, v_cl, v_wl, v_mode, (int)((cmd >> 4) & 1)); } }
#endif
        if (cmd >= 0x60) {
#ifndef HLE_PSP
            if (nat_code_ok == 2) { st_sku[!((imm >> 15) & 1) ? 0 : ((cmd >> 4) & 1) ? 1 : v_mode ? 2 : v_wl > v_cl ? 3 : 4]++; }
#endif
            if (mc_on > 0 && ((imm >> 15) & 1) && !((cmd >> 4) & 1) && !v_mode && v_wl <= v_cl && p + need <= n && npu < NPU && !(final && p + need > n)) {
                PU *u = &pu[npu++]; u->src = w + p; u->cmd = cmd; u->imm = imm; u->num = num; u->nw = need; u->tops = v_tops; u->cl = v_cl; u->wl = v_wl; u->stable = (uint32_t)hle_vif_src_stable;
#ifdef HLE_PSP
                if (!u->stable) { extern const uint32_t *hle_vif_stable_ptr(const uint32_t *, uint32_t); const uint32_t *g = hle_vif_stable_ptr(u->src, need); if (g) { u->src = g; u->stable = 1; } }
#endif
            } else { pu_flush(); unpack(cmd, imm, num, w + p, need); }
            p += need; continue;
        }
        switch (cmd) {
        case 0x06: {
            static int dbg = -1; if (dbg < 0) dbg = getenv("HLE_P3_DBG") != 0;
            if (dbg) LOG("P3DBG frame %llu MSKPATH3 %d at word %u of %u\n", (unsigned long long)hle_frame, (int)((code >> 15) & 1), p, n);
            if (!((code >> 15) & 1)) hle_path3_unmask();                                /* MSKPATH3 off: textures go */
        } break;
        case 0x00: case 0x07: case 0x10: case 0x11: case 0x13: break;
        case 0x01: v_cl = imm & 0xFF; v_wl = (imm >> 8) & 0xFF; break;
        case 0x02: v_ofst = imm & 0x3FF; v_dbf = 0; v_tops = v_base; break;
        case 0x03: v_base = imm & 0x3FF; break;
        case 0x04: v_itops = imm & 0x3FF; break;
        case 0x05: v_mode = imm & 3; break;
        case 0x14: case 0x15: mscal_start(); vu1_run(imm); break;
        case 0x17: mscal_start(); vu1_run(vu.tpc); break;
        case 0x20: if (need >= 1) v_mask = w[p]; break;
        case 0x30: for (uint32_t i = 0; i < 4 && i < need; i++) v_row[i] = w[p + i]; break;
        case 0x31: for (uint32_t i = 0; i < 4 && i < need; i++) v_col[i] = w[p + i]; break;
        case 0x4A:
            st_mpg++; nat_code_ok = -1;
            for (uint32_t i = 0; i < need; i++) memcpy(vu.code + (((imm * 8) + 4 * i) & 0x3FFF), &w[p + i], 4);
            break;
        case 0x50: case 0x51: {
            uint32_t q0 = (p + 3) & ~3u, qws = (p + need - q0) / 4;
            if (hle_gs_lite_packets && qws) { ds_w = w; hle_gs_lite_packets(ds_get, (void *)(uintptr_t)q0, qws); }
        } break;
        default: st_badop++; break;
        }
        p += need;
    }
    pu_flush();
    return n;
}

/* a complete VIF1 stream (words); returns 1 when handled (the caller then does not draw DIRECT packets itself) */
int hle_vu1_on(void)
{
    static int on = -1;
    if (on < 0) {
#ifdef HLE_PSP
        const char *e = hle_cfg_get("x_vu1");               /* ps2rc.cfg x_vu1=0: no VU1 (2D only / timing tests) */
#else
        const char *e = getenv("HLE_VU1");
#endif
        on = !(e && e[0] == '0');
    }
    return on;
}
int hle_vif1_stream(const uint32_t *w, uint32_t n)
{
    if (!hle_vu1_on()) return 0;
    hle_vif1_chunk(w, n, 0, 1);
    return 1;
}

/* per PERF window (60 game frames): ms per frame in each timed part of the graphics path */
void hle_vu1_tprof_window(uint32_t frames)
{
#ifdef HLE_PSP
    static uint64_t last[12];
    if (!frames) return;
    double f = 1000.0 * frames;
    LOG("TWIN ms/frame: VIF1 %.1f = unpacks %.1f + VU1 run %.1f (native %.1f: mesh %.1f, skin %.1f, mesh begin %.1f) + ..., GS packets %.1f (image data %.1f)\n",
        (hle_tprof[5] - last[5]) / f, (hle_tprof[8] - last[8]) / f, (hle_tprof[0] - last[0]) / f, (hle_tprof[1] - last[1]) / f, (hle_tprof[6] - last[6]) / f,
        (hle_tprof[7] - last[7]) / f, (hle_tprof[3] - last[3]) / f, (hle_tprof[2] - last[2]) / f, (hle_tprof[9] - last[9]) / f);
    memcpy(last, hle_tprof, sizeof last);
    static uint64_t lr, ls, lk, ln, lskin;
    LOG("TWIN per frame: %.0f VU1 runs, %.0f interpreted instructions, %.0f XGKICKs, %.0f native mesh batches\n",
        (double)(st_runs - lr) / frames, (double)(st_steps - ls) / frames, (double)(st_kicks - lk) / frames, (double)(st_nat - ln) / frames);
    lr = st_runs; ls = st_steps; lk = st_kicks; ln = st_nat; (void)lskin;
    {   /* the interpreted / ahead-of-time programs by entry address (runs and instructions per frame), top 4 */
        char b[256]; int o = 0;
        for (int t = 0; t < 4; t++) {
            int bi = -1; for (int i = 0; i < 0x800; i++) if (st_pcsteps[i] && (bi < 0 || st_pcsteps[i] > st_pcsteps[bi])) bi = i;
            if (bi < 0) break;
            o += snprintf(b + o, sizeof b - (size_t)o, " %03x: %.1f runs %.0f insns;", bi, (double)st_pcrun[bi] / frames, (double)st_pcsteps[bi] / frames);
            st_pcsteps[bi] = 0; st_pcrun[bi] = 0;
        }
        memset(st_pcsteps, 0, sizeof st_pcsteps); memset(st_pcrun, 0, sizeof st_pcrun);
        LOG("TWIN interpreted programs:%s\n", o ? b : " none");
#if defined(HLE_PSP) && !defined(NAT_SIM)
        { static uint64_t ln_, lf_, lx_; LOG("TWIN partly clipped mesh batches per frame: near plane %.0f, far plane %.0f, beyond the guard band only %.0f\n",
              (double)(st_part_near - ln_) / frames, (double)(st_part_far - lf_) / frames, (double)(st_part_xy - lx_) / frames);
          ln_ = st_part_near; lf_ = st_part_far; lx_ = st_part_xy; }
        { static uint64_t lm_; LOG("TWIN mesh keys from the memo %.0f per frame\n", (double)(st_mc_memo - lm_) / frames); lm_ = st_mc_memo; }
        LOG("TWIN skin cache rejects: no unpacks %llu, vi15 %llu, count %llu, layout %llu, not stable %llu, rows %llu\n", (unsigned long long)st_skr[0], (unsigned long long)st_skr[1], (unsigned long long)st_skr[2], (unsigned long long)st_skr[3], (unsigned long long)st_skr[4], (unsigned long long)st_skr[5]);
        LOG("TWIN skin unpacks: no FLG %llu, masked %llu, mode %llu, wl>cl %llu, deferrable %llu (total)\n", (unsigned long long)st_sku[0], (unsigned long long)st_sku[1], (unsigned long long)st_sku[2], (unsigned long long)st_sku[3], (unsigned long long)st_sku[4]);
        { static uint64_t lk_; extern uint64_t hle_p3_qw; static uint64_t lp_;
          LOG("TWIN GS path qwords per frame: XGKICK %.0f, PATH3 %.0f\n", (double)(st_kick_qw - lk_) / frames, (double)(hle_p3_qw - lp_) / frames); lk_ = st_kick_qw; lp_ = hle_p3_qw; }
        { static uint64_t lf_, lv_, lb_; LOG("TWIN foliage batches native %.1f (%.0f vertices), handed to the VU program %.1f per frame\n",
              (double)(st_fol - lf_) / frames, (double)(st_fol_v - lv_) / frames, (double)(st_fol_back - lb_) / frames); lf_ = st_fol; lv_ = st_fol_v; lb_ = st_fol_back; }
        { static uint64_t lh_, lf2_; LOG("TWIN skin vertex rows: %.0f batches from the cache, %.0f unpacked and kept per frame\n",
              (double)(st_skc_hit - lh_) / frames, (double)(st_skc_fill - lf2_) / frames); lh_ = st_skc_hit; lf2_ = st_skc_fill; }
#endif
#ifdef VU1_FAST
        { static uint64_t lfr, lfb; LOG("TWIN fast VU1 runs %.1f, handed back to the interpreter %.1f per frame\n",
              (double)(st_fast_runs - lfr) / frames, (double)(st_fast_back - lfb) / frames); lfr = st_fast_runs; lfb = st_fast_back; }
#endif
        static uint64_t lc, lq[3], lu[3];
        for (int i = 0; i < 128; i++) st_vcmd += st_vc[i];
        LOG("TWIN VIF codes %.0f; unpacks run (qwords) mesh %.0f (%.0f), skin %.0f (%.0f), other %.0f (%.0f) per frame\n",
            (double)(st_vcmd - lc) / frames, (double)(st_unum[0] - lu[0]) / frames, (double)(st_uq[0] - lq[0]) / frames,
            (double)(st_unum[1] - lu[1]) / frames, (double)(st_uq[1] - lq[1]) / frames, (double)(st_unum[2] - lu[2]) / frames, (double)(st_uq[2] - lq[2]) / frames);
        lc = st_vcmd; memcpy(lq, st_uq, sizeof lq); memcpy(lu, st_unum, sizeof lu);
        {   char b2[200]; int o2 = 0;
            for (int t = 0; t < 8; t++) {
                int bi = -1; for (int i = 0; i < 128; i++) if (st_vc[i] && (bi < 0 || st_vc[i] > st_vc[bi])) bi = i;
                if (bi < 0) break;
                o2 += snprintf(b2 + o2, sizeof b2 - (size_t)o2, " %02x:%.0f", bi, (double)st_vc[bi] / frames); st_vc[bi] = 0;
            }
            memset(st_vc, 0, sizeof st_vc);
            LOG("TWIN VIF codes per frame by command:%s\n", b2);
        }
    }
#else
    (void)frames;
#endif
}
void hle_vu1_report(void)
{
#ifndef HLE_PSP
    {   uint64_t h = 1469598103934665603ull; for (int i = 0; i < 16384; i++) h = (h ^ vu.mem[i]) * 1099511628211ull;
        for (int r = 0; r < 32; r++) for (int i = 0; i < 4; i++) h = (h ^ vu.vf[r].u[i]) * 1099511628211ull;
        for (int r = 0; r < 16; r++) h = (h ^ vu.vi[r]) * 1099511628211ull;
        LOG("VU1 check: kicked packets %016llx, final state %016llx (steps %llu)\n", (unsigned long long)kick_hash, (unsigned long long)h, (unsigned long long)st_steps); }
#endif
#ifdef VU1_AOT
    LOG("VU1 ahead-of-time programs: %llu runs entered, %llu hand-backs to the interpreter\n", (unsigned long long)st_aot_runs, (unsigned long long)st_aot_back);
#endif
#ifdef HLE_PSP
    LOG("mesh cache: %llu batches drawn from it, %llu converted, %llu culled by their box (%llu drawn whole, %llu clipped per vertex), %llu not keyable, %llu pool full, %llu evicted; %llu vertices from it\n",
        (unsigned long long)st_mc_hit, (unsigned long long)st_mc_miss, (unsigned long long)st_mc_cull, (unsigned long long)st_mc_allin, (unsigned long long)st_mc_part,
        (unsigned long long)st_mc_nocov, (unsigned long long)st_mc_full, (unsigned long long)st_mc_evict, (unsigned long long)st_mc_v);
    LOG("TPROF ms: mesh vertices %llu, skin vertices %llu (mesh begin: slot 3 below); %llu direct batches, %llu mesh / %llu skin vertices, %llu clipped, %llu batches (%llu vertices) culled whole\n", (unsigned long long)(hle_tprof[6] / 1000), (unsigned long long)(hle_tprof[7] / 1000),
        (unsigned long long)st_dir_batches, (unsigned long long)st_fast_v, (unsigned long long)st_skin_v, (unsigned long long)st_nat_clip, (unsigned long long)st_cull_b, (unsigned long long)st_cull_v);
    LOG("TPROF ms: VIF1 unpacks %llu, image uploads %llu, mesh vertex loop %llu\n", (unsigned long long)(hle_tprof[8] / 1000), (unsigned long long)(hle_tprof[9] / 1000), (unsigned long long)(hle_tprof[10] / 1000));
    LOG("TPROF ms: vu1 run (incl. native, kicks) %llu, native %llu, GS packets %llu, textures %llu, GE emit %llu, VIF1 total %llu\n",
        (unsigned long long)(hle_tprof[0] / 1000), (unsigned long long)(hle_tprof[1] / 1000), (unsigned long long)(hle_tprof[2] / 1000),
        (unsigned long long)(hle_tprof[3] / 1000), (unsigned long long)(hle_tprof[4] / 1000), (unsigned long long)(hle_tprof[5] / 1000));
#endif
    if (prof_on > 0) {
        for (int i = 0; i < nprof; i++)
            LOG("VU1 program pc %03x hash %08x: %llu runs, %llu instructions (%llu per run), %llu XGKICKs\n", prof[i].pc, prof[i].hash,
                (unsigned long long)prof[i].runs, (unsigned long long)prof[i].steps,
                (unsigned long long)(prof[i].runs ? prof[i].steps / prof[i].runs : 0), (unsigned long long)prof[i].kicks);
    }
    LOG("VU1 model: %llu VIF UNPACKs, %llu MPG uploads, %llu program runs (%llu instructions, %llu overruns), %llu XGKICKs, "
        "%llu unknown ops\n", (unsigned long long)st_unpack, (unsigned long long)st_mpg, (unsigned long long)st_runs,
        (unsigned long long)st_steps, (unsigned long long)st_overrun, (unsigned long long)st_kicks, (unsigned long long)st_badop);
}

/* ------------------------------------------------------------------ state (graphics captures: PC record, PSP replay)
   Explicit little-endian layout, the same on both targets. Pipeline state is not saved: captures start at a frame
   boundary, when no program runs. */
#define VU1_STATE_SIZE (32 * 16 + 16 + 16 * 4 + 7 * 4 + 16384 + 16384 + 16 * 4 + 4 * 4 + 4 * 4)
int hle_vu1_state_size(void) { return VU1_STATE_SIZE; }
static void put32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
static uint32_t get32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
void hle_vu1_state_save(uint8_t *out)
{
    uint8_t *p = out;
    for (int r = 0; r < 32; r++) for (int i = 0; i < 4; i++) put32(&p, vu.vf[r].u[i]);
    for (int i = 0; i < 4; i++) put32(&p, vu.acc.u[i]);
    for (int r = 0; r < 16; r++) put32(&p, vu.vi[r]);
    uint32_t t; memcpy(&t, &vu.I, 4); put32(&p, t); memcpy(&t, &vu.Q, 4); put32(&p, t); memcpy(&t, &vu.P, 4); put32(&p, t);
    put32(&p, vu.mac); put32(&p, vu.status); put32(&p, vu.clip); put32(&p, vu.R);
    memcpy(p, vu.mem, 16384); p += 16384;
    memcpy(p, vu.code, 16384); p += 16384;
    put32(&p, vu.tpc); put32(&p, v_cl); put32(&p, v_wl); put32(&p, v_mode); put32(&p, v_mask);
    for (int i = 0; i < 4; i++) put32(&p, v_row[i]);
    for (int i = 0; i < 4; i++) put32(&p, v_col[i]);
    put32(&p, v_base); put32(&p, v_ofst); put32(&p, v_tops); put32(&p, v_top); put32(&p, v_itops); put32(&p, v_itop); put32(&p, v_dbf);
    /* pad to the fixed size */
    while (p < out + VU1_STATE_SIZE) put32(&p, 0);
}
void hle_vu1_state_load(const uint8_t *in)
{
    const uint8_t *p = in;
    memset(&vu, 0, sizeof vu);
    for (int r = 0; r < 32; r++) for (int i = 0; i < 4; i++) vu.vf[r].u[i] = get32(&p);
    for (int i = 0; i < 4; i++) vu.acc.u[i] = get32(&p);
    for (int r = 0; r < 16; r++) vu.vi[r] = (uint16_t)get32(&p);
    uint32_t t; t = get32(&p); memcpy(&vu.I, &t, 4); t = get32(&p); memcpy(&vu.Q, &t, 4); t = get32(&p); memcpy(&vu.P, &t, 4);
    vu.mac = get32(&p); vu.status = get32(&p); vu.clip = get32(&p); vu.R = get32(&p);
    memcpy(vu.mem, p, 16384); p += 16384;
    memcpy(vu.code, p, 16384); p += 16384;
    vu.tpc = get32(&p); v_cl = get32(&p); v_wl = get32(&p); v_mode = get32(&p); v_mask = get32(&p);
    for (int i = 0; i < 4; i++) v_row[i] = get32(&p);
    for (int i = 0; i < 4; i++) v_col[i] = get32(&p);
    v_base = get32(&p); v_ofst = get32(&p); v_tops = get32(&p); v_top = get32(&p); v_itops = get32(&p); v_itop = get32(&p); v_dbf = get32(&p);
}
/* analysis: the VU1 code memory as it is now (16 KB) */
void hle_vu1_dump_code(const char *path)
{
#ifndef HLE_PSP
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(vu.code, 1, 16384, f); fclose(f); }
#else
    (void)path;
#endif
}

#ifndef HLE_PSP
uint64_t hle_vu1_pchist[2048];
void hle_vu1_dump_pchist(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < 2048; i++) if (hle_vu1_pchist[i]) fprintf(f, "%03x %llu\n", i, (unsigned long long)hle_vu1_pchist[i]);
    fclose(f);
}
#endif
