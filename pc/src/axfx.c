/**
 * @file axfx.c
 * The AX auxiliary effects the game uses: the standard reverb on aux bus A
 * and the delay on aux bus B (lbaudio_ax.c sets both up once at boot; the
 * chorus and the "hi" reverb are never selected and stay stubs).
 *
 * The SDK sources under extern/dolphin/src/dolphin/axfx are the reference.
 * Their delay-line management and parameter code is plain C and is
 * repeated here; the reverb's per-sample core (HandleReverb) is PowerPC
 * assembly there and is written out in C below, instruction for
 * instruction: per channel a pre-delay, two parallel comb filters, an
 * all-pass, a one-pole low-pass ("damping"), a second all-pass, then the
 * wet/dry mix. Delay-line points are byte offsets, as in the SDK.
 *
 * The buffers are the mixer's aux accumulators: 160 samples of s32 per
 * frame, left, right and surround laid out one after the other, at the
 * same scale as the main mix (16-bit PCM range).
 */
#include "pc_runtime.h"

#include <dolphin/ax.h>
#include <dolphin/axfx.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AXFX_FRAME 160

static void* default_alloc(unsigned long size)
{
    return calloc(1, size);
}

static void default_free(void* p)
{
    free(p);
}

void* (*__AXFXAlloc)(unsigned long) = default_alloc;
void (*__AXFXFree)(void*) = default_free;

void AXFXSetHooks(void* (*alloc_hook)(unsigned long), void (*free_hook)(void*))
{
    /* the game's heap for the delay lines (axdriver.c's AXDriverAlloc) */
    __AXFXAlloc = alloc_hook != NULL ? alloc_hook : default_alloc;
    __AXFXFree = free_hook != NULL ? free_hook : default_free;
}

/* --- delay ---------------------------------------------------------------- */

void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* bufferUpdate, struct AXFX_DELAY* delay)
{
    long* left = bufferUpdate->left;
    long* right = bufferUpdate->right;
    long* sur = bufferUpdate->surround;
    long* lBuf = delay->left + delay->currentPos[0] * AXFX_FRAME;
    long* rBuf = delay->right + delay->currentPos[1] * AXFX_FRAME;
    long* sBuf = delay->sur + delay->currentPos[2] * AXFX_FRAME;
    u32 i;

    if (delay->left == NULL || delay->right == NULL || delay->sur == NULL) {
        return;
    }
    for (i = 0; i < AXFX_FRAME; i++) {
        long l = *lBuf, r = *rBuf, s = *sBuf;
        *lBuf++ = *left + ((s32) (l * (s32) delay->currentFeedback[0]) >> 7);
        *rBuf++ = *right + ((s32) (r * (s32) delay->currentFeedback[1]) >> 7);
        *sBuf++ = *sur + ((s32) (s * (s32) delay->currentFeedback[2]) >> 7);
        *left++ = (s32) (l * (s32) delay->currentOutput[0]) >> 7;
        *right++ = (s32) (r * (s32) delay->currentOutput[1]) >> 7;
        *sur++ = (s32) (s * (s32) delay->currentOutput[2]) >> 7;
    }
    delay->currentPos[0] = (delay->currentPos[0] + 1) % delay->currentSize[0];
    delay->currentPos[1] = (delay->currentPos[1] + 1) % delay->currentSize[1];
    delay->currentPos[2] = (delay->currentPos[2] + 1) % delay->currentSize[2];
}

int AXFXDelayShutdown(struct AXFX_DELAY* delay)
{
    if (delay->left != NULL) {
        __AXFXFree(delay->left);
    }
    if (delay->right != NULL) {
        __AXFXFree(delay->right);
    }
    if (delay->sur != NULL) {
        __AXFXFree(delay->sur);
    }
    delay->left = delay->right = delay->sur = NULL;
    return 1;
}

int AXFXDelaySettings(struct AXFX_DELAY* delay)
{
    unsigned long i;
    AXFXDelayShutdown(delay);
    for (i = 0; i < 3; i++) {
        /* delay[i] is in milliseconds; the line is whole frames long */
        delay->currentSize[i] = (((delay->delay[i] - 5) << 5) + 0x9F) / AXFX_FRAME;
        if (delay->currentSize[i] == 0) {
            delay->currentSize[i] = 1;
        }
        delay->currentPos[i] = 0;
        delay->currentFeedback[i] = (delay->feedback[i] << 7) / 100U;
        delay->currentOutput[i] = (delay->output[i] << 7) / 100U;
    }
    delay->left = (long*) __AXFXAlloc(delay->currentSize[0] * AXFX_FRAME * 4);
    delay->right = (long*) __AXFXAlloc(delay->currentSize[1] * AXFX_FRAME * 4);
    delay->sur = (long*) __AXFXAlloc(delay->currentSize[2] * AXFX_FRAME * 4);
    if (delay->left == NULL || delay->right == NULL || delay->sur == NULL) {
        AXFXDelayShutdown(delay);
        return 0;
    }
    memset(delay->left, 0, delay->currentSize[0] * AXFX_FRAME * 4);
    memset(delay->right, 0, delay->currentSize[1] * AXFX_FRAME * 4);
    memset(delay->sur, 0, delay->currentSize[2] * AXFX_FRAME * 4);
    return 1;
}

