/* iop_snd_unit.c - this game's IOP sound modules (SCREAM.IRX: the sound library, RPC 0x123456 / 0x123457;
 * AUDVAG.IRX: the audio streaming module, RPC 0xBCFF), translated on the player's machine from the player's own disc
 * by PS2RC's IOP static recompiler (convert/iopgen.py -> {work}/iopgen, units iop_scream_unit.c / iop_audvag_unit.c),
 * run by PS2RC's generic IOP runtime (ps2rc/iop/rt, included here as one unit). Replaces the silent responders of
 * src/hle_iop_servers.c when HLE_IOP_SND is defined (pack [build] defines).
 *
 * Structure adapted from PS2RC's SLES-52541 pack (iop/snd_glue.c, PS2RC License, https://github.com/SandstoneDev/PS2RC):
 * the coroutine and memory hooks of the platform, the timer clocking. What is specific to this game:
 *   - the SPU behind libsd is SILENT and instant: every voice's envelope reads 0 (a sound ends at once), keys,
 *     parameters and the SPU2 IRQ do nothing, the SPU RAM is a plain buffer - no audio output yet;
 *   - SIF DMA IOP -> EE (sifman #32 sceSifSetDmaIntr, #7 sceSifSetDma, #8 sceSifDmaStat): the transfers are copied
 *     into the EE RAM at once and the completion handler runs at once. SCREAM answers every command this way: the EE
 *     polls a 12-byte buffer for {0xFFFFFFFF, result, 0xFFFFFFFF} (EE side 0x3177D8 / 0x315FD0);
 *   - sifcmd #23 sceSifGetOtherData (EE -> IOP copy);
 *   - sifcmd #15 sceSifBindRpc of AUDVAG to the game's EE-side server 0xABBA (the EE audio thread 0x40D060): IOP -> EE
 *     calls are not modelled yet, the bind reports failure (AUDVAG then logs and carries on without callbacks);
 *   - IOP time follows the EE's virtual time: before every forwarded RPC the IOP clock advances by the EE time since the
 *     last one, one IOP VBlank per EE VBlank, and every active hardware timer expires once per elapsed period.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#ifdef HLE_IOP_SND
#include "iop_rt.h"                               /* the runtime itself: iop_rt_unit.c */
#ifndef HLE_PSP
#include "iop_rt_ucontext.h"
#endif

extern const IopModule scream_irx, audvag_irx;   /* the generated modules ({work}/iopgen, never distributed) */
#include "iop_exports.h"                          /* EXP_scream_irx_scream[]: SCREAM's export table (convert/iopgen.py) */

#define RPC_SND 0x00123456u
#define RPC_SND2 0x00123457u
#define RPC_AUDVAG 0x0000BCFFu
#ifdef HLE_PSP
#define IOPS_MEM 0x180000u                        /* 1.5 MB: the modules' heap peaks at ~934 KB (host runs) */
#else
#define IOPS_MEM 0x200000u                        /* the IOP's 2 MB */
#endif
#ifdef HLE_PSP
#define IOPS_SPU_RAM 0x10000u                     /* the SPU is silent here: its RAM is only written (64 KB, masked) */
#else
#define IOPS_SPU_RAM 0x200000u                    /* SPU2 RAM (2 MB) */
#endif
#define IOPS_STACK (32u * 1024u)
#define IOPS_LOAD_PRIO 8

