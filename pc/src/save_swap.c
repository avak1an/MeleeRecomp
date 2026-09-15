/**
 * @file save_swap.c
 * The game's save payloads in console byte order.
 *
 * The memory-card file is written by sysdolphin's card library
 * (hsd_3A94.c): each 8 KB sector carries a 32-byte header, an MD5 of the
 * rest and a byte cipher over it, then the payload the game handed it: the
 * main save block (struct gmm_x1868 up to its name-tag banks, 0x1790
 * bytes) and seven name-tag banks (struct NameTagDataBank, 0x1F2C bytes
 * each). On the console those are big-endian structures; the PC keeps them
 * in host order, so the copy into a sector is converted here on the way
 * out and back on the way in, and the card image stays byte for byte what
 * a GameCube or Dolphin writes.
 *
 * The one field with no byte-order equivalent is the 16-bit group of
 * bit-fields in a FighterData record: the console's compiler allocates
 * bit-fields from the most significant bit, this one from the least, so
 * it is converted field by field.
 */
#include "pc_runtime.h"

#include <pc_endian.h>
#include <pc_game_swap.h>

#include <melee/gm/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_SAVE_MAIN_SIZE 0x1790  /* gmm_x1868 up to x2FF8 */
#define PC_SAVE_TAGS_SIZE 0x1F2C  /* one NameTagDataBank */

static void sw64(void* p)
{
    u8* b = (u8*) p;
    int i;
    for (i = 0; i < 4; i++) {
        u8 t = b[i];
        b[i] = b[7 - i];
        b[7 - i] = t;
    }
}

static void fighter_flags_to_console(struct FighterData* fd)
{
    u16 v = 0;
    v |= (u16) ((fd->x7C.b0 & 1) << 15);
    v |= (u16) ((fd->x7C.b1 & 1) << 14);
    v |= (u16) ((fd->x7C.b2 & 1) << 13);
    v |= (u16) ((fd->x7C.b3 & 1) << 12);
    v |= (u16) ((fd->x7C.b4 & 1) << 11);
    v |= (u16) ((fd->x7C.b5 & 1) << 10);
    v |= (u16) ((fd->x7C.b6 & 1) << 9);
    v |= (u16) ((fd->x7C.b789 & 7) << 6);
    v |= (u16) ((fd->x7C.b10_to_12 & 7) << 3);
    v |= (u16) (fd->x7C.b13_to_15 & 7);
    v = PC_BSWAP16(v);
    memcpy(&fd->x7C, &v, 2);
}

static void fighter_flags_from_console(struct FighterData* fd)
{
    u16 v;
    memcpy(&v, &fd->x7C, 2);
    v = PC_BSWAP16(v);
    fd->x7C.b0 = (v >> 15) & 1;
    fd->x7C.b1 = (v >> 14) & 1;
    fd->x7C.b2 = (v >> 13) & 1;
    fd->x7C.b3 = (v >> 12) & 1;
    fd->x7C.b4 = (v >> 11) & 1;
    fd->x7C.b5 = (v >> 10) & 1;
    fd->x7C.b6 = (v >> 9) & 1;
    fd->x7C.b789 = (v >> 6) & 7;
    fd->x7C.b10_to_12 = (v >> 3) & 7;
    fd->x7C.b13_to_15 = v & 7;
}

static void swap_fighter_data(struct FighterData* fd, int to_console)
{
    pc_swap16_range(fd->fighter_kos, sizeof(fd->fighter_kos));
    pc_swap16(&fd->sd_count);
    pc_swap32_range(&fd->attacks_hit, 5 * 4); /* attacks_hit .. damage_recovered */
    pc_swap16_range(&fd->peak_damage, 4 * 2); /* peak_damage .. losses */
    pc_swap32_range(&fd->play_time, 9 * 4);   /* play_time .. coins_lost */
    if (to_console) {
        fighter_flags_to_console(fd);
    } else {
        fighter_flags_from_console(fd);
    }
    pc_swap16(&fd->x7C.x7E);
    pc_swap32_range(&fd->x7C.x84, 7 * 4); /* x84 .. x9C */
    pc_swap16(&fd->x7C.xA0);
    pc_swap16(&fd->x7C.xA2);
    pc_swap32(&fd->x7C.xA4);
    pc_swap32(&fd->x7C.xA8);
}

