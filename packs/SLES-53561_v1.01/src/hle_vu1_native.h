/* hle_vu1_native.h (included by hle_vu1.c) - the game's main mesh microprogram (code hash 48d66b55 at 0) done in C
 * instead of interpreting it: one batch = one MSCNT at 0x424 (vi15 = 0x29). Worked out from the disassembly
 * (work/vu1/progs, tools/vu1dis.py):
 *   0x029  decompress the records at TOP (ITOP vertices, 4 qwords each): position int/128, UV int/128 + 974.xy,
 *          colour: 976.w != 0: lerp(c2, c3, 973.x) (973.w: grey = 12 + .x) x 976.xyz x 1020.x / 255, MINI 255,
 *          x 1019; else c2.rgb, alpha 255, x 1019
 *   C = vf31 + vf28 x + vf29 y + vf30 z (object-to-clip, set up by the MSCAL at 0)
 *   1023.y = 0x0cb  no clipping, XYZF2 = vf02 + C / Cw, F = 0
 *            0x114  no clipping, fog F = clamp(k Cw, a, a + 255) - a (k = 1014.z, a = 1014.w k)
 *            0x166  clipping (guard band 1021.y / 1022.y, near plane from 1021.z / 1022.z), fog as at L426
 *   output: one GIF packet per strip (tag from 1018, STQ / RGBA / XYZF2), then vi12..14 rotate, vi15 = 0x29 [E].
 * The clipping variant: triangles of the strip with every vertex inside go out as strips, triangles wholly outside
 * one plane are dropped, the others are clipped in clip space (Sutherland-Hodgman, 6 planes) and sent as small strips.
 * Mode (PC env HLE_VU1_NATIVE, PSP cfg x_vu1n): 1 on (default), 0 off (interpret), 2 compare with the interpreter
 * (PC only: runs both, compares the unclipped triangles, logs NATCMP lines). */

#define NAT_HASH 0x48d66b55u
#ifndef NAT_NOCCACHE
#define NAT_NOCCACHE 0     /* 1: the per-batch constants recomputed every batch (A/B: no measurable difference) */
#endif
#ifndef NAT_GUARD
#define NAT_GUARD 1
#endif
static int nat_mode = -1, nat_code_ok = -1;
static uint64_t st_nat, st_nat_skip, st_nat_tri, st_nat_clip, st_nat_drop, st_nat_v;
uint64_t hle_vu1_vertices(void) { return st_nat_v; }   /* vertices through the native programs (PERF lines) */

static inline float nf(float x) { uint32_t u = fres(x); float f; memcpy(&f, &u, 4); return f; }
static inline float mf(uint32_t qa, int l) { uint32_t u; memcpy(&u, vmem(qa) + 4 * l, 4); return fclamp(u); }
static inline int32_t mi(uint32_t qa, int l) { int32_t v; memcpy(&v, vmem(qa) + 4 * l, 4); return v; }
static inline float rf(int r, int l) { return fclamp(vu.vf[r].u[l]); }
static inline int32_t ftoi_s(float x, float sc)
{
    float d = x * sc;                                       /* sc is a power of two: exact (float only: no soft doubles on the PSP) */
    return d >= 2147483648.0f ? 0x7FFFFFFF : d <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)d;
}
static inline int32_t smk(float f) { int32_t x; memcpy(&x, &f, 4); return x < 0 ? (int32_t)(0x80000000u - (uint32_t)x) : x; }
static inline float vmax(float a, float b) { return smk(a) > smk(b) ? a : b; }
static inline float vmin(float a, float b) { return smk(a) < smk(b) ? a : b; }

typedef struct { float c[4], uv[2], col[4], p[3]; } NV;    /* clip-space position, UV, colour (floats), object position */
typedef struct { uint8_t q[3][16]; } NO;                    /* output vertex: STQ, RGBA, XYZF2 */

static struct {
    int var;                                                /* 0x0cb / 0x114 / 0x166 */
    float v02[4], k, fa, f22w, f27w;                        /* projection / fog constants */
    float sx, sy, zc, zk;                                   /* clip: x*sx, y*sy, zc + zk w against |w| */
    uint32_t tag[4];
} nb;

/* the output of one vertex as the program computes it (per variant) */
static void nat_out(const NV *v, NO *o)
{
    float q, X[4], stq[3];
    if (nb.var == 0x114) {
        q = nb.k / v->c[3];                       /* C'w = Cw k (scaled w lanes); here c[3] already holds C'w */
        q = nf(q);
        float w = vmin(vmax(v->c[3], nb.f22w), nb.f27w);
        for (int i = 0; i < 3; i++) X[i] = nf(nf(v->c[i] * q) - nf(0.0f - nb.v02[i]));
        X[3] = nf(w - nb.f22w);
    } else {
        q = nf(1.0f / v->c[3]);
        for (int i = 0; i < 3; i++) X[i] = nf(nb.v02[i] + v->c[i] * q);
        if (nb.var == 0x166) {
            float accw = nf(nb.v02[3] - nb.fa * nb.k);
            float w = nf(accw + v->c[3] * nb.k);
            w = vmin(w, 255.0f);
            X[3] = vmax(w, nb.v02[3]);
        } else X[3] = 0;
    }
    stq[0] = nf(v->uv[0] * q); stq[1] = nf(v->uv[1] * q); stq[2] = q;
    uint32_t z = 0;
    memcpy(o->q[0], stq, 12); memcpy(o->q[0] + 12, &z, 4);
    for (int i = 0; i < 4; i++) { int32_t a = ftoi_s(v->col[i], 1.0f), b = ftoi_s(X[i], 16.0f); memcpy(o->q[1] + 4 * i, &a, 4); memcpy(o->q[2] + 4 * i, &b, 4); }
}