/* ------------------------------------------------------------------ coroutines and memory of the platform */
#ifdef HLE_PSP
#include "hle_plat.h"
typedef struct { hle_hostctx ctx; } GCoro;
static GCoro g_main;
static void *pc_create(void *u, size_t st, void (*entry)(void))
{
    (void)u;
    GCoro *g = (GCoro *)calloc(1, sizeof *g);
    void *s = hle_plat_stack_alloc(st);
    if (!g || !s) { free(g); return 0; }
    hle_plat_ctx_make(&g->ctx, s, st, entry);
    return g;
}
static void pc_swap(void *u, void *from, void *to) { (void)u; hle_plat_ctx_swap(&((GCoro *)from)->ctx, &((GCoro *)to)->ctx); }
static void *pc_main(void *u) { (void)u; return &g_main; }
static void coro_ops(IopCoroOps *o) { o->user = 0; o->create = pc_create; o->swap = pc_swap; o->main_ctx = pc_main; }
/* big blocks from user memory (the runtime's newlib heap is only 768 KB); never freed */
#include <pspsysmem.h>
static void *psp_big(void *u, size_t n)
{
    (void)u;
    if (n < 64 * 1024) return calloc(1, n);
    SceUID id = sceKernelAllocPartitionMemory(2, "iop_snd", PSP_SMEM_High, n + 64, NULL);
    if (id < 0) { LOG("IOP sound modules: no memory for %u KB (free %u KB)\n", (unsigned)(n >> 10), (unsigned)(sceKernelTotalFreeMemSize() >> 10)); return 0; }
    void *p = (void *)(((uintptr_t)sceKernelGetBlockHeadAddr(id) + 63u) & ~(uintptr_t)63u);
    memset(p, 0, n);
    return p;
}
static void psp_big_free(void *u, void *p) { (void)u; (void)p; }
#else
static void coro_ops(IopCoroOps *o) { iop_rt_coro_ucontext(o); }
#endif

/* ------------------------------------------------------------------ state */
static IopRt *g_rt;
static int g_ok;
static uint64_t g_last_vtime, g_last_frame, g_timer_ns[8];
static uint64_t g_calls[3], g_dma, g_dma_bytes, g_other, g_other_bytes;
static uint8_t *g_spu_ram;

static void g_log(void *u, int level, const char *fmt, ...)
{
    (void)u;
    if (level > 1 && level > hle_verbose) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    size_t n = strlen(b);
    /* a module repeating the same message (e.g. a missing sound looked up every frame): each log line is a memory
       card write on the PSP - after 4 of a kind (same first 32 characters) they are only counted */
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < n && i < 32; i++) h = (h ^ (uint8_t)b[i]) * 16777619u;
        static uint32_t seen_h[64]; static uint32_t seen_n[64];
        int k = (int)(h & 63);
        if (seen_h[k] == h) {
            if (++seen_n[k] > 4) { if (seen_n[k] == 5) LOG("IOP: (this message repeats: not logged any more)\n"); return; }
        } else { seen_h[k] = h; seen_n[k] = 1; }
    }
    LOG("IOP: %s%s", b, n && b[n - 1] == '\n' ? "" : "\n");
}

