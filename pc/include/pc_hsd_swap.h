/**
 * @file pc_hsd_swap.h
 * Byte-swapping of HSD descriptors loaded from disc (see pc/src/hsd_swap.c).
 * Each engine loader calls the matching function on entry, inside an
 * `#ifdef TARGET_PC` block. Every function accepts NULL and swaps a given
 * descriptor at most once.
 */
#ifndef PC_HSD_SWAP_H
#define PC_HSD_SWAP_H

#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/fobj.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/lobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/robj.h>
#include <sysdolphin/baselib/spline.h>
#include <sysdolphin/baselib/tobj.h>
#include <sysdolphin/baselib/wobj.h>

void pc_swap_joint(HSD_Joint* joint);
void pc_swap_cobjdesc(HSD_CObjDesc* desc);
void pc_swap_wobjdesc(HSD_WObjDesc* desc);
void pc_swap_lightdesc(HSD_LightDesc* desc);
void pc_swap_fogdesc(HSD_FogDesc* desc);
void pc_swap_fogadjdesc(HSD_FogAdjDesc* desc);
void pc_swap_aobjdesc(HSD_AObjDesc* desc);
void pc_swap_fobjdesc(HSD_FObjDesc* desc);
void pc_swap_mobjdesc(HSD_MObjDesc* desc);
void pc_swap_tobjdesc(HSD_TObjDesc* desc);
void pc_swap_tlutdesc(HSD_TlutDesc* desc);
void pc_swap_tobjtevdesc(HSD_TObjTevDesc* desc);
void pc_swap_pobjdesc(HSD_PObjDesc* desc);
void pc_swap_robjdesc(HSD_RObjDesc* desc);
void pc_swap_spline(HSD_Spline* spline);
void pc_swap_animjoint(HSD_AnimJoint* anim);
void pc_swap_texanim(HSD_TexAnim* anim);

/// Forget every "already swapped" record inside [base, base + size): call
/// when a loaded file's memory is released so a later file at the same
/// address is swapped again.
void pc_swap_forget_range(void* base, size_t size);

#endif
