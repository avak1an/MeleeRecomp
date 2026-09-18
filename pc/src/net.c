/**
 * @file net.c
 * Online play between two copies of the port: direct IP over UDP, input
 * delay plus rollback.
 *
 * Both sides run the same deterministic game (see determinism.c) from the
 * same seed, without a memory card and with the same rules, so the only
 * thing that has to cross the network is each player's controller state.
 * The host is player 1, the joiner player 2. A controller sample taken on
 * frame f is played on frame f + delay on both machines.
 *
 * Rollback: when the other player's sample for a frame has not arrived, the
 * frame runs anyway with a prediction (their last known controller state),
 * up to NET_MAX_PREDICT frames ahead of the last confirmed one. A snapshot
 * of the whole game state (state.c) is kept for each of those frames. When
 * the real sample arrives and differs from what was played, the snapshot of
 * that frame is restored and the frames since are simulated again with the
 * right inputs, without rendering, sound output or pacing, all inside one
 * displayed frame. With a connection faster than the input delay nothing is
 * ever predicted and this is plain lockstep; --lockstep forces that.
 *
 * Every packet repeats the sender's recent samples, so a lost packet costs
 * nothing as long as one of the next few arrives. A sample also carries the
 * sender's random generator state once that frame is final on its side: the
 * two must agree, and a disagreement is reported as a desync with its frame.
 *
 *   --host PORT          wait for a player on UDP port PORT
 *   --join ADDRESS:PORT  connect to a host
 *   --delay N            frames of input delay (host decides, default 2)
 *   --lockstep           never predict: wait for every sample
 *   --net-lag MS[,JITTER]  testing: hold outgoing packets MS (+ up to JITTER) ms
 */
#include "pc_runtime.h"

#include <dolphin/pad.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define NET_MAGIC 0x4D4C4E50u /* MLNP */
#define NET_PROTOCOL 2
#define NET_HISTORY 24     /* samples repeated in every packet */
#define NET_WINDOW 1024    /* ring of samples per player */
#define NET_TIMEOUT_MS 15000
#define NET_MAX_PREDICT 7  /* frames run ahead of the last confirmed one */
#define NET_SNAPSHOTS 8    /* NET_MAX_PREDICT + 1 */

enum { MSG_HELLO = 1, MSG_CONFIG = 2, MSG_READY = 3, MSG_INPUT = 4, MSG_BYE = 5 };

typedef struct {
    u16 button;
    s8 stick_x, stick_y, sub_x, sub_y;
    u8 trigger_l, trigger_r, analog_a, analog_b;
} NetPad; /* 10 bytes */

typedef struct {
    u32 seq;
    u32 rng; /* the owner's random generator state when it ran this frame; 0 until final */
    NetPad pad;
    u16 valid;
} NetSample;

#pragma pack(push, 1)
typedef struct {
    u32 magic;
    u8 protocol, type;
    u16 count;
    u32 seed;
    u32 delay;
    u32 ack;
} NetHeader;

typedef struct {
    u32 seq;
    u32 rng;
    NetPad pad;
} NetWireSample;
#pragma pack(pop)

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in peer;
static int have_peer;
static int active, is_host;
static u32 input_delay = 2;
static NetSample local_ring[NET_WINDOW], remote_ring[NET_WINDOW];
static u32 next_seq;    /* the next exchange's frame number; part of the game state (rolls back) */
static u32 remote_high; /* highest remote seq seen + 1 */
static u32 stall_frames, desyncs;
static int desync_reported;

/* rollback */
static int rollback_on = 1;
static NetPad played_remote[NET_WINDOW]; /* what each frame was run with for the other player */
static u8 played_predicted[NET_WINDOW];
static u32 first_unconfirmed; /* every frame below it ran with the real remote sample */
static u32 checked_until;     /* desync check progress */
static u32 resim_until;       /* while next_seq is below it, frames are being simulated again */
static void* snaps[NET_SNAPSHOTS];
static size_t snap_cap[NET_SNAPSHOTS];
static u32 snap_seq[NET_SNAPSHOTS];
static u8 snap_valid[NET_SNAPSHOTS];
static u32 rollbacks, rollback_frames, rollback_max, predicted_frames;

/* testing: delayed sends */
typedef struct {
    DWORD due;
    int len;
    u8 data[sizeof(NetHeader) + NET_HISTORY * sizeof(NetWireSample)];
} HeldPacket;
static HeldPacket held[256];
static int held_head, held_count;

extern u32* seed_ptr; /* src/sysdolphin/baselib/random.c */
extern int pc_resimulating;
extern volatile int pc_watchdog_hold;

