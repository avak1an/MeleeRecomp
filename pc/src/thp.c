/**
 * @file thp.c
 * THP video decoder for the PC port. THP frames are baseline JPEG images
 * (4:2:0, Huffman coded) without byte stuffing in the entropy data; the
 * SDK decoder writes the Y, U and V planes as GX I8 textures, which are
 * stored in 8x4 tiles of 32 bytes. The game's movie player calls
 * THPVideoDecode() to parse a frame's headers into a work area and then
 * THPDec_80331340() / THPDec_803313D0() to decode the scan into planes.
 * The work area layout is private to this file; only its size, computed by
 * THPDec_8032FD40(), is shared with the game.
 */
#include "pc_runtime.h"

#include <dolphin/thp/thp.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_THP_M_PI 3.14159265358979323846

static const u8 zigzag[80] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};

struct thp_huff {
    u8 bits[17];
    u8 vals[256];
    s32 maxcode[18];
    s32 valptr[17];
    s32 mincode[17];
    u8 look_val[256]; /* 8-bit fast lookup */
    u8 look_len[256]; /* 0 when the code is longer than 8 bits */
};

struct thp_state {
    const u8* pos;
    u32 bitbuf;
    int bitcnt;
    u16 width, height;
    u8 ncomp;
    struct {
        u8 id, h, v, tq, td, ta;
        s32 dc;
    } comp[3];
    u16 qt[4][64]; /* zigzag order, as stored in the file */
    u8 qt_valid;
    u8 huff_valid;
    struct thp_huff huff[4]; /* index = (table id << 1) + class */
    u16 restart_interval;
    u32 bad_codes; /* Huffman codes that matched no table entry (diagnostic) */
    const u8* scan_start;
    u32 magic;
};

#define THP_STATE_MAGIC 0x54485053u /* "THPS" */

static float cos_tab[8][8];
static int thp_ready;

void THPInit(void)
{
    int x, u;
    if (thp_ready) {
        return;
    }
    for (x = 0; x < 8; x++) {
        for (u = 0; u < 8; u++) {
            float cu = u == 0 ? (float) (1.0 / sqrt(2.0)) : 1.0f;
            cos_tab[x][u] = (float) (cu * cos((2.0 * x + 1.0) * u * PC_THP_M_PI / 16.0) / 2.0);
        }
    }
    thp_ready = 1;
}

/* --- probing helpers used by the player ------------------------------- */

s32 THPDec_8032F8D4(u8* data, THPDec_8032FD40_Data* out)
{
    u8 hSample[4], vSample[4];
    u8 marker, componentCount, i, valid = 0;
    static const u8 tag[5] = "JFIF";
    u32 j;
    u16 length;

    memset(out, 0, 0xC);
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return 0;
    }
    data += 2;
    for (;;) {
        if (*data++ != 0xFF) {
            return 0;
        }
        while (*data == 0xFF) {
            data++;
        }
        marker = *data++;
        if (marker == 0xC0) {
            out->_pad = (u16) (data[4] | (data[3] << 8));
            out->val1 = (u16) (data[6] | (data[5] << 8));
            componentCount = data[7];
            data += 8;
            if (componentCount != 3) {
                return 0;
            }
            for (i = 0; i < componentCount; i++) {
                u8 factors;
                data++;
                factors = *data++;
                hSample[i] = (u8) (factors >> 4);
                vSample[i] = (u8) (factors & 0xF);
                data++;
            }
            if (hSample[0] / hSample[1] == 2 && hSample[0] / hSample[2] == 2) {
                if (vSample[0] / vSample[1] == 2 && vSample[0] / vSample[2] == 2) {
                    out->val2 = 4;
                } else if (vSample[0] == vSample[1] && vSample[0] == vSample[2]) {
                    out->val2 = 2;
                }
            } else if (hSample[0] == hSample[1] && hSample[0] == hSample[2]) {
                if (vSample[0] == vSample[1] && vSample[0] == vSample[2]) {
                    out->val2 = 1;
                }
            } else {
                return 0;
            }
        } else if (marker == 0xE0) {
            length = (u16) ((data[0] << 8) | data[1]);
            data += 2;
            for (i = 0; i < 5; i++) {
                if (*data++ != tag[i]) {
                    return 0;
                }
            }
            valid = 1;
            for (j = 0; j < (u32) (length - 7); j++) {
                data++;
            }
        } else if (marker == 0xDA) {
            break;
        } else if (marker >= 0xC0 && marker <= 0xFE) {
            length = (u16) (data[1] | (data[0] << 8));
            data += 2 + (length - 2);
        }
        if (out->val2 != 0 && valid != 0) {
            break;
        }
    }
    return 1;
}

