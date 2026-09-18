/**
 * @file ax.c
 * Software replacement for the AX sound driver and the AI streaming
 * interface: a 64-voice mixer that renders 5 ms frames (160 samples at
 * 32 kHz) from ADPCM/PCM samples in the emulated ARAM, driven from the
 * video retrace, and plays the result through waveOut.
 *
 * Voice parameter blocks (AXPB) are the console's DSP structures. The game
 * reads and writes a few of their 16-bit high/low pairs as one 32-bit word
 * (sample addresses, the sample-rate ratio), so on PC every such pair is
 * kept as a native 32-bit value in place; the sound data swappers in
 * synth.c produce that layout and this file only ever accesses the pairs
 * through the PAIR() macro.
 */
#include "pc_runtime.h"

#include <dolphin/ax.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#define AX_RATE 32000
#define AX_FRAME 160 /* samples per 5 ms mixer frame */
#define PAIR(hi) (*(u32*) &(hi))

#define AX_PB_STATE_STOP 0
#define AX_PB_STATE_RUN 1
#define AX_PB_FORMAT_ADPCM 0
#define AX_PB_FORMAT_PCM16 10
#define AX_PB_FORMAT_PCM8 25

extern u8* pc_aram_data(void);
extern u32 pc_aram_size(void);

/* --- voice pool ------------------------------------------------------- */

static AXVPB voices[AX_MAX_VOICES];
static u8 voice_used[AX_MAX_VOICES];
static void (*frame_callback)(void);
static int ax_ready;
/* the auxiliary effect buses: each voice sends a share of itself to bus A
 * (the game's reverb) and bus B (its delay); the registered effect
 * processes the bus in place and the result is added to the main mix */
typedef void (*AuxCallback)(void*, void*);
static AuxCallback aux_cb[2];
static void* aux_ctx[2];
static s32 aux_buf[2][3][AX_FRAME]; /* left, right, surround; consecutive */

static double ax_pending;

void pc_ax_register_state(void)
{
    pc_state_register(voices, sizeof(voices), "ax voices");
    pc_state_register(voice_used, sizeof(voice_used), "ax voice used");
    pc_state_register(aux_cb, sizeof(aux_cb), "ax aux callbacks");
    pc_state_register(aux_ctx, sizeof(aux_ctx), "ax aux contexts");
    pc_state_register(&ax_pending, sizeof(ax_pending), "ax pending samples");
    pc_state_register(&ax_ready, sizeof(ax_ready), "ax ready");
    pc_state_register(&frame_callback, sizeof(frame_callback), "ax frame callback");
}
static int aux_off = -1;             /* MELEE_AX_NOAUX=1: dry, as before */

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    int i, steal = -1;
    for (i = 0; i < AX_MAX_VOICES; i++) {
        if (!voice_used[i]) {
            break;
        }
    }
    if (i == AX_MAX_VOICES) {
        /* no free voice: take the lowest-priority one below the request */
        for (i = 0; i < AX_MAX_VOICES; i++) {
            if ((u32) voices[i].priority < priority &&
                (steal < 0 || voices[i].priority < voices[steal].priority)) {
                steal = i;
            }
        }
        if (steal < 0) {
            return NULL;
        }
        i = steal;
        if (voices[i].callback != NULL) {
            voices[i].callback(&voices[i]);
        }
    }
    {
        AXVPB* v = &voices[i];
        voice_used[i] = 1;
        memset(v, 0, sizeof(*v));
        v->index = (u32) i;
        v->priority = (int) priority;
        v->callback = callback;
        v->userContext = userContext;
        v->pb.state = AX_PB_STATE_STOP;
        return v;
    }
}

void AXFreeVoice(AXVPB* p)
{
    if (p != NULL && p >= voices && p < voices + AX_MAX_VOICES) {
        voice_used[p - voices] = 0;
        p->pb.state = AX_PB_STATE_STOP;
    }
}

void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    p->priority = (int) priority;
}