int pc_net_active(void)
{
    return active;
}

int pc_net_player(void)
{
    return is_host ? 0 : 1;
}

void pc_net_register_net_state(void)
{
    pc_state_register(&next_seq, sizeof(next_seq), "net frame");
}

static void net_fail(const char* what)
{
    fprintf(stderr, "[pc] net: %s (error %d)\n", what, WSAGetLastError());
    pc_exit(9);
}

static void flush_held(void)
{
    DWORD now = GetTickCount();
    while (held_count > 0 && (int) (now - held[held_head].due) >= 0) {
        sendto(sock, (const char*) held[held_head].data, held[held_head].len, 0, (const struct sockaddr*) &peer, sizeof(peer));
        held_head = (held_head + 1) % 256;
        held_count--;
    }
}

static void send_packet(u8 type, u32 seed, const NetWireSample* samples, u16 count)
{
    u8 buf[sizeof(NetHeader) + NET_HISTORY * sizeof(NetWireSample)];
    NetHeader* h = (NetHeader*) buf;
    int len = (int) (sizeof(*h) + count * sizeof(NetWireSample));
    if (!have_peer) {
        return;
    }
    h->magic = NET_MAGIC;
    h->protocol = NET_PROTOCOL;
    h->type = type;
    h->count = count;
    h->seed = seed;
    h->delay = input_delay;
    h->ack = remote_high;
    if (count != 0) {
        memcpy(buf + sizeof(*h), samples, count * sizeof(NetWireSample));
    }
    if (pc_config.net_lag > 0 && type == MSG_INPUT && held_count < 256) {
        HeldPacket* p = &held[(held_head + held_count) % 256];
        p->due = GetTickCount() + (DWORD) pc_config.net_lag +
                 (pc_config.net_jitter > 0 ? (DWORD) (rand() % (pc_config.net_jitter + 1)) : 0);
        p->len = len;
        memcpy(p->data, buf, (size_t) len);
        held_count++;
        return;
    }
    sendto(sock, (const char*) buf, len, 0, (const struct sockaddr*) &peer, sizeof(peer));
}

/// Receives one packet if there is one within `wait_ms`; returns its type
/// (0 = none). Input samples are stored as they come.
static int recv_packet(int wait_ms, NetHeader* out)
{
    u8 buf[2048];
    struct sockaddr_in from;
    int from_len = sizeof(from), n;
    fd_set set;
    struct timeval tv;
    flush_held();
    FD_ZERO(&set);
    FD_SET(sock, &set);
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    out->type = 0;
    if (select(0, &set, NULL, NULL, &tv) <= 0) {
        return 0;
    }
    n = recvfrom(sock, (char*) buf, sizeof(buf), 0, (struct sockaddr*) &from, &from_len);
    if (n < (int) sizeof(NetHeader)) {
        return 0;
    }
    memcpy(out, buf, sizeof(*out));
    if (out->magic != NET_MAGIC || out->protocol != NET_PROTOCOL) {
        out->type = 0;
        return 0;
    }
    if (!have_peer) {
        if (!is_host || out->type != MSG_HELLO) {
            out->type = 0;
            return 0;
        }
        peer = from; /* the first caller is the opponent */
        have_peer = 1;
    } else if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) {
        out->type = 0;
        return 0;
    }
    if (out->type == MSG_INPUT) {
        u16 i;
        if (n < (int) (sizeof(NetHeader) + out->count * sizeof(NetWireSample)) || out->count > NET_HISTORY) {
            out->type = 0;
            return 0;
        }
        for (i = 0; i < out->count; i++) {
            NetWireSample w;
            NetSample* s;
            memcpy(&w, buf + sizeof(NetHeader) + i * sizeof(NetWireSample), sizeof(w));
            s = &remote_ring[w.seq % NET_WINDOW];
            if (!s->valid || s->seq != w.seq) {
                if (w.seq + NET_WINDOW / 2 < remote_high) {
                    continue; /* far too old: its slot belongs to a newer frame */
                }
                s->seq = w.seq;
                s->rng = 0;
                s->pad = w.pad;
                s->valid = 1;
            }
            if (w.rng != 0) {
                s->rng = w.rng; /* filled in once that frame is final on the sender's side */
            }
            if (w.seq + 1 > remote_high) {
                remote_high = w.seq + 1;
            }
        }
    }
    return out->type;
}

