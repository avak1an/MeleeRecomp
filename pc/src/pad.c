/**
 * @file pad.c
 * Controllers. Ports 1-4 map to XInput devices 0-3; when no gamepad is
 * connected to port 1, the keyboard drives it:
 *
 *   arrows        main stick        Z / X / C / V   A / B / X / Y
 *   I J K L       C stick           Q / E           L / R (full press)
 *   space         Z trigger         Enter           Start
 *   numpad 8/2/4/6  D-pad
 *
 * Keyboard state is read with GetAsyncKeyState, so the console window (or
 * later the game window) must have focus.
 */
#include "pc_runtime.h"

#include <dolphin/pad.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

static u32 pad_spec;
static int pad_initialized;

BOOL PADInit(void)
{
    pad_initialized = 1;
    return 1;
}

void PADSetSpec(u32 spec)
{
    pad_spec = spec;
}

unsigned long PADGetSpec(void)
{
    return pad_spec;
}

void PADSetSamplingRate(unsigned long msec)
{
    (void) msec;
}

int PADReset(unsigned long mask)
{
    (void) mask;
    return 1;
}

BOOL PADRecalibrate(u32 mask)
{
    (void) mask;
    return 1;
}

BOOL PADSync(void)
{
    return 1;
}

void PADControlMotor(s32 chan, u32 command)
{
    XINPUT_VIBRATION vib;
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS) {
        return;
    }
    pc_gcadapter_rumble(chan, (int) command);
    memset(&vib, 0, sizeof(vib));
    if (command == 1) { /* PAD_MOTOR_RUMBLE */
        vib.wLeftMotorSpeed = 48000;
        vib.wRightMotorSpeed = 32000;
    }
    XInputSetState((DWORD) chan, &vib);
}

void PADControlAllMotors(const u32* commandArray)
{
    int i;
    for (i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        PADControlMotor(i, commandArray[i]);
    }
}

static s8 axis(SHORT v)
{
    /* XInput thumbsticks are +-32767; GameCube sticks read about +-100. */
    int s = v / 327;
    if (s > 100) {
        s = 100;
    }
    if (s < -100) {
        s = -100;
    }
    return (s8) s;
}