/* ------------------------------------------------------------------ the disc (cdvdman / ioman of the modules) */
static int d_lookup(void *u, const char *path, uint32_t *lsn, uint32_t *size)
{
    (void)u;
    return hle_disc_lookup(path, lsn, size) == 0 ? 0 : -1;
}
static int d_read(void *u, uint32_t lsn, uint32_t n, void *dst)
{
    (void)u;
    return hle_disc_read_lsn(lsn, n, dst) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ the silent, instant SPU */
static void sd_param(void *u, unsigned v, unsigned p, uint16_t x) { (void)u; (void)v; (void)p; (void)x; }
static void sd_addr(void *u, unsigned v, unsigned w, uint32_t a) { (void)u; (void)v; (void)w; (void)a; }
static uint16_t sd_envx(void *u, unsigned v) { (void)u; (void)v; return 0; }
static uint32_t sd_nax(void *u, unsigned v) { (void)u; (void)v; return 0; }
static void sd_key(void *u, unsigned core, uint32_t m) { (void)u; (void)core; (void)m; }
static void sd_irq(void *u, uint32_t a, int e) { (void)u; (void)a; (void)e; }

/* ------------------------------------------------------------------ SIF between the IOP and the EE RAM */
static void dma_list(IopCpu *c, uint32_t desc, uint32_t n)
{
    for (uint32_t i = 0; i < n && i < 64; i++) {
        uint32_t d = desc + 16 * i;
        uint32_t src = IOP_LW(c->mem, d), dst = IOP_LW(c->mem, d + 4), size = IOP_LW(c->mem, d + 8);
        dst &= 0x01FFFFFFu;
        if (!size || size > 0x100000u || dst + size > hle_ram_end) {
            LOG("IOP: SIF DMA %u bytes IOP %08x -> EE %08x refused\n", size, src, dst);
            continue;
        }
        for (uint32_t k = 0; k < size; k += 4) {
            uint32_t w = IOP_LW(c->mem, src + k);
            memcpy(gptr(dst + k, 4), &w, size - k < 4 ? size - k : 4);
        }
        g_dma++;
        g_dma_bytes += size;
    }
}
/* sifman #7 sceSifSetDma(desc, count) -> id */
static void b_set_dma(IopCpu *c) { dma_list(c, iop_arg(c, 0), iop_arg(c, 1)); iop_ret(c, 1); }
/* sifman #32 sceSifSetDmaIntr(desc, count, handler, arg) -> id: the transfer completes at once, then the handler */
static void b_set_dma_intr(IopCpu *c)
{
    uint32_t desc = iop_arg(c, 0), n = iop_arg(c, 1), h = iop_arg(c, 2), arg = iop_arg(c, 3);
    dma_list(c, desc, n);
    if (h) {
        uint32_t save[32];
        memcpy(save, c->r, sizeof save);
        c->r[4] = arg;
        iop_rt_call(c, h);
        memcpy(c->r, save, sizeof save);
    }
    iop_ret(c, 1);
}
/* sifman #8 sceSifDmaStat(id): < 0 = finished */
static void b_dma_stat(IopCpu *c) { iop_ret(c, 0xFFFFFFFFu); }
/* sifcmd #23 sceSifGetOtherData(rd, ee_src, iop_dst, size, mode) */
static void b_get_other(IopCpu *c)
{
    uint32_t src = iop_arg(c, 1) & 0x01FFFFFFu, dst = iop_arg(c, 2), size = iop_arg(c, 3);
    if (size && size <= 0x100000u && src + size <= hle_ram_end) {
        for (uint32_t k = 0; k < size; k++) IOP_SB(c->mem, dst + k, *(uint8_t *)gptr(src + k, 1));
        g_other++;
        g_other_bytes += size;
    }
    iop_ret(c, 0);
}
/* sifcmd #15 sceSifBindRpc(client, rpc_number, mode) to an EE-side server (AUDVAG -> the game's audio thread, server
   0xADDA; AUDVAG retries a failed bind forever): the bind succeeds (client->server, offset 36 of the client data,
   gets a non-zero token) - sifcmd #16 sceSifCallRpc(client, fno, mode, send, ssize, recv, rsize, end, param) is then
   a no-op that clears the receive buffer: the EE does not see AUDVAG's notifications yet (counted). */
static uint64_t g_ee_calls;
static uint32_t g_ee_fno_seen[64];
static void b_bind_ee(IopCpu *c)
{
    static int once;
    if (!once++) LOG("IOP: a module binds EE-side RPC server %08x - bound, its calls are dropped (IOP->EE not modelled)\n",
                     iop_arg(c, 1));
    IOP_SW(c->mem, iop_arg(c, 0) + 36, 0x00000EE0u);
    iop_ret(c, 0);
}
static void b_call_ee(IopCpu *c)
{
    uint32_t fno = iop_arg(c, 1), recv = iop_arg(c, 5), rsize = iop_arg(c, 6);
    g_ee_calls++;
    if (fno < 64 && !g_ee_fno_seen[fno]++) LOG("IOP: module -> EE call fno %u dropped (first)\n", fno);
    if (recv && rsize && rsize < 0x10000u)
        for (uint32_t k = 0; k < rsize; k++) IOP_SB(c->mem, recv + k, 0);
    iop_ret(c, 0);
}

/* ------------------------------------------------------------------ module-to-module imports
   AUDVAG imports library "scream" (SCREAM's own exports) and "cdstream" (CDSTREAM.IRX, not translated): the runtime
   links only its built-in libraries, so each "scream" number gets a trampoline into SCREAM's translated function at
   the export address, and "cdstream" is answered here (same protocol as PS2RC's reference pack: #5 read
   (channel, iop_buf, lsn, sectors) -> 1 accepted, synchronous; #7 sync -> 0 done; #8 drive position -> 0; others 0). */
#define NEXP (int)(sizeof EXP_scream_irx_scream / sizeof EXP_scream_irx_scream[0])
static void exp_call(IopCpu *c, int n)
{
    uint32_t a = n < NEXP ? EXP_scream_irx_scream[n] : 0;
    if (!a) { iop_ret(c, 0); return; }
    iop_rt_call(c, a);
}
#define T(n) static void T##n(IopCpu *c) { exp_call(c, n); }
#define T10(d) T(d##0) T(d##1) T(d##2) T(d##3) T(d##4) T(d##5) T(d##6) T(d##7) T(d##8) T(d##9)
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9) T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9)
#define R10(d) T##d##0, T##d##1, T##d##2, T##d##3, T##d##4, T##d##5, T##d##6, T##d##7, T##d##8, T##d##9
static const IopFn TRAMP[100] = {T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, R10(1), R10(2), R10(3), R10(4), R10(5), R10(6),
                                 R10(7), R10(8), R10(9)};
