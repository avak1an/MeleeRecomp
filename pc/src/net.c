/**
 * @file net.c
 * Online play between two copies of the port: direct IP over UDP, input
 * delay lockstep.
 *
 * Both sides run the same deterministic game (see determinism.c) from the
 * same seed, without a memory card and with the same rules, so the only
 * thing that has to cross the network is each player's controller state.
 * The host is player 1, the joiner player 2. A controller sample taken on
 * frame f is played on frame f + delay on both machines; a frame only
 * runs once both samples for it are known, so the slower or later side
 * holds the other for as long as it takes.
 *
 * Every packet repeats the sender's recent samples, so a lost packet costs
 * nothing as long as one of the next few arrives. Each sample carries the
 * sender's random generator state at that frame: the two must agree, and
 * a disagreement is reported as a desync with its frame.
 *
 *   --host PORT          wait for a player on UDP port PORT
 *   --join ADDRESS:PORT  connect to a host
 *   --delay N            frames of input delay (host decides, default 2)
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
#define NET_PROTOCOL 1
#define NET_HISTORY 16   /* samples repeated in every packet */
#define NET_WINDOW 1024  /* ring of samples per player */
#define NET_TIMEOUT_MS 15000

enum { MSG_HELLO = 1, MSG_CONFIG = 2, MSG_READY = 3, MSG_INPUT = 4, MSG_BYE = 5 };

typedef struct {
    u16 button;
    s8 stick_x, stick_y, sub_x, sub_y;
    u8 trigger_l, trigger_r, analog_a, analog_b;
} NetPad; /* 10 bytes */

typedef struct {
    u32 seq;
    u32 rng; /* the sender's random generator state when it ran frame `seq - delay` ... see exchange */
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
    u32 ack; /* highest remote seq received in order */
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
static u32 next_seq;          /* the next exchange's frame number */
static u32 remote_high;       /* highest remote seq seen + 1 */
static u32 stall_frames, desyncs;
static int desync_reported;

extern u32* seed_ptr; /* src/sysdolphin/baselib/random.c */

int pc_net_active(void)
{
    return active;
}

int pc_net_player(void)
{
    return is_host ? 0 : 1;
}

static void net_fail(const char* what)
{
    fprintf(stderr, "[pc] net: %s (error %d)\n", what, WSAGetLastError());
    pc_exit(9);
}

static void send_packet(u8 type, u32 seed, const NetWireSample* samples, u16 count)
{
    u8 buf[sizeof(NetHeader) + NET_HISTORY * sizeof(NetWireSample)];
    NetHeader* h = (NetHeader*) buf;
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
    sendto(sock, (const char*) buf, (int) (sizeof(*h) + count * sizeof(NetWireSample)), 0, (const struct sockaddr*) &peer,
           sizeof(peer));
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
    FD_ZERO(&set);
    FD_SET(sock, &set);
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    if (select(0, &set, NULL, NULL, &tv) <= 0) {
        return 0;
    }
    n = recvfrom(sock, (char*) buf, sizeof(buf), 0, (struct sockaddr*) &from, &from_len);
    if (n < (int) sizeof(NetHeader)) {
        return 0;
    }
    memcpy(out, buf, sizeof(*out));
    if (out->magic != NET_MAGIC || out->protocol != NET_PROTOCOL) {
        return 0;
    }
    if (!have_peer) {
        if (!is_host || out->type != MSG_HELLO) {
            return 0;
        }
        peer = from; /* the first caller is the opponent */
        have_peer = 1;
    } else if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) {
        return 0;
    }
    if (out->type == MSG_INPUT) {
        u16 i;
        if (n < (int) (sizeof(NetHeader) + out->count * sizeof(NetWireSample)) || out->count > NET_HISTORY) {
            return 0;
        }
        for (i = 0; i < out->count; i++) {
            NetWireSample w;
            NetSample* s;
            memcpy(&w, buf + sizeof(NetHeader) + i * sizeof(NetWireSample), sizeof(w));
            s = &remote_ring[w.seq % NET_WINDOW];
            if (!s->valid || s->seq != w.seq) {
                s->seq = w.seq;
                s->rng = 0;
                s->pad = w.pad;
                s->valid = 1;
            }
            if (w.rng != 0) {
                s->rng = w.rng; /* filled in once the sender has run that frame */
            }
            if (w.seq + 1 > remote_high) {
                remote_high = w.seq + 1;
            }
        }
    }
    return out->type;
}