static void read_xinput(int chan, const XINPUT_GAMEPAD* g, PADStatus* st)
{
    u16 b = 0;
    (void) chan;
    if (g->wButtons & XINPUT_GAMEPAD_A) b |= PAD_BUTTON_A;
    if (g->wButtons & XINPUT_GAMEPAD_B) b |= PAD_BUTTON_B;
    if (g->wButtons & XINPUT_GAMEPAD_X) b |= PAD_BUTTON_X;
    if (g->wButtons & XINPUT_GAMEPAD_Y) b |= PAD_BUTTON_Y;
    if (g->wButtons & XINPUT_GAMEPAD_START) b |= PAD_BUTTON_START;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= PAD_BUTTON_UP;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= PAD_BUTTON_DOWN;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= PAD_BUTTON_LEFT;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= PAD_BUTTON_RIGHT;
    if (g->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= PAD_TRIGGER_Z;
    if (g->bLeftTrigger > 200) b |= PAD_TRIGGER_L;
    if (g->bRightTrigger > 200) b |= PAD_TRIGGER_R;
    st->button = b;
    st->stickX = axis(g->sThumbLX);
    st->stickY = axis(g->sThumbLY);
    st->substickX = axis(g->sThumbRX);
    st->substickY = axis(g->sThumbRY);
    st->triggerLeft = (u8) (g->bLeftTrigger * 200 / 255);
    st->triggerRight = (u8) (g->bRightTrigger * 200 / 255);
    st->analogA = (b & PAD_BUTTON_A) ? 200 : 0;
    st->analogB = (b & PAD_BUTTON_B) ? 200 : 0;
    st->err = PAD_ERR_NONE;
}

static int key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

enum {
    KEY_A, KEY_B, KEY_X, KEY_Y, KEY_Z, KEY_L, KEY_R, KEY_START,
    KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT,
    KEY_SUP, KEY_SDOWN, KEY_SLEFT, KEY_SRIGHT,
    KEY_CUP, KEY_CDOWN, KEY_CLEFT, KEY_CRIGHT,
    KEY_COUNT
};

static const char* const key_action_names[KEY_COUNT] = {
    "A", "B", "X", "Y", "Z", "L", "R", "START",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
    "STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT",
    "C_UP", "C_DOWN", "C_LEFT", "C_RIGHT",
};

/* default layout, see the file comment */
static int key_vk[KEY_COUNT] = {
    'Z', 'X', 'C', 'V', VK_SPACE, 'Q', 'E', VK_RETURN,
    VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6,
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    'I', 'K', 'J', 'L',
};

static const struct {
    const char* name;
    int vk;
} key_names[] = {
    { "ENTER", VK_RETURN }, { "SPACE", VK_SPACE }, { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK },
    { "SHIFT", VK_SHIFT }, { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT }, { "CTRL", VK_CONTROL },
    { "LCTRL", VK_LCONTROL }, { "RCTRL", VK_RCONTROL }, { "ALT", VK_MENU },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
    { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
    { "NUMPAD0", VK_NUMPAD0 }, { "NUMPAD1", VK_NUMPAD1 }, { "NUMPAD2", VK_NUMPAD2 },
    { "NUMPAD3", VK_NUMPAD3 }, { "NUMPAD4", VK_NUMPAD4 }, { "NUMPAD5", VK_NUMPAD5 },
    { "NUMPAD6", VK_NUMPAD6 }, { "NUMPAD7", VK_NUMPAD7 }, { "NUMPAD8", VK_NUMPAD8 },
    { "NUMPAD9", VK_NUMPAD9 }, { "NUMPAD+", VK_ADD }, { "NUMPAD-", VK_SUBTRACT },
    { "NUMPAD*", VK_MULTIPLY }, { "NUMPAD/", VK_DIVIDE }, { "NUMPAD.", VK_DECIMAL },
    { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "MINUS", VK_OEM_MINUS },
    { "PLUS", VK_OEM_PLUS }, { "SEMICOLON", VK_OEM_1 }, { "SLASH", VK_OEM_2 },
    { "BACKTICK", VK_OEM_3 }, { "LBRACKET", VK_OEM_4 }, { "BACKSLASH", VK_OEM_5 },
    { "RBRACKET", VK_OEM_6 }, { "QUOTE", VK_OEM_7 },
};

static int parse_key(const char* name)
{
    size_t i;
    if (name[0] != '\0' && name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') {
            c = (char) (c - 'a' + 'A');
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return c;
        }
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24) {
            return VK_F1 + n - 1;
        }
    }
    for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
        if (_stricmp(key_names[i].name, name) == 0) {
            return key_names[i].vk;
        }
    }
    return 0;
}