int AXFXDelayInit(struct AXFX_DELAY* delay)
{
    delay->left = delay->right = delay->sur = NULL;
    return AXFXDelaySettings(delay);
}

/* --- standard reverb ------------------------------------------------------ */

static void DLsetdelay(struct AXFX_REVSTD_DELAYLINE* dl, long lag)
{
    dl->outPoint = dl->inPoint - lag * 4;
    while (dl->outPoint < 0) {
        dl->outPoint += dl->length;
    }
}

static void DLcreate(struct AXFX_REVSTD_DELAYLINE* dl, long max_length)
{
    dl->length = max_length * 4;
    dl->inputs = (float*) __AXFXAlloc(max_length * 4);
    memset(dl->inputs, 0, max_length * 4);
    dl->lastOutput = 0.0f;
    DLsetdelay(dl, max_length >> 1);
    dl->inPoint = 0;
    dl->outPoint = 0;
}

static void DLdelete(struct AXFX_REVSTD_DELAYLINE* dl)
{
    if (dl->inputs != NULL) {
        __AXFXFree(dl->inputs);
        dl->inputs = NULL;
    }
}

static int ReverbSTDCreate(struct AXFX_REVSTD_WORK* rv, float coloration, float time, float mix, float damping,
                           float predelay)
{
    static const long lens[4] = { 0x6FD, 0x7CF, 0x1B1, 0x95 };
    u8 i, k;

    if (coloration < 0.0f || coloration > 1.0f || time < 0.01f || time > 10.0f || mix < 0.0f || mix > 1.0f ||
        damping < 0.0f || damping > 1.0f || predelay < 0.0f || predelay > 0.1f)
    {
        return 0;
    }
    memset(rv, 0, sizeof(*rv));
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 2; i++) {
            DLcreate(&rv->C[i + k * 2], lens[i] + 2);
            DLsetdelay(&rv->C[i + k * 2], lens[i]);
            rv->combCoef[i + k * 2] = powf(10.0f, (float) (lens[i] * -3) / (32000.0f * time));
        }
        for (i = 0; i < 2; i++) {
            DLcreate(&rv->AP[i + k * 2], lens[i + 2] + 2);
            DLsetdelay(&rv->AP[i + k * 2], lens[i + 2]);
        }
        rv->lpLastout[k] = 0.0f;
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping < 0.05f ? 0.05f : damping;
    rv->damping = 1.0f - (0.05f + 0.8f * rv->damping);
    if (predelay != 0.0f) {
        rv->preDelayTime = (long) (32000.0f * predelay);
        for (i = 0; i < 3; i++) {
            rv->preDelayLine[i] = (float*) __AXFXAlloc(rv->preDelayTime * 4);
            memset(rv->preDelayLine[i], 0, rv->preDelayTime * 4);
            rv->preDelayPtr[i] = rv->preDelayLine[i];
        }
    } else {
        rv->preDelayTime = 0;
        for (i = 0; i < 3; i++) {
            rv->preDelayPtr[i] = NULL;
            rv->preDelayLine[i] = NULL;
        }
    }
    return 1;
}

static int ReverbSTDModify(struct AXFX_REVSTD_WORK* rv, float coloration, float time, float mix, float damping,
                           float predelay)
{
    static const long lens[4] = { 0x6FD, 0x7CF, 0x1B1, 0x95 };
    u8 i, k;

    if (coloration < 0.0f || coloration > 1.0f || time < 0.01f || time > 10.0f || mix < 0.0f || mix > 1.0f ||
        damping < 0.0f || damping > 1.0f || predelay < 0.0f || predelay > 100.0f)
    {
        return 0;
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping < 0.05f ? 0.05f : damping;
    rv->damping = 1.0f - (0.05f + 0.8f * rv->damping);
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 2; i++) {
            rv->combCoef[i + k * 2] = powf(10.0f, (float) (lens[i] * -3) / (32000.0f * time));
        }
    }
    if (predelay != 0.0f) {
        long t = (long) (32000.0f * predelay);
        if (t != rv->preDelayTime) {
            for (i = 0; i < 3; i++) {
                if (rv->preDelayLine[i] != NULL) {
                    __AXFXFree(rv->preDelayLine[i]);
                }
                rv->preDelayLine[i] = (float*) __AXFXAlloc(t * 4);
                memset(rv->preDelayLine[i], 0, t * 4);
                rv->preDelayPtr[i] = rv->preDelayLine[i];
            }
            rv->preDelayTime = t;
        }
    } else {
        for (i = 0; i < 3; i++) {
            if (rv->preDelayLine[i] != NULL) {
                __AXFXFree(rv->preDelayLine[i]);
            }
            rv->preDelayLine[i] = NULL;
            rv->preDelayPtr[i] = NULL;
        }
        rv->preDelayTime = 0;
    }
    return 1;
}