/// Work-area size for a video of the given format; kept identical to the
/// SDK so the player's allocation does not change.
s32 THPDec_8032FD40(THPDec_8032FD40_Data* data, u16 num)
{
    s32 base = data->val0 + 0x4028;
    if (data->val2 != 4) {
        OSReport("ERROR: THP only supports 4:2:0!!!\n");
        return 0;
    }
    return base + (data->val1 / 2) * (num / 2) * 2 + (data->val1 * num);
}

/* --- header parsing -------------------------------------------------------- */

static int build_huff(struct thp_huff* h)
{
    u8 huffsize[257];
    u32 huffcode[257];
    int k = 0, l, i, code, si;
    for (l = 1; l <= 16; l++) {
        for (i = 0; i < h->bits[l]; i++) {
            if (k >= 256) {
                return 0;
            }
            huffsize[k++] = (u8) l;
        }
    }
    huffsize[k] = 0;
    code = 0;
    si = huffsize[0];
    k = 0;
    while (huffsize[k]) {
        while (huffsize[k] == si) {
            huffcode[k++] = (u32) code++;
        }
        code <<= 1;
        si++;
    }
    k = 0;
    for (l = 1; l <= 16; l++) {
        if (h->bits[l]) {
            h->valptr[l] = k;
            h->mincode[l] = (s32) huffcode[k];
            k += h->bits[l];
            h->maxcode[l] = (s32) huffcode[k - 1];
        } else {
            h->maxcode[l] = -1;
        }
    }
    h->maxcode[17] = 0x7FFFFFFF;
    memset(h->look_len, 0, sizeof(h->look_len));
    k = 0;
    for (l = 1; l <= 8; l++) {
        for (i = 0; i < h->bits[l]; i++, k++) {
            int first = (int) (huffcode[k] << (8 - l));
            int n = 1 << (8 - l), j;
            for (j = 0; j < n; j++) {
                h->look_val[first + j] = h->vals[k];
                h->look_len[first + j] = (u8) l;
            }
        }
    }
    return 1;
}

static u8 read_dht(struct thp_state* st)
{
    int length = (st->pos[0] << 8) | st->pos[1];
    st->pos += 2;
    length -= 2;
    while (length > 0) {
        u8 tc_th = *st->pos++;
        int idx = ((tc_th & 15) << 1) + (tc_th >> 4);
        int i, n = 0;
        if (idx >= 4) {
            return 3;
        }
        st->huff[idx].bits[0] = 0;
        for (i = 1; i <= 16; i++) {
            st->huff[idx].bits[i] = *st->pos++;
            n += st->huff[idx].bits[i];
        }
        if (n > 256) {
            return 3;
        }
        memcpy(st->huff[idx].vals, st->pos, (size_t) n);
        st->pos += n;
        if (!build_huff(&st->huff[idx])) {
            return 3;
        }
        st->huff_valid |= (u8) (1 << idx);
        length -= 17 + n;
    }
    return 0;
}

static u8 read_dqt(struct thp_state* st)
{
    int length = (st->pos[0] << 8) | st->pos[1];
    st->pos += 2;
    length -= 2;
    while (length > 0) {
        u8 id = *st->pos++;
        int i;
        if ((id & 15) >= 4 || (id >> 4) != 0) {
            return 3; /* only 8-bit tables */
        }
        for (i = 0; i < 64; i++) {
            st->qt[id & 15][i] = *st->pos++;
        }
        st->qt_valid |= (u8) (1 << (id & 15));
        length -= 65;
    }
    return 0;
}