static char* trim(char* s)
{
    char* end;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

/**
 * Loads a keyboard layout: lines of `ACTION = KEY`, where ACTION is one of
 * A B X Y Z L R START DPAD_UP/DOWN/LEFT/RIGHT STICK_UP/DOWN/LEFT/RIGHT
 * C_UP/DOWN/LEFT/RIGHT and KEY is a letter, a digit, F1-F24 or a name such
 * as ENTER, SPACE, UP, NUMPAD8, LSHIFT. `#` starts a comment.
 */
int pc_pad_load_keymap(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[256];
    int line_no = 0, errors = 0;
    if (f == NULL) {
        fprintf(stderr, "[pc] keymap: cannot open %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *hash, *eq, *action, *keyname;
        int i, vk;
        line_no++;
        hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue; /* blank or comment */
        }
        *eq = '\0';
        action = trim(line);
        keyname = trim(eq + 1);
        vk = parse_key(keyname);
        for (i = 0; i < KEY_COUNT; i++) {
            if (_stricmp(key_action_names[i], action) == 0) {
                break;
            }
        }
        if (i == KEY_COUNT || vk == 0) {
            fprintf(stderr, "[pc] keymap: %s:%d: unknown %s '%s'\n", path, line_no,
                    i == KEY_COUNT ? "action" : "key", i == KEY_COUNT ? action : keyname);
            errors++;
            continue;
        }
        key_vk[i] = vk;
    }
    fclose(f);
    return errors == 0;
}

static void read_keyboard(PADStatus* st)
{
    u16 b = 0;
    int x = 0, y = 0, cx = 0, cy = 0;
    if (key(key_vk[KEY_A])) b |= PAD_BUTTON_A;
    if (key(key_vk[KEY_B])) b |= PAD_BUTTON_B;
    if (key(key_vk[KEY_X])) b |= PAD_BUTTON_X;
    if (key(key_vk[KEY_Y])) b |= PAD_BUTTON_Y;
    if (key(key_vk[KEY_START])) b |= PAD_BUTTON_START;
    if (key(key_vk[KEY_Z])) b |= PAD_TRIGGER_Z;
    if (key(key_vk[KEY_L])) b |= PAD_TRIGGER_L;
    if (key(key_vk[KEY_R])) b |= PAD_TRIGGER_R;
    if (key(key_vk[KEY_DUP])) b |= PAD_BUTTON_UP;
    if (key(key_vk[KEY_DDOWN])) b |= PAD_BUTTON_DOWN;
    if (key(key_vk[KEY_DLEFT])) b |= PAD_BUTTON_LEFT;
    if (key(key_vk[KEY_DRIGHT])) b |= PAD_BUTTON_RIGHT;
    if (key(key_vk[KEY_SLEFT])) x -= 100;
    if (key(key_vk[KEY_SRIGHT])) x += 100;
    if (key(key_vk[KEY_SDOWN])) y -= 100;
    if (key(key_vk[KEY_SUP])) y += 100;
    if (key(key_vk[KEY_CLEFT])) cx -= 100;
    if (key(key_vk[KEY_CRIGHT])) cx += 100;
    if (key(key_vk[KEY_CDOWN])) cy -= 100;
    if (key(key_vk[KEY_CUP])) cy += 100;
    st->button = b;
    st->stickX = (s8) x;
    st->stickY = (s8) y;
    st->substickX = (s8) cx;
    st->substickY = (s8) cy;
    st->triggerLeft = (b & PAD_TRIGGER_L) ? 200 : 0;
    st->triggerRight = (b & PAD_TRIGGER_R) ? 200 : 0;
    st->analogA = (b & PAD_BUTTON_A) ? 200 : 0;
    st->analogB = (b & PAD_BUTTON_B) ? 200 : 0;
    st->err = PAD_ERR_NONE;
}

/* --- Scripted input (--input FILE) ------------------------------------------
 * Each line: <first frame> <last frame> <buttons or -> [stickX stickY]
 * buttons are A B X Y Z L R START UP DOWN LEFT RIGHT joined by '+'. Stick
 * values are -100..100. Lines starting with '#' are comments. */
typedef struct ScriptStep {
    u32 first, last;
    u16 buttons;
    s8 x, y;
} ScriptStep;

#define MAX_STEPS 256
static ScriptStep steps[MAX_STEPS];
static int num_steps;
static int script_loaded;

static u16 parse_buttons(const char* s)
{
    u16 b = 0;
    char buf[128];
    char* tok;
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (tok = strtok(buf, "+"); tok != NULL; tok = strtok(NULL, "+")) {
        if (!strcmp(tok, "A")) b |= PAD_BUTTON_A;
        else if (!strcmp(tok, "B")) b |= PAD_BUTTON_B;
        else if (!strcmp(tok, "X")) b |= PAD_BUTTON_X;
        else if (!strcmp(tok, "Y")) b |= PAD_BUTTON_Y;
        else if (!strcmp(tok, "Z")) b |= PAD_TRIGGER_Z;
        else if (!strcmp(tok, "L")) b |= PAD_TRIGGER_L;
        else if (!strcmp(tok, "R")) b |= PAD_TRIGGER_R;
        else if (!strcmp(tok, "START")) b |= PAD_BUTTON_START;
        else if (!strcmp(tok, "UP")) b |= PAD_BUTTON_UP;
        else if (!strcmp(tok, "DOWN")) b |= PAD_BUTTON_DOWN;
        else if (!strcmp(tok, "LEFT")) b |= PAD_BUTTON_LEFT;
        else if (!strcmp(tok, "RIGHT")) b |= PAD_BUTTON_RIGHT;
    }
    return b;
}

static void load_script(void)
{
    FILE* f;
    char line[256];
    script_loaded = 1;
    if (pc_config.input_script == NULL) {
        return;
    }
    f = fopen(pc_config.input_script, "r");
    if (f == NULL) {
        fprintf(stderr, "[pc] cannot open input script %s\n", pc_config.input_script);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL && num_steps < MAX_STEPS) {
        char btn[64];
        int x = 0, y = 0;
        unsigned a, b;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (sscanf(line, "%u %u %63s %d %d", &a, &b, btn, &x, &y) >= 3) {
            ScriptStep* s = &steps[num_steps++];
            s->first = a;
            s->last = b;
            s->buttons = strcmp(btn, "-") == 0 ? 0 : parse_buttons(btn);
            s->x = (s8) x;
            s->y = (s8) y;
        }
    }
    fclose(f);
    fprintf(stderr, "[pc] input script: %d steps from %s\n", num_steps, pc_config.input_script);
}

static void apply_script(PADStatus* st)
{
    int i;
    if (!script_loaded) {
        load_script();
    }
    for (i = 0; i < num_steps; i++) {
        const ScriptStep* s = &steps[i];
        if (pc_frame_count >= s->first && pc_frame_count <= s->last) {
            st->button |= s->buttons;
            if (s->x != 0 || s->y != 0) {
                st->stickX = s->x;
                st->stickY = s->y;
            }
            if (s->buttons & PAD_BUTTON_A) st->analogA = 200;
            if (s->buttons & PAD_BUTTON_B) st->analogB = 200;
            if (s->buttons & PAD_TRIGGER_L) st->triggerLeft = 200;
            if (s->buttons & PAD_TRIGGER_R) st->triggerRight = 200;
        }
    }
}

/* Headless runs: tap Start, then A, for two frames each every 150 frames so
 * prompts (memory card, title screen) are dismissed. */
static void autoplay(PADStatus* st)
{
    u32 t = pc_frame_count % 150;
    if (t < 2) {
        st->button |= PAD_BUTTON_START;
    } else if (t >= 60 && t < 62) {
        st->button |= PAD_BUTTON_A;
        st->analogA = 200;
    }
}

u32 PADRead(struct PADStatus* status)
{
    int chan;
    u32 connected = 0;
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++) {
        XINPUT_STATE xs;
        PADStatus* st = &status[chan];
        memset(st, 0, sizeof(*st));
        if (pc_config.headless || pc_config.input_script != NULL) {
            /* scripted / headless runs must not see the host keyboard or
             * controllers, or they stop being repeatable */
            if (chan == 0) {
                connected |= PAD_CHAN0_BIT;
            } else {
                st->err = PAD_ERR_NO_CONTROLLER;
            }
        } else if (pc_gcadapter_read(chan, st)) {
            /* a GameCube controller on the official USB adapter */
            connected |= PAD_CHAN0_BIT >> chan;
        } else if (pad_initialized && XInputGetState((DWORD) chan, &xs) == ERROR_SUCCESS) {
            read_xinput(chan, &xs.Gamepad, st);
            connected |= PAD_CHAN0_BIT >> chan;
        } else if (chan == 0) {
            read_keyboard(st);
            connected |= PAD_CHAN0_BIT;
        } else {
            st->err = PAD_ERR_NO_CONTROLLER;
        }
        if (chan == 0 && pc_config.autoplay) {
            autoplay(st);
        }
        if (chan == 0 && pc_config.input_script != NULL) {
            apply_script(st);
        }
    }
    return connected;
}