void AXRegisterCallback(void (*callback)())
{
    frame_callback = (void (*)(void)) callback;
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{
    aux_cb[0] = (AuxCallback) callback;
    aux_ctx[0] = context;
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{
    aux_cb[1] = (AuxCallback) callback;
    aux_ctx[1] = context;
}

/* --- voice parameters --------------------------------------------------- */

void AXSetVoiceState(AXVPB* p, u16 state)
{
    p->pb.state = state;
}

void AXSetVoiceType(AXVPB* p, u16 type)
{
    p->pb.type = type;
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix)
{
    p->pb.mix = *mix;
}

void AXSetVoiceItdOn(AXVPB* p)
{
    p->pb.itd.flag = 1;
}

void AXSetVoiceItdTarget(AXVPB* p, u16 lShift, u16 rShift)
{
    p->pb.itd.targetShiftL = lShift;
    p->pb.itd.targetShiftR = rShift;
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve)
{
    p->pb.ve = *ve;
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta)
{
    p->pb.ve.currentDelta = delta;
}

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    p->pb.addr = *addr;
}

void AXSetVoiceLoop(AXVPB* p, u16 loop)
{
    p->pb.addr.loopFlag = loop;
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr)
{
    PAIR(p->pb.addr.loopAddressHi) = addr;
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr)
{
    PAIR(p->pb.addr.endAddressHi) = addr;
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr)
{
    PAIR(p->pb.addr.currentAddressHi) = addr;
}

void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm)
{
    p->pb.adpcm = *adpcm;
}

void AXSetVoiceSrc(AXVPB* p, AXPBSRC* src)
{
    p->pb.src = *src;
}

void AXSetVoiceSrcType(AXVPB* p, u32 type)
{
    p->pb.srcSelect = (u16) type;
}

void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{
    PAIR(p->pb.src.ratioHi) = (u32) (ratio * 65536.0f);
}

void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* adpcmloop)
{
    p->pb.adpcmLoop = *adpcmloop;
}

void AXSetVoiceFir(AXVPB* p, AXPBFIR* fir)
{
    p->pb.fir = *fir;
}

void AXSetVoiceDpop(AXVPB* p, AXPBDPOP* dpop)
{
    p->pb.dpop = *dpop;
}

void AXSetVoiceUpdateIncrement(AXVPB* p)
{
    (void) p;
}

void AXSetVoiceUpdateWrite(AXVPB* p, u16 param, u16 data)
{
    (void) p;
    (void) param;
    (void) data;
}

/* --- sample fetch ------------------------------------------------------- */

