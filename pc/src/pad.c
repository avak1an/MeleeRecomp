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

static void read_keyboard(PADStatus* st)
{
    u16 b = 0;
    int x = 0, y = 0, cx = 0, cy = 0;
    if (key('Z')) b |= PAD_BUTTON_A;
    if (key('X')) b |= PAD_BUTTON_B;
    if (key('C')) b |= PAD_BUTTON_X;
    if (key('V')) b |= PAD_BUTTON_Y;
    if (key(VK_RETURN)) b |= PAD_BUTTON_START;
    if (key(VK_SPACE)) b |= PAD_TRIGGER_Z;
    if (key('Q')) b |= PAD_TRIGGER_L;
    if (key('E')) b |= PAD_TRIGGER_R;
    if (key(VK_NUMPAD8)) b |= PAD_BUTTON_UP;
    if (key(VK_NUMPAD2)) b |= PAD_BUTTON_DOWN;
    if (key(VK_NUMPAD4)) b |= PAD_BUTTON_LEFT;
    if (key(VK_NUMPAD6)) b |= PAD_BUTTON_RIGHT;
    if (key(VK_LEFT)) x -= 100;
    if (key(VK_RIGHT)) x += 100;
    if (key(VK_DOWN)) y -= 100;
    if (key(VK_UP)) y += 100;
    if (key('J')) cx -= 100;
    if (key('L')) cx += 100;
    if (key('K')) cy -= 100;
    if (key('I')) cy += 100;
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
        if (pad_initialized && XInputGetState((DWORD) chan, &xs) == ERROR_SUCCESS) {
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
    }
    return connected;
}