static u8 read_sof(struct thp_state* st)
{
    int i;
    st->pos += 2;
    if (*st->pos++ != 8) {
        return 10;
    }
    st->height = (u16) ((st->pos[0] << 8) | st->pos[1]);
    st->pos += 2;
    st->width = (u16) ((st->pos[0] << 8) | st->pos[1]);
    st->pos += 2;
    st->ncomp = *st->pos++;
    if (st->ncomp != 3 && st->ncomp != 1) {
        return 12;
    }
    for (i = 0; i < st->ncomp; i++) {
        u8 f;
        st->comp[i].id = *st->pos++;
        f = *st->pos++;
        st->comp[i].h = (u8) (f >> 4);
        st->comp[i].v = (u8) (f & 15);
        st->comp[i].tq = (u8) (*st->pos++ & 3);
    }
    return 0;
}

static u8 read_sos(struct thp_state* st)
{
    int n, i;
    st->pos += 2;
    n = *st->pos++;
    if (n != st->ncomp) {
        return 12;
    }
    for (i = 0; i < n; i++) {
        u8 cs = *st->pos++;
        u8 t = *st->pos++;
        int c;
        for (c = 0; c < st->ncomp; c++) {
            if (st->comp[c].id == cs) {
                st->comp[c].td = (u8) (t >> 4);
                st->comp[c].ta = (u8) (t & 15);
            }
        }
    }
    st->pos += 3; /* Ss, Se, Ah/Al */
    return 0;
}

static u8 read_app0(struct thp_state* st)
{
    int length = (st->pos[0] << 8) | st->pos[1];
    if (memcmp(st->pos + 2, "JFIF", 5) != 0) {
        return 3;
    }
    if (st->pos[14] != 0 || st->pos[15] != 0) {
        return 7; /* thumbnails are not supported */
    }
    st->pos += length;
    return 0;
}

/**
 * Parses a frame's headers. The arguments follow the SDK entry point that
 * the player calls: @p header is the video size, @p status receives 0 on
 * success or an error code, @p work is the decoder work area (returned on
 * success), @p frame is the frame's JPEG data.
 */