static void cds_read(IopCpu *c)
{
    uint32_t buf = iop_arg(c, 1), lsn = iop_arg(c, 2), n = iop_arg(c, 3);
    int r = iop_rt_disc_read(c->rt, lsn, n, iop_ptr(c, buf));
    iop_ret(c, r == 0 ? 1u : 0u);
}
static void cds_zero(IopCpu *c) { iop_ret(c, 0); }

/* sysclib #42 vsprintf(buf, fmt, va): the runtime's formatter over the va_list memory (o32: va points at the first
   variadic word; with a0-a3 = its first four words and sp = va, argument k >= 4 is read at sp + 16 + 4 (k - 4)) */
int iop_rt_format(IopCpu *c, uint32_t fmt, int argk, char *out, size_t cap);
static void b_vsprintf(IopCpu *c)
{
    uint32_t buf = iop_arg(c, 0), fmt = iop_arg(c, 1), va = iop_arg(c, 2);
    IopCpu t = *c;
    for (int k = 0; k < 4; k++) t.r[4 + k] = IOP_LW(c->mem, va + 4 * k);
    t.r[29] = va;
    char b[1024];
    int n = iop_rt_format(&t, fmt, 0, b, sizeof b);
    if (n < 0) n = 0;
    if (n >= (int)sizeof b) n = sizeof b - 1;
    for (int k = 0; k <= n; k++) IOP_SB(c->mem, buf + k, k < n ? b[k] : 0);
    iop_ret(c, (uint32_t)n);
}

/* AUDVAG's IDLE thread (entry 0xAC94 of the module = 0x49C94 here) polls its queues and the clock in a loop that never
   waits: on the IOP the interrupts preempt it; under the runtime's cooperative threads it would never give the IOP
   back to the EE. Its clock read (thbase #34 GetSystemTime, call returning to module offset 0xAD0C) first sleeps
   1 ms of IOP time (DelayThread), so the other threads run and the runtime returns when everything waits. */