static void swap_name_tag(struct NameTagData* t)
{
    pc_swap16_range(t->vs_kos, sizeof(t->vs_kos));
    pc_swap16(&t->sd_count);
    pc_swap32_range(&t->attacks_hit, 5 * 4);
    pc_swap16_range(&t->peak_damage, 4 * 2);
    pc_swap32_range(&t->play_time, 9 * 4);
    pc_swap32_range(t->play_time_by_fighter, sizeof(t->play_time_by_fighter));
}

static void swap_main(struct gmm_x1868* s, int to_console)
{
    int i;
    pc_swap16(&s->unlocked_characers_bitmask);
    pc_swap16(&s->x186A);
    pc_swap32(&s->unk_8.x0);
    pc_swap32(&s->unk_8.x4);
    pc_swap32_range(&s->unk_8.xC, 5 * 4);
    pc_swap32_range(&s->unk_28, sizeof(s->unk_28));
    pc_swap32_range(&s->unk_30.x0, 6 * 4);
    pc_swap16_range(s->unk_30.x18, sizeof(s->unk_30.x18));
    pc_swap32_range(s->unk_30.x4C, sizeof(s->unk_30.x4C));
    pc_swap32_range(s->unk_30.xB0, sizeof(s->unk_30.xB0));
    pc_swap32_range(s->unk_30.x114, sizeof(s->unk_30.x114));
    pc_swap32(&s->unk_1A8.x0);
    pc_swap32_range(&s->time_matches, 20 * 4); /* time_matches .. x1A64 */
    sw64(&s->x1A68);
    pc_swap32_range(s->x1A70, sizeof(s->x1A70));
    pc_swap32_range(s->x1B40, sizeof(s->x1B40));
    pc_swap32_range(s->x1B4C, sizeof(s->x1B4C));
    pc_swap32_range(s->x1B58, sizeof(s->x1B58));
    pc_swap32_range(s->x1B80, sizeof(s->x1B80));
    pc_swap32_range(s->x1C88, sizeof(s->x1C88));
    sw64(&s->x1CB0.item_mask);
    pc_swap32(&s->x1CB0.stage_mask);
    pc_swap16(&s->trophy_count);
    pc_swap16(&s->trophy_category_flags);
    pc_swap16_range(s->trophy_flags, sizeof(s->trophy_flags));
    for (i = 0; i < SELKIND_COUNT; i++) {
        swap_fighter_data(&s->x1F2C[i], to_console);
    }
}

/// Convert a card payload between host and console order, in place.
/// `to_console` says which way; the payload is recognised by its size.
void pc_card_swap_payload(void* payload, int size, int to_console)
{
    static int warned;
    int trace = getenv("MELEE_TRACE_CARD") != NULL;
    if (size == PC_SAVE_MAIN_SIZE) {
        if (trace) {
            fprintf(stderr, "[pc] card: main save block %s console order" "%c", to_console ? "to" : "from", 10);
        }
        swap_main((struct gmm_x1868*) payload, to_console);
        if (trace && !to_console) {
            const struct gmm_x1868* m = (const struct gmm_x1868*) payload;
            fprintf(stderr, "[pc] card: loaded save: characters %#x stages %#x trophies %d vs matches %u" "%c",
                    m->unlocked_characers_bitmask, m->x186A, m->trophy_count, m->time_matches, 10);
        }
    } else if (size == PC_SAVE_TAGS_SIZE) {
        int i;
        struct NameTagDataBank* bank = (struct NameTagDataBank*) payload;
        if (trace) {
            fprintf(stderr, "[pc] card: name-tag bank %s console order" "%c", to_console ? "to" : "from", 10);
        }
        for (i = 0; i < 19; i++) {
            swap_name_tag(&bank->inner[i]);
        }
    } else if (size > 0 && !warned && trace) {
        /* the banner/icon sector: raw texture data, nothing to convert */
        warned = 1;
        fprintf(stderr, "[pc] card: payload of %d bytes is copied as it is\n", size);
    }
}