/* --- output packets */
static uint8_t nat_pk[16 * (1 + 3 * 256)];
static uint32_t nat_pkn;                                    /* vertices in the current strip */
static const uint8_t *nat_get(void *u, uint32_t k) { (void)u; return nat_pk + 16 * k; }
#ifndef HLE_PSP
typedef struct { NO v[3]; } NTri;
static NTri *ncap_a, *ncap_b; static int ncap_na, ncap_nb, ncap_cap_a, ncap_cap_b; static int ncap_on, nat_inclip;   /* compare mode */
static void ncap_add(NTri **arr, int *n, int *cap, const NO *a, const NO *b, const NO *c)
{
    if (*n >= *cap) { *cap = *cap ? *cap * 2 : 256; *arr = realloc(*arr, sizeof(NTri) * *cap); }
    NTri *t = &(*arr)[(*n)++]; t->v[0] = *a; t->v[1] = *b; t->v[2] = *c;
}
/* the interpreter's kicks in compare mode: strips of 3-register packed packets -> triangles */
static void ncap_kick(uint32_t qa)
{
    uint32_t a = qa;
    for (int guard = 0; guard < 64; guard++) {
        uint64_t lo, hi; memcpy(&lo, vmem(a), 8); memcpy(&hi, vmem(a) + 8, 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        if (flg == 0 && nreg == 3 && (uint32_t)hi == 0x412) {
            NO w[3];
            for (uint32_t k = 0; k < nloop; k++) {
                NO cur; for (int r = 0; r < 3; r++) memcpy(cur.q[r], vmem(a + 1 + 3 * k + r), 16);
                memset(cur.q[0] + 12, 0, 4);
                if (k >= 2) ncap_add(&ncap_b, &ncap_nb, &ncap_cap_b, &w[(k - 2) % 3], &w[(k - 1) % 3], &cur);
                w[k % 3] = cur;
            }
        }
        uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        a += 1 + body;
        if ((lo >> 15) & 1) break;
    }
}
#endif
static void nat_flush(void)
{
    if (nat_pkn >= 3) {
        uint32_t t0 = nat_pkn | 0x8000u;
        memcpy(nat_pk, &t0, 4); memcpy(nat_pk + 4, &nb.tag[1], 12);
        st_kicks++;
#ifndef HLE_PSP
        if (hle_vu1_kickdump) fwrite(nat_pk, 16, 1 + 3 * nat_pkn, hle_vu1_kickdump);
        if (ncap_on) {
            if (!nat_inclip) for (uint32_t k = 2; k < nat_pkn; k++)
                ncap_add(&ncap_a, &ncap_na, &ncap_cap_a, (NO *)(nat_pk + 16 * (1 + 3 * (k - 2))), (NO *)(nat_pk + 16 * (1 + 3 * (k - 1))), (NO *)(nat_pk + 16 * (1 + 3 * k)));
        } else
#endif
        if (hle_gs_lite_packets) hle_gs_lite_packets(nat_get, 0, 1 + 3 * nat_pkn);
    }
    nat_pkn = 0;
}
static void nat_put(const NO *o)
{
    if (nat_pkn >= 256) {                                   /* keep the strip going across packets: repeat the last two */
        NO a, b; memcpy(&a, nat_pk + 16 * (1 + 3 * (nat_pkn - 2)), 48); memcpy(&b, nat_pk + 16 * (1 + 3 * (nat_pkn - 1)), 48);
        nat_flush(); memcpy(nat_pk + 16, &a, 48); memcpy(nat_pk + 64, &b, 48); nat_pkn = 2;
    }
    memcpy(nat_pk + 16 * (1 + 3 * nat_pkn), o, 48); nat_pkn++;
}

#if defined(HLE_PSP) || defined(NAT_SIM)
/* direct meshes (PSP): triangles go to the GE in object space (psp/hle_ge_gs.c), no GS packets, no CPU projection */
typedef struct { float u, v; uint32_t c; float x, y, z; } HGV;
int hle_ge_mesh_begin(uint64_t prim, const float Mo[4][4], float offx, float offy, float zK, float z0, int fog, float fa, float fb, int *dbl_rgb);
void hle_ge_mesh_draw(int prim, const void *v, int n);
void *hle_ge_mesh_alloc(int n);
void hle_ge_mesh_draw_at(int prim, const void *v, int n);
void hle_ge_mesh_end(void);
#define GE_STRIP 4
#define GE_TRIS 3
#define GE_FAN 5
static int nat_direct = -1;
static uint64_t st_dir_batches, st_fast_v, st_fast_t, st_skin_v, st_cull_b, st_cull_v;
static inline uint32_t ge_col(const uint32_t *gs, int dbl)            /* GS RGBA (low bytes) -> GE 8888 */
{
    uint32_t r = gs[0] & 0xFF, g = gs[1] & 0xFF, b = gs[2] & 0xFF, a = gs[3] & 0xFF;
    a = a >= 0x80 ? 0xFF : a * 2;
    if (dbl) { r = r >= 0x80 ? 0xFF : r * 2; g = g >= 0x80 ? 0xFF : g * 2; b = b >= 0x80 ? 0xFF : b * 2; }
    return r | g << 8 | b << 16 | a << 24;
}
static inline void hgv_of(HGV *g, const NV *v, const float *pos, int dbl)
{
    uint32_t c[4]; for (int i = 0; i < 4; i++) c[i] = (uint32_t)ftoi_s(v->col[i], 1.0f);
    g->u = v->uv[0]; g->v = v->uv[1]; g->c = ge_col(c, dbl); g->x = pos[0]; g->y = pos[1]; g->z = pos[2];
}
#ifdef NAT_SIM
/* PC simulation of the PSP's direct path: the GE draws become GS packets (screen space computed here) for the
   software GS, so the PC shows what the PSP's direct path draws */
static const char *hle_cfg_get(const char *k) { (void)k; return getenv("HLE_GEDIRECT_CFG"); }
static struct { uint64_t prim; float Mo[4][4], offx, offy, zK, z0, fa, fb; int fog; } sim;
static uint8_t sim_pk[16 * (1 + 3 * 260)];
static const uint8_t *sim_get(void *u, uint32_t k) { (void)u; return sim_pk + 16 * k; }
int hle_ge_mesh_begin(uint64_t prim, const float Mo[4][4], float offx, float offy, float zK, float z0, int fog, float fa, float fb, int *dbl_rgb)
{
    sim.prim = prim; memcpy(sim.Mo, Mo, sizeof sim.Mo); sim.offx = offx; sim.offy = offy; sim.zK = zK; sim.z0 = z0;
    sim.fog = fog; sim.fa = fa; sim.fb = fb; *dbl_rgb = 1;
    return 1;
}
static HGV sim_mem[512];
void hle_ge_mesh_draw(int prim, const void *v, int n);
void *hle_ge_mesh_alloc(int n) { return n <= 512 ? sim_mem : 0; }
void hle_ge_mesh_draw_at(int prim, const void *v, int n) { hle_ge_mesh_draw(prim, v, n); }
void hle_ge_mesh_end(void) {}
void hle_ge_mesh_draw(int prim, const void *v, int n)
{
    const HGV *g = (const HGV *)v;
    if (n < 3 || n > 256 || !hle_gs_lite_packets) return;
    /* a fan becomes a strip: 0, 1, n-1, 2, n-2, ... */
    int ord[256], m = 0;
    if (prim == 5) { int lo = 1, hi = n - 1, k = 0; ord[m++] = 0; while (lo <= hi) ord[m++] = (k++ & 1) ? hi-- : lo++; }
    else for (int i = 0; i < n; i++) ord[m++] = i;
    uint64_t pr = (sim.prim & ~7ull) | (prim == 3 ? 3 : 4);
    uint64_t lo = (uint64_t)m | 1ull << 15 | 1ull << 46 | pr << 47 | 3ull << 60, hi = 0x412;
    memcpy(sim_pk, &lo, 8); memcpy(sim_pk + 8, &hi, 8);
    for (int i = 0; i < m; i++) {
        const HGV *a = &g[ord[i]];
        float C[4];
        for (int c = 0; c < 4; c++) C[c] = sim.Mo[3][c] + sim.Mo[0][c] * a->x + sim.Mo[1][c] * a->y + sim.Mo[2][c] * a->z;
        float q = 1.0f / C[3], stq[4] = {a->u * q, a->v * q, q, 0};
        uint8_t *o = sim_pk + 16 * (1 + 3 * i);
        memcpy(o, stq, 16);
        uint32_t rg[4]; for (int c = 0; c < 4; c++) { uint32_t b = (a->c >> (8 * c)) & 0xFF; rg[c] = b >= 0xFF ? 0x80 : b / 2; }
        memcpy(o + 16, rg, 16);
        float f = sim.fog ? (sim.fa * C[3] + sim.fb) * 255.0f : 0; if (f < 0) f = 0; if (f > 255) f = 255;
        int32_t x[4] = {ftoi_s(sim.offx + C[0] * q, 16.0f), ftoi_s(sim.offy + C[1] * q, 16.0f), ftoi_s(sim.z0 + sim.zK * q, 16.0f), ftoi_s(f, 16.0f)};
        memcpy(o + 32, x, 16);
    }
    if (getenv("SIM_DBG")) {
        for (int i = 2; i < m; i++) {
            int32_t p[3][2];
            for (int j = 0; j < 3; j++) memcpy(p[j], sim_pk + 16 * (1 + 3 * (i - 2 + j)) + 32, 8);
            double ar = fabs((double)(p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (double)(p[2][0] - p[0][0]) * (p[1][1] - p[0][1])) / 512.0;
            double e1 = fabs((double)(p[1][0] - p[0][0])) + fabs((double)(p[1][1] - p[0][1])), e2 = fabs((double)(p[2][0] - p[0][0])) + fabs((double)(p[2][1] - p[0][1]));
            static int sh2; if ((nb.var & 0x1000) && (e1 > 200 * 16 || e2 > 200 * 16) && sh2++ < 5) LOG("SIMDBG v0 %g %g %g uv %g %g c %08x run-ofs %d\n", g[ord[i - 2]].x, g[ord[i - 2]].y, g[ord[i - 2]].z, g[ord[i - 2]].u, g[ord[i - 2]].v, g[ord[i - 2]].c, ord[i - 2]);
            static int sh; if ((nb.var & 0x1000) && (e1 > 200 * 16 || e2 > 200 * 16) && sh++ < 20) LOG("SIMDBG var %x prim %d n %d tri %d area %.0f: (%d %d) (%d %d) (%d %d)\n", nb.var, prim, n, i, ar, p[0][0] / 16, p[0][1] / 16, p[1][0] / 16, p[1][1] / 16, p[2][0] / 16, p[2][1] / 16);
        }
    }
    if (ncap_on == 1) {                                     /* compare mode: the triangles, not the GS */
        for (int i = 2; i < m; i++) ncap_add(&ncap_a, &ncap_na, &ncap_cap_a, (NO *)(sim_pk + 16 * (1 + 3 * (i - 2))), (NO *)(sim_pk + 16 * (1 + 3 * (i - 1))), (NO *)(sim_pk + 16 * (1 + 3 * i)));
        return;
    }
    hle_gs_lite_packets(sim_get, 0, 1 + 3 * (uint32_t)m);
}
#endif
/* after a direct mesh: the GS registers / vertex queue as the program's packet would have left them (later packets
   may rely on ST, RGBAQ or the last vertices): the last two vertices again as XYZF3 (no drawing) */
static uint8_t tail_pk[16 * 7];
static const uint8_t *tail_get(void *u, uint32_t k) { (void)u; return tail_pk + 16 * k; }
static void nat_tail(int nv, const float (*C)[4], const float (*uv)[2], const uint32_t (*rgba)[4], int fogline, float fk, float fb255)
{
    if (!hle_gs_lite_packets || nv <= 0) return;
#ifndef HLE_PSP
    if (ncap_on == 1) return;
#endif
    if (nv > 2) nv = 2;
    uint64_t lo = (uint64_t)nv | 1ull << 15 | 3ull << 60, hi = 0xC12;            /* STQ, RGBA, XYZF3; EOP */
    memcpy(tail_pk, &lo, 8); memcpy(tail_pk + 8, &hi, 8);
    for (int i = 0; i < nv; i++) {
        uint8_t *o = tail_pk + 16 * (1 + 3 * i);
        float q = 1.0f / C[i][3], stq[4] = {uv[i][0] * q, uv[i][1] * q, q, 0};
        memcpy(o, stq, 16);
        memcpy(o + 16, rgba[i], 16);
        int32_t x[4];
        x[0] = ftoi_s(nb.v02[0] + C[i][0] * q, 16.0f); x[1] = ftoi_s(nb.v02[1] + C[i][1] * q, 16.0f); x[2] = ftoi_s(nb.v02[2] + C[i][2] * q, 16.0f);
        float f = fogline ? fk * C[i][3] + fb255 : 0; if (f < 0) f = 0; if (f > 255) f = 255;
        x[3] = ftoi_s(f, 16.0f);
        memcpy(o + 32, x, 16);
    }
    { void hle_gs_lite_tail(const uint8_t *pk, uint32_t n) __attribute__((weak));
      if (hle_gs_lite_tail) { hle_gs_lite_tail(tail_pk, 1 + 3 * (uint32_t)nv); return; } }
    hle_gs_lite_packets(tail_get, 0, 1 + 3 * (uint32_t)nv);
}
/* begin a direct mesh for this batch: 1 yes, 0 no (GS packets), -1 nothing to draw */
static int nat_direct_begin(const float Mo[4][4], int fog, float fa, float fb, int *dbl)
{
    if (nat_direct < 0) {
        const char *e = hle_cfg_get("x_gedirect"); nat_direct = !(e && e[0] == '0');
#ifdef NAT_NODIRECT
        nat_direct = 0;
#endif
    }
    if (!nat_direct) return 0;
    if (!((nb.tag[1] >> 14) & 1)) return 0;                             /* no PRIM in the tag */
    if (Mo[0][2] != 0 || Mo[1][2] != 0 || Mo[2][2] != 0) return 0;      /* GS Z must be z0 + zK / w */
    uint64_t prim = (nb.tag[1] >> 15) & 0x7FF;
    int r; TPROF(3, r = hle_ge_mesh_begin(prim, Mo, nb.v02[0], nb.v02[1], Mo[3][2], nb.v02[2], fog, fa, fb, dbl)); return r;
}
#endif

/* clip flags as the program's CLIP: bit 0/1 +x/-x, 2/3 y, 4/5 z */
static inline uint32_t nat_flags(const NV *v)
{
    float cx = nf(v->c[0] * nb.sx), cy = nf(v->c[1] * nb.sy), cz = nf(nb.zc + nb.zk * v->c[3]), l = fabsf(v->c[3]);
    return (cx > l ? 1u : 0) | (cx < -l ? 2u : 0) | (cy > l ? 4u : 0) | (cy < -l ? 8u : 0) | (cz > l ? 16u : 0) | (cz < -l ? 32u : 0);
}
static inline float nat_dist(const NV *v, int p)            /* >= 0 inside plane p */
{
    float w = v->c[3];
    switch (p) {
    case 0: return w - v->c[0] * nb.sx;  case 1: return w + v->c[0] * nb.sx;
    case 2: return w - v->c[1] * nb.sy;  case 3: return w + v->c[1] * nb.sy;
    case 4: return w - (nb.zc + nb.zk * w); default: return w + (nb.zc + nb.zk * w);
    }
}
static int nat_clip_poly(const NV *a, const NV *b, const NV *c, uint32_t any, NV **res);
#if defined(HLE_PSP) || defined(NAT_SIM)
static void nat_clip_poly_ge(const NV *a, const NV *b, const NV *c, uint32_t any, int dbl)
{
    NV *v; int n = nat_clip_poly(a, b, c, any, &v);
    if (n < 3) return;
    st_nat_clip++;
    HGV g[12];
    for (int i = 0; i < n; i++) { float pos[3] = {v[i].c[0], v[i].c[1], v[i].c[3]}; hgv_of(&g[i], &v[i], pos, dbl); }
    hle_ge_mesh_draw(GE_FAN, g, n);
}
#endif
static int nat_clip_poly(const NV *a, const NV *b, const NV *c, uint32_t any, NV **res)
{
    static NV buf[2][12]; int n = 3, s = 0;
    buf[0][0] = *a; buf[0][1] = *b; buf[0][2] = *c;
    for (int p = 0; p < 6 && n >= 3; p++) {
        if (!(any & (1u << p))) continue;
        NV *in = buf[s], *out = buf[s ^ 1]; int m = 0;
        for (int i = 0; i < n; i++) {
            const NV *P = &in[i], *Q = &in[(i + 1) % n];
            float dp = nat_dist(P, p), dq = nat_dist(Q, p);
            if (dp >= 0) out[m++] = *P;
            if ((dp >= 0) != (dq >= 0) && m < 12) {
                float t = dp / (dp - dq);
                NV *r = &out[m++];
                for (int k = 0; k < 4; k++) r->c[k] = P->c[k] + (Q->c[k] - P->c[k]) * t;
                for (int k = 0; k < 2; k++) r->uv[k] = P->uv[k] + (Q->uv[k] - P->uv[k]) * t;
                for (int k = 0; k < 4; k++) r->col[k] = P->col[k] + (Q->col[k] - P->col[k]) * t;
            }
        }
        n = m; s ^= 1;
    }
    *res = buf[s];
    return n;
}
static void nat_clip_tri(const NV *a, const NV *b, const NV *c, uint32_t any)
{
    NV *v; int n = nat_clip_poly(a, b, c, any, &v);
    if (n < 3) return;
    st_nat_clip++;
    NO o;
    nat_flush();
#ifndef HLE_PSP
    nat_inclip = 1;
#endif
    /* fan 0,1,..,n-1 as a strip: 0, 1, n-1, 2, n-2, ... */
    int lo = 1, hi = n - 1, k = 0;
    nat_out(&v[0], &o); nat_put(&o);
    while (lo <= hi) { nat_out(&v[(k++ & 1) ? hi-- : lo++], &o); nat_put(&o); }
    nat_flush();
#ifndef HLE_PSP
    nat_inclip = 0;
#endif
}

/* one batch; returns 0 when this is not the program / variant handled here (the interpreter runs it) */

#if defined(HLE_PSP) || defined(NAT_SIM)
/* the direct path's vertex work in plain float (the GE transforms; exact VU rounding does not matter here) */
static NV nat_V[256]; static uint32_t nat_F[256];
static void nat_emit(uint32_t var, uint32_t n, const float M[4][4], int dbl, HGV *G, int allin);
static void nat_clip_polys(uint32_t n, const float M[4][4], int dbl, HGV *G, int ncq, const uint16_t *clipq);
/* Gout: convert into Gout (the mesh cache) without culling, flags or drawing */
static void nat_fast_mesh(uint32_t var, uint32_t top, uint32_t n, const float M[4][4], const float *uvo, const float *m19,
                          int cm, int grey, float lf, const float *sc, int dbl, HGV *Gout)
{
    NV *V = nat_V; uint32_t *F = nat_F;
    st_fast_v += n;
    const int32_t *r = (const int32_t *)(vu.mem + 16 * top);
    const int fl166 = var == 0x166 && !Gout;
    if (fl166 && n) {
        /* the batch's box: when its 8 corners are all outside one clip plane, every triangle would be dropped
           (45% of these vertices at the school gate): nothing to decode, nothing to draw */
        int32_t lo[3] = {r[0], r[1], r[2]}, hi[3] = {r[0], r[1], r[2]};
        for (uint32_t k = 1; k < n; k++) {
            const int32_t *q = r + 16 * k;
            for (int i = 0; i < 3; i++) { if (q[i] < lo[i]) lo[i] = q[i]; if (q[i] > hi[i]) hi[i] = q[i]; }
        }
        uint32_t all = 63;
        for (int c = 0; c < 8 && all; c++) {
            float px = (float)((c & 1) ? hi[0] : lo[0]) * (1.0f / 128.0f), py = (float)((c & 2) ? hi[1] : lo[1]) * (1.0f / 128.0f), pz = (float)((c & 4) ? hi[2] : lo[2]) * (1.0f / 128.0f);
            float cx = M[3][0] + M[0][0] * px + M[1][0] * py + M[2][0] * pz, cy = M[3][1] + M[0][1] * px + M[1][1] * py + M[2][1] * pz;
            float cw = M[3][3] + M[0][3] * px + M[1][3] * py + M[2][3] * pz;
            float ex = cx * nb.sx, ey = cy * nb.sy, ez = nb.zc + nb.zk * cw, l = fabsf(cw);
            all &= (ex > l ? 1u : 0) | (ex < -l ? 2u : 0) | (ey > l ? 4u : 0) | (ey < -l ? 8u : 0) | (ez > l ? 16u : 0) | (ez < -l ? 32u : 0);
        }
        if (all) { st_cull_b++; st_cull_v += n; return; }
    }
    HGV *G = Gout ? Gout : (HGV *)hle_ge_mesh_alloc((int)n);   /* the vertices go straight into the display list */
    if (!G) return;
    const float k128 = 1.0f / 128.0f;
    /* colour scale with the GE's doubling folded in */
    float s2[4];
    for (int i = 0; i < 4; i++) s2[i] = m19[i] * ((i == 3 || dbl) ? 2.0f : 1.0f);
#if defined(HLE_PSP) && defined(HLE_TPROF_FINE)
    uint64_t tl0_ = sceKernelGetSystemTimeWide();
#endif
#if defined(HLE_PSP) && !defined(NAT_NOVFPU)
    if (cm && !grey) {
        /* the VFPU: 4 lanes per instruction. Constants: C700 UV offset, C710 colour scale, C720 255, C730 material x
           GE doubling, S600 the lerp factor, M400 = the object-to-clip matrix, clip row i in column C4i0 (vtfm4 dots the columns; for the clip flags) */
        float __attribute__((aligned(16))) kc[4][4] = {{uvo[0], uvo[1], 0, 0}, {sc[0], sc[1], sc[2], sc[3]}, {255, 255, 255, 255}, {s2[0], s2[1], s2[2], s2[3]}};
        float __attribute__((aligned(16))) kl[4] = {lf, 0, 0, 0}, T[4][4];
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) T[i][j] = M[j][i];
        __asm__ __volatile__(
            "lv.q C700, 0(%0)\n lv.q C710, 16(%0)\n lv.q C720, 32(%0)\n lv.q C730, 48(%0)\n lv.q C600, 0(%1)\n"
            "lv.q C400, 0(%2)\n lv.q C410, 16(%2)\n lv.q C420, 32(%2)\n lv.q C430, 48(%2)\n"
            : : "r"(kc), "r"(kl), "r"(T) : "memory");
        const float bsx = nb.sx, bsy = nb.sy, bzc = nb.zc, bzk = nb.zk;
        HGV *gp = G;
        for (uint32_t k = 0; k < n; k++, r += 16, gp++) {
            __asm__ __volatile__(
                "lv.q C000, 0(%0)\n lv.q C010, 16(%0)\n lv.q C020, 32(%0)\n lv.q C030, 48(%0)\n"
                "vi2f.q C000, C000, 7\n vi2f.p C010, C010, 7\n vi2f.q C020, C020, 0\n vi2f.q C030, C030, 0\n"
                "vadd.p C010, C010, C700\n"
                "vsub.q C200, C030, C020\n vscl.q C200, C200, S600\n vadd.q C200, C020, C200\n vadd.s S203, S023, S033\n"
                "vmul.q C200, C200, C710\n vmin.q C200, C200, C720\n vmul.q C200, C200, C730\n"
                "vf2iz.q C200, C200, 23\n vi2uc.q S200, C200\n"
                "sv.s S010, 0(%1)\n sv.s S011, 4(%1)\n sv.s S200, 8(%1)\n sv.s S000, 12(%1)\n sv.s S001, 16(%1)\n sv.s S002, 20(%1)\n"
                : : "r"(r), "r"(gp) : "memory");
            if (fl166) {
                NV *v = &V[k];
                __asm__ __volatile__(
                    "vone.s S003\n vtfm4.q C300, M400, C000\n"
                    "sv.s S300, 0(%0)\n sv.s S301, 4(%0)\n sv.s S302, 8(%0)\n sv.s S303, 12(%0)\n"
                    : : "r"(v->c) : "memory");
                float cx_ = v->c[0], cy_ = v->c[1], cw_ = v->c[3];
                v->uv[0] = gp->u; v->uv[1] = gp->v;
                float ex = cx_ * bsx, ey = cy_ * bsy, ez = bzc + bzk * cw_, l = fabsf(cw_);
                uint32_t f = (ex > l ? 1u : 0) | (ex < -l ? 2u : 0) | (ey > l ? 4u : 0) | (ey < -l ? 8u : 0) | (ez > l ? 16u : 0) | (ez < -l ? 32u : 0);
                if (NAT_GUARD && f && !(f & 48u) && fabsf(cx_) < 1500.0f * l && fabsf(cy_) < 1500.0f * l) f |= 64u;
                F[k] = f;
            }
        }
    } else
#endif
    {   /* constants in locals: the stores into the display list would otherwise force reloads (aliasing) */
        const float u0 = uvo[0], v0 = uvo[1], s20 = s2[0], s21 = s2[1], s22 = s2[2], s23 = s2[3];
        const float sc0 = sc[0], sc1 = sc[1], sc2 = sc[2], sc3 = sc[3];
        const float m30 = M[3][0], m00 = M[0][0], m10 = M[1][0], m20 = M[2][0];
        const float m31 = M[3][1], m01 = M[0][1], m11 = M[1][1], m21 = M[2][1];
        const float m32 = M[3][2], m22 = M[2][2];
        const float m33 = M[3][3], m03 = M[0][3], m13 = M[1][3], m23 = M[2][3];
        const float bsx = nb.sx, bsy = nb.sy, bzc = nb.zc, bzk = nb.zk;
        const int is166 = fl166;
        HGV *__restrict gp = G;
        for (uint32_t k = 0; k < n; k++, r += 16, gp++) {
            float px = (float)r[0] * k128, py = (float)r[1] * k128, pz = (float)r[2] * k128;
            float tu = (float)r[4] * k128 + u0, tv = (float)r[5] * k128 + v0;
            float c0, c1, c2, c3;
            if (cm) {
                float a0 = (float)r[8], a1 = (float)r[9], a2 = (float)r[10], a3 = (float)r[11];
                float b0 = (float)r[12], b1 = (float)r[13], b2 = (float)r[14], b3 = (float)r[15];
                if (grey) { a0 = a1 = a2 = 12.0f + a0; b0 = b1 = b2 = 12.0f + b0; }
                c0 = (a0 + (b0 - a0) * lf) * sc0; c1 = (a1 + (b1 - a1) * lf) * sc1; c2 = (a2 + (b2 - a2) * lf) * sc2; c3 = (a3 + b3) * sc3;
                c0 = (c0 > 255.0f ? 255.0f : c0) * s20; c1 = (c1 > 255.0f ? 255.0f : c1) * s21;
                c2 = (c2 > 255.0f ? 255.0f : c2) * s22; c3 = (c3 > 255.0f ? 255.0f : c3) * s23;
            } else {
                c0 = (float)r[8] * s20; c1 = (float)r[9] * s21; c2 = (float)r[10] * s22; c3 = 255.0f * s23;
            }
            int i0 = (int)c0, i1 = (int)c1, i2 = (int)c2, i3 = (int)c3;
            i0 = i0 > 255 ? 255 : i0 < 0 ? 0 : i0; i1 = i1 > 255 ? 255 : i1 < 0 ? 0 : i1;
            i2 = i2 > 255 ? 255 : i2 < 0 ? 0 : i2; i3 = i3 > 255 ? 255 : i3 < 0 ? 0 : i3;
            gp->u = tu; gp->v = tv; gp->c = (uint32_t)i0 | (uint32_t)i1 << 8 | (uint32_t)i2 << 16 | (uint32_t)i3 << 24;
            gp->x = px; gp->y = py; gp->z = pz;
            if (is166) {
                NV *v = &V[k];
                float cx_ = m30 + m00 * px + m10 * py + m20 * pz, cy_ = m31 + m01 * px + m11 * py + m21 * pz;
                float cw_ = m33 + m03 * px + m13 * py + m23 * pz;
                v->c[0] = cx_; v->c[1] = cy_; v->c[2] = m32 + m22 * pz; v->c[3] = cw_;
                v->uv[0] = tu; v->uv[1] = tv;               /* (the colour for the clipper comes from G when needed) */
                float ex = cx_ * bsx, ey = cy_ * bsy, ez = bzc + bzk * cw_, l = fabsf(cw_);
                uint32_t f = (ex > l ? 1u : 0) | (ex < -l ? 2u : 0) | (ey > l ? 4u : 0) | (ey < -l ? 8u : 0) | (ez > l ? 16u : 0) | (ez < -l ? 32u : 0);
                /* outside the PS2 guard band only in x / y but well inside the GE's coordinate range: the GE's
                   scissor does the clipping (bit 6: no CPU clipping needed for this vertex) */
                if (NAT_GUARD && f && !(f & 48u) && fabsf(cx_) < 1500.0f * l && fabsf(cy_) < 1500.0f * l) f |= 64u;
                F[k] = f;
            }
        }
    }
#if defined(HLE_PSP) && defined(HLE_TPROF_FINE)
    hle_tprof[10] += sceKernelGetSystemTimeWide() - tl0_;
#endif
    if (Gout) return;
    nat_emit(var, n, M, dbl, G, 0);
}
/* draw a converted batch G (flags / clip-space positions of 0x166 in nat_F / nat_V): strips, CPU-clipped polygons,
   the GS state after it */
static void nat_emit(uint32_t var, uint32_t n, const float M[4][4], int dbl, HGV *G, int allin)
{
    NV *V = nat_V; uint32_t *F = nat_F;
    {   /* GS state after the batch: its last two vertices */
        float C[2][4], uv[2][2]; uint32_t rgba[2][4]; int nt = n >= 2 ? 2 : (int)n;
        for (int i = 0; i < nt; i++) {
            const HGV *g = &G[n - nt + i];
            C[i][0] = M[3][0] + M[0][0] * g->x + M[1][0] * g->y + M[2][0] * g->z;
            C[i][1] = M[3][1] + M[0][1] * g->x + M[1][1] * g->y + M[2][1] * g->z;
            C[i][2] = M[3][2] + M[2][2] * g->z;
            C[i][3] = M[3][3] + M[0][3] * g->x + M[1][3] * g->y + M[2][3] * g->z;
            uv[i][0] = g->u; uv[i][1] = g->v;
            for (int c = 0; c < 4; c++) { uint32_t b = (g->c >> (8 * c)) & 0xFF; rgba[i][c] = (c == 3 || dbl) ? (b >= 0xFF ? 0x80 : b / 2) : b; }
        }
        nat_tail(nt, C, uv, rgba, var != 0x0cb, nb.k, var == 0x166 ? nb.v02[3] - nb.fa * nb.k : -nb.fa * nb.k);
    }
    if (var != 0x166 || allin) { hle_ge_mesh_draw_at(GE_STRIP, G, (int)n); hle_ge_mesh_end(); return; }
    static uint16_t clipq[256]; int ncq = 0;
    int run = -1;                                           /* first vertex of the current run of inside triangles */
    for (uint32_t k = 0; k + 2 < n; k++) {
        uint32_t f0 = F[k], f1 = F[k + 1], f2 = F[k + 2];
        int ok = !((f0 | f1 | f2) & 63u) || ((f0 == 0 || (f0 & 64u)) && (f1 == 0 || (f1 & 64u)) && (f2 == 0 || (f2 & 64u)) && !(f0 & f1 & f2 & 63u));
        if (ok) { if (run < 0) run = (int)k; continue; }
        if (run >= 0) { hle_ge_mesh_draw_at(GE_STRIP, &G[run], (int)k + 2 - run); run = -1; }
        if (!(f0 & f1 & f2 & 63u)) clipq[ncq++] = (uint16_t)k;
    }
    if (run >= 0) hle_ge_mesh_draw_at(GE_STRIP, &G[run], (int)n - run);
    hle_ge_mesh_end();
    if (ncq) ZN(15, nat_clip_polys(n, M, dbl, G, ncq, clipq));
}
/* nat_emit for a triangle list (prim 3): runs of triangles inside go out as lists, the others are clipped */
static void nat_emit_list(uint32_t n, const float M[4][4], int dbl, HGV *G)
{
    uint32_t *F = nat_F;
    {   /* GS state after the batch: its last two vertices */
        float C[2][4], uv[2][2]; uint32_t rgba[2][4]; int nt = n >= 2 ? 2 : (int)n;
        for (int i = 0; i < nt; i++) {
            const HGV *g = &G[n - nt + i];
            C[i][0] = M[3][0] + M[0][0] * g->x + M[1][0] * g->y + M[2][0] * g->z;
            C[i][1] = M[3][1] + M[0][1] * g->x + M[1][1] * g->y + M[2][1] * g->z;
            C[i][2] = M[3][2] + M[2][2] * g->z;
            C[i][3] = M[3][3] + M[0][3] * g->x + M[1][3] * g->y + M[2][3] * g->z;
            uv[i][0] = g->u; uv[i][1] = g->v;
            for (int c = 0; c < 4; c++) { uint32_t b = (g->c >> (8 * c)) & 0xFF; rgba[i][c] = (c == 3 || dbl) ? (b >= 0xFF ? 0x80 : b / 2) : b; }
        }
        nat_tail(nt, C, uv, rgba, 1, nb.k, nb.v02[3] - nb.fa * nb.k);
    }
    static uint16_t clipq[96]; int ncq = 0;
    int run = -1;
    for (uint32_t k = 0; k + 2 < n; k += 3) {
        uint32_t f0 = F[k], f1 = F[k + 1], f2 = F[k + 2];
        int ok = !((f0 | f1 | f2) & 63u) || ((f0 == 0 || (f0 & 64u)) && (f1 == 0 || (f1 & 64u)) && (f2 == 0 || (f2 & 64u)) && !(f0 & f1 & f2 & 63u));
        if (ok) { if (run < 0) run = (int)k; continue; }
        if (run >= 0) { hle_ge_mesh_draw_at(GE_TRIS, &G[run], (int)k - run); run = -1; }
        if (!(f0 & f1 & f2 & 63u) && ncq < 96) clipq[ncq++] = (uint16_t)k;
    }
    if (run >= 0) hle_ge_mesh_draw_at(GE_TRIS, &G[run], (int)(n - n % 3) - run);
    hle_ge_mesh_end();
    if (ncq) ZN(15, nat_clip_polys(n, M, dbl, G, ncq, clipq));
}
static void nat_clip_polys(uint32_t n, const float M[4][4], int dbl, HGV *G, int ncq, const uint16_t *clipq)
{
    NV *V = nat_V; uint32_t *F = nat_F; (void)n;
    {                                                       /* clipped polygons, in the PS2 clip space (Cx, Cy, Cw) */
        float Mcz[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 0}}; Mcz[3][2] = M[3][2];
        float fa_ = nb.k / 255.0f, fb_ = (nb.v02[3] - nb.fa * nb.k) / 255.0f; int d2;
        if (nat_direct_begin(Mcz, 1, fa_, fb_, &d2) > 0)
            for (int q = 0; q < ncq; q++) {
                uint32_t k = clipq[q];
                for (uint32_t j = k; j < k + 3; j++)          /* GE colour -> the GS units the clipper interpolates */
                    for (int i = 0; i < 4; i++) V[j].col[i] = (float)((G[j].c >> (8 * i)) & 0xFF) * ((i == 3 || dbl) ? 0.5f : 1.0f);
                nat_clip_poly_ge(&V[k], &V[k + 1], &V[k + 2], (F[k] | F[k + 1] | F[k + 2]) & 63u, d2);
            }
    }
}

