/**
 * @file net_game.c
 * The game-facing side of online play: the fixed online rules, and the
 * autopilot that takes both machines from power-on to the character
 * select without anyone touching a controller.
 *
 * Online play runs without a memory card (nobody's save can differ), so
 * the game starts from its built-in defaults; on every scene change the
 * online rules are written over them: stock match, 3 stocks, items off,
 * every character and stage unlocked. Both machines do this at the same
 * frame, so their states stay identical.
 *
 * The autopilot answers the "no memory card" prompt, skips the logo and the
 * opening, presses Start on the title and walks the main menu to VS Mode,
 * Melee. It is a function of the synchronized state alone (the scene and
 * the frames spent in it), so both machines produce the same presses
 * without exchanging them. It switches itself off for good when the
 * character select opens; from there on the two players' controllers are
 * ports 1 and 2.
 */
#include "pc_runtime.h"

#include <dolphin/pad.h>

#include <melee/gm/gmmain_lib.h>
#include <melee/gm/types.h>

/* GameSceneKind values (src/melee/gm/forward.h) */
enum {
    SCENE_TITLE = 0,
    SCENE_MAIN_MENU = 1,
    SCENE_CSS = 8,
    SCENE_MOVIE = 28,
    SCENE_MESSAGE = 39, /* the achievement pop-ups the unlocks queue up */
    SCENE_BOOT = 42,
};

static int scene_kind = -1;
static u32 scene_frame;
static int autopilot_done;

void pc_net_register_state(void)
{
    pc_state_register(&scene_kind, sizeof(scene_kind), "net scene kind");
    pc_state_register(&scene_frame, sizeof(scene_frame), "net scene frame");
    pc_state_register(&autopilot_done, sizeof(autopilot_done), "net autopilot done");
}

static void apply_online_rules(void)
{
    GameRules* rules = gmMainLib_GetGameRules();
    u16* characters = gmMainLib_GetUnlockedCharactersBitmaskPtr();
    u16* stages = gmMainLib_8015EDA4();
    struct gmm_x1CB0* items = gmMainLib_8015CC58();
    rules->mode = 1;          /* stock */
    rules->stock_count = 3;
    items->item_freq = 0xFF;  /* the item switch's "None" (menu index 0, stored minus one) */
    *characters = 0x7FF;      /* the eleven unlockable characters */
    *stages = 0x7FF;          /* the eleven unlockable stages */
}

/// Called by the game at every scene change (gm_1A3F.c).
void pc_net_on_scene(int mode, int state, int kind)
{
    (void) mode;
    (void) state;
    scene_kind = kind;
    scene_frame = pc_frame_count;
    if (!pc_net_active()) {
        return;
    }
    apply_online_rules();
    if (kind == SCENE_CSS) {
        autopilot_done = 1;
    }
}

static void press(PADStatus* st, u16 button)
{
    st->button |= button;
    if (button == PAD_BUTTON_A) {
        st->analogA = 200;
    }
}

/// While it returns 1, `st` (port 1) is the autopilot's and the players'
/// controllers are ignored.
int pc_net_autopilot(PADStatus* st)
{
    u32 t = pc_frame_count - scene_frame;
    if (autopilot_done || scene_kind < 0) {
        return autopilot_done ? 0 : 1;
    }
    switch (scene_kind) {
    case SCENE_BOOT: /* "no memory card", then "continue without saving?": OK */
        if (t > 20 && t % 20 < 2) {
            press(st, PAD_BUTTON_A);
        }
        break;
    case SCENE_MOVIE:
        if (t > 10 && t % 20 < 2) {
            press(st, PAD_BUTTON_START);
        }
        break;
    case SCENE_TITLE:
        if (t > 20 && t % 20 < 2) {
            press(st, PAD_BUTTON_START);
        }
        break;
    case SCENE_MESSAGE:
        if (t > 5 && t % 10 < 2) {
            press(st, PAD_BUTTON_A);
        }
        break;
    case SCENE_MAIN_MENU: /* down to VS Mode, A, then A on Melee */
        if (t >= 45 && t < 47) {
            st->stickY = -100;
        } else if ((t >= 62 && t < 64) || (t >= 92 && t < 94)) {
            press(st, PAD_BUTTON_A);
        }
        break;
    default:
        break;
    }
    return 1;
}
