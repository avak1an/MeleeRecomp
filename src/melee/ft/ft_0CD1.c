#include "ft_0CD1.h"

#include "fighter.h"
#include "forward.h"
#include "ft_081B.h"
#include "ft_084E.h"
#include "ft_0892.h"
#include "ftanim.h"
#include "ftcommon.h"
#include "ftswing.h"
#ifdef TARGET_PC
#include "pc_runtime.h"
#include <stdlib.h>
#include <melee/ft/ftparts.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/jobj.h>
#endif
#include "inlines.h"
#include "kinds/ftCommon/ftCo_Fall.h"

void ftCo_800CD140(Fighter_GObj* gobj, int arg1, int arg2, int arg3,
                   float arg4)
{
    Fighter* fp = GET_FIGHTER(gobj);
    fp->throw_flags = 0;
    Fighter_ChangeMotionState(gobj, arg1, arg2, 0.0f, arg4, 0.0f, NULL);
    ftAnim_8006EBA4(gobj);
    fp->mv.co.swing.x0 = 1;
    fp->mv.co.swing.x4 = arg3;
    ftCommon_8007E79C(gobj, 1);
}

void ftCo_800CD1BC(Fighter_GObj* gobj)
{
#ifdef TARGET_PC
    if (getenv("MELEE_TRACE_MOTION") != NULL && (pc_frame_count % 20) == 0) {
        Fighter* fp = GET_FIGHTER(gobj);
        int i;
        OSReport("[pc] frame %u: swing state: remaining %d blend %g loop %d parts %d\n", pc_frame_count,
                 ftAnim_IsFramesRemaining(gobj), fp->x8A4_animBlendFrames, fp->x594_b1_loop,
                 ftPartsTable[fp->kind]->parts_num);
        for (i = 0; i < ftPartsTable[fp->kind]->parts_num && i < 3; i++) {
            HSD_JObj* j = fp->parts[i].joint;
            OSReport("[pc]   part %d: b1 %d b0 %d b5 %d jobj %p aobj %p frame %g end %g flags %08x\n", i,
                     fp->parts[i].flags_b1, fp->parts[i].flags_b0, fp->parts[i].flags_b5, (void*) j,
                     j != NULL ? (void*) j->aobj : NULL, j != NULL && j->aobj != NULL ? j->aobj->curr_frame : -1.0f,
                     j != NULL && j->aobj != NULL ? j->aobj->end_frame : -1.0f,
                     j != NULL && j->aobj != NULL ? j->aobj->flags : 0);
        }
    }
#endif
    if (ftAnim_IsFramesRemaining(gobj) == 0) {
        ft_8008A2BC(gobj);
        ftCommon_8007E7E4(gobj, 1);
    }
}

void ftCo_800CD204(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    if ((fp->input.held_buttons[0] & HSD_PAD_A) == 0) {
        fp->mv.co.swing.x0 = 0;
    }
    if (ftCheckThrowB3(fp) && fp->mv.co.swing.x0 != 0) {
        ftCo_Attack_800CCF58(gobj, 3);
    }
}

void ftCo_800CD278(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    if (fp->mv.co.swing.x4 != 4) {
        ft_80084FA8(gobj);
    } else {
        ft_80085030(gobj,
                    p_ftCommonData->x420 * (0, fp->co_attrs.ground_friction),
                    fp->facing_dir);
    }
}

void ftCo_800CD2C4(Fighter_GObj* gobj, void (*cb)(Fighter_GObj*))
{
    if (ft_800827A0(gobj) == 0) {
        cb(gobj);
        ftCo_Fall_Enter(gobj);
    }
}

void ft_800CD31C(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    if (fp->item_gobj != NULL) {
        ftCommon_8007E7E4(gobj, 1);
    }
}
