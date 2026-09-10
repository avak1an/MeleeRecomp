/**
 * @file gcadapter.c
 * The official Wii U / Switch GameCube controller adapter (USB 057E:0337)
 * over WinUSB. The adapter must use the WinUSB driver (installed with
 * Zadig, the same step Dolphin needs); with Windows' default driver the
 * device is not reachable and the game silently uses XInput or the
 * keyboard instead.
 *
 * Protocol: a single 0x13 byte written to endpoint 2 starts the adapter's
 * reports; endpoint 0x81 then delivers 37-byte reports (0x21 followed by
 * four 9-byte port blocks: status, two button bytes, main stick X/Y, C
 * stick X/Y, L and R analog). Rumble is 0x11 followed by four bytes.
 *
 * A thread reads reports continuously and keeps the latest one; the game
 * reads it once per frame. The adapter is looked for again every two
 * seconds while absent, so it can be plugged in during play.
 */
#include "pc_runtime.h"

#include <dolphin/pad.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

#define GC_VID_PID "vid_057e&pid_0337"
#define GC_REPORT_SIZE 37
#define GC_PORTS 4

static const GUID usb_device_guid = { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

static HANDLE dev_handle = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE usb;
static HANDLE reader;
static CRITICAL_SECTION lock;
static int lock_ready;
static volatile LONG connected;
static u8 latest[GC_REPORT_SIZE];
static int have_report;
static DWORD last_probe;
static int announced_missing;
static u8 origin[GC_PORTS][6]; /* stick and trigger rest positions per port */
static int have_origin[GC_PORTS];
static u8 rumble_state[GC_PORTS];

static void adapter_close(void)
{
    if (usb != NULL) {
        WinUsb_Free(usb);
        usb = NULL;
    }
    if (dev_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev_handle);
        dev_handle = INVALID_HANDLE_VALUE;
    }
    InterlockedExchange(&connected, 0);
}

static DWORD WINAPI reader_main(LPVOID arg)
{
    u8 buf[GC_REPORT_SIZE];
    (void) arg;
    for (;;) {
        ULONG got = 0;
        if (!WinUsb_ReadPipe(usb, 0x81, buf, sizeof(buf), &got, NULL)) {
            break; /* unplugged or error: the main thread reconnects */
        }
        if (got == GC_REPORT_SIZE && buf[0] == 0x21) {
            EnterCriticalSection(&lock);
            memcpy(latest, buf, sizeof(latest));
            have_report = 1;
            LeaveCriticalSection(&lock);
        }
    }
    InterlockedExchange(&connected, 0);
    return 0;
}

static int adapter_open(void)
{
    HDEVINFO set = SetupDiGetClassDevsA(&usb_device_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA iface;
    DWORD index;
    int opened = 0;
    if (set == INVALID_HANDLE_VALUE) {
        return 0;
    }
    iface.cbSize = sizeof(iface);
    for (index = 0; !opened && SetupDiEnumDeviceInterfaces(set, NULL, &usb_device_guid, index, &iface); index++) {
        char buf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) buf;
        DWORD need = 0;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(buf), &need, NULL)) {
            continue;
        }
        {
            char lower[1024];
            size_t i;
            for (i = 0; detail->DevicePath[i] != '\0' && i < sizeof(lower) - 1; i++) {
                lower[i] = (char) tolower((unsigned char) detail->DevicePath[i]);
            }
            lower[i] = '\0';
            if (strstr(lower, GC_VID_PID) == NULL) {
                continue;
            }
        }
        dev_handle = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
        if (dev_handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        if (!WinUsb_Initialize(dev_handle, &usb)) {
            CloseHandle(dev_handle);
            dev_handle = INVALID_HANDLE_VALUE;
            continue;
        }
        {
            u8 start = 0x13;
            ULONG sent = 0;
            WinUsb_WritePipe(usb, 0x02, &start, 1, &sent, NULL);
        }
        opened = 1;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!opened) {
        return 0;
    }
    have_report = 0;
    memset(have_origin, 0, sizeof(have_origin));
    InterlockedExchange(&connected, 1);
    reader = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
    if (reader == NULL) {
        adapter_close();
        return 0;
    }
    CloseHandle(reader);
    reader = NULL;
    fprintf(stderr, "[pc] GameCube adapter connected\n");
    announced_missing = 0;
    return 1;
}

/// Called once per frame before the ports are read: (re)connects.
static void adapter_poll(void)
{
    DWORD now;
    if (!lock_ready) {
        InitializeCriticalSection(&lock);
        lock_ready = 1;
    }
    if (connected) {
        return;
    }
    if (usb != NULL) {
        adapter_close(); /* the reader thread stopped: the device went away */
        fprintf(stderr, "[pc] GameCube adapter disconnected\n");
    }
    now = GetTickCount();
    if (last_probe != 0 && now - last_probe < 2000) {
        return;
    }
    last_probe = now;
    if (!adapter_open() && !announced_missing) {
        announced_missing = 1;
    }
}