#ifdef HLE_PSP
/* ------------------------------------------------------------------ mesh cache (PSP)
 * A world batch converted once: its GE vertices (object space, 24 bytes each) stay in a pool and are drawn from
 * there with the current matrix. Keyed by the batch's UNPACK commands and data (deferred in hle_vu1.c: on a hit
 * they are never run) and the object constants the conversion uses (UV offset, material colour, lighting lerp).
 * The pool is a ring; copies drawn in the current frame are stepped over (the GE may still read them). */
void *hle_psp_hole_take(size_t size, size_t align) __attribute__((weak));
unsigned hle_psp_hole_free_kb(void) __attribute__((weak));
void *hle_ge_gs_alloc(size_t n, const char *what) __attribute__((weak));
uint32_t hle_gs_lite_frame(void) __attribute__((weak));
typedef struct { uint64_t key; uint32_t off, size, n, last; int32_t lo[3], hi[3]; int live, next, has; } MC;
#define MC_MAX 4096
#define MC_HB 2048
static MC mce[MC_MAX]; static int mc_n, mc_free = -1, mc_bucket[MC_HB];
static uint8_t *mc_pool; static uint32_t mc_size, mc_head;
static uint64_t st_mc_hit, st_mc_miss, st_mc_nocov, st_mc_full, st_mc_evict, st_mc_cull, st_mc_allin, st_mc_part, st_mc_v;
static inline uint32_t mc_frame(void) { return hle_gs_lite_frame ? hle_gs_lite_frame() : 0; }   /* GE frames (a flip waits for the GE) */
static void mc_init(void)
{
    mc_on = 0;
    const char *e = hle_cfg_get("x_meshcache"); if (e && e[0] == '0') { LOG("mesh cache: off (x_meshcache=0)\n"); return; }
    uint32_t kb = 0;
    if (hle_psp_hole_take && hle_psp_hole_free_kb) {
        unsigned f = hle_psp_hole_free_kb();
        if (f >= 1024) { kb = f - 16 > 2048 ? 2048 : f - 16; mc_pool = (uint8_t *)hle_psp_hole_take(kb * 1024, 64); }
    }
    if (!mc_pool && hle_ge_gs_alloc) { kb = 1536; mc_pool = (uint8_t *)hle_ge_gs_alloc(kb * 1024, "mesh_cache"); }
    if (!mc_pool) { LOG("mesh cache: no memory\n"); return; }
    mc_size = kb * 1024;
    for (int i = 0; i < MC_HB; i++) mc_bucket[i] = -1;
    mc_on = 1;
    LOG("mesh cache: %u KB\n", (unsigned)kb);
}
static inline int mc_hash(uint64_t k) { return (int)((k * 0x9E3779B97F4A7C15ull) >> 53) & (MC_HB - 1); }
static void mc_unlink(int i)
{
    int *pp = &mc_bucket[mc_hash(mce[i].key)];
    while (*pp >= 0 && *pp != i) pp = &mce[*pp].next;
    if (*pp == i) *pp = mce[i].next;
    mce[i].live = 0; mce[i].next = mc_free; mc_free = i;
}
static int mc_slot(void)
{
    if (mc_free >= 0) { int i = mc_free; mc_free = mce[i].next; return i; }
    if (mc_n < MC_MAX) return mc_n++;
    /* all slots live: drop the oldest one not drawn in this frame */
    uint32_t f = mc_frame(); int o = -1;
    for (int i = 0; i < mc_n; i++) if (mce[i].last != f && (o < 0 || mce[i].last < mce[o].last)) o = i;
    if (o < 0) return -1;
    mc_unlink(o); st_mc_evict++;
    int i = mc_free; mc_free = mce[i].next; return i;
}
/* the pool's 64-byte cells: the entry whose vertices start ... cover each one (index + 1; stale values are checked
   against the entry): the entries a range overlaps without scanning every entry */