#define AUDVAG_RELOC (0x40000u - 0x1000u)             /* convert/iopgen.py base 0x40000; the module links at 0x1000 */
static IopFn g_get_time, g_delay;
static uint64_t g_idle_naps;
static void b_get_system_time(IopCpu *c)
{
    if (c->r[31] == 0xAD0Cu + AUDVAG_RELOC && g_delay) {
        uint32_t save[32];
        memcpy(save, c->r, sizeof save);
        c->r[4] = 1000;
        g_delay(c);
        memcpy(c->r, save, sizeof save);
        g_idle_naps++;
    }
    g_get_time(c);
}

/* ------------------------------------------------------------------ IOP time follows the EE */
static void sync_time(void)
{
    uint64_t dv = hle_vtime - g_last_vtime;
    g_last_vtime = hle_vtime;
    uint64_t us = dv * 1000000ull / EE_HZ;
    if (us) iop_rt_advance_us(g_rt, us);
    for (uint64_t f = g_last_frame; f < hle_frame && f < g_last_frame + 8; f++) iop_rt_vblank(g_rt);
    g_last_frame = hle_frame;
    uint64_t ns = dv * 1000000000ull / EE_HZ;
    for (int k = 0; k < iop_rt_timer_count(g_rt) && k < 8; k++) {
        if (!iop_rt_timer_active(g_rt, k)) { g_timer_ns[k] = 0; continue; }
        uint64_t per = iop_rt_timer_period_ns(g_rt, k);
        if (!per) per = 1000000;                       /* unknown source: 1 ms */
        g_timer_ns[k] += ns;
        int n = 0;
        while (g_timer_ns[k] >= per && n < 64) { g_timer_ns[k] -= per; iop_rt_timer_expire(g_rt, k); n++; }
        if (n == 64) g_timer_ns[k] = 0;
    }
    iop_rt_run(g_rt);
}

static int forward(int idx, uint32_t sid, R5900Ctx *c, uint32_t fno, uint32_t send, uint32_t ssize, uint32_t recv,
                   uint32_t rsize)
{
    (void)c;
    g_calls[idx]++;
    if (!g_ok) {
        if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
        return 0;
    }
    sync_time();
    void *s = send && ssize ? gptr(send, ssize) : 0;
    void *r = recv && rsize ? gptr(recv, rsize) : 0;
    int e = iop_rt_sif_call(g_rt, sid, fno, s, ssize, r, rsize);
    if (e != 0) {
        static int warned;
        if (warned++ < 8) LOG("IOP: RPC %08x fno %u not answered by the translated module\n", sid, fno);
        if (r) memset(r, 0, rsize);
    }
    iop_rt_run(g_rt);
    return 0;
}
static int srv_snd(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                   uint32_t rsize) { (void)mode; return forward(0, RPC_SND, c, fno, send, ssize, recv, rsize); }
static int srv_snd2(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                    uint32_t rsize) { (void)mode; return forward(1, RPC_SND2, c, fno, send, ssize, recv, rsize); }
static int srv_audvag(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                      uint32_t rsize) { (void)mode; return forward(2, RPC_AUDVAG, c, fno, send, ssize, recv, rsize); }

