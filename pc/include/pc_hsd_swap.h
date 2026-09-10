/**
 * @file pc_hsd_swap.h
 * Byte-swapping of HSD descriptors loaded from disc (see pc/src/hsd_swap.c).
 * Each engine loader calls the matching function on entry, inside an
 * `#ifdef TARGET_PC` block. Every function accepts NULL and swaps a given
 * descriptor at most once.
 */
#ifndef PC_HSD_SWAP_H
#define PC_HSD_SWAP_H

#include <stddef.h>
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
void pc_swap_imagedesc(HSD_ImageDesc* im);
void pc_swap_tobjtevdesc(HSD_TObjTevDesc* desc);
void pc_swap_pobjdesc(HSD_PObjDesc* desc);
void pc_swap_robjdesc(HSD_RObjDesc* desc);
void pc_swap_spline(HSD_Spline* spline);
void pc_swap_joint_tree(HSD_Joint* joint);
void pc_swap_animjoint(HSD_AnimJoint* anim);
void pc_swap_texanim(HSD_TexAnim* anim);
void pc_swap_sis_message(u8* message);
void pc_swap_ps_banks(void* cmdBank, void* texBank, s32* formBank);

/// Returns true (and records p) the first time p is seen, false afterwards
/// and for anything outside the emulated main memory. For game-side swappers.
bool pc_swap_once(const void* p);

/// True if p was already recorded as swapped (no side effect).
bool pc_swap_is_done(const void* p);

/// True if p is NULL or lies inside the emulated main memory. Debug aid for
/// spotting descriptor pointers that were never relocated or got swapped.
bool pc_swap_ptr_ok(const void* p);

/// Called by the archive pre-pass for every pointer slot it relocates, and
/// queried by swappers whose fields hold either a pointer or an integer.
void pc_swap_note_reloc_slot(const void* slot);
/// MELEE_ARCHIVE_CHECK=1: report relocated pointer slots that changed since parsing.
void pc_swap_verify_relocs(const char* tag);
bool pc_swap_is_archive(const void* p);
bool pc_swap_is_reloc_slot(const void* slot);

/// Record where a relocated slot points, and find the start of the first
/// referenced object in (start, limit) (limit if none): the end of an
/// unsized data block.
void pc_swap_note_reloc_target(const void* slot, const void* target);
void pc_swap_note_archive(const void* base, size_t size); ///< file extent, for the same purpose
const void* pc_swap_next_object(const void* start, const void* limit);

/// Reverse the bit order of a byte / 32-bit word: converts bit-fields laid
/// out MSB-first (the console compiler) to LSB-first (MSVC ABI).
static __inline void pc_swap_bits8(unsigned char* p)
{
    unsigned char b = *p, r = 0;
    int i;
    for (i = 0; i < 8; i++) {
        r = (unsigned char) ((r << 1) | (b & 1));
        b >>= 1;
    }
    *p = r;
}

/// Forget every "already swapped" record inside [base, base + size): call
/// when a loaded file's memory is released so a later file at the same
/// address is swapped again.
void pc_swap_forget_range(void* base, size_t size);

#endif