static s16 clamp16(s32 v)
{
    return (s16) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Reads the next source sample of a voice, advancing its address and
 * handling the loop/end points. Returns false when the voice stopped. */
static int next_sample(AXVPB* v, s16* out)
{
    AXPB* pb = &v->pb;
    u8* aram = pc_aram_data();
    u32 cur = PAIR(pb->addr.currentAddressHi);
    u32 end = PAIR(pb->addr.endAddressHi);
    s32 sample;

    if (pb->addr.format == AX_PB_FORMAT_ADPCM) {
        u32 byte;
        s32 nibble, scale, c1, c2;
        if ((cur & 0xF) == 0) {
            /* frame header: predictor/scale byte, two nibbles */
            byte = cur >> 1;
            if (byte >= pc_aram_size()) {
                pb->state = AX_PB_STATE_STOP;
                return 0;
            }
            pb->adpcm.pred_scale = aram[byte];
            cur += 2;
        }
        byte = cur >> 1;
        if (byte >= pc_aram_size()) {
            pb->state = AX_PB_STATE_STOP;
            return 0;
        }
        nibble = (cur & 1) ? (aram[byte] & 0xF) : (aram[byte] >> 4);
        nibble = (nibble << 28) >> 28; /* sign-extend the 4-bit value */
        scale = 1 << (pb->adpcm.pred_scale & 0xF);
        c1 = (s16) pb->adpcm.a[(pb->adpcm.pred_scale >> 4) & 7][0];
        c2 = (s16) pb->adpcm.a[(pb->adpcm.pred_scale >> 4) & 7][1];
        sample = ((nibble * scale) << 11) + 0x400 + c1 * (s16) pb->adpcm.yn1 + c2 * (s16) pb->adpcm.yn2;
        sample >>= 11;
        sample = clamp16(sample);
        pb->adpcm.yn2 = pb->adpcm.yn1;
        pb->adpcm.yn1 = (u16) sample;
        cur++;
    } else if (pb->addr.format == AX_PB_FORMAT_PCM16) {
        u32 byte = cur * 2;
        if (byte + 1 >= pc_aram_size()) {
            pb->state = AX_PB_STATE_STOP;
            return 0;
        }
        sample = (s16) ((aram[byte] << 8) | aram[byte + 1]);
        cur++;
    } else {
        if (cur >= pc_aram_size()) {
            pb->state = AX_PB_STATE_STOP;
            return 0;
        }
        sample = (s8) aram[cur] << 8;
        cur++;
    }

    if (cur > end) {
        if (pb->addr.loopFlag) {
            cur = PAIR(pb->addr.loopAddressHi);
            if (pb->addr.format == AX_PB_FORMAT_ADPCM) {
                pb->adpcm.pred_scale = pb->adpcmLoop.loop_pred_scale;
                pb->adpcm.yn1 = pb->adpcmLoop.loop_yn1;
                pb->adpcm.yn2 = pb->adpcmLoop.loop_yn2;
            }
        } else {
            pb->state = AX_PB_STATE_STOP;
        }
    }
    PAIR(pb->addr.currentAddressHi) = cur;
    *out = (s16) sample;
    return 1;
}

/* --- mixing ---------------------------------------------------------------- */

static s32 mix_buf[AX_FRAME * 2];

static void ramp(u16* vol, u16 delta_u)
{
    s32 v = (s32) *vol + (s16) delta_u;
    *vol = (u16) (v < 0 ? 0 : v > 0x8000 ? 0x8000 : v);
}

static void mix_voice(AXVPB* v)
{
    AXPB* pb = &v->pb;
    u32 ratio = PAIR(pb->src.ratioHi);
    u32 frac = pb->src.currentAddressFrac;
    s32 vol = pb->ve.currentVolume;
    s32 vl = (s16) pb->mix.vL, vr = (s16) pb->mix.vR;
    s32 al = (s16) pb->mix.vAuxAL, ar = (s16) pb->mix.vAuxAR, as = (s16) pb->mix.vAuxAS;
    s32 bl = (s16) pb->mix.vAuxBL, br = (s16) pb->mix.vAuxBR, bs = (s16) pb->mix.vAuxBS;
    int sends = (al | ar | as | bl | br | bs) != 0;
    s16 prev = (s16) pb->src.last_samples[3];
    s16 curs = (s16) pb->src.last_samples[2];
    int i;

    if (pb->state != AX_PB_STATE_RUN) {
        return;
    }
    if (ratio == 0) {
        ratio = 0x10000;
    }
    for (i = 0; i < AX_FRAME; i++) {
        s32 s;
        /* advance the source position by the sample-rate ratio */
        frac += ratio;
        while (frac >= 0x10000) {
            s16 n;
            frac -= 0x10000;
            prev = curs;
            if (!next_sample(v, &n)) {
                curs = 0;
                break;
            }
            curs = n;
        }
        if (pb->state != AX_PB_STATE_RUN) {
            break;
        }
        /* linear interpolation between the last two source samples */
        s = prev + (((curs - prev) * (s32) (frac >> 1)) >> 15);
        s = (s * vol) >> 15;
        mix_buf[i * 2] += (s * vl) >> 15;
        mix_buf[i * 2 + 1] += (s * vr) >> 15;
        if (sends) {
            aux_buf[0][0][i] += (s * al) >> 15;
            aux_buf[0][1][i] += (s * ar) >> 15;
            aux_buf[0][2][i] += (s * as) >> 15;
            aux_buf[1][0][i] += (s * bl) >> 15;
            aux_buf[1][1][i] += (s * br) >> 15;
            aux_buf[1][2][i] += (s * bs) >> 15;
        }
    }
    pb->src.currentAddressFrac = (u16) frac;
    pb->src.last_samples[3] = (u16) prev;
    pb->src.last_samples[2] = (u16) curs;
    /* per-frame volume ramps */
    ramp(&pb->ve.currentVolume, (u16) pb->ve.currentDelta);
    ramp(&pb->mix.vL, pb->mix.vDeltaL);
    ramp(&pb->mix.vR, pb->mix.vDeltaR);
    ramp(&pb->mix.vAuxAL, pb->mix.vDeltaAuxAL);
    ramp(&pb->mix.vAuxAR, pb->mix.vDeltaAuxAR);
    ramp(&pb->mix.vAuxAS, pb->mix.vDeltaAuxAS);
    ramp(&pb->mix.vAuxBL, pb->mix.vDeltaAuxBL);
    ramp(&pb->mix.vAuxBR, pb->mix.vDeltaAuxBR);
    ramp(&pb->mix.vAuxBS, pb->mix.vDeltaAuxBS);
}

/* Run the effect on a bus and add its output to the main mix. The surround
 * share goes to both speakers at half level: the output is stereo. */
static void mix_aux(int bus)
{
    struct {
        long* left;
        long* right;
        long* surround;
    } upd;
    int i;
    if (aux_cb[bus] == NULL) {
        return;
    }
    upd.left = (long*) aux_buf[bus][0];
    upd.right = (long*) aux_buf[bus][1];
    upd.surround = (long*) aux_buf[bus][2];
    aux_cb[bus](&upd, aux_ctx[bus]);
    for (i = 0; i < AX_FRAME; i++) {
        mix_buf[i * 2] += aux_buf[bus][0][i] + (aux_buf[bus][2][i] >> 1);
        mix_buf[i * 2 + 1] += aux_buf[bus][1][i] + (aux_buf[bus][2][i] >> 1);
    }
}

/* --- output device ------------------------------------------------------ */

#define OUT_BUFFERS 8
#define OUT_BUFFER_FRAMES 10 /* 50 ms per waveOut buffer */
static HWAVEOUT wave_out;
static WAVEHDR wave_hdr[OUT_BUFFERS];
static s16 wave_data[OUT_BUFFERS][OUT_BUFFER_FRAMES * AX_FRAME * 2];
static int wave_cur, wave_fill;
static int wave_ready;
static u32 wave_dropped, wave_starved, wave_pushed;
static DWORD wave_report_ms;

/* Report device trouble once in a while: "dropped" frames are mixed
 * faster than the device plays them (all buffers still queued), "starved"
 * is the opposite (every buffer done: the device ran out of data and
 * played silence, the audible stutter). */
static void out_stats(void)
{
    DWORD now = timeGetTime();
    if (wave_report_ms == 0) {
        wave_report_ms = now;
        return;
    }
    if (now - wave_report_ms < 5000) {
        return;
    }
    if (wave_dropped != 0 || wave_starved != 0) {
        fprintf(stderr, "[pc] audio: in the last %u ms %u of %u mixer frames were dropped (mixed faster than the device "
                        "plays: an unpaced run) and the device ran dry %u time(s) (the game ran slower than 60 Hz: stutter)\n",
                (unsigned) (now - wave_report_ms), wave_dropped, wave_pushed, wave_starved);
    }
    wave_report_ms = now;
    wave_dropped = wave_starved = wave_pushed = 0;
}

static void out_open(void)
{
    WAVEFORMATEX fmt;
    int i;
    if (pc_config.headless || pc_config.no_audio || getenv("MELEE_NO_AUDIO") != NULL) {
        return;
    }
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = AX_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = AX_RATE * 4;
    if (waveOutOpen(&wave_out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[pc] audio: cannot open the output device, running silent\n");
        return;
    }
    for (i = 0; i < OUT_BUFFERS; i++) {
        memset(&wave_hdr[i], 0, sizeof(WAVEHDR));
        wave_hdr[i].lpData = (LPSTR) wave_data[i];
        wave_hdr[i].dwBufferLength = sizeof(wave_data[i]);
        waveOutPrepareHeader(wave_out, &wave_hdr[i], sizeof(WAVEHDR));
        wave_hdr[i].dwFlags |= WHDR_DONE;
    }
    wave_ready = 1;
}

static void out_push(const s32* frame)
{
    WAVEHDR* h;
    s16* dst;
    int i;
    if (!wave_ready) {
        return;
    }
    h = &wave_hdr[wave_cur];
    wave_pushed++;
    out_stats();
    if (wave_fill == 0 && !(h->dwFlags & WHDR_DONE)) {
        wave_dropped++;
        return; /* the device is behind: drop this frame */
    }
    if (wave_fill == 0) {
        /* every buffer already played back: the device had nothing left */
        for (i = 0; i < OUT_BUFFERS; i++) {
            if (!(wave_hdr[i].dwFlags & WHDR_DONE)) {
                break;
            }
        }
        if (i == OUT_BUFFERS && wave_pushed > OUT_BUFFERS * OUT_BUFFER_FRAMES) {
            wave_starved++;
        }
    }
    dst = wave_data[wave_cur] + wave_fill * AX_FRAME * 2;
    for (i = 0; i < AX_FRAME * 2; i++) {
        dst[i] = clamp16((frame[i] * pc_config.volume) / 100);
    }
    if (++wave_fill == OUT_BUFFER_FRAMES) {
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(wave_out, h, sizeof(WAVEHDR));
        wave_fill = 0;
        wave_cur = (wave_cur + 1) % OUT_BUFFERS;
    }
}

/* --- debug dump ----------------------------------------------------------- */

static FILE* dump_file;
static u32 dump_bytes;

static void dump_open(void)
{
    const char* path = getenv("MELEE_AUDIO_DUMP");
    u8 header[44];
    if (path == NULL) {
        return;
    }
    dump_file = fopen(path, "wb");
    if (dump_file == NULL) {
        return;
    }
    memset(header, 0, sizeof(header));
    fwrite(header, 1, sizeof(header), dump_file); /* patched on exit */
}

static void dump_push(const s32* frame)
{
    s16 tmp[AX_FRAME * 2];
    int i;
    if (dump_file == NULL) {
        return;
    }
    for (i = 0; i < AX_FRAME * 2; i++) {
        tmp[i] = clamp16(frame[i]);
    }
    fwrite(tmp, 1, sizeof(tmp), dump_file);
    dump_bytes += sizeof(tmp);
}

static void put32(u8* p, u32 v)
{
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
    p[2] = (u8) (v >> 16);
    p[3] = (u8) (v >> 24);
}

/// Finishes the WAV dump; called from the exit path.
void pc_ax_shutdown(void)
{
    u8 h[44];
    if (dump_file == NULL) {
        return;
    }
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + dump_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    put32(h + 24, AX_RATE);
    put32(h + 28, AX_RATE * 4);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, dump_bytes);
    fseek(dump_file, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), dump_file);
    fclose(dump_file);
    dump_file = NULL;
}