#define MC_CELLS (2048u * 1024u / 64u)
static uint16_t mc_own[MC_CELLS];
static void mc_own_set(int i) { for (uint32_t c = mce[i].off / 64; c < (mce[i].off + mce[i].size) / 64 && c < MC_CELLS; c++) mc_own[c] = (uint16_t)(i + 1); }
static int mc_alloc(uint32_t n, uint32_t *off)
{
    n = (n + 63) & ~63u;
    if (n > mc_size) return 0;
    uint32_t f = mc_frame(), a = mc_head, moved = 0;
    for (;;) {
        if (a + n > mc_size) { moved += mc_size - a; a = 0; }
        uint32_t b = a + n, hot_end = 0;
        for (uint32_t c = a / 64; c < b / 64; c++) {
            int i = (int)mc_own[c] - 1;
            if (i >= 0 && mce[i].live && mce[i].size && mce[i].last == f && mce[i].off < b && a < mce[i].off + mce[i].size) { uint32_t e = mce[i].off + mce[i].size; if (e > hot_end) hot_end = e; }
        }
        if (!hot_end) break;
        moved += hot_end - a; a = hot_end;
        if (moved >= mc_size) { st_mc_full++; return 0; }
    }
    uint32_t b = a + n;
    for (uint32_t c = a / 64; c < b / 64; c++) {
        int i = (int)mc_own[c] - 1;
        if (i >= 0 && mce[i].live && mce[i].size && mce[i].off < b && a < mce[i].off + mce[i].size) { mc_unlink(i); st_mc_evict++; }
    }
    mc_head = b; *off = a;
    return 1;
}
/* the key of the pending UNPACKs + constants; 0 when they do not fill the batch's 4 qwords per vertex */
static uint64_t mc_keyof(uint32_t var, uint32_t top, uint32_t n, const float *uvo, const float *m19, int cm, int grey, float lf, const float *sc, int dbl)
{
    /* four independent multiply-xor lanes (the multiplies overlap), folded at the end */
    uint32_t mask = 0, a0 = 2166136261u, a1 = 0x9747B28Cu, a2 = 0x85EBCA6Bu, a3 = 0xC2B2AE35u;
#define MCW(w_) do { a0 = (a0 ^ (w_)) * 16777619u; } while (0)
    for (int i = 0; i < npu; i++) {
        const PU *u = &pu[i];
        uint32_t rel = (u->imm & 0x3FF) + u->tops - top, num = u->num ? u->num : 256;
        if (u->wl != 1 || u->cl != 4 || rel >= 4 || num < n) continue;
        mask |= 1u << rel;
        MCW(u->cmd | (u->imm & 0xFC00) << 8 | rel << 26); MCW(num); MCW(u->nw);
        const uint32_t *d = u->src;
        uint32_t k = 0;
        for (; k + 4 <= u->nw; k += 4) {
            a0 = (a0 ^ d[k]) * 16777619u; a1 = (a1 ^ d[k + 1]) * 0x01000193u + 0x7F4A7C15u;
            a2 = (a2 ^ d[k + 2]) * 0x85EBCA6Bu; a3 = (a3 ^ d[k + 3]) * 0xC2B2AE35u;
        }
        for (; k < u->nw; k++) MCW(d[k]);
    }
    if (mask != 15) return 0;
    uint32_t f[16]; memcpy(f, uvo, 8); memcpy(f + 2, m19, 16); memcpy(f + 6, &lf, 4); memcpy(f + 7, sc, 16);
    f[11] = var | (uint32_t)cm << 12 | (uint32_t)grey << 13 | (uint32_t)dbl << 14; f[12] = n;
    for (int i = 0; i < 13; i++) MCW(f[i]);
#undef MCW
    uint32_t h1 = a0 ^ (a2 >> 15) ^ (a2 << 17), h2 = a1 ^ (a3 >> 13) ^ (a3 << 19);
    h1 = (h1 ^ (h1 >> 16)) * 0x7FEB352Du; h2 = (h2 ^ (h2 >> 15)) * 0x846CA68Bu; h2 ^= h1;
    uint64_t k = (uint64_t)h1 << 32 | h2;
    return k ? k : 1;
}
/* 0x166: the box against the clip planes: 2 = every corner outside one plane, 1 = every corner inside, 0 = mixed */
/* VFPU: E100 = the matrix rows M[0..3] as columns of M100, transposed: C = M[3] + px M[0] + py M[1] + pz M[2] */
static inline void mc_vload(const float M[4][4])
{
    float __attribute__((aligned(16))) m[4][4]; memcpy(m, M, sizeof m);
    __asm__ __volatile__("lv.q C100, 0(%0)\n lv.q C110, 16(%0)\n lv.q C120, 32(%0)\n lv.q C130, 48(%0)\n" : : "r"(m) : "memory");
}
static uint64_t st_part_near, st_part_far, st_part_xy;
static uint64_t st_mc_memo;                                 /* mesh keys taken from the memo (mc_mesh) */      /* partly clipped batches: the planes their boxes cross */
static int mc_box(const MC *e, const float M[4][4])
{
    uint32_t all = 63, any = 0;
#if !defined(NAT_NOVFPU)
    float __attribute__((aligned(16))) P[8][4], O[8][4];
    mc_vload(M);
    float lo_[3] = {(float)e->lo[0] * (1.0f / 128.0f), (float)e->lo[1] * (1.0f / 128.0f), (float)e->lo[2] * (1.0f / 128.0f)};
    float hi_[3] = {(float)e->hi[0] * (1.0f / 128.0f), (float)e->hi[1] * (1.0f / 128.0f), (float)e->hi[2] * (1.0f / 128.0f)};
    {   float __attribute__((aligned(16))) kf[4] = {nb.sx, nb.sy, nb.zk, nb.zc};
        __asm__ __volatile__("lv.q C200, 0(%0)\n vone.t C220\n" : : "r"(kf) : "memory"); }
    static const uint8_t spread[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    for (int c = 0; c < 8; c++) {
        P[c][0] = (c & 1) ? hi_[0] : lo_[0]; P[c][1] = (c & 2) ? hi_[1] : lo_[1]; P[c][2] = (c & 4) ? hi_[2] : lo_[2]; P[c][3] = 1.0f;
        uint32_t gt_, lt_;
        __asm__ __volatile__("lv.q C000, 0(%2)\n vtfm4.q C010, E100, C000\n sv.q C010, 0(%3)\n"
                             "vmul.p C030, C010, C200\n vmul.s S032, S013, S202\n vadd.s S032, S032, S203\n"
                             "vabs.s S033, S013\n vscl.t C210, C220, S033\n vneg.t C230, C210\n"
                             "vcmp.t GT, C030, C210\n mfvc %0, $131\n vcmp.t LT, C030, C230\n mfvc %1, $131\n"
                             : "=&r"(gt_), "=&r"(lt_) : "r"(P[c]), "r"(O[c]) : "memory");
        uint32_t f = (uint32_t)spread[gt_ & 7] | (uint32_t)spread[lt_ & 7] << 1;
        all &= f;
        if (NAT_GUARD && f && !(f & 48u)) { float l = fabsf(O[c][3]); if (fabsf(O[c][0]) < 1500.0f * l && fabsf(O[c][1]) < 1500.0f * l) f = 0; }
        any |= f;
    }
    if (0)
#endif
    for (int c = 0; c < 8; c++) {   /* (all: the raw flags, as nat_fast_mesh culls; any: with the GE scissor margin) */
#if !defined(NAT_NOVFPU)
        float cx = O[c][0], cy = O[c][1], cw = O[c][3];
#else
        float px = (float)((c & 1) ? e->hi[0] : e->lo[0]) * (1.0f / 128.0f), py = (float)((c & 2) ? e->hi[1] : e->lo[1]) * (1.0f / 128.0f), pz = (float)((c & 4) ? e->hi[2] : e->lo[2]) * (1.0f / 128.0f);
        float cx = M[3][0] + M[0][0] * px + M[1][0] * py + M[2][0] * pz, cy = M[3][1] + M[0][1] * px + M[1][1] * py + M[2][1] * pz;
        float cw = M[3][3] + M[0][3] * px + M[1][3] * py + M[2][3] * pz;
#endif
        float ex = cx * nb.sx, ey = cy * nb.sy, ez = nb.zc + nb.zk * cw, l = fabsf(cw);
        uint32_t f = (ex > l ? 1u : 0) | (ex < -l ? 2u : 0) | (ey > l ? 4u : 0) | (ey < -l ? 8u : 0) | (ez > l ? 16u : 0) | (ez < -l ? 32u : 0);
        all &= f;
        if (NAT_GUARD && f && !(f & 48u) && fabsf(cx) < 1500.0f * l && fabsf(cy) < 1500.0f * l) f = 0;   /* GE scissor */
        any |= f;
    }
    if (!all && any) { if (any & 32u) st_part_near++; else if (any & 16u) st_part_far++; else st_part_xy++; }
    return all ? 2 : any ? 0 : 1;
}
static void mc_draw(MC *e, uint32_t var, const float M[4][4], int dbl, int b)   /* b: mc_box (var 0x166) */
{
    HGV *G = (HGV *)(mc_pool + e->off);
    uint32_t n = e->n;
    st_mc_v += n;
    if (var == 0x166) {
        if (b == 1) { st_mc_allin++; nat_emit(var, n, M, dbl, G, 1); return; }
        st_mc_part++;
        /* per vertex: clip-space position and flags (as nat_fast_mesh) */
        const float bsx = nb.sx, bsy = nb.sy, bzc = nb.zc, bzk = nb.zk;
#if !defined(NAT_NOVFPU)
        mc_vload(M);
        /* C200 = (sx, sy, zk, zc), C220 = (1, 1, 1); the flags: CC of R > |w| and R < -|w| with R = (x sx, y sy, zc + zk w) */
        {   float __attribute__((aligned(16))) kf[4] = {bsx, bsy, bzk, bzc};
            __asm__ __volatile__("vone.s S003\n lv.q C200, 0(%0)\n vone.t C220\n" : : "r"(kf) : "memory"); }
        static const uint8_t spread[8] = {0, 1, 4, 5, 16, 17, 20, 21};
#endif
        for (uint32_t k = 0; k < n; k++) {
            const HGV *g = &G[k]; NV *v = &nat_V[k];
            float px = g->x, py = g->y, pz = g->z;
#if !defined(NAT_NOVFPU)
            float __attribute__((aligned(16))) o[4];
            uint32_t gt_, lt_;
            __asm__ __volatile__("lv.s S000, 0(%2)\n lv.s S001, 4(%2)\n lv.s S002, 8(%2)\n vtfm4.q C010, E100, C000\n sv.q C010, 0(%3)\n"
                                 "vmul.p C030, C010, C200\n vmul.s S032, S013, S202\n vadd.s S032, S032, S203\n"
                                 "vabs.s S033, S013\n vscl.t C210, C220, S033\n vneg.t C230, C210\n"
                                 "vcmp.t GT, C030, C210\n mfvc %0, $131\n vcmp.t LT, C030, C230\n mfvc %1, $131\n"
                                 : "=&r"(gt_), "=&r"(lt_) : "r"(&g->x), "r"(o) : "memory");
            float cx_ = o[0], cy_ = o[1], cw_ = o[3];
            v->c[0] = cx_; v->c[1] = cy_; v->c[2] = M[3][2] + M[2][2] * pz; v->c[3] = cw_;
            v->uv[0] = g->u; v->uv[1] = g->v;
            uint32_t f = (uint32_t)spread[gt_ & 7] | (uint32_t)spread[lt_ & 7] << 1;
            if (NAT_GUARD && f && !(f & 48u)) { float l = fabsf(cw_); if (fabsf(cx_) < 1500.0f * l && fabsf(cy_) < 1500.0f * l) f |= 64u; }
            nat_F[k] = f;
            (void)px; (void)py;
#else
            float cx_ = M[3][0] + M[0][0] * px + M[1][0] * py + M[2][0] * pz, cy_ = M[3][1] + M[0][1] * px + M[1][1] * py + M[2][1] * pz;
            float cw_ = M[3][3] + M[0][3] * px + M[1][3] * py + M[2][3] * pz;
            v->c[0] = cx_; v->c[1] = cy_; v->c[2] = M[3][2] + M[2][2] * pz; v->c[3] = cw_;
            v->uv[0] = g->u; v->uv[1] = g->v;
            float ex = cx_ * bsx, ey = cy_ * bsy, ez = bzc + bzk * cw_, l = fabsf(cw_);
            uint32_t f = (ex > l ? 1u : 0) | (ex < -l ? 2u : 0) | (ey > l ? 4u : 0) | (ey < -l ? 8u : 0) | (ez > l ? 16u : 0) | (ez < -l ? 32u : 0);
            if (NAT_GUARD && f && !(f & 48u) && fabsf(cx_) < 1500.0f * l && fabsf(cy_) < 1500.0f * l) f |= 64u;
            nat_F[k] = f;
#endif
        }
    }
    nat_emit(var, n, M, dbl, G, 0);
}
static void mc_mesh(uint32_t var, uint32_t top, uint32_t n, const float M[4][4], const float *uvo, const float *m19,
                    int cm, int grey, float lf, const float *sc, int dbl)
{
    uint64_t key = 0;
    if (n >= 3) {
        /* memo: data parsed in place in RAM (stable addresses): the source addresses, sizes, the batch's other inputs
           and 8 sampled data words -> the full key, recomputed at least every 8 frames */
        int stable = npu > 0;
        uint32_t q = 0x811C9DC5u, fr = mc_frame();
        for (int i = 0; i < npu && stable; i++) {
            const PU *u = &pu[i];
            if (!u->stable || !u->nw) { stable = 0; break; }
            q = (q ^ (uint32_t)(uintptr_t)u->src) * 16777619u; q = (q ^ u->nw ^ u->cmd << 8 ^ (u->imm & 0x3FF) << 20 ^ u->tops) * 16777619u;
            q = (q ^ u->src[0]) * 16777619u; q = (q ^ u->src[u->nw - 1]) * 16777619u; q = (q ^ u->src[u->nw >> 1]) * 16777619u;
        }
        if (stable) {
            uint32_t f[16]; memcpy(f, uvo, 8); memcpy(f + 2, m19, 16); memcpy(f + 6, &lf, 4); memcpy(f + 7, sc, 16);
            f[11] = var | (uint32_t)cm << 12 | (uint32_t)grey << 13 | (uint32_t)dbl << 14; f[12] = n; f[13] = top;
            for (int i = 0; i < 14; i++) q = (q ^ f[i]) * 16777619u;
            static struct { uint32_t q, fr; uint64_t key; } memo[4096];
            uint32_t mi = (q ^ (q >> 13)) & 4095;
            if (memo[mi].q == q && memo[mi].key && fr - memo[mi].fr < 8) { key = memo[mi].key; st_mc_memo++; }
            else { ZN(4, key = mc_keyof(var, top, n, uvo, m19, cm, grey, lf, sc, dbl)); memo[mi].q = q; memo[mi].fr = fr; memo[mi].key = key; }
        } else ZN(4, key = mc_keyof(var, top, n, uvo, m19, cm, grey, lf, sc, dbl));
    }
    int hb = mc_hash(key), ei = -1;
    for (int i = mc_bucket[hb]; i >= 0; i = mce[i].next) if (mce[i].key == key) { ei = i; break; }
    uint32_t fr = mc_frame();
    if (ei >= 0) {
        MC *e = &mce[ei];
        int b = var == 0x166 ? mc_box(e, M) : 0;
        if (b == 2) { npu = 0; e->last = fr; st_mc_cull++; st_cull_b++; st_cull_v += n; return; }
        if (e->has) { npu = 0; e->last = fr; st_mc_hit++; ZN(5, mc_draw(e, var, M, dbl, b)); return; }
        mc_unlink(ei);                                      /* known box, no vertices yet: convert now */
    }
    st_mc_miss++;
    pu_flush();
    const int32_t *r = (const int32_t *)(vu.mem + 16 * top);
    int32_t lo[3] = {r[0], r[1], r[2]}, hi[3] = {r[0], r[1], r[2]};
    for (uint32_t k = 1; k < n; k++) { const int32_t *q = r + 16 * k; for (int i = 0; i < 3; i++) { if (q[i] < lo[i]) lo[i] = q[i]; if (q[i] > hi[i]) hi[i] = q[i]; } }
    int si = mc_slot();
    if (si < 0) { nat_fast_mesh(var, top, n, M, uvo, m19, cm, grey, lf, sc, dbl, 0); return; }
    MC *e = &mce[si];
    memset(e, 0, sizeof *e);
    e->key = key; e->n = n; e->last = fr; memcpy(e->lo, lo, sizeof lo); memcpy(e->hi, hi, sizeof hi);
    e->live = 1; e->next = mc_bucket[hb]; mc_bucket[hb] = si;
    int b = var == 0x166 ? mc_box(e, M) : 0;
    if (b == 2) { st_mc_cull++; st_cull_b++; st_cull_v += n; return; }   /* the box only, for now */
    uint32_t off;
    if (!mc_alloc(n * (uint32_t)sizeof(HGV), &off)) { mc_unlink(si); nat_fast_mesh(var, top, n, M, uvo, m19, cm, grey, lf, sc, dbl, 0); return; }
    e = &mce[si];                                           /* (mc_alloc may have unlinked others, not this one) */
    e->off = off; e->size = (n * (uint32_t)sizeof(HGV) + 63) & ~63u; e->has = 1;
    mc_own_set(si);
    HGV *G = (HGV *)(mc_pool + off);
    ZN(14, nat_fast_mesh(var, top, n, M, uvo, m19, cm, grey, lf, sc, dbl, G));
    sceKernelDcacheWritebackRange(G, n * sizeof(HGV));
    ZN(5, mc_draw(e, var, M, dbl, b));
}
#endif
#endif

#ifndef HLE_PSP
static void mc_init(void) { mc_on = 0; }
#endif

/* the object setup (MSCAL at 0, 0x000..0x028): matrix columns vf28..31 (x / y re-centred by w/2, scaled by 1015),
 * vf02 = screen offset (1016 + 1017.xy, w 0), the variant from the flags 1023.x into 1023.y, the output buffers */
static uint32_t nat_variant(uint32_t fl)
{
    if (!(fl & 2)) return (fl & 1) ? 0x114 : 0x0cb;
    if (!(fl & 4)) return (fl & 8) ? 0x166 : 0x294;
    return (fl & 8) ? 0x2db : 0x3eb;
}
static void nat_setup(void)
{
    uint32_t fl = (uint32_t)mi(1023, 0) & 0xFFFF, var = nat_variant(fl);
    float s15[3] = {mf(1015, 0), mf(1015, 1), mf(1015, 2)};
    for (int r = 0; r < 4; r++) {
        uint32_t a = 1008 + (uint32_t)r; V4 *v = &vu.vf[28 + r];
        float x = mf(a, 0), y = mf(a, 1), w = mf(a, 3), h = nf(w * 0.5f);
        float z = r == 3 ? nf(1.0f * s15[2]) : 0.0f;
        float o[4] = {nf(nf(x - h) * s15[0]), nf(nf(y - h) * s15[1]), z, w};
        memcpy(v->u, o, 16);
    }
    float o2[4] = {nf(mf(1016, 0) + mf(1017, 0)), nf(mf(1016, 1) + mf(1017, 1)), mf(1016, 2), 0.0f};
    memcpy(vu.vf[2].u, o2, 16);
    vu.vi[12] = 0x1c0;
    if (fl & 2) { vu.vi[13] = 0x236; vu.vi[14] = 0x2ac; } else { vu.vi[13] = 0x26d; vu.vi[14] = 0x31a; }
    uint32_t y; memcpy(&y, vu.mem + 16 * 1023 + 4, 4); y = var;                 /* ISW: 16 bits, zero-extended */
    memcpy(vu.mem + 16 * 1023 + 4, &y, 4);
}

static int nat_batch(int setup)
{
    uint32_t var;
    if (setup) var = nat_variant((uint32_t)mi(1023, 0) & 0xFFFF);
    else { if (vu.vi[15] != 0x29) return 0; memcpy(&var, vu.mem + 16 * 1023 + 4, 4); var &= 0xFFFF; }
    if (var != 0x0cb && var != 0x114 && var != 0x166) return 0;
    if (!(mi(973, 2) & 0xFFFF)) return 0;                  /* positions left as integers: not seen, interpret */
    uint32_t top = v_top & 0x3FF, n = v_itop & 0x3FF;
    if (n > 256 || top + 4 * n > 973) return 0;
    if (setup) nat_setup();
    st_nat_v += n;
#ifndef HLE_PSP
    {   /* per object statistics for one frame (env HLE_OBJSTAT=<VIF1 frame>): one line per batch */
        static long at = -2; if (at == -2) { const char *e = getenv("HLE_OBJSTAT"); at = e ? atol(e) : -1; }
        extern uint64_t hle_vif1_frames(void) __attribute__((weak));
        if (at >= 0 && hle_vif1_frames && (long)hle_vif1_frames() == at) {
            uint64_t tex0 = 0;
            LOG("OBJSTAT %d %u var %x pos %.1f %.1f %.1f w %.2f tex %llx\n", setup, n, var, rf(31, 0), rf(31, 1), rf(31, 3), rf(31, 3), (unsigned long long)tex0);
        }
    }
#endif
    st_nat++;
    nb.var = (int)var;
    /* the constants below come from 973..976, 1014, 1018..1022, vf02, vf28..31: while those words equal the previous
       batch's (the same object), its values again (no clamps / conversions) */
    static uint32_t ckey[60]; static int cvalid;
    static float c_uvo[2], c_m19[4], c_lf, c_sc[4], c_M[4][4]; static int c_cm, c_grey; static __typeof__(nb) c_nb;
    {
        uint32_t k_[60];
        memcpy(k_, vu.mem + 16 * 973, 64); memcpy(k_ + 16, vu.mem + 16 * 1014, 16); memcpy(k_ + 20, vu.mem + 16 * 1018, 80);
        memcpy(k_ + 40, vu.vf[2].u, 16); memcpy(k_ + 44, vu.vf[28].u, 64);
        uint32_t d_ = !cvalid || NAT_NOCCACHE; for (int i = 0; i < 60 && !d_; i++) d_ |= k_[i] ^ ckey[i];
        if (d_) {
            memcpy(ckey, k_, sizeof ckey); cvalid = 1;
            for (int i = 0; i < 4; i++) nb.v02[i] = rf(2, i);
            nb.k = mf(1014, 2); nb.fa = mf(1014, 3);
            nb.f22w = nf(nb.fa * nb.k); nb.f27w = nf(255.0f + nb.fa * nb.k);
            nb.sx = mf(1021, 1); nb.sy = mf(1022, 1); nb.zc = nf(0.0f + mf(1021, 2)); nb.zk = mf(1022, 2);
            for (int i = 1; i < 4; i++) memcpy(&nb.tag[i], vu.mem + 16 * 1018 + 4 * i, 4);
            /* constants of the decompression / colour */
            c_uvo[0] = mf(974, 0); c_uvo[1] = mf(974, 1);
            for (int i = 0; i < 4; i++) c_m19[i] = mf(1019, i);
            c_cm = (mi(976, 3) & 0xFFFF) != 0; c_grey = (mi(973, 3) & 0xFFFF) != 0;
            c_lf = mf(973, 0); c_sc[0] = c_sc[1] = c_sc[2] = 0; c_sc[3] = 1.0f;
            { uint32_t ic = 0x3b807fedu; float f; memcpy(&f, &ic, 4); if (c_cm) for (int i = 0; i < 3; i++) c_sc[i] = nf(nf(mf(976, i) * mf(1020, 0)) * f); }
            for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) c_M[r][i] = rf(28 + r, i);
            c_nb = nb;
        } else { nb = c_nb; nb.var = (int)var; }             /* (other programs use nb too) */
    }
    float uvo[2] = {c_uvo[0], c_uvo[1]}, m19[4] = {c_m19[0], c_m19[1], c_m19[2], c_m19[3]};
    int cm = c_cm, grey = c_grey;
    float lf = c_lf, sc[4] = {c_sc[0], c_sc[1], c_sc[2], c_sc[3]};
    float M[4][4];                                          /* vf28..31 (x, y, z, translation), w lanes scaled for 0x114 */
    memcpy(M, c_M, sizeof M);
    int direct = 0, dbl = 0;
#if defined(HLE_PSP) || defined(NAT_SIM)
    {
        float fa_ = 0, fb_ = 0;
        if (var != 0x0cb) { fa_ = nb.k / 255.0f; fb_ = (var == 0x166 ? nb.v02[3] - nb.fa * nb.k : -nb.fa * nb.k) / 255.0f; }
#ifndef NAT_DMASK
#define NAT_DMASK 15
#endif
        if (NAT_DMASK & (var == 0x0cb ? 1 : var == 0x114 ? 2 : 4)) direct = nat_direct_begin(M, var != 0x0cb, fa_, fb_, &dbl);
        if (direct < 0) { pu_flush(); goto end; }          /* not drawn on the GS path either */
#ifdef HLE_PSP
        if (direct && mc_on > 0 && npu) { st_dir_batches++; TPROF(6, mc_mesh(var, top, n, M, uvo, m19, cm, grey, lf, sc, dbl)); goto end; }
#endif
        pu_flush();
        if (direct) { st_dir_batches++; ZN(14, TPROF(6, nat_fast_mesh(var, top, n, M, uvo, m19, cm, grey, lf, sc, dbl, 0))); goto end; }
    }
#endif
    pu_flush();
    if (var == 0x114) for (int r = 0; r < 4; r++) M[r][3] = nf(M[r][3] * nb.k);

    static NV V[256];
    for (uint32_t k = 0; k < n; k++) {
        uint32_t t = top + 4 * k;
        NV *v = &V[k];
        float p[3];
        for (int i = 0; i < 3; i++) v->p[i] = p[i] = nf(nf((float)mi(t, i)) * 0.0078125f);
        for (int i = 0; i < 2; i++) v->uv[i] = nf(nf(nf((float)mi(t + 1, i)) * 0.0078125f) + uvo[i]);
        if (cm) {
            float c2[4], c3[4];
            for (int i = 0; i < 4; i++) { c2[i] = nf((float)mi(t + 2, i)); c3[i] = nf((float)mi(t + 3, i)); }
            if (grey) { float a = nf(12.0f + c2[0]), b = nf(12.0f + c3[0]); for (int i = 0; i < 3; i++) { c2[i] = a; c3[i] = b; } }
            for (int i = 0; i < 3; i++) { float d = nf(c3[i] - c2[i]); d = nf(d * lf); c2[i] = nf(c2[i] + d); }
            c2[3] = nf(c2[3] + c3[3]);
            for (int i = 0; i < 4; i++) { float c = nf(c2[i] * sc[i]); c = vmin(c, 255.0f); v->col[i] = nf(c * m19[i]); }
        } else {
            for (int i = 0; i < 3; i++) v->col[i] = nf(nf((float)mi(t + 2, i)) * m19[i]);
            v->col[3] = nf(255.0f * m19[3]);
        }
        for (int i = 0; i < 4; i++) {
            float acc = nf(M[3][i] * 1.0f);
            if (i != 2) { acc = nf(acc + M[0][i] * p[0]); acc = nf(acc + M[1][i] * p[1]); }
            v->c[i] = nf(acc + M[2][i] * p[2]);
        }
    }

    NO o;
    nat_pkn = 0;
    if (var != 0x166) {
        for (uint32_t k = 0; k < n; k++) { nat_out(&V[k], &o); nat_put(&o); }
        nat_flush();
    } else {
        static uint32_t F[256];
        for (uint32_t k = 0; k < n; k++) F[k] = nat_flags(&V[k]);
        uint32_t run = 0;                                   /* vertices of the current strip already sent */
        for (uint32_t k = 0; k + 2 < n; k++) {
            uint32_t f0 = F[k], f1 = F[k + 1], f2 = F[k + 2];
            st_nat_tri++;
            if (f0 & f1 & f2) { st_nat_drop++; nat_flush(); run = 0; continue; }
            if (f0 | f1 | f2) { nat_clip_tri(&V[k], &V[k + 1], &V[k + 2], f0 | f1 | f2); run = 0; continue; }
            if (run == 0) { nat_out(&V[k], &o); nat_put(&o); nat_out(&V[k + 1], &o); nat_put(&o); }
            nat_out(&V[k + 2], &o); nat_put(&o); run = 1;
        }
        nat_flush();
    }
#if defined(HLE_PSP) || defined(NAT_SIM)
end:
#endif
    /* the program's end state */
    uint16_t t12 = vu.vi[12]; vu.vi[12] = vu.vi[13]; vu.vi[13] = vu.vi[14]; vu.vi[14] = t12;
    vu.vi[15] = 0x29; vu.tpc = 0x424;
    return 1;
}


