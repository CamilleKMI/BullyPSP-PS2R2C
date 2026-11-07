/* hle_iop_servers.c - this game's IOP-side services reached through SIF RPC (registered with the core's registry).
 *
 * The disc streaming module (CDSTREAM.IRX, RPC 0x65686577) speaks the same protocol as the one modelled in PS2RC's
 * SLES-52541 pack (engine/iop_rpc/hle_iop_servers.c, PS2RC License, https://github.com/SandstoneDev/PS2RC); the
 * model below is adapted from it:
 *   fno 0 Init(nchannels)   fno 1 Read(channel, ee_buffer, lsn, nsectors) -> 1 = accepted
 *   fno 2 GetStatus(channel) -> 0 ready   fno 3 Sync(channel) -> status   fno 4 GetLastPosn()
 * Reads are synchronous from the extracted disc (LSN map): every request is complete when it returns.
 *
 * Other custom servers (sound: SCREAM.IRX, AUDVAG.IRX, SDRDRV.IRX) answer through silent responders until a model or
 * the IOP recompiler's output replaces them; each unknown function number is logged once.
 */
#include <string.h>
#include "hle.h"

#define RPC_DISC_STREAM 0x65686577u

static uint32_t cd_last_posn;
static uint64_t cd_reads, cd_sectors;

static void recv_word(uint32_t recv, uint32_t rsize, uint32_t v)
{
    if (recv && rsize >= 4) {
        memset(gptr(recv, rsize), 0, rsize);
        sw_(recv, v);
    }
}

static int srv_disc_stream(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                           uint32_t rsize)
{
    (void)mode; (void)ssize;
    switch (fno) {
    case 0:
        LOG("disc stream: Init(%u channels)\n", send ? gw(send) : 0);
        hle_milestone("disc stream RPC bound and initialised");
        recv_word(recv, rsize, 1);
        return 0;
    case 1: {
        uint32_t ch = gw(send), buf = gw(send + 4), lsn = gw(send + 8), n = gw(send + 12);
        cd_reads++;
        cd_sectors += n;
        hle_safe_point(c, 4000 + (uint64_t)n * 2048 / 8);
        VLOG("disc stream: Read(ch %u, buf %08x, lsn %u (%s), %u sectors)\n", ch, buf, lsn, hle_disc_name_of_lsn(lsn), n);
        if (n > 0x4000) hle_die(c, "disc stream read of %u sectors", n);
        if (hle_disc_read_lsn(lsn, n, gptr(buf, n * 2048)) != 0)
            LOG("disc stream: read lsn %u x%u touches sectors outside any file (zero-filled)\n", lsn, n);
        cd_last_posn = lsn + n;
        if (cd_reads == 1) hle_milestone("first disc stream sector read");
        recv_word(recv, rsize, 1);
        return 0;
    }
    case 2: case 3:
        recv_word(recv, rsize, 0);
        return 0;
    case 4:
        recv_word(recv, rsize, cd_last_posn);
        return 0;
    default:
        LOG("disc stream: unknown fno %u\n", fno);
        recv_word(recv, rsize, 0);
        return 0;
    }
}

/* ---- the audio module (AUDVAG.IRX, RPC 0xBCFF; EE side: the audio thread at 0x40D060 calls fno 0 with a 20-byte
   request and a 1472-byte reply). Silent responder (experiment): the reply is cleared; every fno is counted. */
#define RPC_AUDVAG 0x0000BCFFu
static uint64_t aud_calls[16];
static int srv_audvag(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                      uint32_t rsize)
{
    (void)c; (void)mode; (void)send;
    if (!aud_calls[fno & 15]++)
        LOG("audio module: first call fno %u (send %u bytes, reply %u bytes) - silent responder\n", fno, ssize, rsize);
    if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
    return 0;
}

/* ---- the sound library module (SCREAM.IRX: RPC 0x123456 commands, 0x123457 second channel). Silent responder
   (experiment). The EE side (0x3177D8) sends a command asynchronously with a 12-byte reply buffer and polls
   (0x315FD0) until the reply reads {0xFFFFFFFF, result, 0xFFFFFFFF}; the result is returned to the caller. Here every
   command "succeeds" with result SND_RESULT (non-zero: handles of loaded banks / started sounds look valid). */
#define SND_RESULT 1u
#define RPC_SND 0x00123456u
#define RPC_SND2 0x00123457u
static uint64_t snd_calls;
static uint8_t snd_seen[2][256];
static uint64_t snd_count[2][256];
static int srv_snd(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                   uint32_t rsize, int ch)
{
    (void)c; (void)mode;
    snd_calls++;
    snd_count[ch][fno & 255]++;
    if (!snd_seen[ch][fno & 255]) {
        snd_seen[ch][fno & 255] = 1;
        LOG("sound module %s: first fno %u (send %u bytes: %08x %08x %08x, reply %u bytes) - silent responder\n",
            ch ? "ch2" : "cmd", fno, ssize, send && ssize >= 4 ? gw(send) : 0, send && ssize >= 8 ? gw(send + 4) : 0,
            send && ssize >= 12 ? gw(send + 8) : 0, rsize);
    }
    if (recv && rsize) {
        memset(gptr(recv, rsize), 0, rsize);
        if (rsize >= 12) { sw_(recv, 0xFFFFFFFFu); sw_(recv + 4, SND_RESULT); sw_(recv + 8, 0xFFFFFFFFu); }
    }
    return 0;
}
static int srv_snd1(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                    uint32_t rsize) { return srv_snd(c, fno, mode, send, ssize, recv, rsize, 0); }
static int srv_snd2(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                    uint32_t rsize) { return srv_snd(c, fno, mode, send, ssize, recv, rsize, 1); }

int bully_iop_snd_init(void);            /* iop/iop_snd_unit.c: the translated modules (build define HLE_IOP_SND) */
void bully_iop_snd_report(void);

void hle_pack_iop_init(void)
{
    hle_rpc_register(RPC_DISC_STREAM, srv_disc_stream);
    if (bully_iop_snd_init()) return;   /* the game's own sound modules answer 0x123456 / 0x123457 / 0xBCFF */
    hle_rpc_register(RPC_SND, srv_snd1);
    hle_rpc_register(RPC_SND2, srv_snd2);
    hle_rpc_register(RPC_DISC_STREAM, srv_disc_stream);
    hle_rpc_register(RPC_AUDVAG, srv_audvag);
}

void hle_pack_iop_report(void)
{
    bully_iop_snd_report();
    LOG("IOP: disc stream reads %llu (%llu sectors), audio module calls fno0 %llu fno1 %llu\n",
        (unsigned long long)cd_reads, (unsigned long long)cd_sectors, (unsigned long long)aud_calls[0],
        (unsigned long long)aud_calls[1]);
    LOG("IOP: sound module calls %llu\n", (unsigned long long)snd_calls);
    for (int ch = 0; ch < 2; ch++)
        for (int f = 0; f < 256; f++)
            if (snd_count[ch][f]) LOG("  sound %s fno %3d: %llu calls\n", ch ? "ch2" : "cmd", f,
                                      (unsigned long long)snd_count[ch][f]);
}