/// Opens the socket and agrees on the session before the game boots.
void pc_net_start(void)
{
    WSADATA wsa;
    struct sockaddr_in local;
    NetHeader h;
    DWORD start, last_send = 0;
    u32 seed = 0;
    if (pc_config.net_host_port == 0 && pc_config.net_join == NULL) {
        return;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        net_fail("Winsock did not start");
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        net_fail("no UDP socket");
    }
    {
        /* Windows turns an ICMP "port unreachable" (the joiner calling before
         * the host listens) into a connection-reset error on later receives;
         * a UDP session has no use for that */
        BOOL off = FALSE;
        DWORD ignored = 0;
        WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), NULL, 0, &ignored, NULL, NULL);
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    is_host = pc_config.net_host_port != 0;
    rollback_on = !pc_config.net_lockstep;
    if (pc_config.net_delay > 0 && pc_config.net_delay <= 15) {
        input_delay = (u32) pc_config.net_delay;
    }
    if (is_host) {
        LARGE_INTEGER now;
        local.sin_port = htons((u_short) pc_config.net_host_port);
        if (bind(sock, (const struct sockaddr*) &local, sizeof(local)) != 0) {
            net_fail("cannot listen on that port");
        }
        QueryPerformanceCounter(&now);
        seed = pc_config.seed != 0 ? pc_config.seed : ((u32) now.LowPart | 1u);
        fprintf(stderr, "[pc] net: hosting on UDP port %d, waiting for a player (input delay %u)\n",
                pc_config.net_host_port, input_delay);
    } else {
        char host[256];
        const char* colon = strrchr(pc_config.net_join, ':');
        struct addrinfo hints, *res = NULL;
        if (colon == NULL || (size_t) (colon - pc_config.net_join) >= sizeof(host)) {
            fprintf(stderr, "[pc] net: --join needs ADDRESS:PORT\n");
            pc_exit(2);
        }
        memcpy(host, pc_config.net_join, (size_t) (colon - pc_config.net_join));
        host[colon - pc_config.net_join] = 0;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL) {
            net_fail("cannot resolve the host's address");
        }
        memcpy(&peer, res->ai_addr, sizeof(peer));
        freeaddrinfo(res);
        have_peer = 1;
        local.sin_port = 0;
        if (bind(sock, (const struct sockaddr*) &local, sizeof(local)) != 0) {
            net_fail("cannot open a local port");
        }
        fprintf(stderr, "[pc] net: joining %s\n", pc_config.net_join);
    }
    /* Handshake: the joiner says HELLO until the host answers CONFIG (seed
     * and delay); the joiner answers READY until inputs flow. */
    start = GetTickCount();
    pc_watchdog_hold = 1; /* waiting for a person is not a hang */
    for (;;) {
        DWORD now = GetTickCount();
        int type;
        if (now - last_send >= 100) {
            last_send = now;
            if (!is_host) {
                send_packet(seed == 0 ? MSG_HELLO : MSG_READY, 0, NULL, 0);
            } else if (have_peer) {
                send_packet(MSG_CONFIG, seed, NULL, 0);
            }
        }
        type = recv_packet(20, &h);
        if (!is_host && type == MSG_CONFIG) {
            int k;
            seed = h.seed;
            input_delay = h.delay;
            for (k = 0; k < 4; k++) { /* so the host surely hears it */
                send_packet(MSG_READY, 0, NULL, 0);
                Sleep(10);
            }
            break;
        }
        if (is_host && (type == MSG_READY || type == MSG_INPUT)) {
            break;
        }
        if (is_host && now - start > 10u * 60u * 1000u) {
            fprintf(stderr, "[pc] net: nobody joined in 10 minutes\n");
            pc_exit(9);
        }
        if (!is_host && now - start > NET_TIMEOUT_MS * 4) {
            fprintf(stderr, "[pc] net: no answer from the host\n");
            pc_exit(9);
        }
    }
    pc_watchdog_hold = 0;
    pc_config.seed = seed;
    pc_config.no_card = true; /* online play never reads or writes a save */
    active = 1;
    fprintf(stderr, "[pc] net: connected as player %d, seed %08x, input delay %u, %s\n", pc_net_player() + 1, seed,
            input_delay, rollback_on ? "rollback" : "lockstep");
}

static void pad_to_net(const PADStatus* st, NetPad* n)
{
    n->button = st->button;
    n->stick_x = st->stickX;
    n->stick_y = st->stickY;
    n->sub_x = st->substickX;
    n->sub_y = st->substickY;
    n->trigger_l = st->triggerLeft;
    n->trigger_r = st->triggerRight;
    n->analog_a = st->analogA;
    n->analog_b = st->analogB;
}