/* ===================================================================================================== skinned meshes
 * The characters' program (code hash 4858b62a at 0; batches: MSCNT at 0x262 -> jr vi15 = 0x51). Per batch (ITOP
 * vertices of 4 qwords: q0 position float xyz + w raw (0x8000 = ADC), q1 UV float, q2 normal int/128, q3 weights
 * float whose low 16 bits are bone offsets added to 721..724 (bone rows x, y, z, translation)):
 *   skinning: up to 4 bones (724.w/z/y say the loop's maximum; per vertex the highest non-zero index slot), 1 bone
 *             takes the rows unweighted; vertex 0 always uses the loop's maximum
 *   lighting: N' = 1004..1006 x N, m = min(N', 0), colour = 65536 + 995 s1 - sum(996..999 s2 m) (alpha 65536 + 2 s1),
 *             MINI 65536 + 2*1019
 *             (s1 = 1019 1020.x/128, s2 = 1019 1020.z/128); the float's bits go to RGBA as they are
 *   output (setup flags 1023.x: bit 0 fog vi11, bit 1 ADC clip vi12): 0x195 / 0x1bb / 0x1ed / 0x221 */
#define SKIN_HASH 0x4858b62au
#define FOL_HASH 0xc3651f62u       /* the foliage program (nat_fol, PSP / NAT_SIM) */
static inline uint32_t mlo(uint32_t qa, int l) { return (uint32_t)mi(qa, l) & 0xFFFFu; }
static inline void bone_row(float *o, int nbn, const uint32_t *ix, const float *W, int row)
{
    /* rows 721 + idx .. 724 + idx; nbn bones; accumulation order w, z, y, x (as the program) */
    float acc[4];
    if (nbn == 1) { for (int i = 0; i < 4; i++) o[i] = mf(row + ix[0], i); return; }
    int top = nbn - 1;                                      /* slot index: 0 x, 1 y, 2 z, 3 w */
    for (int i = 0; i < 4; i++) acc[i] = nf(mf(row + ix[top], i) * W[top]);
    for (int s2 = top - 1; s2 >= 1; s2--) for (int i = 0; i < 4; i++) acc[i] = nf(acc[i] + mf(row + ix[s2], i) * W[s2]);
    for (int i = 0; i < 4; i++) o[i] = nf(acc[i] + mf(row + ix[0], i) * W[0]);
}
#if defined(HLE_PSP) || defined(NAT_SIM)
/* the skinned direct path in plain float */
/* skin vertex rows cache (skin_rows_cached, PSP): rows 0..2 of each vertex of this batch here, 64 bytes apart (row 3
   stays in VU memory); anything else than skin_fast puts them in VU memory first (skc_materialize) */