/* --- frame driver ------------------------------------------------------- */

static void ax_step(void)
{
    int i;
    if (frame_callback != NULL) {
        frame_callback();
    }
    memset(mix_buf, 0, sizeof(mix_buf));
    memset(aux_buf, 0, sizeof(aux_buf));
    for (i = 0; i < AX_MAX_VOICES; i++) {
        if (voice_used[i]) {
            mix_voice(&voices[i]);
        }
    }
    if (aux_off < 0) {
        aux_off = getenv("MELEE_AX_NOAUX") != NULL;
    }
    if (!aux_off) {
        mix_aux(0);
        mix_aux(1);
    }
    out_push(mix_buf);
    dump_push(mix_buf);
}

/// Called once per video frame: renders the 5 ms frames that fall into it.
/* ax_pending: samples owed to the next 5 ms frame, declared above with the state registration */

void pc_ax_frame(void)
{
    double* pending_p = &ax_pending;
#define pending (*pending_p)
    if (!ax_ready) {
        return;
    }
    pending += (double) AX_RATE / 60.0;
    while (pending >= AX_FRAME) {
        ax_step();
        pending -= AX_FRAME;
    }
#undef pending
}

void AXInit(void)
{
    if (ax_ready) {
        return;
    }
    memset(voices, 0, sizeof(voices));
    memset(voice_used, 0, sizeof(voice_used));
    out_open();
    dump_open();
    ax_ready = 1;
}

void AXQuit(void)
{
    ax_ready = 0;
}

void AXSetMode(u32 mode)
{
    (void) mode;
}

u32 AXGetMode(void)
{
    return 0;
}

void AXSetMaxDspCycles(u32 cycles)
{
    (void) cycles;
}

u32 AXGetMaxDspCycles(void)
{
    return 0;
}

u32 AXGetDspCycles(void)
{
    return 0;
}

/* --- AI (audio interface) ----------------------------------------------- */

static u32 ai_dsp_rate;

void AIInit(u8* stack)
{
    (void) stack;
}

void AISetDSPSampleRate(u32 rate)
{
    ai_dsp_rate = rate;
}

u32 AIGetDSPSampleRate(void)
{
    return ai_dsp_rate;
}

void AISetStreamVolLeft(u8 vol)
{
    (void) vol; /* disc audio streaming is not used by the game's sound engine */
}

void AISetStreamVolRight(u8 vol)
{
    (void) vol;
}