static void net_to_pad(const NetPad* n, PADStatus* st)
{
    memset(st, 0, sizeof(*st));
    st->button = n->button;
    st->stickX = n->stick_x;
    st->stickY = n->stick_y;
    st->substickX = n->sub_x;
    st->substickY = n->sub_y;
    st->triggerLeft = n->trigger_l;
    st->triggerRight = n->trigger_r;
    st->analogA = n->analog_a;
    st->analogB = n->analog_b;
}

static void send_inputs(void)
{
    NetWireSample w[NET_HISTORY];
    u32 newest = next_seq + input_delay, first, s;
    u16 n = 0;
    if (resim_until > next_seq) {
        newest = resim_until + input_delay; /* the samples beyond the replayed frame are already recorded */
    }
    first = newest + 1 > NET_HISTORY ? newest + 1 - NET_HISTORY : 0;
    for (s = first; s <= newest; s++) {
        const NetSample* ls = &local_ring[s % NET_WINDOW];
        if (ls->valid && ls->seq == s) {
            w[n].seq = s;
            /* a frame's random state is final once no rollback can reach it */
            w[n].rng = s < first_unconfirmed ? ls->rng : 0;
            w[n].pad = ls->pad;
            n++;
        }
    }
    send_packet(MSG_INPUT, 0, w, n);
}

static void check_desync(void)
{
    while (checked_until < first_unconfirmed) {
        const NetSample* a = &local_ring[checked_until % NET_WINDOW];
        const NetSample* b = &remote_ring[checked_until % NET_WINDOW];
        if (checked_until >= input_delay && a->valid && b->valid && a->seq == checked_until && b->seq == checked_until) {
            if (b->rng == 0 || a->rng == 0) {
                if (next_seq - checked_until < 120) {
                    return; /* the other side has not called it final yet */
                }
            } else if (a->rng != b->rng && !desync_reported) {
                desyncs++;
                desync_reported = 1;
                fprintf(stderr, "[pc] net: DESYNC at frame %u: random state %08x here, %08x there\n", checked_until,
                        a->rng, b->rng);
            }
        }
        checked_until++;
    }
}

/// Marks frames as confirmed while the real remote samples match what was
/// played; on the first one that does not, restores that frame's snapshot
/// (and does not return: execution resumes in pc_net_frame's save).
static void confirm_or_roll_back(void)
{
    while (first_unconfirmed < next_seq) {
        u32 q = first_unconfirmed;
        const NetSample* r = &remote_ring[q % NET_WINDOW];
        if (q >= input_delay) {
            if (!(r->valid && r->seq == q)) {
                break; /* not here yet */
            }
            if (played_predicted[q % NET_WINDOW] && memcmp(&played_remote[q % NET_WINDOW], &r->pad, sizeof(NetPad)) != 0) {
                int slot = (int) (q % NET_SNAPSHOTS);
                if (!snap_valid[slot] || snap_seq[slot] != q) {
                    fprintf(stderr, "[pc] net: frame %u was mispredicted and its snapshot is gone; the match will desync\n", q);
                    played_predicted[q % NET_WINDOW] = 0;
                    first_unconfirmed++;
                    continue;
                }
                if (resim_until < next_seq) {
                    resim_until = next_seq;
                }
                rollbacks++;
                rollback_frames += next_seq - q;
                if (next_seq - q > rollback_max) {
                    rollback_max = next_seq - q;
                }
                pc_resimulating = 1;
                pc_state_load(snaps[slot]); /* next_seq is q again */
                return;
            }
            played_predicted[q % NET_WINDOW] = 0;
        }
        first_unconfirmed++;
    }
}

/// Once per retrace, before the controllers are read for frame next_seq.
void pc_net_frame(void)
{
    NetHeader h;
    int slot, r;
    size_t need;
    if (!active) {
        return;
    }
    while (recv_packet(0, &h) != 0) {
    }
    if (!rollback_on) {
        return;
    }
    {
        static int inited;
        if (!inited) {
            inited = 1;
            pc_state_init();
        }
    }
    confirm_or_roll_back();
    if (pc_resimulating && next_seq >= resim_until) {
        pc_resimulating = 0;
        resim_until = 0;
    }
    slot = (int) (next_seq % NET_SNAPSHOTS);
    need = pc_state_size();
    if (need > snap_cap[slot]) {
        free(snaps[slot]);
        snaps[slot] = malloc(need + 65536);
        snap_cap[slot] = snaps[slot] != NULL ? need + 65536 : 0;
    }
    if (snaps[slot] == NULL) {
        snap_valid[slot] = 0;
        return;
    }
    snap_valid[slot] = 0;
    r = pc_state_save(snaps[slot], snap_cap[slot]);
    if (r == 2) {
        /* resumed by a rollback: this is frame next_seq again, about to be
         * run with the corrected sample; its snapshot is still good */
        snap_valid[next_seq % NET_SNAPSHOTS] = 1;
        return;
    }
    snap_seq[slot] = next_seq;
    snap_valid[slot] = (u8) (r == 1);
}