static const float *skc_rows; static uint32_t skc_rows_top, skc_rows_n;
static void skc_materialize(void)
{
    if (!skc_rows) return;
    for (uint32_t k = 0; k < skc_rows_n; k++) memcpy(vu.mem + 16 * (skc_rows_top + 4 * k), skc_rows + 16 * k, 48);
    skc_rows = 0;
}
static void skin_fast(uint32_t top, uint32_t n, int maxb, const float *L20, float L2x[4][4], const float *cl, float LD[3][4], int fog, int dbl)
{
    static HGV Gs[256]; static uint8_t adc[256], bad[256];
    HGV *G = (HGV *)hle_ge_mesh_alloc((int)n);              /* straight into the display list */
    if (!G) G = Gs;
    const int inlist = G != Gs;
    st_skin_v += n;
    const float *mem = (const float *)vu.mem;
    /* constants in locals (the stores to G would otherwise force reloads); only the x / y / z lanes of the bone
       rows are needed */
    float ld[3][4], l20[4], l2x[4][4], clm[4];
    for (int j = 0; j < 3; j++) for (int i = 0; i < 4; i++) ld[j][i] = LD[j][i];
    for (int i = 0; i < 4; i++) { l20[i] = L20[i]; clm[i] = cl[i]; for (int j = 0; j < 4; j++) l2x[j][i] = L2x[j][i]; }
#if defined(HLE_PSP) && !defined(NAT_NOVFPU)
    {
        /* VFPU: M500 = the blended bone (columns: rows 721 x, 722 y, 723 z, 724 translation), E500 its transpose for
           the transforms; M600 columns = light directions 1004-1006 (D = E600 N), M700 columns = the 4 light colours
           (sum = E700 D); C200 base colour, C210 the clamp, C220 = 128 x the GE doubling */
        float __attribute__((aligned(16))) kl[4][4], kc[4][4], kb[3][4], kq[5][4];
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) kc[j][i] = l2x[j][i];
        for (int j = 0; j < 3; j++) for (int i = 0; i < 4; i++) kl[j][i] = ld[j][i];
        kl[3][0] = kl[3][1] = kl[3][2] = kl[3][3] = 0;
        for (int i = 0; i < 4; i++) { kb[0][i] = l20[i]; kb[1][i] = clm[i]; kb[2][i] = 128.0f * ((i == 3 || dbl) ? 2.0f : 1.0f); }
        /* colour constants: C300 65536, C310 the GE doubling (2 / 1), C320 0.5, C330 255, C400 128 */
        for (int i = 0; i < 4; i++) { kq[0][i] = 65536.0f; kq[1][i] = (i == 3 || dbl) ? 2.0f : 1.0f; kq[2][i] = 0.5f - 8388608.0f; kq[3][i] = 255.0f; kq[4][i] = 128.0f; }
        __asm__ __volatile__(
            "lv.q C600, 0(%0)\n lv.q C610, 16(%0)\n lv.q C620, 32(%0)\n lv.q C630, 48(%0)\n"
            "lv.q C700, 0(%1)\n lv.q C710, 16(%1)\n lv.q C720, 32(%1)\n lv.q C730, 48(%1)\n"
            "lv.q C200, 0(%2)\n lv.q C210, 16(%2)\n lv.q C220, 32(%2)\n"
            "lv.q C300, 0(%3)\n lv.q C310, 16(%3)\n lv.q C320, 32(%3)\n lv.q C330, 48(%3)\n lv.q C400, 64(%3)\n"
            : : "r"(kl), "r"(kc), "r"(kb), "r"(kq) : "memory");
        uint32_t lastb = ~0u;                               /* M500 = the single bone at this row already */
        for (uint32_t kk = 0; kk < n; kk++) {
            uint32_t t = top + 4 * kk;
            const float *q3 = mem + 4 * t + 12; const float *q = skc_rows ? skc_rows + 16 * kk : mem + 4 * t; const uint32_t *qi = (const uint32_t *)q; const uint32_t *qi3 = (const uint32_t *)q3;
            uint32_t ix[4] = {qi3[0] & 0xFFFF, qi3[1] & 0xFFFF, qi3[2] & 0xFFFF, qi3[3] & 0xFFFF};
            int nbn = kk == 0 ? maxb : (maxb >= 4 && ix[3]) ? 4 : (maxb >= 3 && ix[2]) ? 3 : (maxb >= 2 && ix[1]) ? 2 : 1;
            if (nbn == 1 && ((721u + ix[0]) & 0x3FF) == lastb) goto same_bone;
            lastb = nbn == 1 ? (721u + ix[0]) & 0x3FF : ~0u;
            for (int s2 = 0; s2 < nbn; s2++) {
                uint32_t a = (721u + ix[s2]) & 0x3FF;
                union { float f; uint32_t u; } wu; wu.f = nbn == 1 ? 1.0f : q3[s2];
                float w = wu.f;
                if (s2 && w == 0) continue;                 /* 0 x a saturated row is 0 on the VU (not NaN) */
                if (a + 3 > 0x3FF) { a = 0x3FF - 3; lastb = ~0u; }   /* (never seen: bone rows wrapping VU memory) */
                const float *b = mem + 4 * a;
                if (s2 == 0)
                    __asm__ __volatile__("lv.q C500, 0(%0)\n lv.q C510, 16(%0)\n lv.q C520, 32(%0)\n lv.q C530, 48(%0)\n mtv %1, S000\n vmscl.q M500, M500, S000\n"
                                         : : "r"(b), "r"(wu.u) : "memory");
                else
                    __asm__ __volatile__("lv.q C100, 0(%0)\n lv.q C110, 16(%0)\n lv.q C120, 32(%0)\n lv.q C130, 48(%0)\n mtv %1, S000\n vmscl.q M100, M100, S000\n"
                                         "vadd.q C500, C500, C100\n vadd.q C510, C510, C110\n vadd.q C520, C520, C120\n vadd.q C530, C530, C130\n"
                                         : : "r"(b), "r"(wu.u) : "memory");
            }
          same_bone:;
            HGV *g = &G[kk];
            uint32_t cc, vcc;
            /* colour: the low byte of the float's bits = (a - 65536) x 128 (rounded, truncated), then the GE doubling
               min(2 v, 255); packed by vi2uc (bits 23..30 of each lane) */
            __asm__ __volatile__(
                "lv.q C010, 0(%2)\n vone.s S013\n"            /* position, w = 1 */
                "lv.q C020, 32(%2)\n vi2f.t C020, C020, 7\n vzero.s S023\n"   /* normal / 128 */
                "vtfm4.q C030, E500, C010\n"                    /* skinned position */
                "vtfm3.t C110, E500, C020\n"                    /* skinned normal (no translation) */
                "vzero.s S113\n vtfm4.q C120, E600, C110\n"    /* D = light dirs . N */
                "vzero.q C130\n vmin.q C120, C120, C130\n"      /* min(D, 0) */
                "vtfm4.q C130, E700, C120\n vsub.q C130, C200, C130\n vmin.q C130, C130, C210\n"
                "sv.s S030, 12(%3)\n sv.s S031, 16(%3)\n sv.s S032, 20(%3)\n"   /* the position straight into the vertex */
                "vsub.t C110, C030, C030\n vcmp.t EZ, C110, C110\n mfvc %1, $131\n"   /* finite: x - x = 0 (CC bit 5: all) */
                /* colour: (a x 128 + (0.5 - 2^23)) = (a - 65536) x 128 + 0.5 exactly, truncated, doubled (1 / 2), then
                   x 2^23 with saturation: vi2uc gives min(v, 255), 0 for negatives */
                "vmul.q C130, C130, C400\n vadd.q C130, C130, C320\n vf2iz.q C130, C130, 0\n vi2f.q C130, C130, 0\n"
                "vmul.q C130, C130, C310\n vf2iz.q C130, C130, 23\n vi2uc.q S130, C130\n mfv %0, S130\n"
                : "=&r"(cc), "=&r"(vcc) : "r"(q), "r"(g) : "memory");
            g->c = cc; g->u = q[4]; g->v = q[5];
            adc[kk] = fog ? (qi[3] & 0xFFFF) != 0 : (qi[3] >> 15) & 1;
            bad[kk] = !(vcc & 0x20);
        }
    }
    if (0)
#endif
    for (uint32_t kk = 0; kk < n; kk++) {
        uint32_t t = top + 4 * kk;
        const float *q3 = mem + 4 * t + 12; const float *q = skc_rows ? skc_rows + 16 * kk : mem + 4 * t; const uint32_t *qi = (const uint32_t *)q; const uint32_t *qi3 = (const uint32_t *)q3;
        uint32_t ix[4] = {qi3[0] & 0xFFFF, qi3[1] & 0xFFFF, qi3[2] & 0xFFFF, qi3[3] & 0xFFFF};
        int nbn = kk == 0 ? maxb : (maxb >= 4 && ix[3]) ? 4 : (maxb >= 3 && ix[2]) ? 3 : (maxb >= 2 && ix[1]) ? 2 : 1;
        float R[4][3];                                       /* rows 721 (x), 722 (y), 723 (z), 724 (translation) */
        {
            uint32_t a0 = (721u + ix[0]) & 0x3FF;
            if (a0 + 3 <= 0x3FF) { const float *b = mem + 4 * a0; for (int row = 0; row < 4; row++) { R[row][0] = b[4 * row]; R[row][1] = b[4 * row + 1]; R[row][2] = b[4 * row + 2]; } }
            else for (int row = 0; row < 4; row++) { const float *b = mem + 4 * ((a0 + (uint32_t)row) & 0x3FF); R[row][0] = b[0]; R[row][1] = b[1]; R[row][2] = b[2]; }
        }
        if (nbn > 1) {
            float w0 = q3[0];
            for (int row = 0; row < 4; row++) { R[row][0] *= w0; R[row][1] *= w0; R[row][2] *= w0; }
            for (int s2 = 1; s2 < nbn; s2++) {
                float w = q3[s2];
                if (w == 0) continue;                       /* 0 x a saturated row is 0 on the VU (not NaN) */
                uint32_t a = (721u + ix[s2]) & 0x3FF;
                for (int row = 0; row < 4; row++) {
                    const float *b = mem + 4 * ((a + (uint32_t)row) & 0x3FF);
                    R[row][0] += b[0] * w; R[row][1] += b[1] * w; R[row][2] += b[2] * w;
                }
            }
        }
        float px = q[0], py = q[1], pz = q[2];
        float nx = (float)(int32_t)qi[8] * (1.0f / 128.0f), ny = (float)(int32_t)qi[9] * (1.0f / 128.0f), nz = (float)(int32_t)qi[10] * (1.0f / 128.0f);
        float gx = R[3][0] + R[0][0] * px + R[1][0] * py + R[2][0] * pz;
        float gy = R[3][1] + R[0][1] * px + R[1][1] * py + R[2][1] * pz;
        float gz = R[3][2] + R[0][2] * px + R[1][2] * py + R[2][2] * pz;
        float N0 = R[0][0] * nx + R[1][0] * ny + R[2][0] * nz, N1 = R[0][1] * nx + R[1][1] * ny + R[2][1] * nz, N2 = R[0][2] * nx + R[1][2] * ny + R[2][2] * nz;
        float D[4];
        for (int i = 0; i < 4; i++) { float d = ld[0][i] * N0 + ld[1][i] * N1 + ld[2][i] * N2; D[i] = d < 0 ? d : 0; }
        uint32_t cc = 0;
        for (int i = 0; i < 4; i++) {
            float a = l20[i] - l2x[0][i] * D[0] - l2x[1][i] * D[1] - l2x[2][i] * D[2] - l2x[3][i] * D[3];
            if (a > clm[i]) a = clm[i];
            int v = (int)((a - 65536.0f) * 128.0f + 0.5f) & 0xFF;       /* the low byte of the float's bits */
            if (i == 3 || dbl) v = v >= 0x80 ? 0xFF : v * 2;
            cc |= (uint32_t)v << (8 * i);
        }
        HGV *g = &G[kk];
        g->x = gx; g->y = gy; g->z = gz; g->c = cc; g->u = q[4]; g->v = q[5];
        adc[kk] = fog ? (qi[3] & 0xFFFF) != 0 : (qi[3] >> 15) & 1;
        /* a position that is not a finite number (the restart vertices of some meshes: the VU saturates it and the
           vertex lands on a vanishing point): no triangle with it */
        bad[kk] = !((gx - gx) == 0 && (gy - gy) == 0 && (gz - gz) == 0);
    }
    for (uint32_t kk = 2; kk < n; kk++) if (bad[kk] || bad[kk - 1] || bad[kk - 2]) adc[kk] = 1;
    {   /* GS state after the batch: its last two vertices */
        float C[2][4], uv[2][2]; uint32_t rgba[2][4]; int nt = n >= 2 ? 2 : (int)n;
        for (int i = 0; i < nt; i++) {
            const HGV *g = &G[n - nt + i];
            for (int c = 0; c < 4; c++) C[i][c] = rf(31, c) + rf(28, c) * g->x + rf(29, c) * g->y + rf(30, c) * g->z;
            C[i][2] = rf(31, 2);
            uv[i][0] = g->u; uv[i][1] = g->v;
            for (int c = 0; c < 4; c++) { uint32_t b = (g->c >> (8 * c)) & 0xFF; rgba[i][c] = (c == 3 || dbl) ? (b >= 0xFF ? 0x80 : b / 2) : b; }
        }
        nat_tail(nt, C, uv, rgba, fog, mf(1014, 2), -mf(1014, 3) * mf(1014, 2));
    }
    int run = -1;                                           /* strips between the triangles the ADC bits skip */
    for (uint32_t kk = 2; kk < n; kk++) {
        if (!adc[kk]) { if (run < 0) run = (int)kk - 2; continue; }
        if (run >= 0) { if (inlist) hle_ge_mesh_draw_at(GE_STRIP, &G[run], (int)kk - run); else hle_ge_mesh_draw(GE_STRIP, &G[run], (int)kk - run); run = -1; }
    }
    if (run >= 0) { if (inlist) hle_ge_mesh_draw_at(GE_STRIP, &G[run], (int)n - run); else hle_ge_mesh_draw(GE_STRIP, &G[run], (int)n - run); }
    if (inlist) hle_ge_mesh_end();
}
#endif
/* the skinned program's object setup (MSCAL at 0, 0x000..0x050): unless 994.w says it is done already, vf28..31 =
 * 1008..1011 (z lanes halved) x 991..994, x / y re-centred by z and scaled by 1015, vf02 = 1016 + 1017.xy; the light
 * matrix 1004..1006 = (991..993 . 1000..1003) / |991| (Q also to 993.w); 994.w = 1; buffers 0x180 / 0x22a; flags */
static void skin_setup(void)
{
    if (!(mlo(994, 3))) {
        float P[4][4];                                      /* vf07..10 */
        for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) P[r][i] = mf(1008 + r, i);
        for (int r = 0; r < 4; r++) P[r][2] = nf(P[r][2] * 0.5f);
        float A[4][3];                                      /* vf03..06 = 991..994 xyz */
        for (int r = 0; r < 4; r++) for (int i = 0; i < 3; i++) A[r][i] = mf(991 + r, i);
        float O[4][4];
        for (int r = 0; r < 4; r++)
            for (int i = 0; i < 4; i++) {
                float a = nf(P[0][i] * A[r][0]); a = nf(a + P[1][i] * A[r][1]); a = nf(a + P[2][i] * A[r][2]);
                if (r == 3) a = nf(a + P[3][i] * 1.0f);
                O[r][i] = a;
            }
        for (int r = 0; r < 4; r++) { O[r][0] = nf(O[r][0] - O[r][2]); O[r][1] = nf(O[r][1] - O[r][2]); }
        float s15[3] = {mf(1015, 0), mf(1015, 1), mf(1015, 2)};
        for (int r = 0; r < 4; r++) { O[r][2] = r == 3 ? nf(1.0f * s15[2]) : 0.0f; O[r][0] = nf(O[r][0] * s15[0]); O[r][1] = nf(O[r][1] * s15[1]); }
        for (int r = 0; r < 4; r++) memcpy(vu.vf[28 + r].u, O[r], 16);
        float o2[4] = {nf(mf(1016, 0) + mf(1017, 0)), nf(mf(1016, 1) + mf(1017, 1)), mf(1016, 2), 0.0f};
        memcpy(vu.vf[2].u, o2, 16);
        /* light matrix */
        float sq[3]; for (int i = 0; i < 3; i++) sq[i] = nf(A[0][i] * A[0][i]);
        float len2 = nf(nf(sq[0] + sq[2]) + 1.0f * sq[1]);
        float q = len2 > 0 ? nf(1.0f / sqrtf(len2)) : FLT_MAX;
        float Lr[4][3]; for (int j = 0; j < 4; j++) for (int i = 0; i < 3; i++) Lr[j][i] = mf(1000 + j, i);
        for (int r = 0; r < 3; r++) {
            float pr[4][3];
            for (int j = 0; j < 4; j++) for (int i = 0; i < 3; i++) pr[j][i] = nf(A[r][i] * Lr[j][i]);
            float o[4];
            o[0] = nf(nf(pr[0][0] + pr[0][2]) + 1.0f * pr[0][1]);
            o[1] = nf(nf(pr[1][1] + pr[1][2]) + 1.0f * pr[1][0]);
            o[2] = nf(nf(pr[2][2] + pr[2][0]) + 1.0f * pr[2][1]);
            o[3] = nf(nf(nf(1.0f * pr[3][0]) + 1.0f * pr[3][1]) + 1.0f * pr[3][2]);
            for (int i = 0; i < 4; i++) o[i] = nf(o[i] * q);
            memcpy(vu.mem + 16 * (1004 + r), o, 16);
        }
        memcpy(vu.mem + 16 * 993 + 12, &q, 4);
        uint32_t one = 1; memcpy(vu.mem + 16 * 994 + 12, &one, 4);
        uint32_t fl = mlo(1023, 0);
        vu.vi[12] = (uint16_t)(fl & 2); vu.vi[11] = (uint16_t)(fl & 1);
    }
    vu.vi[13] = 0x180; vu.vi[14] = 0x22a; vu.vi[15] = 0x51;
}
static int skin_batch(void)
{
    if (vu.vi[15] != 0x51) return 0;
    uint32_t top = v_top & 0x3FF, n = v_itop & 0x3FF;
    if (n > 256 || top + 4 * n > 721) return 0;
    int fog = vu.vi[11] != 0, clip = vu.vi[12] != 0;
    nb.var = 0x1000 | (fog ? 1 : 0) | (clip ? 2 : 0);
    st_nat++; st_nat_v += n;
    int maxb = mlo(724, 3) ? 4 : mlo(724, 2) ? 3 : mlo(724, 1) ? 2 : 1;
    for (int i = 1; i < 4; i++) memcpy(&nb.tag[i], vu.mem + 16 * 1018 + 4 * i, 4);
    /* lighting constants */
    /* (the same words as the batch before - the same object: its constants again) */
    static float L20[4], L2x[4][4], cl[4], LD[3][4]; static uint8_t lkey[14 * 16]; static int lvalid;
    if (!lvalid || memcmp(lkey, vu.mem + 16 * 995, 12 * 16) || memcmp(lkey + 12 * 16, vu.mem + 16 * 1019, 2 * 16)) {
    memcpy(lkey, vu.mem + 16 * 995, 12 * 16); memcpy(lkey + 12 * 16, vu.mem + 16 * 1019, 2 * 16); lvalid = 1;
    float s18x = nf(mf(1020, 0) * 0.0078125f), s18z = nf(mf(1020, 2) * 0.0078125f);
    { uint32_t u = 0x3ffffff8u; float i2; memcpy(&i2, &u, 4);
      for (int i = 0; i < 4; i++) {
          float m19 = mf(1019, i), v16 = nf(m19 * s18x), v17 = nf(m19 * s18z);
          cl[i] = nf(65536.0f + m19 * i2);
          L20[i] = nf(65536.0f + (i < 3 ? mf(995, i) : 2.0f) * v16);    /* w: vf00 + vf00.w = 2 */
          for (int j = 0; j < 4; j++) L2x[j][i] = nf((i < 3 ? mf(996 + j, i) : 0.0f) * v17);       /* w: vf00 - vf00.w = 0 */
      } }
    for (int j = 0; j < 3; j++) for (int i = 0; i < 4; i++) LD[j][i] = mf(1004 + j, i);
    }
    /* projection constants */
    float M[4][4]; for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) M[r][i] = rf(28 + r, i);
    float v02[4]; for (int i = 0; i < 4; i++) nb.v02[i] = v02[i] = rf(2, i);   /* nb.v02: for nat_direct_begin */
    float k = mf(1014, 2), fa = mf(1014, 3);
    float f22w = nf(fa * k), f27w = nf(255.0f + fa * k);
    float sxy[2] = {mf(1021, 1), mf(1022, 1)}, zc = mf(1021, 2), zk = mf(1022, 2), rx = mf(1021, 0), ry = mf(1022, 0);
    uint32_t hist = vu.clip;
    NO o; nat_pkn = 0;
    int direct = 0, dbl = 0;
