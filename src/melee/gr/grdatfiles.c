#include "grdatfiles.h"
#ifdef TARGET_PC
#include <pc_game_swap.h>
#include <stdlib.h>
#include <pc_hsd_swap.h>
#include <pc_endian.h>
extern const unsigned int* pc_debug_watch;
void pc_debug_watch_install(void);
extern int pc_debug_gx;
#endif

#include "ground.h"
#include "types.h"
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbheap.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/debug.h>
#include <sysdolphin/baselib/particle.h>
#include <sysdolphin/baselib/psstructs.h>

/* 1C6228 */ static void grDatFiles_801C6228(UnkStageDat*);
/* 1C62B4 */ static UnkArchiveStruct* grDatFiles_801C62B4(void);

/// @todo Merge declaration and definition
/* static */ extern GroundParam grDatFiles_803E0848;

/// @todo Merge declaration and definition
/* static */ extern UnkStageDat grDatFiles_803E0924;

void grDatFiles_801C5FC0(HSD_Archive* archive, void* data, size_t length)
{
    HSD_Archive* map_ptcl;
    HSD_Archive* map_texg;
    lbArchive_InitializeDAT(archive, data, length);
    map_ptcl = HSD_ArchiveGetPublicAddress(archive, "map_ptcl");
    map_texg = HSD_ArchiveGetPublicAddress(archive, "map_texg");

    if (map_ptcl != NULL && map_texg != NULL) {
        psInitDataBankLocate(map_ptcl, map_texg, NULL);
    }
}

void grDatFiles_801C6038(void* arg0, s32 arg1, s32 arg2)
{
    UnkArchiveStruct* temp_r3 = grDatFiles_801C62B4();
    if (arg0 != NULL) {
        HSD_Archive* sp14;
        s32 phi_r28;
        void* r4 = arg0;
        if (arg2 != 0) {
            phi_r28 =
                lbArchive_800171CC(&sp14, r4, &temp_r3->unk4, "map_head", 0);
        } else {
            sp14 =
                lbArchive_80016DBC(r4, (void**) &temp_r3->unk4, "map_head", 0);
            phi_r28 = 0;
        }
        temp_r3->unk8 = 0;
        if (arg1 == 0) {
            stage_info.coll_data =
                HSD_ArchiveGetPublicAddress(sp14, "coll_data");
            stage_info.param =
                HSD_ArchiveGetPublicAddress(sp14, "grGroundParam");
            stage_info.itemdata =
                HSD_ArchiveGetPublicAddress(sp14, "itemdata");
            stage_info.ald_yaku_all =
                HSD_ArchiveGetPublicAddress(sp14, "ALDYakuAll");
            stage_info.map_ptcl =
                HSD_ArchiveGetPublicAddress(sp14, "map_ptcl");
            stage_info.map_texg =
                HSD_ArchiveGetPublicAddress(sp14, "map_texg");
            stage_info.yakumono_param =
                HSD_ArchiveGetPublicAddress(sp14, "yakumono_param");
            stage_info.map_plit =
                HSD_ArchiveGetPublicAddress(sp14, "map_plit");
            stage_info.quake_model_set =
                HSD_ArchiveGetPublicAddress(sp14, "quake_model_set");
        }
        temp_r3->unk0 = sp14;
#ifdef TARGET_PC
        /* debugging aid (slow): MELEE_WATCH_PTCL=1 catches swaps that reach
         * into the particle bank */
        if (getenv("MELEE_WATCH_PTCL") != NULL) {
            pc_debug_watch = (const unsigned int*) stage_info.map_ptcl;
        }
        if (pc_debug_gx) {
            OSReport("[gx] stage grkind %d (arg1 %d); on_check_shadow_render %p at %p\n", stage_info.grkind,
                     arg1, (void*) stage_info.on_check_shadow_render, (void*) &stage_info.on_check_shadow_render);
        }
        pc_debug_watch_install();
        pc_swap_stage_data(temp_r3->unk4, arg1 == 0 ? stage_info.param : NULL,
                           arg1 == 0 ? stage_info.coll_data : NULL,
                           arg1 == 0 ? (void**) stage_info.itemdata : NULL);
#endif
        if (stage_info.map_ptcl != NULL && stage_info.map_texg != NULL) {
            if (phi_r28 != 0) {
                psInitDataBankLoad(0x40, stage_info.map_ptcl,
                                   stage_info.map_texg, 0, 0);
            } else {
                psInitDataBank(0x40, stage_info.map_ptcl, stage_info.map_texg,
                               0, 0);
            }
        }
        grDatFiles_801C6228(temp_r3->unk4);
    } else {
        temp_r3->unk4 = &grDatFiles_803E0924;
        if (arg1 == 0) {
            stage_info.coll_data = NULL;
            stage_info.param = &grDatFiles_803E0848;
            stage_info.itemdata = NULL;
            stage_info.ald_yaku_all = NULL;
            stage_info.map_ptcl = NULL;
            stage_info.map_texg = NULL;
            stage_info.yakumono_param = NULL;
            stage_info.map_plit = NULL;
            stage_info.x6C8 = NULL;
        }
        temp_r3->unk0 = (void*) -1;
    }
}