/// One frame: `local` is this machine's controller now; `out` receives both
/// players' controllers for the frame that runs now.
void pc_net_exchange(const PADStatus* local, PADStatus* out)
{
    u32 seq = next_seq, target = seq + input_delay;
    NetSample* mine = &local_ring[target % NET_WINDOW];
    const NetSample* a;
    const NetPad* theirs;
    NetHeader h;
    DWORD start = GetTickCount(), last_send = 0, last_pump = start;
    int waited = 0;
    static const NetPad neutral;

    /* record this machine's sample once: a replayed frame keeps the one it sent */
    if (!(mine->valid && mine->seq == target)) {
        mine->seq = target;
        mine->rng = 0;
        pad_to_net(local, &mine->pad);
        mine->valid = 1;
    }
    if (seq < input_delay) {
        NetSample* early = &local_ring[seq % NET_WINDOW];
        if (!early->valid || early->seq != seq) {
            early->seq = seq;
            early->pad = neutral;
            early->valid = 1;
        }
    }
    local_ring[seq % NET_WINDOW].rng = seed_ptr != NULL ? *seed_ptr : 0;
    send_inputs();
    last_send = GetTickCount();

    for (;;) {
        const NetSample* r = &remote_ring[seq % NET_WINDOW];
        DWORD now;
        if (seq < input_delay || (r->valid && r->seq == seq)) {
            break;
        }
        if (rollback_on && seq - first_unconfirmed < NET_MAX_PREDICT) {
            break; /* run it on a prediction */
        }
        waited = 1;
        recv_packet(2, &h);
        if (rollback_on) {
            confirm_or_roll_back(); /* may not return */
        }
        now = GetTickCount();
        if (now - last_send >= 8) {
            send_inputs();
            last_send = now;
        }
        if (now - last_pump >= 16) {
            extern int pc_window_pump(void);
            last_pump = now;
            if (!pc_window_pump()) {
                send_packet(MSG_BYE, 0, NULL, 0);
                pc_exit(0);
            }
        }
        if (h.type == MSG_BYE) {
            fprintf(stderr, "[pc] net: the other player left\n");
            pc_exit(0);
        }
        if (now - start > NET_TIMEOUT_MS) {
            fprintf(stderr, "[pc] net: no input from the other player for %d s at frame %u\n", NET_TIMEOUT_MS / 1000, seq);
            pc_exit(9);
        }
    }
    if (waited) {
        stall_frames++;
    }
    a = &local_ring[seq % NET_WINDOW];
    if (seq < input_delay) {
        theirs = &neutral;
        played_predicted[seq % NET_WINDOW] = 0;
    } else {
        const NetSample* r = &remote_ring[seq % NET_WINDOW];
        if (r->valid && r->seq == seq) {
            theirs = &r->pad;
            played_predicted[seq % NET_WINDOW] = 0;
        } else {
            /* their last known controller state */
            const NetSample* last = remote_high > 0 ? &remote_ring[(remote_high - 1) % NET_WINDOW] : NULL;
            theirs = last != NULL && last->valid && last->seq == remote_high - 1 && last->seq < seq ? &last->pad : &neutral;
            played_predicted[seq % NET_WINDOW] = 1;
            predicted_frames++;
        }
        played_remote[seq % NET_WINDOW] = *theirs;
    }
    if (!rollback_on) {
        first_unconfirmed = seq + 1;
    }
    check_desync();
    net_to_pad(is_host ? &a->pad : theirs, &out[0]);
    net_to_pad(is_host ? theirs : &a->pad, &out[1]);
    if (seq < input_delay) {
        net_to_pad(&neutral, &out[0]);
        net_to_pad(&neutral, &out[1]);
    }
    next_seq++;
}

void pc_net_close(void)
{
    if (!active) {
        return;
    }
    send_packet(MSG_BYE, 0, NULL, 0);
    fprintf(stderr, "[pc] net: %u frames, %u waited for the other player, %u desync(s)\n", next_seq, stall_frames, desyncs);
    if (rollback_on) {
        fprintf(stderr, "[pc] net: %u frame(s) ran on a prediction, %u rollback(s), %.1f frames on average, %u at most\n",
                predicted_frames, rollbacks, rollbacks != 0 ? (double) rollback_frames / rollbacks : 0.0, rollback_max);
    }
    closesocket(sock);
    WSACleanup();
    active = 0;
}
