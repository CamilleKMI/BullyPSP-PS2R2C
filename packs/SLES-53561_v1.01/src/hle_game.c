/* hle_game.c - this game's boot-time glue: the code-overlay table and the overlay loader hook. */
#include "hle.h"

void fn_00122900__guest(R5900Ctx *c);   /* the translated originals (renamed by the prepare step) */
void fn_0049ab40__guest(R5900Ctx *c);

#define OVL(n) extern rc_fn ov##n##_rc_lookup(uint32_t);
OVL(1) OVL(2) OVL(3) OVL(4) OVL(5) OVL(6) OVL(7) OVL(8) OVL(9) OVL(10) OVL(11) OVL(12) OVL(13) OVL(14) OVL(15)
OVL(16) OVL(17)

static void overlays_register(void)
{
    rc_overlay_lookup[1] = ov1_rc_lookup;   rc_overlay_lookup[2] = ov2_rc_lookup;   rc_overlay_lookup[3] = ov3_rc_lookup;
    rc_overlay_lookup[4] = ov4_rc_lookup;   rc_overlay_lookup[5] = ov5_rc_lookup;   rc_overlay_lookup[6] = ov6_rc_lookup;
    rc_overlay_lookup[7] = ov7_rc_lookup;   rc_overlay_lookup[8] = ov8_rc_lookup;   rc_overlay_lookup[9] = ov9_rc_lookup;
    rc_overlay_lookup[10] = ov10_rc_lookup; rc_overlay_lookup[11] = ov11_rc_lookup; rc_overlay_lookup[12] = ov12_rc_lookup;
    rc_overlay_lookup[13] = ov13_rc_lookup; rc_overlay_lookup[14] = ov14_rc_lookup; rc_overlay_lookup[15] = ov15_rc_lookup;
    rc_overlay_lookup[16] = ov16_rc_lookup; rc_overlay_lookup[17] = ov17_rc_lookup;
    rc_overlay_current = 0;
    rc_overlay_lo = 0x757500;
    rc_overlay_hi = 0x76A080;                  /* the largest slot's end (DBLL.OLF) */
}

/* 0x122900(path, load address): the game's overlay loader; after it the dispatcher switches to that overlay */
HLE(00122900)
{
    uint32_t name = ARG(0), addr = ARG(1);
    LOG("loading code overlay \"%s\" to %08x\n", gstr(name), addr);
    hle_trace_call("overlay_file_load", 0x122900, c);
    fn_00122900__guest(c);
    hle_overlay_loaded(addr);
}

void hle_pack_boot_init(void)
{
    overlays_register();
}

/* MPEG.OLF (overlay 2): the FMV player. 0x1F5CD0 loads it, calls init 0x759050, play 0x758FC0(path, 0, size) for the
   intro movie of the current language, then shutdown 0x759040. No IPU / SPU2 model: the movies are skipped (the
   PSP may later play a converted PMF here as PS2RC's hle_movie.c does for its reference pack). */
HLE_OV(2, 00759050) { hle_trace_call("movie_init (skipped)", 0x759050, c); RET(0); }
HLE_OV(2, 00759040) { hle_trace_call("movie_shutdown (skipped)", 0x759040, c); RET(0); }
HLE_OV(2, 00758fc0)
{
    LOG("FMV \"%s\" skipped (no IPU/SPU2 model)\n", gstr(ARG(0)));
    hle_trace_call("movie_play (skipped)", 0x758fc0, c);
    hle_milestone("intro movie skipped (MPEG.OLF 0x758FC0)");
    RET(0);
}

/* 0x49AB40(path, load address): the second overlay loader (callers 0x33DE30, 0x408260, 0x431700, 0x467510,
   0x4EB850, 0x4EB920: the class / minigame overlays) */
HLE(0049ab40)
{
    uint32_t name = ARG(0), addr = ARG(1);
    LOG("loading code overlay \"%s\" to %08x (loader 2)\n", gstr(name), addr);
    hle_trace_call("overlay_file_load2", 0x49ab40, c);
    fn_0049ab40__guest(c);
    hle_overlay_loaded(addr);
}

/* 0x305AD0(stream, offset, whence): seek through the stream's device ops (stream+80 = device, device+40 = ops;
   whence 0/1/2 -> ops+16 with mode 1/2/3). Diagnostic wrapper: logs the device the first time an op is missing. */