void grDatFiles_801C6228(UnkStageDat* arg0)
{
    if (arg0 != NULL && arg0->unk28 != NULL && arg0->unk2C != 0) {
        s32 i;
#ifdef TARGET_PC
        if (pc_debug_gx) {
            for (i = 0; i < arg0->unk2C; i++) {
                OSReport("[gx] x28[%d] = %p (flag %08x)", i, arg0->unk28[i], arg0->unk28[i] != NULL ? arg0->unk28[i]->unk4 : 0);
            }
        }
#endif
        for (i = 0; i < arg0->unk2C; i++) {
            UnkStageDatInternal* temp_r4 = arg0->unk28[i];
            if (temp_r4 != NULL) {
#ifdef TARGET_PC
                /* joint or material descriptors still in disc byte order
                 * until their loader swaps them: set the bit accordingly */
                if (!pc_swap_is_done(temp_r4)) {
                    temp_r4->unk4 |= PC_BSWAP32(0x4000000);
                } else {
                    temp_r4->unk4 |= 0x4000000;
                }
#else
                temp_r4->unk4 |= 0x4000000;
#endif
            }
        }
    }
}

static UnkArchiveStruct grDatFiles_8049EE10[4];

void grDatFiles_801C6288(void)
{
    memzero(&grDatFiles_8049EE10, sizeof(grDatFiles_8049EE10));
}

UnkArchiveStruct* grDatFiles_801C62B4(void)
{
    s32 i;
    for (i = 0; i < 4; i++) {
        if (grDatFiles_8049EE10[i].unk0 == NULL) {
            return &grDatFiles_8049EE10[i];
        }
    }
    HSD_ASSERT(229, 0);
}

UnkArchiveStruct* grDatFiles_GetArchive(void)
{
    return grDatFiles_8049EE10;
}

UnkArchiveStruct* grDatFiles_801C6330(s32 arg0)
{
    if (arg0 >= 0) {
        s32 i;
        for (i = 0; i < 4; i++) {
            if (grDatFiles_8049EE10[i].unk0 != NULL) {
                UnkStageDat* temp_r7 = grDatFiles_8049EE10[i].unk4;
                if (temp_r7 != NULL && temp_r7->unkC > arg0 &&
                    temp_r7->unk8[arg0].unk0 != 0)
                {
                    return &grDatFiles_8049EE10[i];
                }
            }
        }
    }
    return NULL;
}

UnkArchiveStruct* grDatFiles_801C6478(void* data, s32 length)
{
    UnkArchiveStruct* arc;

    HSD_Archive* archive = lbHeap_80015BD0(0, sizeof(HSD_Archive));
    lbArchive_InitializeDAT(archive, data, length);
    arc = grDatFiles_801C62B4();
    HSD_ASSERT(290, arc);
    arc->unk0 = archive;
    arc->unk4 = HSD_ArchiveGetPublicAddress(archive, "map_head");
    arc->unk8 = 1;
#ifdef TARGET_PC
    /* the transformation's map_head is read right away (its x28 table
     * count); unswapped the count was garbage and the walk crashed */
    pc_swap_map_head(arc->unk4);
#endif

    grDatFiles_801C6228(arc->unk4);

    return arc;
}

static StageParam grDatFiles_803E07E4 = {
    0, -1, -1, 0, 0, 0, 0, 0, { 0 },
};

GroundParam grDatFiles_803E0848 = {
    1,  0x80, { 0 }, 0x1E, 0,  1,     0x8000, 10,
    0,  0,    1,     1,    1,  { 0 }, 40,     10,
    50, 100,  10,    10,   10, 10,    false,  0,
    0,  0,    30,    10,   0,  0,     { 0 },  &grDatFiles_803E07E4,
    1,
};

UnkStageDat grDatFiles_803E0924 = { 0 };