/// Opens the socket and agrees on the session before the game boots.
/// Returns the seed both sides use.
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
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    is_host = pc_config.net_host_port != 0;
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
            seed = h.seed;
            input_delay = h.delay;
            send_packet(MSG_READY, 0, NULL, 0);
            /* keep answering for a moment so the host surely hears it */
            if (now - start > 0 && seed != 0) {
                int k;
                for (k = 0; k < 3; k++) {
                    Sleep(10);
                    send_packet(MSG_READY, 0, NULL, 0);
                }
                break;
            }
        }
        if (is_host && (type == MSG_READY || type == MSG_INPUT)) {
            break;
        }
        if (!is_host && now - start > NET_TIMEOUT_MS * 4) {
            fprintf(stderr, "[pc] net: no answer from the host\n");
            pc_exit(9);
        }
    }
    pc_config.seed = seed;
    pc_config.no_card = true; /* online play never reads or writes a save */
    active = 1;
    fprintf(stderr, "[pc] net: connected as player %d, seed %08x, input delay %u\n", pc_net_player() + 1, seed, input_delay);
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
    first = newest + 1 > NET_HISTORY ? newest + 1 - NET_HISTORY : 0;
    for (s = first; s <= newest; s++) {
        const NetSample* ls = &local_ring[s % NET_WINDOW];
        if (ls->valid && ls->seq == s) {
            w[n].seq = s;
            w[n].rng = ls->rng;
            w[n].pad = ls->pad;
            n++;
        }
    }
    send_packet(MSG_INPUT, 0, w, n);
}

/// One frame of lockstep: `local` is this machine's controller now; `out`
/// receives both players' controllers for the frame that runs now.
void pc_net_exchange(const PADStatus* local, PADStatus out[2])
{
    u32 seq = next_seq, target = seq + input_delay;
    NetSample* mine = &local_ring[target % NET_WINDOW];
    const NetSample *a, *b;
    NetHeader h;
    DWORD start = GetTickCount(), last_send = 0, last_pump = start;
    int waited = 0;
    static const NetPad neutral;

    mine->seq = target;
    mine->rng = 0;
    pad_to_net(local, &mine->pad);
    mine->valid = 1;
    /* the frames before the first delayed sample arrive are neutral */
    if (seq < input_delay) {
        NetSample* early = &local_ring[seq % NET_WINDOW];
        if (!early->valid || early->seq != seq) {
            early->seq = seq;
            early->pad = neutral;
            early->rng = 0;
            early->valid = 1;
        }
    }
    /* this frame's random generator state rides with the sample played now */
    local_ring[seq % NET_WINDOW].rng = seed_ptr != NULL ? *seed_ptr : 0;
    send_inputs();
    last_send = GetTickCount();

    for (;;) {
        const NetSample* r = &remote_ring[seq % NET_WINDOW];
        DWORD now;
        if (seq < input_delay || (r->valid && r->seq == seq)) {
            break;
        }
        waited = 1;
        recv_packet(2, &h);
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
        if (h.type == MSG_BYE && h.magic == NET_MAGIC) {
            fprintf(stderr, "[pc] net: the other player left\n");
            pc_exit(0);
        }
        if (now - start > NET_TIMEOUT_MS) {
            fprintf(stderr, "[pc] net: no input from the other player for %d s at frame %u\n", NET_TIMEOUT_MS / 1000, seq);
            pc_exit(9);
        }
    }
    /* drain whatever else is waiting, so the ring stays ahead */
    while (recv_packet(0, &h) != 0) {
    }
    if (waited) {
        stall_frames++;
    }
    /* The other side's random state for a frame is only known once it has
     * run that frame, so an older frame is compared: both samples have been
     * repeated in several packets by now. */
    if (seq >= 8 + input_delay) {
        const NetSample* ca = &local_ring[(seq - 8) % NET_WINDOW];
        const NetSample* cb = &remote_ring[(seq - 8) % NET_WINDOW];
        if (ca->valid && cb->valid && ca->seq == seq - 8 && cb->seq == seq - 8 && ca->rng != 0 && cb->rng != 0 &&
            ca->rng != cb->rng && !desync_reported)
        {
            desyncs++;
            desync_reported = 1;
            fprintf(stderr, "[pc] net: DESYNC at frame %u: random state %08x here, %08x there\n", seq - 8, ca->rng,
                    cb->rng);
        }
    }
    a = &local_ring[seq % NET_WINDOW];
    b = &remote_ring[seq % NET_WINDOW];
    if (seq >= input_delay) {
        net_to_pad(is_host ? &a->pad : &b->pad, &out[0]);
        net_to_pad(is_host ? &b->pad : &a->pad, &out[1]);
    } else {
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
    closesocket(sock);
    WSACleanup();
    active = 0;
}