static s8 stick(u8 raw, u8 rest)
{
    int v = (int) raw - (int) rest;
    /* the adapter reports about +-80 at full deflection; the game clamps to
     * its own circle, so keep the raw scale */
    if (v > 127) {
        v = 127;
    }
    if (v < -128) {
        v = -128;
    }
    return (s8) v;
}

/**
 * Fills the status of an adapter port. Returns false when no adapter is
 * present or no controller is plugged into that port.
 */
int pc_gcadapter_read(int port, PADStatus* st)
{
    u8 r[GC_REPORT_SIZE];
    const u8* p;
    u16 b = 0;
    if (port == 0) {
        adapter_poll();
    }
    if (!connected || port < 0 || port >= GC_PORTS) {
        return 0;
    }
    EnterCriticalSection(&lock);
    if (!have_report) {
        LeaveCriticalSection(&lock);
        return 0;
    }
    memcpy(r, latest, sizeof(r));
    LeaveCriticalSection(&lock);
    p = r + 1 + port * 9;
    {
        /* MELEE_TRACE_PAD=1: the port states once, then a connected
         * controller's raw values about once a second */
        static int trace = -1;
        static DWORD last_trace;
        static int announced;
        if (trace < 0) {
            trace = getenv("MELEE_TRACE_PAD") != NULL;
        }
        if (trace && port == 0 && !announced) {
            fprintf(stderr, "[pad] adapter ports: %02x %02x %02x %02x (0x10 wired, 0x20 wireless)\n", r[1], r[10],
                    r[19], r[28]);
            announced = 1;
        }
        if (trace && (p[0] & 0x30) && GetTickCount() - last_trace > 1000) {
            last_trace = GetTickCount();
            fprintf(stderr, "[pad] port %d: buttons %02x %02x stick %3u %3u c %3u %3u triggers %3u %3u\n", port + 1,
                    p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
        }
    }
    if (!(p[0] & 0x30)) { /* neither wired (0x10) nor wireless (0x20) */
        have_origin[port] = 0;
        return 0;
    }
    if (!have_origin[port]) {
        /* the first report gives the rest position, as PADInit's origin
         * read does on the console */
        memcpy(origin[port], p + 3, 6);
        have_origin[port] = 1;
    }
    if (p[1] & 0x01) b |= PAD_BUTTON_A;
    if (p[1] & 0x02) b |= PAD_BUTTON_B;
    if (p[1] & 0x04) b |= PAD_BUTTON_X;
    if (p[1] & 0x08) b |= PAD_BUTTON_Y;
    if (p[1] & 0x10) b |= PAD_BUTTON_LEFT;
    if (p[1] & 0x20) b |= PAD_BUTTON_RIGHT;
    if (p[1] & 0x40) b |= PAD_BUTTON_DOWN;
    if (p[1] & 0x80) b |= PAD_BUTTON_UP;
    if (p[2] & 0x01) b |= PAD_BUTTON_START;
    if (p[2] & 0x02) b |= PAD_TRIGGER_Z;
    if (p[2] & 0x04) b |= PAD_TRIGGER_R;
    if (p[2] & 0x08) b |= PAD_TRIGGER_L;
    st->button = b;
    st->stickX = stick(p[3], origin[port][0]);
    st->stickY = stick(p[4], origin[port][1]);
    st->substickX = stick(p[5], origin[port][2]);
    st->substickY = stick(p[6], origin[port][3]);
    st->triggerLeft = (u8) (p[7] > origin[port][4] ? p[7] - origin[port][4] : 0);
    st->triggerRight = (u8) (p[8] > origin[port][5] ? p[8] - origin[port][5] : 0);
    st->analogA = (b & PAD_BUTTON_A) ? 200 : 0;
    st->analogB = (b & PAD_BUTTON_B) ? 200 : 0;
    st->err = PAD_ERR_NONE;
    return 1;
}

/// Sets a port's rumble motor (1 on, 0 off, 2 stop/brake).
void pc_gcadapter_rumble(int port, int command)
{
    u8 msg[5];
    ULONG sent = 0;
    if (!connected || port < 0 || port >= GC_PORTS) {
        return;
    }
    /* PAD_MOTOR_RUMBLE (1) starts the motor; PAD_MOTOR_STOP (0) and
     * PAD_MOTOR_STOP_HARD (2) both stop it. The adapter's own "2" is a
     * brake pulse that left the motor running on the official adapter, and
     * the game ends every fight rumble with STOP_HARD. */
    rumble_state[port] = (u8) (command == 1 ? 1 : 0);
    msg[0] = 0x11;
    memcpy(msg + 1, rumble_state, 4);
    WinUsb_WritePipe(usb, 0x02, msg, sizeof(msg), &sent, NULL);
}

/// True when the adapter is open (for the startup log).
int pc_gcadapter_present(void)
{
    return connected != 0;
}