#if defined(HLE_PSP) || defined(NAT_SIM)
    if (!clip) {
        if (NAT_DMASK & 8) direct = nat_direct_begin(M, fog, k / 255.0f, -(fa * k) / 255.0f, &dbl);
        if (direct < 0) goto end;
        if (direct) { st_dir_batches++; ZN(7, TPROF(7, skin_fast(top, n, maxb, L20, L2x, cl, LD, fog, dbl))); skc_rows = 0; goto end; }
    }
#endif
#if defined(HLE_PSP) || defined(NAT_SIM)
    skc_materialize();                                       /* the loop below reads the rows from VU memory */
#endif
    for (uint32_t kk = 0; kk < n; kk++) {
        uint32_t t = top + 4 * kk;
        /* skinning */
        int nbn;
        uint32_t ix[4] = {mlo(t + 3, 0), mlo(t + 3, 1), mlo(t + 3, 2), mlo(t + 3, 3)};
        if (kk == 0) nbn = maxb;
        else nbn = (maxb >= 4 && ix[3]) ? 4 : (maxb >= 3 && ix[2]) ? 3 : (maxb >= 2 && ix[1]) ? 2 : 1;
        float W[4] = {mf(t + 3, 0), mf(t + 3, 1), mf(t + 3, 2), mf(t + 3, 3)};
        float R0[4], R1[4], R2[4], R3[4];                   /* 724 (translation), 721, 722, 723 */
        bone_row(R0, nbn, ix, W, 724); bone_row(R1, nbn, ix, W, 721); bone_row(R2, nbn, ix, W, 722); bone_row(R3, nbn, ix, W, 723);
        float p[3] = {mf(t, 0), mf(t, 1), mf(t, 2)}, nn[3], P[4], N[4];
        for (int i = 0; i < 3; i++) nn[i] = nf(nf((float)mi(t + 2, i)) * 0.0078125f);
        for (int i = 0; i < 4; i++) {
            float a = nf(R0[i] * 1.0f); a = nf(a + R1[i] * p[0]); a = nf(a + R2[i] * p[1]); P[i] = nf(a + R3[i] * p[2]);
            float b = nf(R1[i] * nn[0]); b = nf(b + R2[i] * nn[1]); N[i] = nf(b + R3[i] * nn[2]);
        }
        /* lighting */
        float D[4], col[4];
        for (int i = 0; i < 4; i++) { float a = nf(LD[0][i] * N[0]); a = nf(a + LD[1][i] * N[1]); D[i] = vmin(nf(a + LD[2][i] * N[2]), 0.0f); }
        for (int i = 0; i < 4; i++) {
            float a = nf(L20[i] * 1.0f);
            for (int j = 0; j < 4; j++) a = nf(a - L2x[j][i] * D[j]);
            col[i] = vmin(a, cl[i]);
        }
        memcpy(o.q[1], col, 16);
        /* projection */
        uint32_t w0; memcpy(&w0, vmem(t) + 12, 4);           /* q0.w as uploaded */
        float uv[2] = {mf(t + 1, 0), mf(t + 1, 1)}, q, X[4];
        int32_t xi[4];
        if (!fog && !clip) {
            float C[4];
            for (int i = 0; i < 4; i++) { float a = nf(M[3][i] * 1.0f); if (i != 2) { a = nf(a + M[0][i] * P[0]); a = nf(a + M[1][i] * P[1]); } C[i] = nf(a + M[2][i] * P[2]); }
            q = nf(1.0f / C[3]);
            for (int i = 0; i < 3; i++) xi[i] = ftoi_s(nf(v02[i] + C[i] * q), 16.0f);
            memcpy(&xi[3], &w0, 4);
        } else if (!fog) {
            float Cl[4], C3[4];                             /* columns with the z lanes for the clip test */
            float col0[4] = {M[3][0], M[3][1], nf(nf(0.0f + zc) + zk * M[3][3]), M[3][3]};
            float colr[3][4];
            for (int r = 0; r < 3; r++) { colr[r][0] = M[r][0]; colr[r][1] = M[r][1]; colr[r][2] = nf(zk * M[r][3]); colr[r][3] = M[r][3]; }
            for (int i = 0; i < 4; i++) { float a = nf(col0[i] * 1.0f); a = nf(a + colr[0][i] * P[0]); a = nf(a + colr[1][i] * P[1]); C3[i] = nf(a + colr[2][i] * P[2]); }
            float sc4[4] = {sxy[0], sxy[1], 1.0f, 1.0f};
            for (int i = 0; i < 4; i++) Cl[i] = nf(C3[i] * sc4[i]);
            float l = fabsf(Cl[3]); uint32_t f = 0;
            for (int i = 0; i < 3; i++) { if (Cl[i] > l) f |= 1u << (2 * i); if (Cl[i] < -l) f |= 2u << (2 * i); }
            hist = ((hist << 6) | f) & 0xFFFFFFu;
            q = nf(1.0f / C3[3]);
            float zz[3] = {C3[0], C3[1], M[3][2]};
            for (int i = 0; i < 3; i++) xi[i] = ftoi_s(nf(v02[i] + zz[i] * q), 16.0f);
            if (hist & 0x3FFFFu) xi[3] = 0x8000; else memcpy(&xi[3], &w0, 4);
        } else if (!clip) {
            float C[4], Mw[4] = {nf(M[3][3] * k), nf(M[0][3] * k), nf(M[1][3] * k), nf(M[2][3] * k)};
            for (int i = 0; i < 4; i++) {
                float c3 = i == 3 ? Mw[0] : M[3][i], c0 = i == 3 ? Mw[1] : M[0][i], c1 = i == 3 ? Mw[2] : M[1][i], c2 = i == 3 ? Mw[3] : M[2][i];
                float a = nf(c3 * 1.0f); if (i != 2) { a = nf(a + c0 * P[0]); a = nf(a + c1 * P[1]); } C[i] = nf(a + c2 * P[2]);
            }
            q = nf(k / C[3]);
            float w = vmin(vmax(C[3], f22w), f27w);
            int adc = (w0 & 0xFFFF) != 0;
            for (int i = 0; i < 3; i++) X[i] = nf(nf(C[i] * q) - nf(0.0f - v02[i]));
            X[3] = nf(w - (adc ? nf(f22w - 2048.5f) : f22w));
            for (int i = 0; i < 4; i++) xi[i] = ftoi_s(X[i], 16.0f);
        } else {
            float col0[4] = {nf(M[3][0] * sxy[0]), nf(M[3][1] * sxy[1]), nf(nf(0.0f + zc) + zk * M[3][3]), nf(M[3][3] * 1.0f)};
            float colr[3][4];
            for (int r = 0; r < 3; r++) { colr[r][0] = nf(M[r][0] * sxy[0]); colr[r][1] = nf(M[r][1] * sxy[1]); colr[r][2] = nf(zk * M[r][3]); colr[r][3] = nf(M[r][3] * 1.0f); }
            float Pc[4];
            for (int i = 0; i < 4; i++) { float a = nf(col0[i] * 1.0f); a = nf(a + colr[0][i] * P[0]); a = nf(a + colr[1][i] * P[1]); Pc[i] = nf(a + colr[2][i] * P[2]); }
            float l = fabsf(Pc[3]); uint32_t f = 0;
            for (int i = 0; i < 3; i++) { if (Pc[i] > l) f |= 1u << (2 * i); if (Pc[i] < -l) f |= 2u << (2 * i); }
            hist = ((hist << 6) | f) & 0xFFFFFFu;
            q = nf(1.0f / Pc[3]);
            float v3[3] = {nf(Pc[0] * rx), nf(Pc[1] * ry), M[3][2]};
            float w = nf(Pc[3] * k);
            w = vmin(vmax(w, f22w), f27w);
            int adc = (hist & 0x3FFFFu) || (w0 & 0xFFFF);
            for (int i = 0; i < 3; i++) X[i] = nf(nf(v3[i] * q) - nf(0.0f - v02[i]));
            X[3] = nf(w - (adc ? nf(f22w - 2048.5f) : f22w));
            for (int i = 0; i < 4; i++) xi[i] = ftoi_s(X[i], 16.0f);
        }
        float stq[3] = {nf(uv[0] * q), nf(uv[1] * q), q};
        uint32_t z0 = 0; memcpy(o.q[0], stq, 12); memcpy(o.q[0] + 12, &z0, 4);
        memcpy(o.q[2], xi, 16);
        nat_put(&o);
    }
    nat_flush();
    if (clip) vu.clip = hist;
#if defined(HLE_PSP) || defined(NAT_SIM)
end:
#endif
    ;
    uint16_t t13 = vu.vi[13]; vu.vi[13] = vu.vi[14]; vu.vi[14] = t13;
    vu.vi[15] = 0x51; vu.tpc = 0x262;
    return 1;
}

static int skin_try(uint32_t pc)
{
    if (pc == 0) {
        uint32_t top = v_top & 0x3FF, n = v_itop & 0x3FF;     /* the batch's checks first: all or nothing */
        if (n > 256 || top + 4 * n > 721) return 0;
        skin_setup();
    }
    return skin_batch();
}
#if defined(HLE_PSP) && !defined(NAT_SIM)
/* skin vertex rows: the unpacked rows of a skin batch whose vertex data comes from stable RAM addresses (the four
   row unpacks: stride 4, rows 0..3 of each vertex) are kept; the same sources again (addresses, sizes, codes, 3
   sampled words per unpack) take the rows from here instead of unpacking, re-unpacked at least every 8 frames.
   The other unpacks of the batch (bone matrices, constants) run as usual. */
#define SKC_POOL (2048u * 1024u)
#define SKC_N 2048
static uint8_t *skc_pool; static uint64_t skc_head;           /* bytes ever allocated (ring position) */
static struct { uint32_t q, n, fr, size; uint64_t pos; } skc[SKC_N];
static uint64_t st_skc_hit, st_skc_fill, st_skr[6];
static int skin_rows_cached(void)                              /* 1: the vertex rows are in VU memory, PUs consumed */
{
    static int on = -1;
    if (on < 0) { const char *e = hle_cfg_get("x_skincache"); on = e ? atoi(e) : 1; if (on) { extern void *hle_ge_gs_alloc(size_t, const char *); skc_pool = (uint8_t *)hle_ge_gs_alloc(SKC_POOL, "skin_rows"); if (!skc_pool) on = 0; }
                  LOG("skin vertex rows cache: %s (%u KB)\n", on ? "on" : "off", on ? SKC_POOL >> 10 : 0); }
    if (!on) return 0;
    if (!npu) { st_skr[0]++; return 0; }
    if (vu.vi[15] != 0x51) { st_skr[1]++; return 0; }
    uint32_t top = v_top & 0x3FF, nv = v_itop & 0x3FF;
    if (!nv || nv > 256) { st_skr[2]++; return 0; }
    int vix[4] = {-1, -1, -1, -1}; uint32_t N = 0;
    for (int i = 0; i < npu; i++) {
        const PU *u = &pu[i];
        uint32_t dst = (u->imm & 0x3FF) + u->tops, num = u->num ? u->num : 256;
        if (dst + 4 * num <= top || dst >= top + 4 * nv) continue;             /* not the vertex rows */
        uint32_t rel = dst - top;
        if (dst < top || rel >= 4 || u->wl != 1 || u->cl != 4 || !u->nw || vix[rel] >= 0 || (N && num != N)) { st_skr[3]++; return 0; }
        if (!u->stable) { st_skr[4]++; return 0; }
        vix[rel] = i; N = num;
    }
    uint32_t rmask = (vix[0] >= 0) | (vix[1] >= 0) << 1 | (vix[2] >= 0) << 2 | (vix[3] >= 0) << 3;
    if (!rmask || N < nv || top + 4 * N > 0x400) {
        st_skr[5]++;
        static int nl; if (nl < 6) { nl++;
            LOG("skin rows layout: top %u nv %u npu %d\n", top, nv, npu);
            for (int i = 0; i < npu; i++) LOG("  pu %d: cmd %02x imm %04x dst %u num %u cl %u wl %u nw %u stable %u\n", i, pu[i].cmd, pu[i].imm, (pu[i].imm & 0x3FF) + pu[i].tops, pu[i].num, pu[i].cl, pu[i].wl, pu[i].nw, pu[i].stable); }
        return 0;
    }
    uint32_t q = 0x811C9DC5u ^ N ^ rmask << 12;
    for (int r = 0; r < 4; r++) {
        if (vix[r] < 0) continue;
        const PU *u = &pu[vix[r]];
        q = (q ^ (uint32_t)(uintptr_t)u->src) * 16777619u; q = (q ^ u->nw ^ u->cmd << 8 ^ (u->imm & 0xFC00) << 16) * 16777619u;
        q = (q ^ u->src[0]) * 16777619u; q = (q ^ u->src[u->nw - 1]) * 16777619u; q = (q ^ u->src[u->nw >> 1]) * 16777619u;
    }
    uint32_t size = 64 * N, fr = mc_frame(), ei = (q ^ (q >> 15)) & (SKC_N - 1);
    int hit = skc[ei].q == q && skc[ei].n == N && fr - skc[ei].fr < 8 && skc_head - skc[ei].pos <= SKC_POOL - size;
    /* the other unpacks run (in order); the vertex row ones only on a miss */
    uint32_t cl = v_cl, wl = v_wl, tops = v_tops;
    for (int i = 0; i < npu; i++) {
        if (hit && (i == vix[0] || i == vix[1] || i == vix[2] || i == vix[3])) continue;
        v_cl = pu[i].cl; v_wl = pu[i].wl; v_tops = pu[i].tops; unpack(pu[i].cmd, pu[i].imm, pu[i].num, pu[i].src, pu[i].nw);
    }
    v_cl = cl; v_wl = wl; v_tops = tops; npu = 0;
    if (hit && rmask == 7) {                                    /* rows 0..2: read from here (skin_fast); row 3 stays */
        skc_rows = (const float *)(skc_pool + (skc[ei].pos % SKC_POOL)); skc_rows_top = top; skc_rows_n = N; st_skc_hit++; return 1;
    }
    if (hit) {                                                  /* only the rows these unpacks write */
        const uint8_t *src = skc_pool + (skc[ei].pos % SKC_POOL);
        if (rmask == 15) memcpy(vu.mem + 16 * top, src, size);
        else for (uint32_t k = 0; k < N; k++) for (int r = 0; r < 4; r++) if (rmask >> r & 1) memcpy(vu.mem + 16 * (top + 4 * k + r), src + 64 * k + 16 * r, 16);
        st_skc_hit++; return 1;
    }
    uint32_t off = (uint32_t)(skc_head % SKC_POOL);
    if (off + size > SKC_POOL) { skc_head += SKC_POOL - off; off = 0; }               /* no wrap inside an entry */
    memcpy(skc_pool + off, vu.mem + 16 * top, size);
    skc[ei].q = q; skc[ei].n = N; skc[ei].fr = fr; skc[ei].size = size; skc[ei].pos = skc_head; skc_head += (size + 63) & ~63u;
    st_skc_fill++;
    return 1;
}
#else
static int skin_rows_cached(void) { return 0; }
#endif
#if defined(HLE_PSP) || defined(NAT_SIM)
/* ===================================================================================================== foliage
 * The tree foliage program (code hash c3651f62 at 0; MSCAL 0 = setup + one batch, MSCNT at 0x555 = jr vi15 = 0x27 =
 * one more batch with the same setup). Worked out from the disassembly (work/vu1/progs):
 *   setup (1023.x flags 10 / 14 / 15): vf28..31 = 1008..1011 (z 0, vf31.z 1), x / y minus w / 2, times 1015 (vf31.z
 *         too); vf02 = 1016.xyz + 1017.xy, w 0; vi13 / vi14 = 0x228 / 0x2fc; 1023.y = 0x260 (flags 10) or 0x3db
 *   batch (TOP: ITOP vertices of 4 qwords: q0 position float, q1 UV float, q2 colour int, q3 normal int):
 *         1020.w (vertex blend) 0 only here; op list at 980.w: 0 -> colour = q2 * 1019;
 *         else q3 = normal / 128, q2 as float, then ops 2 (q2.xyz += 980+1 .xyz * 1020.x), 1 (light: direction
 *         976..978 x op.dir, times -979.w; q2.xyz += op.colour * 1020.z * max(N . dir, 0)), until a 0: colour =
 *         min(q2, 255) * 1019
 *   0x260 / 0x3db: clip tests as the mesh program's 0x166 (1021 / 1022), triangles of the strip clipped
 *         (Sutherland-Hodgman), out as STQ (UV / w), RGBA, XYZF (vf02 + C / Cw, Z = vf02.z + vf31.z / Cw, fog
 *         clamp(k Cw - a k, 0, 255): k = 1014.z, a = 1014.w).
 * Here: drawn directly on the GE like the mesh program's 0x166 (nat_emit: strips, CPU-clipped polygons). Anything
 * else (other flags, blending, light ops 0x80..0x82, no direct path) -> the VU program (hle_vu1_fast.h). */