s32 THPVideoDecode(void* header, void* status, void* work, void* frame, void* size_data)
{
    struct thp_state* st = (struct thp_state*) work;
    u8* status_out = (u8*) status;
    u8 marker, err;
    (void) header;
    (void) size_data;

    THPInit();
    memset(st, 0, sizeof(*st));
    st->magic = THP_STATE_MAGIC;
    st->pos = (const u8*) frame;
    for (;;) {
        if (*st->pos++ != 0xFF) {
            *status_out = 3;
            marker = 0;
            break;
        }
        while (*st->pos == 0xFF) {
            st->pos++;
        }
        marker = *st->pos++;
        err = 0;
        switch (marker) {
        case 0xD8: /* SOI */
            break;
        case 0xC0:
            err = read_sof(st);
            break;
        case 0xC4:
            err = read_dht(st);
            break;
        case 0xDB:
            err = read_dqt(st);
            break;
        case 0xDD: /* DRI */
            st->restart_interval = (u16) ((st->pos[2] << 8) | st->pos[3]);
            st->pos += 4;
            break;
        case 0xE0:
            err = read_app0(st);
            break;
        case 0xDA:
            err = read_sos(st);
            if (err == 0) {
                st->scan_start = st->pos;
                *status_out = 0;
                return (s32) (uintptr_t) st;
            }
            break;
        case 0xFE:
        case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
            st->pos += (st->pos[0] << 8) | st->pos[1];
            break;
        default:
            *status_out = 11;
            err = 11;
            break;
        }
        if (err != 0) {
            *status_out = err;
            break;
        }
    }
    {
        const u8* f = (const u8*) frame;
        fprintf(stderr, "[pc] THP: frame header rejected (status %u, marker %02X at offset %u, data starts"
                " %02X %02X %02X %02X %02X %02X %02X %02X)\n", *status_out, marker,
                (unsigned) (st->pos - f), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
    return 0;
}

/* --- scan decoding ------------------------------------------------------- */

static void fill_bits(struct thp_state* st)
{
    while (st->bitcnt <= 24) {
        st->bitbuf |= (u32) *st->pos++ << (24 - st->bitcnt);
        st->bitcnt += 8;
    }
}

static u32 get_bits(struct thp_state* st, int n)
{
    u32 v;
    if (n == 0) {
        return 0;
    }
    fill_bits(st);
    v = st->bitbuf >> (32 - n);
    st->bitbuf <<= n;
    st->bitcnt -= n;
    return v;
}

static int huff_decode(struct thp_state* st, const struct thp_huff* h)
{
    u32 peek;
    int l, code;
    fill_bits(st);
    peek = st->bitbuf >> 24;
    if (h->look_len[peek]) {
        l = h->look_len[peek];
        st->bitbuf <<= l;
        st->bitcnt -= l;
        return h->look_val[peek];
    }
    code = (int) (st->bitbuf >> 23);
    st->bitbuf <<= 9;
    st->bitcnt -= 9;
    for (l = 9; l <= 16; l++) {
        if (code <= h->maxcode[l]) {
            return h->vals[h->valptr[l] + code - h->mincode[l]];
        }
        code = (code << 1) | (int) get_bits(st, 1);
    }
    st->bad_codes++;
    return 0; /* corrupt code */
}

static s32 extend(u32 v, int t)
{
    return (s32) v < (1 << (t - 1)) ? (s32) v - (1 << t) + 1 : (s32) v;
}

static void decode_block(struct thp_state* st, int c, float* out)
{
    const struct thp_huff* dc = &st->huff[(st->comp[c].td << 1)];
    const struct thp_huff* ac = &st->huff[(st->comp[c].ta << 1) + 1];
    const u16* q = st->qt[st->comp[c].tq];
    int t, k;
    memset(out, 0, 64 * sizeof(float));
    t = huff_decode(st, dc);
    if (t) {
        st->comp[c].dc += extend(get_bits(st, t), t);
    }
    out[0] = (float) (st->comp[c].dc * q[0]);
    for (k = 1; k < 64;) {
        int rs = huff_decode(st, ac);
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) {
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) {
            break;
        }
        out[zigzag[k]] = (float) (extend(get_bits(st, s), s) * q[k]);
        k++;
    }
}

static void idct_block(const float* in, u8* pix /* 64 */)
{
    float tmp[64];
    int x, y, u, v;
    for (v = 0; v < 8; v++) {
        for (x = 0; x < 8; x++) {
            float s = 0;
            for (u = 0; u < 8; u++) {
                s += cos_tab[x][u] * in[v * 8 + u];
            }
            tmp[v * 8 + x] = s;
        }
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            float s = 128.0f;
            int iv;
            for (v = 0; v < 8; v++) {
                s += cos_tab[y][v] * tmp[v * 8 + x];
            }
            iv = (int) (s + 0.5f);
            pix[y * 8 + x] = (u8) (iv < 0 ? 0 : iv > 255 ? 255 : iv);
        }
    }
}

/// Stores an 8x8 block at (bx, by) of a GX I8 plane that is `w` texels wide.
static void store_block(u8* plane, u32 w, u32 h, u32 bx, u32 by, const u8* pix)
{
    u32 r;
    for (r = 0; r < 8; r++) {
        u32 y = by + r;
        u8* dst;
        if (y >= h || bx + 8 > w) {
            return;
        }
        dst = plane + ((y >> 2) * (w >> 3) + (bx >> 3)) * 32 + (y & 3) * 8;
        memcpy(dst, pix + r * 8, 8);
    }
}