void fn_00305ad0__guest(R5900Ctx *c);
HLE(00305ad0)
{
    uint32_t s = ARG(0), dev = gw(s + 80), fn = gw(dev + 56);
    static int logged;
    if (!fn && logged++ < 8) {
        LOG("file stream %08x: device %08x has no seek op (words:", s, dev);
        for (int i = 0; i < 24; i++) LOG(" %08x", gw(dev + 4 * i));
        LOG(") stream words:");
        for (int i = 0; i < 24; i++) LOG(" %08x", gw(s + 4 * i));
        LOG("\n");
    }
    fn_00305ad0__guest(c);
}

/* 0x3058E8(path, mode): opens a stream through the game's device list (0x305638 matches "dev:" prefixes). Logged. */
void fn_003058e8__guest(R5900Ctx *c);
HLE(003058e8)
{
    uint32_t p = ARG(0), m = ARG(1);
    char path[128], mode[8];
    snprintf(path, sizeof path, "%s", p ? gstr(p) : "(null)");
    snprintf(mode, sizeof mode, "%s", m ? gstr(m) : "");
    fn_003058e8__guest(c);
    uint32_t r = (uint32_t)c->r[2].d[0];
    if (1) {
        uint32_t d = gw(0x5E83F0u - 7248u);
        LOG("file stream open \"%s\" \"%s\" -> %08x%s; default device %08x, list %08x count %08x\n", path, mode, r,
            r ? "" : " (FAILED)", d, gw(0x712890u), gw(0x712898u));
        if (d) {
            uint32_t n = gw(d + 4), base = gw(d + 88);
            for (uint32_t i = 0; i < n && i < 16; i++)
                LOG("  slot %u @%08x: state(+56) %u +48 %08x +52 %08x +60 %08x +96 %08x\n", i, base + 112 * i,
                    gw(base + 112 * i + 56), gw(base + 112 * i + 48), gw(base + 112 * i + 52), gw(base + 112 * i + 60),
                    gw(base + 112 * i + 96));
        }
    }
}

/* 0x122870(start, end): calls every constructor of a table. Both overlay loaders run the overlay's constructors
   (loader 1 inside 0x122900, loader 2 through 0x122A00) before they return, so the overlay is selected here, as
   soon as a table inside the overlay window is run (the image is in memory by then). */
void fn_00122870__guest(R5900Ctx *c);
HLE(00122870)
{
    uint32_t t = ARG(0);
    if (t >= 0x757500u && t < 0x76A080u) hle_overlay_loaded(0x757500u);
    fn_00122870__guest(c);
}

/* 0x415D80(list): calls 0x315DB0 (the sound library's tick) and every object's update (vtable slot 3) until none is
   left. Diagnostic wrapper: logs the objects (count at list+168) on the first call. */
void fn_00415d80__guest(R5900Ctx *c);
HLE(00415d80)
{
    static int n;
    uint32_t l = ARG(0), cnt = gw(l + 168);
    if (n++ < 2) {
        LOG("sound object drain %08x: %u objects\n", l, cnt);
        for (uint32_t i = 0; i < cnt && i < 24; i++) {
            uint32_t o = gw(l + 4 * i);
            if (o) LOG("  [%u] obj %08x vtable %08x update %08x  words %08x %08x %08x %08x  +128 %08x %08x\n", i, o,
                       gw(o), gw(gw(o) + 12), gw(o + 4), gw(o + 8), gw(o + 12), gw(o + 16), gw(o + 128), gw(o + 132));
        }
    }
    fn_00415d80__guest(c);
}

/* 0x146970(key): the index of the record whose first u16 is key in the sorted table of 6-byte records at 0x6448F0
   (count: u16 at gp - 11284), else -(insertion index + 1). The original calls the generic binary search 0x1469C0
   with the comparator 0x1469B0 (*a - *b of u16) through a function pointer: one indirect call per step. Same steps
   here (same result for unsorted or duplicate keys). */
HLE(00146970)
{
    uint32_t key = ARG(0) & 0xFFFF, base = 0x6448F0, size = 6;
    uint32_t n = gh((uint32_t)c->r[28].d[0] - 11284), lo = base;
    while (n) {
        uint32_t mid = lo + (n >> 1) * size;
        int32_t r = (int32_t)key - (int32_t)gh(mid);
        if (r == 0) { RET((mid - base) / size); return; }
        if (r > 0) { lo = mid + size; n = (n - 1) >> 1; } else n >>= 1;
    }
    RET(-(int32_t)((lo - base) / size + 1));
}