/* called by src/hle_iop_servers.c's hle_pack_iop_init */
int bully_iop_snd_init(void)
{
    IopRtConfig rc;
    memset(&rc, 0, sizeof rc);
    rc.mem_size = IOPS_MEM;
    rc.host_stack = IOPS_STACK;
    coro_ops(&rc.coro);
    rc.disc.lookup = d_lookup;
    rc.disc.read = d_read;
#ifdef HLE_PSP
    rc.alloc = psp_big; rc.free = psp_big_free; rc.alloc_user = 0;
    g_spu_ram = (uint8_t *)psp_big(0, IOPS_SPU_RAM);
#else
    g_spu_ram = (uint8_t *)calloc(1, IOPS_SPU_RAM);
#endif
    rc.sd.ram = g_spu_ram;
    rc.sd.ram_size = IOPS_SPU_RAM;
    rc.sd.set_param = sd_param;
    rc.sd.set_addr = sd_addr;
    rc.sd.get_envx = sd_envx;
    rc.sd.get_nax = sd_nax;
    rc.sd.key_on = sd_key;
    rc.sd.key_off = sd_key;
    rc.sd.set_irq = sd_irq;
    rc.log = g_log;
    g_rt = g_spu_ram ? iop_rt_create(&rc) : 0;
    if (!g_rt) { LOG("IOP sound modules: runtime not created\n"); return 0; }
    iop_rt_bind(g_rt, "sifman", 7, b_set_dma);
    iop_rt_bind(g_rt, "sifman", 8, b_dma_stat);
    iop_rt_bind(g_rt, "sifman", 32, b_set_dma_intr);
    iop_rt_bind(g_rt, "sifcmd", 23, b_get_other);
    iop_rt_bind(g_rt, "sifcmd", 15, b_bind_ee);
    iop_rt_bind(g_rt, "sifcmd", 16, b_call_ee);
    for (int n = 0; n < NEXP && n < 100; n++)
        if (EXP_scream_irx_scream[n]) iop_rt_bind(g_rt, "scream", (unsigned)n, TRAMP[n]);
    iop_rt_bind(g_rt, "sysclib", 42, b_vsprintf);
    const char *nm = 0;
    g_get_time = iop_rt_builtin("thbase", 34, &nm);
    g_delay = iop_rt_builtin("thbase", 33, 0);
    if (g_get_time && g_delay) iop_rt_bind(g_rt, "thbase", 34, b_get_system_time);
    else LOG("IOP: thbase #34 / #33 not built in - AUDVAG's idle loop is not tamed\n");
    iop_rt_bind(g_rt, "cdstream", 5, cds_read);
    for (unsigned n = 4; n < 13; n++)
        if (n != 5) iop_rt_bind(g_rt, "cdstream", n, cds_zero);
    static const char *const a1[] = {"SCREAM.IRX"}, *const a2[] = {"AUDVAG.IRX"};
    int r1 = iop_rt_load(g_rt, &scream_irx, 1, a1, IOPS_LOAD_PRIO);
    int r2 = iop_rt_load(g_rt, &audvag_irx, 1, a2, IOPS_LOAD_PRIO);
    iop_rt_run(g_rt);
    g_ok = 1;
    g_last_vtime = hle_vtime;
    g_last_frame = hle_frame;
    hle_rpc_register(RPC_SND, srv_snd);
    hle_rpc_register(RPC_SND2, srv_snd2);
    hle_rpc_register(RPC_AUDVAG, srv_audvag);
    const IopRtStats *s = iop_rt_stats(g_rt);
    LOG("IOP sound modules (translated from the disc): %s md5 %s start %d, %s md5 %s start %d; threads %u, heap %u KB, "
        "imports unresolved %u\n", scream_irx.name, scream_irx.md5, r1, audvag_irx.name, audvag_irx.md5, r2, s->threads,
        s->mem_used >> 10, s->imports_unresolved);
    hle_milestone("IOP sound modules running (translated)");
    return 1;
}

void bully_iop_snd_report(void)
{
    if (!g_rt) return;
    const IopRtStats *s = iop_rt_stats(g_rt);
    LOG("IOP sound modules: RPC calls snd %llu snd2 %llu audvag %llu; SIF DMA %llu (%llu bytes), other-data %llu "
        "(%llu bytes); IOP->EE calls dropped %llu; IOP threads %u, switches %u, unimplemented calls %u, heap peak %u KB\n",
        (unsigned long long)g_calls[0], (unsigned long long)g_calls[1], (unsigned long long)g_calls[2],
        (unsigned long long)g_dma, (unsigned long long)g_dma_bytes, (unsigned long long)g_other,
        (unsigned long long)g_other_bytes, (unsigned long long)g_ee_calls, s->threads, s->switches, s->unimpl_calls, s->mem_peak >> 10);
}
#else
int bully_iop_snd_init(void) { return 0; }
void bully_iop_snd_report(void) {}
#endif