static void decode_frame(struct thp_state* st, u8* py, u8* pu, u8* pv, u32 w)
{
    u32 mcus_x = (st->width + 15) / 16, mcus_y = (st->height + 15) / 16;
    u32 mx, my, rst_left = st->restart_interval;
    float coef[64];
    u8 pix[64];
    int c;

    if (st == NULL || st->magic != THP_STATE_MAGIC || st->scan_start == NULL || st->ncomp != 3 || st->width == 0) {
        return;
    }
    for (c = 0; c < 4; c++) {
        if (!(st->huff_valid & (1 << c))) {
            return;
        }
    }
    st->pos = st->scan_start;
    st->bitbuf = 0;
    st->bitcnt = 0;
    st->bad_codes = 0;
    for (c = 0; c < 3; c++) {
        st->comp[c].dc = 0;
    }
    for (my = 0; my < mcus_y; my++) {
        for (mx = 0; mx < mcus_x; mx++) {
            static const u8 yoff[4][2] = { { 0, 0 }, { 8, 0 }, { 0, 8 }, { 8, 8 } };
            int b;
            for (b = 0; b < 4; b++) {
                decode_block(st, 0, coef);
                idct_block(coef, pix);
                store_block(py, w, st->height, mx * 16 + yoff[b][0], my * 16 + yoff[b][1], pix);
            }
            decode_block(st, 1, coef);
            idct_block(coef, pix);
            store_block(pu, w / 2, st->height / 2, mx * 8, my * 8, pix);
            decode_block(st, 2, coef);
            idct_block(coef, pix);
            store_block(pv, w / 2, st->height / 2, mx * 8, my * 8, pix);
            if (st->restart_interval != 0 && --rst_left == 0) {
                /* the encoder padded to a byte boundary; no RST marker */
                int drop = st->bitcnt & 7;
                st->bitbuf <<= drop;
                st->bitcnt -= drop;
                rst_left = st->restart_interval;
                for (c = 0; c < 3; c++) {
                    st->comp[c].dc = 0;
                }
            }
        }
    }
}

/* MELEE_THP_DUMP=DIR writes each decoded Y plane as a PGM image. */
static const struct thp_state* dump_state;
static void dump_plane(const u8* py, u32 w, u32 h)
{
    static unsigned frame_no;
    const char* dir = getenv("MELEE_THP_DUMP");
    char path[512];
    FILE* f;
    u32 x, y;
    unsigned long long sum = 0;
    if (dir == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            sum += py[((y >> 2) * (w >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)];
        }
    }
    fprintf(stderr, "[pc] THP: frame %u decoded, %ux%u, mean Y %.1f, bad codes %u, restart %u\n", frame_no, w, h,
            (double) sum / (w * h), dump_state != NULL ? dump_state->bad_codes : 0,
            dump_state != NULL ? dump_state->restart_interval : 0);
    snprintf(path, sizeof(path), "%s/thp%04u.pgm", dir, frame_no++);
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            fputc(py[((y >> 2) * (w >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)], f);
        }
    }
    fclose(f);
}

extern void pc_gx_texture_changed(const void* addr, u32 bytes);

static void planes_changed(struct thp_state* st, void* py, void* pu, void* pv, u32 w)
{
    if (st == NULL) {
        return;
    }
    pc_gx_texture_changed(py, w * st->height);
    pc_gx_texture_changed(pu, w * st->height / 4);
    pc_gx_texture_changed(pv, w * st->height / 4);
    dump_state = st;
    dump_plane(py, w, st->height);
}

void THPDec_80331340(s32 info, void* tileY, void* tileU, void* tileV)
{
    decode_frame((struct thp_state*) (uintptr_t) info, tileY, tileU, tileV, 640);
    planes_changed((struct thp_state*) (uintptr_t) info, tileY, tileU, tileV, 640);
}

void THPDec_803313D0(s32 info, void* tileY, void* tileU, void* tileV, u32 width)
{
    decode_frame((struct thp_state*) (uintptr_t) info, tileY, tileU, tileV, width);
    planes_changed((struct thp_state*) (uintptr_t) info, tileY, tileU, tileV, width);
}
