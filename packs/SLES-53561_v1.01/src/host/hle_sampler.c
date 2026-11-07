/* hle_sampler.c (host only) - a sampling profiler for the translated game code: env HLE_SAMPLE=<file> starts a
 * SIGPROF timer (CPU time, every HLE_SAMPLE_US microseconds, default 500) after HLE_SAMPLE_FROM virtual seconds
 * (default 0) and writes the sampled host program counters (hex, one per line, with counts) at exit. Map them to
 * functions with `nm -n` of the host binary (tools/sample_report.py). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <ucontext.h>
#include "hle.h"

#define NS (1u << 20)
static uintptr_t pcs[NS];
static uint32_t cnt[NS];
static volatile uint64_t nsamp, ndrop;
static double from_s;
static const char *out;

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    if ((double)hle_vtime / (double)EE_HZ < from_s) return;
    ucontext_t *uc = (ucontext_t *)uc_;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uint32_t h = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) >> 44) & (NS - 1);
    for (int k = 0; k < 64; k++) {
        uint32_t i = (h + (uint32_t)k) & (NS - 1);
        if (pcs[i] == pc) { cnt[i]++; nsamp++; return; }
        if (!pcs[i]) { pcs[i] = pc; cnt[i] = 1; nsamp++; return; }
    }
    ndrop++;
}

void hle_sampler_dump(void)
{
    if (!out) return;
    static int done; if (done) return; done = 1;
    FILE *f = fopen(out, "w");
    if (!f) return;
    for (uint32_t i = 0; i < NS; i++) if (pcs[i]) fprintf(f, "%lx %u\n", (unsigned long)pcs[i], cnt[i]);
    fclose(f);
    fprintf(stderr, "sampler: %llu samples (%llu dropped) -> %s\n", (unsigned long long)nsamp, (unsigned long long)ndrop, out);
}

__attribute__((constructor)) static void sampler_start(void)
{
    out = getenv("HLE_SAMPLE");
    if (!out || !*out) return;
    const char *e = getenv("HLE_SAMPLE_FROM"); from_s = e ? atof(e) : 0;
    e = getenv("HLE_SAMPLE_US"); long us = e ? atol(e) : 500;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = us; it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, 0);
    atexit(hle_sampler_dump);
}