/* write to a delay line at its in point and advance it */
static void dl_put(struct AXFX_REVSTD_DELAYLINE* dl, float v)
{
    dl->inputs[dl->inPoint / 4] = v;
    dl->inPoint += 4;
    if (dl->inPoint == dl->length) {
        dl->inPoint = 0;
    }
}

/* read a delay line at its out point and advance it */
static float dl_get(struct AXFX_REVSTD_DELAYLINE* dl)
{
    float v = dl->inputs[dl->outPoint / 4];
    dl->outPoint += 4;
    if (dl->outPoint == dl->length) {
        dl->outPoint = 0;
    }
    return v;
}

static long to_long(float v)
{
    /* fctiwz: truncate toward zero, saturating */
    if (v >= 2147483520.0f) {
        return 0x7FFFFFFF;
    }
    if (v <= -2147483648.0f) {
        return (long) 0x80000000;
    }
    return (long) v;
}

static void HandleReverb(long* sptr, struct AXFX_REVSTD_WORK* rv)
{
    const float ap = rv->allPassCoeff;
    const float damp = rv->damping;
    const float wet = rv->level * 0.6f;
    const float dry = 0.6f - wet;
    int k, i;

    for (k = 0; k < 3; k++) {
        struct AXFX_REVSTD_DELAYLINE* c0 = &rv->C[k * 2];
        struct AXFX_REVSTD_DELAYLINE* c1 = &rv->C[k * 2 + 1];
        struct AXFX_REVSTD_DELAYLINE* a0 = &rv->AP[k * 2];
        struct AXFX_REVSTD_DELAYLINE* a1 = &rv->AP[k * 2 + 1];
        const float cc0 = rv->combCoef[k * 2], cc1 = rv->combCoef[k * 2 + 1];
        float lp = rv->lpLastout[k];
        float* pre = rv->preDelayPtr[k];
        float* pre_line = rv->preDelayLine[k];
        float* pre_end = pre_line != NULL ? pre_line + (rv->preDelayTime - 1) : NULL;
        long* s = sptr + k * AXFX_FRAME;

        for (i = 0; i < AXFX_FRAME; i++) {
            float in = (float) s[i];
            float x = in, o0, o1, y, z, out;
            if (rv->preDelayTime != 0 && pre != NULL) {
                x = *pre;
                *pre = in;
                pre++;
                if (pre == pre_end) {
                    pre = pre_line;
                }
            }
            /* two parallel comb filters */
            dl_put(c0, cc0 * c0->lastOutput + x);
            dl_put(c1, cc1 * c1->lastOutput + x);
            o0 = dl_get(c0);
            o1 = dl_get(c1);
            c0->lastOutput = o0;
            c1->lastOutput = o1;
            /* first all-pass */
            y = ap * a0->lastOutput + (o0 + o1);
            dl_put(a0, y);
            z = a0->lastOutput - ap * y;
            a0->lastOutput = dl_get(a0);
            /* damping low-pass */
            z = damp * lp + z * 0.3f;
            lp = z;
            /* second all-pass */
            y = ap * a1->lastOutput + z;
            dl_put(a1, y);
            z = a1->lastOutput - ap * y;
            a1->lastOutput = dl_get(a1);
            out = wet * z + dry * in;
            s[i] = to_long(out);
        }
        rv->lpLastout[k] = lp;
        rv->preDelayPtr[k] = pre;
    }
}

static void ReverbSTDFree(struct AXFX_REVSTD_WORK* rv)
{
    u8 i;
    for (i = 0; i < 6; i++) {
        DLdelete(&rv->AP[i]);
    }
    for (i = 0; i < 6; i++) {
        DLdelete(&rv->C[i]);
    }
    if (rv->preDelayTime != 0) {
        for (i = 0; i < 3; i++) {
            if (rv->preDelayLine[i] != NULL) {
                __AXFXFree(rv->preDelayLine[i]);
                rv->preDelayLine[i] = NULL;
            }
        }
    }
}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev)
{
    rev->tempDisableFX = 0;
    return ReverbSTDCreate(&rev->rv, rev->coloration, rev->time, rev->mix, rev->damping, rev->preDelay);
}

int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev)
{
    ReverbSTDFree(&rev->rv);
    return 1;
}

int AXFXReverbStdSettings(struct AXFX_REVERBSTD* rev)
{
    int ret;
    rev->tempDisableFX = 1;
    ret = ReverbSTDModify(&rev->rv, rev->coloration, rev->time, rev->mix, rev->damping, rev->preDelay);
    rev->tempDisableFX = 0;
    return ret;
}

void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* bufferUpdate, struct AXFX_REVERBSTD* reverb)
{
    /* the three buffers are consecutive (left, right, surround) */
    if (reverb->tempDisableFX == 0 && bufferUpdate->left != NULL) {
        HandleReverb(bufferUpdate->left, &reverb->rv);
    }
}