static uint64_t st_fol, st_fol_v, st_fol_back;
static int fol_setup(void)
{
    uint32_t fl = (uint32_t)mi(1023, 0) & 0xFFFF;
    if ((fl & 10) != 10) return 0;                              /* (the other setups: other paths, not seen) */
    float s[3] = {mf(1015, 0), mf(1015, 1), mf(1015, 2)};
    for (int r = 28; r <= 31; r++) {
        float x = mf(1008 + r - 28, 0), y = mf(1008 + r - 28, 1), w = mf(1008 + r - 28, 3);
        float h = nf(w * 0.5f);
        x = nf(nf(x - h) * s[0]); y = nf(nf(y - h) * s[1]);
        float z = r == 31 ? nf(1.0f * s[2]) : 0.0f;
        float v[4] = {x, y, z, w}; memcpy(vu.vf[r].u, v, 16);
    }
    float v2[4] = {nf(mf(1016, 0) + mf(1017, 0)), nf(mf(1016, 1) + mf(1017, 1)), mf(1016, 2), 0.0f};
    memcpy(vu.vf[2].u, v2, 16);
    vu.vi[13] = 0x228; vu.vi[14] = 0x2fc;
    vu.vi[12] = (fl & 4) ? 0x3db : 0x260;
    uint32_t y12 = vu.vi[12]; memcpy(vu.mem + 16 * 1023 + 4, &y12, 4);
    return 1;
}
static int nat_fol(uint32_t pc)
{
    if (pc == 0x555 ? vu.vi[15] != 0x27 : pc != 0) return 0;
    uint32_t y12; memcpy(&y12, vu.mem + 16 * 1023 + 4, 4); y12 &= 0xFFFF;
    if (pc == 0) { uint32_t fl = (uint32_t)mi(1023, 0) & 0xFFFF; if ((fl & 10) != 10) return 0; }
    else if (y12 != 0x260 && y12 != 0x3db) return 0;
    if (mf(1020, 3) != 0) return 0;                             /* vertex blend */
    uint32_t top = v_top & 0x3FF, n = v_itop & 0x3FF;
    if (n > 256 || top + 4 * n > 1008) return 0;
    /* the op list: 2 / 1 then 0, or 0 alone */
    int nop_ = 0; uint32_t opa[8]; int opc[8]; uint32_t a = 980;
    for (;;) {
        int op = mi(a, 3) & 0xFFFF;
        if (op == 2) { opc[nop_] = 2; opa[nop_++] = a; a += 1; }
        else if (op == 1) { opc[nop_] = 1; opa[nop_++] = a; a += 2; }
        else if (op == 0) break;
        else return 0;
        if (nop_ >= 8 || a >= 1008) return 0;
    }
    pu_flush();
    if (pc == 0 && !fol_setup()) return 0;
    float M[4][4];
    for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) M[r][i] = rf(28 + r, i);
    nb.var = 0x166;
    for (int i = 0; i < 4; i++) nb.v02[i] = rf(2, i);
    nb.k = mf(1014, 2); nb.fa = mf(1014, 3);
    nb.f22w = nf(nb.fa * nb.k); nb.f27w = nf(255.0f + nb.fa * nb.k);
    nb.sx = mf(1021, 1); nb.sy = mf(1022, 1); nb.zc = nf(0.0f + mf(1021, 2)); nb.zk = mf(1022, 2);
    for (int i = 1; i < 4; i++) memcpy(&nb.tag[i], vu.mem + 16 * 1018 + 4 * i, 4);
    int dbl = 0, direct = 0;
#if defined(HLE_PSP) || defined(NAT_SIM)
    direct = nat_direct_begin(M, 1, nb.k / 255.0f, (nb.v02[3] - nb.fa * nb.k) / 255.0f, &dbl);
#endif
    if (direct == 0) { st_fol_back++; return 0; }            /* (the setup done above is the program's own) */
    st_fol++; st_fol_v += n;
    if (direct > 0) {
        /* lighting constants */
        float m19[4], k20x = mf(1020, 0), k20z = mf(1020, 2);
        for (int i = 0; i < 4; i++) m19[i] = mf(1019, i);
        float L[8][3], Lc[8][3];
        for (int o = 0; o < nop_; o++) {
            uint32_t q = opa[o];
            if (opc[o] == 2) { for (int i = 0; i < 3; i++) Lc[o][i] = mf(q, i) * k20x; }
            else {
                float d[3] = {mf(q + 1, 0), mf(q + 1, 1), mf(q + 1, 2)}, s9 = -mf(979, 3);
                for (int i = 0; i < 3; i++) L[o][i] = (mf(976, i) * d[0] + mf(977, i) * d[1] + mf(978, i) * d[2]) * s9;
                for (int i = 0; i < 3; i++) Lc[o][i] = mf(q, i) * k20z;
            }
        }
        HGV *G = (HGV *)hle_ge_mesh_alloc((int)n);
        if (!G) { hle_ge_mesh_end(); goto end; }
        const float bsx = nb.sx, bsy = nb.sy, bzc = nb.zc, bzk = nb.zk;
        float cs[4]; for (int i = 0; i < 4; i++) cs[i] = (i == 3 || dbl) ? 2.0f : 1.0f;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t t = top + 4 * k;
            const float *q0 = (const float *)vmem(t), *q1 = (const float *)vmem(t + 1);
            const int32_t *q2 = (const int32_t *)vmem(t + 2), *q3 = (const int32_t *)vmem(t + 3);
            float c[4];
            if (!nop_) for (int i = 0; i < 4; i++) c[i] = (float)q2[i] * m19[i];
            else {
                for (int i = 0; i < 4; i++) c[i] = (float)q2[i];
                float N[3] = {(float)q3[0] * 0.0078125f, (float)q3[1] * 0.0078125f, (float)q3[2] * 0.0078125f};
                for (int o = 0; o < nop_; o++) {
                    if (opc[o] == 2) { for (int i = 0; i < 3; i++) c[i] += Lc[o][i]; }
                    else {
                        float d = N[0] * L[o][0] + N[1] * L[o][1] + N[2] * L[o][2];
                        if (d > 0) for (int i = 0; i < 3; i++) c[i] += Lc[o][i] * d;
                    }
                }
                for (int i = 0; i < 4; i++) c[i] = (c[i] > 255.0f ? 255.0f : c[i]) * m19[i];
            }
            uint32_t col = 0;
            for (int i = 0; i < 4; i++) { int v = (int)(c[i] * cs[i]); v = v > 255 ? 255 : v < 0 ? 0 : v; col |= (uint32_t)v << (8 * i); }
            HGV *g = &G[k];
            float px = q0[0], py = q0[1], pz = q0[2];
            g->u = q1[0]; g->v = q1[1]; g->c = col; g->x = px; g->y = py; g->z = pz;
            NV *v = &nat_V[k];
            float cx_ = M[3][0] + M[0][0] * px + M[1][0] * py + M[2][0] * pz, cy_ = M[3][1] + M[0][1] * px + M[1][1] * py + M[2][1] * pz;
            float cw_ = M[3][3] + M[0][3] * px + M[1][3] * py + M[2][3] * pz;
            v->c[0] = cx_; v->c[1] = cy_; v->c[2] = M[3][2] + M[2][2] * pz; v->c[3] = cw_;
            v->uv[0] = g->u; v->uv[1] = g->v;
            float ex = cx_ * bsx, ey = cy_ * bsy, ez = bzc + bzk * cw_, l = fabsf(cw_);
            uint32_t f = (ex > l ? 1u : 0) | (ex < -l ? 2u : 0) | (ey > l ? 4u : 0) | (ey < -l ? 8u : 0) | (ez > l ? 16u : 0) | (ez < -l ? 32u : 0);
            if (NAT_GUARD && f && !(f & 48u) && fabsf(cx_) < 1500.0f * l && fabsf(cy_) < 1500.0f * l) f |= 64u;
            nat_F[k] = f;
        }
        if (n < 3) hle_ge_mesh_end();
        else if (((nb.tag[1] >> 15) & 7) == 3) nat_emit_list(n, M, dbl, G);
        else nat_emit(0x166, n, M, dbl, G, 0);
    }
end:
    vu.vi[15] = 0x27; vu.tpc = 0x555;
    return 1;
}
#endif
static int nat_try(uint32_t pc)
{
    if (nat_mode < 0) {
#ifdef HLE_PSP
        const char *e = hle_cfg_get("x_vu1n");
#else
        const char *e = getenv("HLE_VU1_NATIVE");
#endif
        nat_mode = e ? atoi(e) : 1;
    }
    if (!nat_mode) return 0;
    if (nat_code_ok < 0) { uint32_t h = code_hash(0); nat_code_ok = h == NAT_HASH ? 1 : h == SKIN_HASH ? 2 : h == FOL_HASH ? 3 : 0; }
#if defined(HLE_PSP) || defined(NAT_SIM)
    if (nat_code_ok == 3) {
        static int fon = -1;
#ifdef HLE_PSP
        if (fon < 0) { const char *e = hle_cfg_get("x_folnat"); fon = e ? atoi(e) : 1; }
#else
        if (fon < 0) { const char *e = getenv("HLE_FOLNAT"); fon = e ? atoi(e) : 1; }
#endif
        if (!fon) return 0;
        int r_; ZN(18, r_ = nat_fol(pc)); return r_;
    }
#endif
    if (nat_code_ok == 1 ? (pc != 0x424 && pc != 0) : nat_code_ok == 2 ? (pc != 0x262 && pc != 0) : 1) { st_nat_skip++; return 0; }
#ifndef HLE_PSP
    if (nat_mode == 2) {                                   /* compare: native into ncap_a, then the interpreter */
        static uint8_t save[sizeof vu]; memcpy(save, &vu, sizeof vu);
        ncap_na = ncap_nb = 0; ncap_on = 1;
        int ok = nat_code_ok == 2 ? skin_try(pc) : nat_batch(pc == 0);
        ncap_on = 0;
        memcpy(&vu, save, sizeof vu);
        if (!ok) return 0;
        ncap_on = 2;                                        /* the interpreter's kicks go to ncap_b (and to the GS) */
        return 0;
    }
#endif
    if (nat_code_ok != 1 && !(nat_code_ok == 2 && skin_rows_cached())) pu_flush();
    if (nat_code_ok == 2) {
        int ok = skin_try(pc);
#if defined(HLE_PSP) || defined(NAT_SIM)
        skc_materialize();                                  /* (skin_fast consumed it, or the rows go to VU memory) */
#endif
        return ok;
    }
    return nat_batch(pc == 0);
}

#ifndef HLE_PSP
static void nat_compare_done(void)
{
    static uint64_t nb_, na_, matched, bad, xmax;
    int *used = calloc((size_t)ncap_nb + 1, sizeof(int));
    for (int i = 0; i < ncap_na; i++) {
        const NTri *A = &ncap_a[i];
        int best = -1; int64_t bestd = INT64_MAX;
        for (int j = 0; j < ncap_nb; j++) {
            if (used[j]) continue;
            const NTri *B = &ncap_b[j]; int64_t d = 0;
            for (int v = 0; v < 3 && d < bestd; v++) {
                int32_t a[12], b[12]; memcpy(a, A->v[v].q, 48); memcpy(b, B->v[v].q, 48);
                for (int l = 4; l < 12; l++) { int64_t e = l < 8 ? (int64_t)(a[l] & 0xFF) - (b[l] & 0xFF) : (int64_t)a[l] - b[l]; d += e < 0 ? -e : e; }
                for (int l = 0; l < 3; l++) { float fa, fb; memcpy(&fa, &a[l], 4); memcpy(&fb, &b[l], 4); float e = fabsf(fa - fb) / (fabsf(fb) + 1e-6f); if (e > (getenv("NATCMP_TOL") ? 1e-2f : 1e-4f)) d += 1000; }
            }
            if (d < bestd) { bestd = d; best = j; }
        }
        if (best >= 0 && bestd <= (getenv("NATCMP_TOL") ? atoi(getenv("NATCMP_TOL")) : 64)) { used[best] = 1; matched++; if ((uint64_t)bestd > xmax) xmax = (uint64_t)bestd; }
        else {
            bad++;
            static int shown; if ((!getenv("NATCMP_VAR") || (uint32_t)nb.var == strtoul(getenv("NATCMP_VAR"), 0, 16)) && shown++ < 8) {
                const NTri *B = best >= 0 ? &ncap_b[best] : 0;
                LOG("NATCMP mismatch var %x: d %lld\n", nb.var, (long long)bestd);
                for (int v = 0; v < 3; v++) {
                    int32_t a[12], b[12] = {0}; memcpy(a, A->v[v].q, 48); if (B) memcpy(b, B->v[v].q, 48);
                    float sa[3], sb[3]; memcpy(sa, a, 12); memcpy(sb, b, 12);
                    LOG("  nat %g %g %g | %d %d %d %d | %d %d %d %d\n", sa[0], sa[1], sa[2], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
                    LOG("  int %g %g %g | %d %d %d %d | %d %d %d %d\n", sb[0], sb[1], sb[2], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
                }
            }
        }
    }
    na_ += (uint64_t)ncap_na; nb_ += (uint64_t)ncap_nb;
    if ((nb.var & 0x1000) && ncap_na != ncap_nb) { static int sh; if (sh++ < 10) LOG("NATCNT var %x native %d interp %d n %u\n", nb.var, ncap_na, ncap_nb, v_itop); }
    free(used);
    static uint64_t calls; if ((++calls & 1023) == 0)
        LOG("NATCMP %llu batches: native %llu tris, interp %llu, matched %llu, unmatched %llu (max sum diff %llu), clipped %llu dropped %llu\n",
            (unsigned long long)calls, (unsigned long long)na_, (unsigned long long)nb_, (unsigned long long)matched, (unsigned long long)bad,
            (unsigned long long)xmax, (unsigned long long)st_nat_clip, (unsigned long long)st_nat_drop);
}
#endif
