#ifndef GALE01_391580
#define GALE01_391580

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h>

#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/psstructs.h>
#ifdef TARGET_PC
#include <sysdolphin/baselib/objalloc.h>
#endif

/* 3983A4 */ void hsd_803983A4(HSD_Generator*);
/* 3984F4 */ void psInitDataBankLoad(int bank, const int* cmdBank,
                                     const int* texBank, const u32* ref,
                                     const int* formBank);
/* 398614 */ void psInitDataBankLocate(HSD_Archive* cmdBank,
                                       HSD_Archive* texBank, int* formBank);
/* 3989A0 */ void psInitDataBankRelocate(int* cmdBank, int* texBank,
                                         int* formBank, int* newCmdBank,
                                         int* newTexBank, int* newFormBank);
/* 398A08 */ void hsd_80398A08(u32);
/* 398C04 */ HSD_Particle*
psGenerateParticle0(HSD_Particle** head, int linkNo, int bank, u32 kind,
                    u16 texGroup, u8* list, int life, int palflag, f32 x,
                    f32 y, f32 z, f32 vx, f32 vy, f32 vz, f32 size, f32 grav,
                    f32 fric, HSD_Generator* gp, int flgInterpret);
/* 398F0C */ void hsd_80398F0C(s32, s32, s32, u16, s32, s32, s32, s32, f32,
                               f32, f32, f32, f32, f32, f32, f32, f32);
/* 398F8C */ void hsd_80398F8C(HSD_Particle*, f32);
/* 3991D8 */ s32 hsd_803991D8(HSD_Generator*, HSD_JObj*, f32, f32);
/* 39930C */ void* hsd_8039930C(HSD_Particle*, HSD_Particle*);
/* 39CEAC */ void hsd_8039CEAC(u32);
/* 39CF4C */ void hsd_8039CF4C(s32, HSD_JObj*);
/* 39D048 */ void hsd_8039D048(void* particle);
/* 4D0B50 */ extern HSD_PSTexGroup** psTexGroupArray[65];
/* 4D0C54 */ extern HSD_PSFormGroup** psNumCmdList[65];
/* 4D0D58 */ extern int psCmdListArray[65];
/* 4D0E5C */ extern HSD_PSCmdList** ptclref_804D0E5C[65];

#ifdef TARGET_PC
/* hsd_8039D048 reads the particle tables as one struct starting at
 * hsd_804D08E8 (jobj slots, particle list heads, the per-bank tables and
 * the particle allocator, in link order); on PC they are one object. */
struct pc_particle_block {
    /* 0x000 */ HSD_JObj* jobj[8];
    /* 0x020 */ HSD_Particle* particle[16];
    /* 0x060 */ u32* refs[65];
    /* 0x164 */ HSD_PSFormGroup** form_groups[65];
    /* 0x268 */ HSD_PSTexGroup** tex_groups[65];
    /* 0x36C */ HSD_PSFormGroup** num_cmd_list[65];
    /* 0x470 */ int cmd_list[65];
    /* 0x574 */ HSD_PSCmdList** ptclref[65];
    /* 0x678 */ HSD_ObjAllocData alloc_data;
    void* alloc_pad;
};
extern struct pc_particle_block pc_particle_block;
#define hsd_804D08E8 (pc_particle_block.jobj)
#define hsd_804D0908 (pc_particle_block.particle)
#define hsd_804D0948 (pc_particle_block.refs)
#define psFormGroupArray (pc_particle_block.form_groups)
#define psTexGroupArray (pc_particle_block.tex_groups)
#define psNumCmdList (pc_particle_block.num_cmd_list)
#define psCmdListArray (pc_particle_block.cmd_list)
#define ptclref_804D0E5C (pc_particle_block.ptclref)
#endif
/* 4D78D8 */ extern u16 hsd_804D78D8;
/* 4D78DA */ extern u16 hsd_804D78DA;
/* 4D78DE */ extern u16 hsd_804D78DE;
/* 4D78E0 */ extern u16 hsd_804D78E0;
/* 4D78E8 */ extern u32 hsd_804D78E8;
/* 4D78EC */ extern u32 hsd_804D78EC;
/* 4D78F0 */ extern HSD_CObj* psCamera;
/* 4D78F4 */ extern u32 hsd_804D78F4;

#endif
