/**
 * @file launcher.c
 * Graphical launcher for the PC port: a plain Win32 window (no runtime
 * dependencies) that lives next to melee.exe. It picks the disc image,
 * builds the game, and sets display, audio, keyboard, save and mod
 * options, remembers them in launcher.ini, extracts the disc's files for
 * modding, keeps mods/enabled.txt in step with the mod list, and starts
 * the game. The window is drawn by hand in a dark style: a sidebar of
 * pages, cards for each group of settings, and a Play button.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <setupapi.h>
#include <wincrypt.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/pc_version.h"

#define APP_TITLE "Super Smash Bros. Melee PC " PC_PORT_VERSION
#define INI_NAME "launcher.ini"

enum {
    IDC_ISO_EDIT = 100,
    IDC_ISO_BROWSE,
    IDC_ISO_STATUS, /* painted labels: set_text stores their text */
    IDC_ISO_VERIFY,
    IDC_ISO_HASH,
    IDC_SCALE_1,
    IDC_SCALE_2,
    IDC_SCALE_3,
    IDC_SCALE_4,
    IDC_INTERNAL_0, /* window, 1x .. 4x */
    IDC_INTERNAL_1,
    IDC_INTERNAL_2,
    IDC_INTERNAL_3,
    IDC_INTERNAL_4,
    IDC_MSAA_0, /* off, 2x, 4x, 8x */
    IDC_MSAA_1,
    IDC_MSAA_2,
    IDC_MSAA_3,
    IDC_ANISO_0, /* off, 4x, 16x */
    IDC_ANISO_1,
    IDC_ANISO_2,
    IDC_FULLSCREEN,
    IDC_NO_CONSOLE,
    IDC_VOLUME,
    IDC_MUTE,
    IDC_KEYMAP_EDIT,
    IDC_KEYMAP_BROWSE,
    IDC_KEYMAP_CREATE,
    IDC_ADAPTER,
    IDC_ADAPTER_STATUS,
    IDC_SAVES_EDIT,
    IDC_SAVES_BROWSE,
    IDC_SAVES_OPEN,
    IDC_MODS_LIST,
    IDC_MODS_UP,
    IDC_MODS_DOWN,
    IDC_MODS_REFRESH,
    IDC_MODS_OPEN,
    IDC_EXTRACT,
    IDC_EXTRACT_OPEN,
    IDC_EXTRA_EDIT,
    IDC_SRC_EDIT,
    IDC_SRC_BROWSE,
    IDC_BUILD,
    IDC_BUILD_STATUS,
    IDC_PLAY,
    IDC_STATUS,
    IDC_NAV_FIRST, /* one sidebar item per page */
    IDC_NAV_LAST = IDC_NAV_FIRST + 7,
    IDC_LAST
};

#define TIMER_PROCESS 1
#define WM_HASH_DONE (WM_APP + 1)

/* Super Smash Bros. Melee (USA) v1.02: the image the decompilation targets,
 * and its main.dol (the hash in the repository README). */
#define KNOWN_ISO_SHA1 "d4e70c064cc714ba8400a849cf299dbd1aa326fc"
#define KNOWN_DOL_SHA1 "08e0bf20134dfcb260699671004527b2d6bb1a45"

static HINSTANCE app;
static HWND main_wnd, content_wnd; /* content_wnd: the scrollable card area */
static HFONT ui_font, bold_font;
static char exe_dir[MAX_PATH];
static char exe_path[MAX_PATH]; /* this program's own file */
static char ini_path[MAX_PATH];
static char extract_dir[MAX_PATH];
static HANDLE child; /* running game, extraction or build */
static int child_kind;  /* 0 game, 1 extraction, 2 build */

static int find_game_exe(char* out, int size);
static int is_label(int id);
static void ui_iso_changed(int ok, const char* path);
static void ui_mods_changed(void);
static char labels[IDC_LAST - 100][512]; /* the painted texts, by control id */

static HWND ctl(int id)
{
    HWND c = content_wnd != NULL ? GetDlgItem(content_wnd, id) : NULL;
    return c != NULL ? c : GetDlgItem(main_wnd, id);
}

static void set_text(int id, const char* text)
{
    if (is_label(id)) {
        strncpy(labels[id - 100], text, sizeof(labels[0]) - 1);
        labels[id - 100][sizeof(labels[0]) - 1] = 0;
        if (content_wnd != NULL) {
            InvalidateRect(content_wnd, NULL, FALSE);
        }
        if (main_wnd != NULL) {
            InvalidateRect(main_wnd, NULL, FALSE);
        }
        return;
    }
    SetWindowTextA(ctl(id), text);
}

static void get_text(int id, char* out, int size)
{
    GetWindowTextA(ctl(id), out, size);
}

static int file_exists(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* --- settings ------------------------------------------------------------- */

static void ini_get(const char* key, char* out, int size, const char* def)
{
    GetPrivateProfileStringA("melee", key, def, out, (DWORD) size, ini_path);
}

static void ini_set(const char* key, const char* value)
{
    WritePrivateProfileStringA("melee", key, value, ini_path);
}

static int ini_get_int(const char* key, int def)
{
    return (int) GetPrivateProfileIntA("melee", key, def, ini_path);
}

static void ini_set_int(const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    ini_set(key, buf);
}

/* --- disc image ------------------------------------------------------------ */

static int probe_iso(const char* path, char* status, int size)
{
    FILE* f;
    unsigned char hdr[0x40];
    if (path[0] == '\0') {
        snprintf(status, size, "Choose the GALE01 disc image (.iso or .gcm).");
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(status, size, "File not found.");
        return 0;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        snprintf(status, size, "Not a disc image.");
        return 0;
    }
    fclose(f);
    if (memcmp(hdr, "GALE01", 6) != 0) {
        snprintf(status, size, "Not a Super Smash Bros. Melee (GALE01) image: id %.6s.", hdr);
        return 0;
    }
    snprintf(status, size, "Disc image found (GALE01, NTSC)");
    return 1;
}

static void find_default_iso(char* out, int size)
{
    static const char* names[] = { "GALE01.iso", "GALE01.gcm", "melee.iso", NULL };
    static const char* dirs[] = { "%s", "%s\\..", "%s\\..\\..", "%s\\..\\..\\..", NULL };
    int d, n;
    for (d = 0; dirs[d] != NULL; d++) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), dirs[d], exe_dir);
        for (n = 0; names[n] != NULL; n++) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\%s", dir, names[n]);
            if (file_exists(path)) {
                GetFullPathNameA(path, size, out, NULL);
                return;
            }
        }
    }
    out[0] = '\0';
}

static void refresh_iso_status(void)
{
    char path[MAX_PATH], status[256];
    int ok;
    get_text(IDC_ISO_EDIT, path, sizeof(path));
    ok = probe_iso(path, status, sizeof(status));
    set_text(IDC_ISO_STATUS, status);
    set_text(IDC_ISO_HASH, "");
    ui_iso_changed(ok, path);
}

/* --- disc verification --------------------------------------------------------
 * SHA-1 of the disc's main.dol (a few hundred KB, checked against the
 * decompilation's reference) and of the whole image, on a worker thread. */

static char hash_iso_path[MAX_PATH];
static char hash_result[512];
static volatile LONG hash_running;

static uint32_t rd32(const unsigned char* p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static int sha1_range(FILE* f, unsigned long long offset, unsigned long long length, char* hex)
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    static unsigned char buf[1 << 20];
    unsigned char digest[20];
    DWORD dlen = sizeof(digest), i;
    int ok = 0;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return 0;
    }
    if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash) && _fseeki64(f, (long long) offset, SEEK_SET) == 0) {
        unsigned long long left = length;
        ok = 1;
        while (left > 0) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t) left;
            size_t got = fread(buf, 1, want, f);
            if (got == 0 || !CryptHashData(hash, buf, (DWORD) got, 0)) {
                ok = 0;
                break;
            }
            left -= got;
        }
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) {
            for (i = 0; i < 20; i++) {
                sprintf(hex + i * 2, "%02x", digest[i]);
            }
            hex[40] = '\0';
        } else {
            ok = 0;
        }
    }
    if (hash) {
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return ok;
}

static DWORD WINAPI hash_thread(LPVOID arg)
{
    FILE* f = fopen(hash_iso_path, "rb");
    unsigned char header[0x440], dol[0x100];
    char dol_hex[41], iso_hex[41];
    unsigned long long dol_size = 0, iso_size;
    int s;
    (void) arg;
    if (f == NULL) {
        snprintf(hash_result, sizeof(hash_result), "The disc image could not be opened.");
        goto done;
    }
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || _fseeki64(f, rd32(header + 0x420), SEEK_SET) != 0 ||
        fread(dol, 1, sizeof(dol), f) != sizeof(dol))
    {
        snprintf(hash_result, sizeof(hash_result), "The disc image could not be read.");
        goto done;
    }
    for (s = 0; s < 18; s++) {
        unsigned long long end = (unsigned long long) rd32(dol + s * 4) + rd32(dol + 0x90 + s * 4);
        if (rd32(dol + 0x90 + s * 4) != 0 && end > dol_size) {
            dol_size = end;
        }
    }
    _fseeki64(f, 0, SEEK_END);
    iso_size = (unsigned long long) _ftelli64(f);
    if (!sha1_range(f, rd32(header + 0x420), dol_size, dol_hex) || !sha1_range(f, 0, iso_size, iso_hex)) {
        snprintf(hash_result, sizeof(hash_result), "Hashing failed.");
        goto done;
    }
    if (strcmp(iso_hex, KNOWN_ISO_SHA1) == 0) {
        snprintf(hash_result, sizeof(hash_result), "Verified: the image matches Super Smash Bros. Melee (USA) v1.02 (SHA-1 %s).",
                 iso_hex);
    } else if (strcmp(dol_hex, KNOWN_DOL_SHA1) == 0) {
        snprintf(hash_result, sizeof(hash_result),
                 "main.dol matches v1.02, so the game will run, but the image differs from the reference "
                 "(SHA-1 %s; other files changed or a different dump). Expected %s.",
                 iso_hex, KNOWN_ISO_SHA1);
    } else {
        snprintf(hash_result, sizeof(hash_result),
                 "Not the supported revision: main.dol SHA-1 %s (v1.02 is %s), image SHA-1 %s. "
                 "Use a v1.02 (NTSC) disc image.",
                 dol_hex, KNOWN_DOL_SHA1, iso_hex);
    }
done:
    if (f != NULL) {
        fclose(f);
    }
    InterlockedExchange(&hash_running, 0);
    PostMessageA(main_wnd, WM_HASH_DONE, 0, 0);
    return 0;
}

static void verify_iso(void)
{
    char path[MAX_PATH], status[256];
    HANDLE t;
    get_text(IDC_ISO_EDIT, path, sizeof(path));
    if (!probe_iso(path, status, sizeof(status))) {
        MessageBoxA(main_wnd, status, APP_TITLE, MB_ICONWARNING);
        return;
    }
    if (InterlockedCompareExchange(&hash_running, 1, 0) != 0) {
        return;
    }
    strncpy(hash_iso_path, path, sizeof(hash_iso_path) - 1);
    set_text(IDC_ISO_HASH, "Hashing the disc image (a few seconds)...");
    EnableWindow(ctl(IDC_ISO_VERIFY), FALSE);
    t = CreateThread(NULL, 0, hash_thread, NULL, 0, NULL);
    if (t == NULL) {
        InterlockedExchange(&hash_running, 0);
        EnableWindow(ctl(IDC_ISO_VERIFY), TRUE);
        set_text(IDC_ISO_HASH, "Could not start the hashing thread.");
    } else {
        CloseHandle(t);
    }
}

/* --- file dialogs ----------------------------------------------------------- */

static int browse_file(const char* title, const char* filter, char* path, int size)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = main_wnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD) size;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) != 0;
}

static int CALLBACK browse_folder_init(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    if (msg == BFFM_INITIALIZED && data != 0) {
        SendMessageA(h, BFFM_SETSELECTIONA, TRUE, data);
    }
    return 0;
}

static int browse_folder(const char* title, char* path, int size)
{
    BROWSEINFOA bi;
    LPITEMIDLIST pidl;
    char start[MAX_PATH];
    strncpy(start, path, sizeof(start) - 1);
    start[sizeof(start) - 1] = '\0';
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = main_wnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_folder_init;
    bi.lParam = (LPARAM) (start[0] ? start : NULL);
    pidl = SHBrowseForFolderA(&bi);
    if (pidl == NULL) {
        return 0;
    }
    if (!SHGetPathFromIDListA(pidl, path)) {
        CoTaskMemFree(pidl);
        return 0;
    }
    CoTaskMemFree(pidl);
    (void) size;
    return 1;
}

static void open_folder(const char* path)
{
    CreateDirectoryA(path, NULL);
    ShellExecuteA(main_wnd, "open", path, NULL, NULL, SW_SHOWNORMAL);
}

/* --- keyboard layout ---------------------------------------------------------- */

static const char keymap_template[] =
    "# Keyboard layout for port 1. One line per action: ACTION = KEY\r\n"
    "# Keys: letters, digits, F1-F24, or ENTER SPACE TAB BACKSPACE SHIFT LSHIFT\r\n"
    "# RSHIFT CTRL LCTRL RCTRL ALT UP DOWN LEFT RIGHT INSERT DELETE HOME END\r\n"
    "# PAGEUP PAGEDOWN NUMPAD0-NUMPAD9 NUMPAD+ NUMPAD- NUMPAD* NUMPAD/ NUMPAD.\r\n"
    "# COMMA PERIOD MINUS PLUS SEMICOLON SLASH BACKTICK LBRACKET BACKSLASH\r\n"
    "# RBRACKET QUOTE. Lines starting with # are comments.\r\n"
    "\r\n"
    "A = Z\r\n"
    "B = X\r\n"
    "X = C\r\n"
    "Y = V\r\n"
    "Z = SPACE\r\n"
    "L = Q\r\n"
    "R = E\r\n"
    "START = ENTER\r\n"
    "STICK_UP = UP\r\n"
    "STICK_DOWN = DOWN\r\n"
    "STICK_LEFT = LEFT\r\n"
    "STICK_RIGHT = RIGHT\r\n"
    "C_UP = I\r\n"
    "C_DOWN = K\r\n"
    "C_LEFT = J\r\n"
    "C_RIGHT = L\r\n"
    "DPAD_UP = NUMPAD8\r\n"
    "DPAD_DOWN = NUMPAD2\r\n"
    "DPAD_LEFT = NUMPAD4\r\n"
    "DPAD_RIGHT = NUMPAD6\r\n";

static void create_keymap(void)
{
    char path[MAX_PATH];
    get_text(IDC_KEYMAP_EDIT, path, sizeof(path));
    if (path[0] == '\0') {
        snprintf(path, sizeof(path), "%s\\keymap.txt", exe_dir);
        set_text(IDC_KEYMAP_EDIT, path);
    }
    if (!file_exists(path)) {
        FILE* f = fopen(path, "wb");
        if (f == NULL) {
            MessageBoxA(main_wnd, "The layout file could not be created.", APP_TITLE, MB_ICONERROR);
            return;
        }
        fwrite(keymap_template, 1, sizeof(keymap_template) - 1, f);
        fclose(f);
    }
    ShellExecuteA(main_wnd, "open", "notepad.exe", path, NULL, SW_SHOWNORMAL);
}

/* --- GameCube adapter ----------------------------------------------------------
 * The official adapter (USB 057E:0337) needs the WinUSB driver, which
 * Windows does not assign by itself. Detect whether it is present with that
 * driver (the same interface enumeration the game uses) and explain the
 * one-time Zadig step otherwise. */

static const GUID usb_device_guid = { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

/* 1 = adapter present with WinUSB, 0 = adapter present without a usable
 * driver (as "WUP-028" in Device Manager), -1 = no adapter */
static int adapter_state(void)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA iface;
    SP_DEVINFO_DATA dev;
    DWORD i;
    int state = -1;

    set = SetupDiGetClassDevsA(&usb_device_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set != INVALID_HANDLE_VALUE) {
        iface.cbSize = sizeof(iface);
        for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &usb_device_guid, i, &iface); i++) {
            char buf[1024], lower[1024];
            SP_DEVICE_INTERFACE_DETAIL_DATA_A* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) buf;
            size_t k;
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(buf), NULL, NULL)) {
                continue;
            }
            for (k = 0; detail->DevicePath[k] != '\0' && k < sizeof(lower) - 1; k++) {
                lower[k] = (char) tolower((unsigned char) detail->DevicePath[k]);
            }
            lower[k] = '\0';
            if (strstr(lower, "vid_057e&pid_0337") != NULL) {
                state = 1;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (state == 1) {
        return 1;
    }
    /* any driver: the device is listed by hardware id whatever driver it has */
    set = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set != INVALID_HANDLE_VALUE) {
        dev.cbSize = sizeof(dev);
        for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
            char ids[1024];
            DWORD type = 0;
            if (SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, &type, (BYTE*) ids, sizeof(ids), NULL)) {
                char* p;
                for (p = ids; *p != '\0'; p++) {
                    *p = (char) toupper((unsigned char) *p);
                }
                if (strstr(ids, "VID_057E&PID_0337") != NULL) {
                    state = 0;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    return state;
}

static int adapter_st = -1; /* the last adapter_state(), shown in the Controls card */

static void refresh_adapter_status(void)
{
    adapter_st = adapter_state();
    switch (adapter_st) {
    case 1:
        set_text(IDC_ADAPTER_STATUS, "Adapter detected with the WinUSB driver: ready.");
        break;
    case 0:
        set_text(IDC_ADAPTER_STATUS, "Adapter detected, but it needs the WinUSB driver (click Setup).");
        break;
    default:
        set_text(IDC_ADAPTER_STATUS, "No adapter detected (optional).");
        break;
    }
}

static void adapter_setup(void)
{
    int state = adapter_state();
    char text[1200];
    int r;
    snprintf(text, sizeof(text),
             "%s\n\n"
             "The official Wii U / Switch GameCube controller adapter needs Microsoft's generic WinUSB driver, "
             "which Windows does not assign by itself. This is the same one-time step Dolphin needs; if the "
             "adapter already works in Dolphin through Zadig, nothing more is required.\n\n"
             "1. Plug the adapter into a USB port (the black cable; the grey one only powers rumble).\n"
             "2. Download and run Zadig from zadig.akeo.ie.\n"
             "3. In Zadig, choose Options > List All Devices and select \"WUP-028\" in the list.\n"
             "4. Make sure the driver on the right reads \"WinUSB\", then click \"Replace Driver\".\n"
             "5. Start the game: controllers in ports 1-4 work, with rumble, and can be plugged in at any time.\n\n"
             "The driver stays installed across reboots and only affects the adapter. Device Manager's "
             "\"Uninstall device\" restores Windows' default.\n\n"
             "Open the Zadig download page now?",
             state == 1 ? "Status: adapter detected with the WinUSB driver. It is ready to use."
             : state == 0 ? "Status: adapter detected, but it still has Windows' default driver."
                          : "Status: no adapter detected right now (plug it in and click again to check).");
    r = MessageBoxA(main_wnd, text, "GameCube adapter setup", MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES) {
        ShellExecuteA(main_wnd, "open", "https://zadig.akeo.ie/", NULL, NULL, SW_SHOWNORMAL);
    }
    refresh_adapter_status();
}

/* --- mods -------------------------------------------------------------------- */

/* The game keeps mods\ and saves\ next to melee.exe, which is next to the
 * launcher when both were built together, or under the source tree's
 * build\pc when the launcher lives in the repository (pc\dist). */
static void game_dir(char* out, int size)
{
    char exe[MAX_PATH];
    char* slash;
    if (find_game_exe(exe, sizeof(exe))) {
        slash = strrchr(exe, '\\');
        if (slash != NULL) {
            *slash = '\0';
        }
        strncpy(out, exe, (size_t) size - 1);
        out[size - 1] = '\0';
        return;
    }
    strncpy(out, exe_dir, (size_t) size - 1);
    out[size - 1] = '\0';
}

static void mods_dir(char* out, int size)
{
    char dir[MAX_PATH];
    game_dir(dir, sizeof(dir));
    snprintf(out, size, "%s\\mods", dir);
}

static int mods_count(void)
{
    return (int) SendMessageA(ctl(IDC_MODS_LIST), LVM_GETITEMCOUNT, 0, 0);
}

static void mods_item_text(int i, char* out, int size)
{
    LVITEMA it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = 0;
    it.pszText = out;
    it.cchTextMax = size;
    out[0] = '\0';
    SendMessageA(ctl(IDC_MODS_LIST), LVM_GETITEMTEXTA, (WPARAM) i, (LPARAM) &it);
}

static int mods_item_checked(int i)
{
    return ListView_GetCheckState(ctl(IDC_MODS_LIST), i) != 0;
}

static void mods_add_item(const char* name, int checked)
{
    LVITEMA it;
    int idx;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = mods_count();
    it.pszText = (char*) name;
    idx = (int) SendMessageA(ctl(IDC_MODS_LIST), LVM_INSERTITEMA, 0, (LPARAM) &it);
    ListView_SetCheckState(ctl(IDC_MODS_LIST), idx, checked);
}

/* Rebuilds the list from mods\ and mods\enabled.txt: enabled mods first, in
 * priority order, then the rest alphabetically. */
static void mods_refresh(void)
{
    char dir[MAX_PATH], pattern[MAX_PATH], enabled_path[MAX_PATH];
    char enabled[64][MAX_PATH];
    int n_enabled = 0, i;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    FILE* f;

    SendMessageA(ctl(IDC_MODS_LIST), LVM_DELETEALLITEMS, 0, 0);
    mods_dir(dir, sizeof(dir));
    CreateDirectoryA(dir, NULL);

    snprintf(enabled_path, sizeof(enabled_path), "%s\\enabled.txt", dir);
    f = fopen(enabled_path, "r");
    if (f != NULL) {
        char line[MAX_PATH];
        while (fgets(line, sizeof(line), f) != NULL && n_enabled < 64) {
            char* p = line;
            char* end;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            end = p + strlen(p);
            while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
                *--end = '\0';
            }
            if (*p == '\0' || *p == '#') {
                continue;
            }
            strncpy(enabled[n_enabled], p, MAX_PATH - 1);
            enabled[n_enabled][MAX_PATH - 1] = '\0';
            n_enabled++;
        }
        fclose(f);
    }
    for (i = 0; i < n_enabled; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, enabled[i]);
        if (dir_exists(path) || dir_exists(enabled[i])) {
            mods_add_item(enabled[i], 1);
        }
    }
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int known = 0;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') {
                continue;
            }
            for (i = 0; i < n_enabled; i++) {
                if (_stricmp(enabled[i], fd.cFileName) == 0) {
                    known = 1;
                }
            }
            if (!known) {
                mods_add_item(fd.cFileName, 0);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    ui_mods_changed();
}

/* Writes mods\enabled.txt from the checked items, in list order. */
static void mods_save(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    FILE* f;
    int i, n = mods_count();
    mods_dir(dir, sizeof(dir));
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\enabled.txt", dir);
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "# Enabled mods, highest priority first. Written by the launcher.\n");
    for (i = 0; i < n; i++) {
        if (mods_item_checked(i)) {
            char name[MAX_PATH];
            mods_item_text(i, name, sizeof(name));
            fprintf(f, "%s\n", name);
        }
    }
    fclose(f);
}

static void mods_move(int delta)
{
    HWND list = ctl(IDC_MODS_LIST);
    int sel = (int) SendMessageA(list, LVM_GETNEXTITEM, (WPARAM) -1, LVNI_SELECTED);
    int other = sel + delta, checked, other_checked;
    char a[MAX_PATH], b[MAX_PATH];
    LVITEMA it;
    if (sel < 0 || other < 0 || other >= mods_count()) {
        return;
    }
    mods_item_text(sel, a, sizeof(a));
    mods_item_text(other, b, sizeof(b));
    checked = mods_item_checked(sel);
    other_checked = mods_item_checked(other);
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = sel;
    it.pszText = b;
    SendMessageA(list, LVM_SETITEMA, 0, (LPARAM) &it);
    it.iItem = other;
    it.pszText = a;
    SendMessageA(list, LVM_SETITEMA, 0, (LPARAM) &it);
    ListView_SetCheckState(list, sel, other_checked);
    ListView_SetCheckState(list, other, checked);
    ListView_SetItemState(list, other, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    mods_save();
}

/* --- launching --------------------------------------------------------------- */

static int scale_get(void);
static int volume_get(void);
static int toggle_get(int id);
static int seg_get(int first, int n, int def);
static const int msaa_values[4] = { 0, 2, 4, 8 };
static const int aniso_values[3] = { 0, 4, 16 };

static void save_settings(void)
{
    char buf[MAX_PATH];
    get_text(IDC_ISO_EDIT, buf, sizeof(buf));
    ini_set("iso", buf);
    ini_set_int("scale", scale_get());
    ini_set_int("internal", seg_get(IDC_INTERNAL_0, 5, 0));
    ini_set_int("msaa", msaa_values[seg_get(IDC_MSAA_0, 4, 0)]);
    ini_set_int("aniso", aniso_values[seg_get(IDC_ANISO_0, 3, 0)]);
    ini_set_int("fullscreen", toggle_get(IDC_FULLSCREEN));
    ini_set_int("no_console", toggle_get(IDC_NO_CONSOLE));
    ini_set_int("volume", volume_get());
    ini_set_int("mute", toggle_get(IDC_MUTE));
    get_text(IDC_KEYMAP_EDIT, buf, sizeof(buf));
    ini_set("keymap", buf);
    get_text(IDC_SAVES_EDIT, buf, sizeof(buf));
    ini_set("saves", buf);
    get_text(IDC_EXTRA_EDIT, buf, sizeof(buf));
    ini_set("extra", buf);
    ini_set("extract_dir", extract_dir);
    get_text(IDC_SRC_EDIT, buf, sizeof(buf));
    ini_set("source", buf);
    mods_save();
}

static void set_status(const char* text)
{
    set_text(IDC_STATUS, text);
}

/* melee.exe next to the launcher, else in the source tree's build output */
static int find_game_exe(char* out, int size)
{
    char src[MAX_PATH];
    snprintf(out, (size_t) size, "%s\\melee.exe", exe_dir);
    if (file_exists(out)) {
        return 1;
    }
    get_text(IDC_SRC_EDIT, src, sizeof(src));
    if (src[0] != '\0') {
        snprintf(out, (size_t) size, "%s\\build\\pc\\melee.exe", src);
        if (file_exists(out)) {
            return 1;
        }
    }
    return 0;
}

/* flags: 0 inherits the launcher's (absent) console, so a console
 * program gets a new window; CREATE_NEW_CONSOLE for the build and the
 * extraction, whose progress is that window; CREATE_NO_WINDOW for the
 * game when the console is hidden (its output goes to melee.log). */
static int run_child(const char* cmd_line, const char* dir, DWORD flags, int kind)
{
    char cmd[4096];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    strncpy(cmd, cmd_line, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL, dir, &si, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "The program could not be started (error %lu).", GetLastError());
        MessageBoxA(main_wnd, msg, APP_TITLE, MB_ICONERROR);
        return 0;
    }
    CloseHandle(pi.hThread);
    if (child != NULL) {
        CloseHandle(child);
    }
    child = pi.hProcess;
    child_kind = kind;
    SetTimer(main_wnd, TIMER_PROCESS, 500, NULL);
    EnableWindow(ctl(IDC_PLAY), FALSE);
    EnableWindow(ctl(IDC_EXTRACT), FALSE);
    EnableWindow(ctl(IDC_BUILD), FALSE);
    return 1;
}

static int run_game(const char* args, DWORD flags, int is_extract)
{
    char cmd[4096];
    char exe[MAX_PATH];
    if (!find_game_exe(exe, sizeof(exe))) {
        MessageBoxA(main_wnd,
                    "melee.exe was not found next to the launcher or under the source folder's build\\pc. "
                    "Build the game first (the Build card).",
                    APP_TITLE, MB_ICONERROR);
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    return run_child(cmd, exe_dir, flags, is_extract ? 1 : 0);
}

/* --- building from source ------------------------------------------------------ */

/* The repository checkout: the launcher is built into <repo>\build\pc, so
 * two levels up is the default; the user can point at any checkout. */
static void find_source_dir(char* out, int size)
{
    char probe[MAX_PATH];
    snprintf(out, (size_t) size, "%s\\..\\..", exe_dir);
    snprintf(probe, sizeof(probe), "%s\\pc\\build.cmd", out);
    if (file_exists(probe)) {
        char full[MAX_PATH];
        GetFullPathNameA(out, sizeof(full), full, NULL);
        strncpy(out, full, (size_t) size - 1);
        out[size - 1] = '\0';
        return;
    }
    out[0] = '\0';
}

static int visual_studio_found(void)
{
    char vswhere[MAX_PATH], line[MAX_PATH];
    FILE* f;
    int found = 0;
    const char* pf = getenv("ProgramFiles(x86)");
    if (pf == NULL) {
        return 0;
    }
    snprintf(vswhere, sizeof(vswhere),
             "\"\"%s\\Microsoft Visual Studio\\Installer\\vswhere.exe\" -latest -products * -requires "
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath\"",
             pf);
    f = _popen(vswhere, "r");
    if (f == NULL) {
        return 0;
    }
    if (fgets(line, sizeof(line), f) != NULL && strlen(line) > 3) {
        found = 1;
    }
    _pclose(f);
    return found;
}

static void refresh_build_status(void)
{
    char src[MAX_PATH], probe[MAX_PATH], msg[512];
    int have_src, have_vs;
    get_text(IDC_SRC_EDIT, src, sizeof(src));
    snprintf(probe, sizeof(probe), "%s\\pc\\build.cmd", src);
    have_src = src[0] != '\0' && file_exists(probe);
    have_vs = visual_studio_found();
    if (!have_src) {
        snprintf(msg, sizeof(msg), "Choose the folder of a MeleeRecomp checkout (it contains pc\\build.cmd).");
    } else if (!have_vs) {
        snprintf(msg, sizeof(msg),
                 "Visual Studio with the C++ workload was not found. Install it (Community edition is free) "
                 "with \"Desktop development with C++\" and the Clang component, then click Refresh.");
    } else {
        snprintf(probe, sizeof(probe), "%s\\build\\pc\\melee.exe", src);
        snprintf(msg, sizeof(msg), "Ready to build%s. The build takes a few minutes and shows a console.",
                 file_exists(probe) ? " (a built melee.exe exists; rebuilding updates it)" : "");
    }
    set_text(IDC_BUILD_STATUS, msg);
    EnableWindow(ctl(IDC_BUILD), have_src && have_vs && child == NULL);
}

static void build_game(void)
{
    char iso[MAX_PATH], status[256], src[MAX_PATH], cmd[4096];
    get_text(IDC_ISO_EDIT, iso, sizeof(iso));
    if (!probe_iso(iso, status, sizeof(status))) {
        MessageBoxA(main_wnd, status, APP_TITLE, MB_ICONWARNING);
        return;
    }
    get_text(IDC_SRC_EDIT, src, sizeof(src));
    save_settings();
    /* pc\build.cmd reads MELEE_ISO for the font tables the sources include */
    SetEnvironmentVariableA("MELEE_ISO", iso);
    {
        /* The build copies the new launcher to <source>\pc\dist. When that
         * is this running program, Windows will not let it be overwritten,
         * but a running program may be renamed: move it aside (the next
         * start deletes the old copy). */
        char dist[MAX_PATH], old[MAX_PATH];
        snprintf(dist, sizeof(dist), "%s\\pc\\dist\\melee-launcher.exe", src);
        if (_stricmp(dist, exe_path) == 0) {
            snprintf(old, sizeof(old), "%s\\melee-launcher.old.exe", exe_dir);
            MoveFileExA(exe_path, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    snprintf(cmd, sizeof(cmd), "cmd.exe /S /C \"\"%s\\pc\\build.cmd\" || pause\"", src);
    if (run_child(cmd, src, CREATE_NEW_CONSOLE, 2)) {
        set_status("Building the game from source; the console window shows the progress.");
    }
}

static void play(void)
{
    char iso[MAX_PATH], status[256], buf[MAX_PATH], args[4096];
    int scale, volume;
    size_t n;

    get_text(IDC_ISO_EDIT, iso, sizeof(iso));
    if (!probe_iso(iso, status, sizeof(status))) {
        MessageBoxA(main_wnd, status, APP_TITLE, MB_ICONWARNING);
        return;
    }
    save_settings();
    scale = scale_get();
    volume = volume_get();
    game_dir(buf, sizeof(buf));
    n = (size_t) snprintf(args, sizeof(args), "--iso \"%s\" --scale %d --volume %d --log \"%s\\melee.log\"", iso,
                          scale, volume, buf);
    if (toggle_get(IDC_FULLSCREEN)) {
        n += (size_t) snprintf(args + n, sizeof(args) - n, " --fullscreen");
    }
    n += (size_t) snprintf(args + n, sizeof(args) - n, " --internal %d --msaa %d --aniso %d", seg_get(IDC_INTERNAL_0, 5, 0),
                           msaa_values[seg_get(IDC_MSAA_0, 4, 0)], aniso_values[seg_get(IDC_ANISO_0, 3, 0)]);
    if (toggle_get(IDC_MUTE)) {
        n += (size_t) snprintf(args + n, sizeof(args) - n, " --no-audio");
    }
    get_text(IDC_KEYMAP_EDIT, buf, sizeof(buf));
    if (buf[0] != '\0') {
        n += (size_t) snprintf(args + n, sizeof(args) - n, " --keymap \"%s\"", buf);
    }
    get_text(IDC_SAVES_EDIT, buf, sizeof(buf));
    if (buf[0] != '\0') {
        n += (size_t) snprintf(args + n, sizeof(args) - n, " --saves \"%s\"", buf);
    }
    get_text(IDC_EXTRA_EDIT, buf, sizeof(buf));
    if (buf[0] != '\0') {
        n += (size_t) snprintf(args + n, sizeof(args) - n, " %s", buf);
    }
    if (toggle_get(IDC_NO_CONSOLE)) {
        if (run_game(args, CREATE_NO_WINDOW, 0)) {
            set_status("Game running without a console; its output goes to melee.log. Escape ends it.");
        }
    } else if (run_game(args, 0, 0)) {
        set_status("Game running. Escape or closing the game window ends it; F11 toggles full screen.");
    }
}

static void extract(void)
{
    char iso[MAX_PATH], status[256], args[4096];
    get_text(IDC_ISO_EDIT, iso, sizeof(iso));
    if (!probe_iso(iso, status, sizeof(status))) {
        MessageBoxA(main_wnd, status, APP_TITLE, MB_ICONWARNING);
        return;
    }
    if (extract_dir[0] == '\0') {
        snprintf(extract_dir, sizeof(extract_dir), "%s\\extracted", exe_dir);
    }
    if (!browse_folder("Choose the folder to extract the game's files into (about 1.4 GB)", extract_dir,
                       sizeof(extract_dir)))
    {
        return;
    }
    save_settings();
    snprintf(args, sizeof(args), "--iso \"%s\" --extract \"%s\"", iso, extract_dir);
    if (run_game(args, CREATE_NEW_CONSOLE, 1)) {
        set_status("Extracting the disc's files. A console window shows the progress.");
    }
}

static void child_finished(void)
{
    DWORD code = 0;
    GetExitCodeProcess(child, &code);
    CloseHandle(child);
    child = NULL;
    KillTimer(main_wnd, TIMER_PROCESS);
    EnableWindow(ctl(IDC_PLAY), TRUE);
    EnableWindow(ctl(IDC_EXTRACT), TRUE);
    EnableWindow(ctl(IDC_BUILD), TRUE);
    if (child_kind == 2) {
        char exe[MAX_PATH];
        if (code == 0 && find_game_exe(exe, sizeof(exe))) {
            char msg[MAX_PATH + 64];
            snprintf(msg, sizeof(msg), "Built %s. Press Play.", exe);
            set_status(msg);
        } else {
            set_status("The build did not finish; the console window shows the error.");
        }
        refresh_build_status();
    } else if (child_kind == 1) {
        char msg[MAX_PATH + 128];
        if (code == 0) {
            snprintf(msg, sizeof(msg), "Extracted to %s (files\\ holds the disc's files; copy a mod's layout from it).",
                     extract_dir);
        } else {
            snprintf(msg, sizeof(msg), "Extraction failed (status %lu).", code);
        }
        set_status(msg);
        EnableWindow(ctl(IDC_EXTRACT_OPEN), dir_exists(extract_dir));
    } else {
        char msg[128];
        if (code == 0) {
            snprintf(msg, sizeof(msg), "The game exited normally.");
        } else {
            snprintf(msg, sizeof(msg), "The game exited with status %lu (see melee.log next to melee.exe).",
                     code);
        }
        set_status(msg);
        mods_refresh();
    }
}

/* --- window ---------------------------------------------------------------------
 * The window is painted by hand (GDI, double-buffered): a sidebar with the
 * pages, a header with the state pill, a scrollable content area holding
 * the cards, and a footer with Play. The only standard controls are the
 * edit boxes, the mod list and owner-drawn buttons (plain, primary,
 * toggles, the window-size segments and the sidebar items); the volume
 * slider is a small custom control. Everything scales with the monitor's
 * DPI. */

#define C_BG RGB(0x0F, 0x11, 0x16)
#define C_SIDEBAR RGB(0x14, 0x17, 0x1E)
#define C_CARD RGB(0x1A, 0x1E, 0x26)
#define C_CARD_EDGE RGB(0x28, 0x2D, 0x37)
#define C_INPUT RGB(0x11, 0x13, 0x19)
#define C_INPUT_EDGE RGB(0x2C, 0x31, 0x3C)
#define C_TEXT RGB(0xE8, 0xEA, 0xEE)
#define C_TEXT_DIM RGB(0x9A, 0xA1, 0xAD)
#define C_ACCENT RGB(0x25, 0x74, 0xF0)
#define C_ACCENT_HI RGB(0x3D, 0x86, 0xF7)
#define C_ACCENT_LO RGB(0x1B, 0x5C, 0xC4)
#define C_GREEN RGB(0x2E, 0xC2, 0x6E)
#define C_ORANGE RGB(0xF0, 0xA0, 0x30)
#define C_RED RGB(0xE5, 0x4D, 0x4D)
#define C_BTN RGB(0x24, 0x29, 0x33)
#define C_BTN_HI RGB(0x30, 0x36, 0x42)
#define C_TRACK RGB(0x2C, 0x31, 0x3C)
#define C_WHITE RGB(0xFF, 0xFF, 0xFF)

enum { K_NONE, K_PRIMARY, K_SECONDARY, K_TOGGLE, K_SEGMENT, K_NAV };

enum { CARD_GAME, CARD_BUILD, CARD_DISPLAY, CARD_AUDIO, CARD_CONTROLS, CARD_SAVES, CARD_MODS, CARD_ADVANCED, CARD_COUNT };
enum { PAGE_SETUP, PAGE_BUILD, PAGE_DISPLAY, PAGE_AUDIO, PAGE_CONTROLS, PAGE_SAVES, PAGE_MODS, PAGE_ADVANCED, PAGE_COUNT };

#define SLM_SETPOS (WM_USER + 1)
#define SLM_GETPOS (WM_USER + 2)

static const struct {
    const char* name;
    wchar_t icon;
} nav_items[PAGE_COUNT] = {
    { "Setup", 0xE713 },    { "Build", 0xE90F }, { "Display", 0xE7F4 }, { "Audio", 0xE767 },
    { "Controls", 0xE7FC }, { "Saves", 0xE74E }, { "Mods", 0xEA86 },    { "Advanced", 0xE9E9 },
};

static const unsigned page_cards[PAGE_COUNT] = {
    0xFF,
    (1u << CARD_GAME) | (1u << CARD_BUILD),
    1u << CARD_DISPLAY,
    1u << CARD_AUDIO,
    1u << CARD_CONTROLS,
    1u << CARD_SAVES,
    1u << CARD_MODS,
    1u << CARD_ADVANCED,
};

/* title, icon and height (unscaled) of each card */
static const struct {
    const char* title;
    wchar_t icon;
    int h;
} card_info[CARD_COUNT] = {
    { "Super Smash Bros. Melee", 0, 196 }, { "Build", 0xE90F, 172 },   { "Display", 0xE7F4, 322 },
    { "Audio", 0xE767, 184 },              { "Controls", 0xE7FC, 200 }, { "Saves", 0xE74E, 280 },
    { "Mods", 0xEA86, 280 },               { "Advanced", 0xE9E9, 150 },
};

/* the card grid of the Setup page: one or two cards per row */
static const int card_rows[][2] = {
    { CARD_GAME, -1 },         { CARD_BUILD, -1 },       { CARD_DISPLAY, CARD_AUDIO },
    { CARD_CONTROLS, -1 },     { CARD_MODS, CARD_SAVES }, { CARD_ADVANCED, -1 },
};

struct ctlinfo {
    int kind;     /* K_* for owner-drawn buttons */
    int card;     /* the card it belongs to, -1 for the sidebar and footer */
    int state;    /* toggle / segment / sidebar selection */
    wchar_t icon; /* Segoe MDL2 glyph drawn before the text, 0 for none */
    int framed;   /* an input frame is painted behind it */
    RECT frame;
};

#define NCTL (IDC_LAST - 100)
#define INFO(id) (info[(id) - 100])
static struct ctlinfo info[NCTL];

static int dpi = 96;
static int page = PAGE_SETUP;
static int scroll_y, content_total;
static int in_layout;
static HFONT font_semi, font_head, font_title, font_small, font_icon, font_icon_lg, font_play;
static HBRUSH br_input;
static HBITMAP banner;
static int iso_ok;
static int iso_verified; /* 0 not yet, 1 SHA-1 matched, -1 wrong revision */
static HFONT font_icon_sm;

/* "<size>|<mtime>|<path>" of the image whose SHA-1 last matched, so the
 * launcher does not ask for a new verification of an unchanged file */
static void iso_stamp(const char* path, char* out, int size)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (path[0] != '\0' && GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        snprintf(out, (size_t) size, "%lu:%lu|%lu:%lu|%s", fa.nFileSizeHigh, fa.nFileSizeLow,
                 fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime, path);
    } else {
        out[0] = '\0';
    }
}

static void iso_status_text(void)
{
    if (!iso_ok) {
        return; /* probe_iso's own message stays */
    }
    if (iso_verified > 0) {
        set_text(IDC_ISO_STATUS, "Verified SHA-1 (v1.02, USA)");
    } else if (iso_verified < 0) {
        set_text(IDC_ISO_STATUS, "Wrong disc: not the v1.02 NTSC image");
    } else {
        set_text(IDC_ISO_STATUS, "Not verified: click Verify SHA-1");
    }
}
static RECT card_rc[CARD_COUNT];
static RECT mods_empty_rc;
static int have_icon_font;

static int S(int v)
{
    return MulDiv(v, dpi, 96);
}

static int is_label(int id)
{
    return id == IDC_ISO_STATUS || id == IDC_ISO_HASH || id == IDC_ADAPTER_STATUS || id == IDC_BUILD_STATUS ||
           id == IDC_STATUS;
}

static int toggle_get(int id)
{
    return INFO(id).state;
}

static void toggle_set(int id, int on)
{
    INFO(id).state = on ? 1 : 0;
    InvalidateRect(ctl(id), NULL, FALSE);
}

static int scale_get(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (INFO(IDC_SCALE_1 + i).state) {
            return i + 1;
        }
    }
    return 2;
}

static void scale_set(int scale)
{
    int i;
    for (i = 0; i < 4; i++) {
        INFO(IDC_SCALE_1 + i).state = (i + 1 == scale);
        InvalidateRect(ctl(IDC_SCALE_1 + i), NULL, FALSE);
    }
    InvalidateRect(content_wnd, NULL, FALSE);
}

/* a row of segment buttons: which one is on */
static int seg_get(int first, int n, int def)
{
    int i;
    for (i = 0; i < n; i++) {
        if (INFO(first + i).state) {
            return i;
        }
    }
    return def;
}

static void seg_set(int first, int n, int which)
{
    int i;
    for (i = 0; i < n; i++) {
        INFO(first + i).state = (i == which);
        InvalidateRect(ctl(first + i), NULL, FALSE);
    }
}

static int volume_get(void)
{
    return (int) SendMessageA(ctl(IDC_VOLUME), SLM_GETPOS, 0, 0);
}

static void volume_set(int v)
{
    SendMessageA(ctl(IDC_VOLUME), SLM_SETPOS, (WPARAM) v, 0);
}

/* --- disc banner ------------------------------------------------------------------
 * opening.bnr from the disc: a 96x32 RGB5A3 image in 4x4 tiles, found
 * through the file system table. Shown in the game card. */

static HBITMAP load_banner(const char* iso)
{
    FILE* f = fopen(iso, "rb");
    unsigned char header[0x440];
    unsigned char* fst = NULL;
    unsigned char* bnr = NULL;
    HBITMAP bmp = NULL;
    uint32_t fst_off, fst_size, n, i, off = 0, len = 0;
    if (f == NULL) {
        return NULL;
    }
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        goto done;
    }
    fst_off = rd32(header + 0x424);
    fst_size = rd32(header + 0x428);
    if (fst_size < 12 || fst_size > (4u << 20)) {
        goto done;
    }
    fst = (unsigned char*) malloc(fst_size + 1);
    if (fst == NULL || _fseeki64(f, fst_off, SEEK_SET) != 0 || fread(fst, 1, fst_size, f) != fst_size) {
        goto done;
    }
    fst[fst_size] = '\0';
    n = rd32(fst + 8);
    if (n == 0 || (unsigned long long) n * 12 > fst_size) {
        goto done;
    }
    for (i = 1; i < n; i++) {
        const unsigned char* e = fst + i * 12;
        uint32_t name_off = rd32(e) & 0xFFFFFF;
        if (e[0] != 0 || n * 12 + name_off >= fst_size) {
            continue;
        }
        if (_stricmp((const char*) fst + n * 12 + name_off, "opening.bnr") == 0) {
            off = rd32(e + 4);
            len = rd32(e + 8);
            break;
        }
    }
    if (len < 0x1820) {
        goto done;
    }
    bnr = (unsigned char*) malloc(0x1820);
    if (bnr == NULL || _fseeki64(f, off, SEEK_SET) != 0 || fread(bnr, 1, 0x1820, f) != 0x1820 ||
        memcmp(bnr, "BNR", 3) != 0)
    {
        goto done;
    }
    {
        BITMAPINFO bi;
        void* bits = NULL;
        HDC dc = GetDC(NULL);
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 96;
        bi.bmiHeader.biHeight = -32;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, dc);
        if (bmp != NULL && bits != NULL) {
            unsigned char* px = (unsigned char*) bits;
            const unsigned char* src = bnr + 0x20;
            int ty, tx, y, x;
            for (ty = 0; ty < 8; ty++) {
                for (tx = 0; tx < 24; tx++) {
                    for (y = 0; y < 4; y++) {
                        for (x = 0; x < 4; x++) {
                            unsigned v = ((unsigned) src[0] << 8) | src[1];
                            unsigned r, g, b, a;
                            unsigned char* out = px + ((ty * 4 + y) * 96 + tx * 4 + x) * 4;
                            src += 2;
                            if (v & 0x8000) {
                                r = ((v >> 10) & 31) * 255 / 31;
                                g = ((v >> 5) & 31) * 255 / 31;
                                b = (v & 31) * 255 / 31;
                                a = 255;
                            } else {
                                a = ((v >> 12) & 7) * 255 / 7;
                                r = ((v >> 8) & 15) * 17;
                                g = ((v >> 4) & 15) * 17;
                                b = (v & 15) * 17;
                            }
                            /* composite onto the tile colour */
                            out[0] = (unsigned char) ((b * a + 0x19 * (255 - a)) / 255);
                            out[1] = (unsigned char) ((g * a + 0x13 * (255 - a)) / 255);
                            out[2] = (unsigned char) ((r * a + 0x11 * (255 - a)) / 255);
                            out[3] = 255;
                        }
                    }
                }
            }
        }
    }
done:
    free(fst);
    free(bnr);
    fclose(f);
    return bmp;
}

/* called by refresh_iso_status: remembers whether the image is usable and
 * loads its banner */
static void ui_iso_changed(int ok, const char* path)
{
    char stamp[MAX_PATH + 64], known[MAX_PATH + 64];
    iso_ok = ok;
    iso_verified = 0;
    if (ok) {
        iso_stamp(path, stamp, sizeof(stamp));
        ini_get("verified", known, sizeof(known), "");
        if (stamp[0] != '\0' && strcmp(stamp, known) == 0) {
            iso_verified = 1;
        }
    }
    iso_status_text();
    if (banner != NULL) {
        DeleteObject(banner);
        banner = NULL;
    }
    if (ok) {
        banner = load_banner(path);
    }
    if (content_wnd != NULL) {
        InvalidateRect(content_wnd, NULL, FALSE);
    }
    if (main_wnd != NULL) {
        InvalidateRect(main_wnd, NULL, FALSE);
    }
}

/* --- drawing helpers ------------------------------------------------------------- */

static void fill(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void round_rect(HDC dc, RECT r, int radius, COLORREF fill_c, COLORREF edge_c, int dashed)
{
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 1, edge_c);
    HBRUSH br = CreateSolidBrush(fill_c);
    HPEN op = (HPEN) SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH) SelectObject(dc, br);
    SetBkMode(dc, TRANSPARENT);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

static void dot(HDC dc, int cx, int cy, int radius, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN op = (HPEN) SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH ob = (HBRUSH) SelectObject(dc, br);
    Ellipse(dc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

static void text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, const char* s, UINT flags)
{
    RECT r;
    HFONT of = (HFONT) SelectObject(dc, f);
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, of);
}

static int text_width(HDC dc, HFONT f, const char* s)
{
    SIZE sz;
    HFONT of = (HFONT) SelectObject(dc, f);
    GetTextExtentPoint32A(dc, s, (int) strlen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

static void glyph(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, wchar_t g)
{
    RECT r;
    wchar_t s[2];
    HFONT of;
    if (!have_icon_font || g == 0) {
        return;
    }
    s[0] = g;
    s[1] = 0;
    of = (HFONT) SelectObject(dc, f);
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, 1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
}

/* --- owner-drawn buttons ---------------------------------------------------------- */

static void draw_button(DRAWITEMSTRUCT* di)
{
    int id = (int) di->CtlID;
    struct ctlinfo* ci = &INFO(id);
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    char label[256];
    int pressed = (di->itemState & ODS_SELECTED) != 0;
    int disabled = (di->itemState & ODS_DISABLED) != 0;
    int focus = (di->itemState & ODS_FOCUS) != 0 && !(di->itemState & ODS_NOFOCUSRECT);
    COLORREF bg = ci->kind == K_NAV ? C_SIDEBAR : id == IDC_PLAY ? C_BG : C_CARD;
    COLORREF fg = disabled ? C_TEXT_DIM : C_TEXT;

    GetWindowTextA(di->hwndItem, label, sizeof(label));
    fill(dc, r, bg);
    switch (ci->kind) {
    case K_PRIMARY: {
        COLORREF c = disabled ? C_BTN : pressed ? C_ACCENT_LO : C_ACCENT;
        round_rect(dc, r, S(8), c, c, 0);
        if (ci->icon != 0 && have_icon_font) {
            int tw = text_width(dc, font_play, label) + S(30);
            int x = (r.left + r.right - tw) / 2;
            glyph(dc, font_icon, disabled ? C_TEXT_DIM : C_WHITE, x, r.top, S(24), r.bottom - r.top, ci->icon);
            text(dc, font_play, disabled ? C_TEXT_DIM : C_WHITE, x + S(30), r.top, tw, r.bottom - r.top, label,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        } else {
            text(dc, font_semi, disabled ? C_TEXT_DIM : C_WHITE, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 label, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        break;
    }
    case K_SECONDARY:
        round_rect(dc, r, S(8), pressed ? C_BTN_HI : C_BTN, C_INPUT_EDGE, 0);
        if (ci->icon != 0 && have_icon_font && text_width(dc, ui_font, label) + S(42) <= r.right - r.left) {
            int tw = text_width(dc, ui_font, label) + S(26);
            int x = (r.left + r.right - tw) / 2;
            glyph(dc, font_icon, fg, x, r.top, S(20), r.bottom - r.top, ci->icon);
            text(dc, ui_font, fg, x + S(26), r.top, tw, r.bottom - r.top, label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        } else {
            text(dc, ui_font, fg, r.left + S(4), r.top, r.right - r.left - S(8), r.bottom - r.top, label,
                 DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
        break;
    case K_TOGGLE: {
        int cy = (r.top + r.bottom) / 2;
        RECT pill;
        pill.left = r.left;
        pill.right = r.left + S(40);
        pill.top = cy - S(10);
        pill.bottom = cy + S(10);
        round_rect(dc, pill, S(10), ci->state ? (disabled ? C_ACCENT_LO : C_ACCENT) : C_TRACK,
                   ci->state ? (disabled ? C_ACCENT_LO : C_ACCENT) : C_INPUT_EDGE, 0);
        dot(dc, ci->state ? pill.right - S(10) : pill.left + S(10), cy, S(7), disabled ? C_TEXT_DIM : C_WHITE);
        text(dc, ui_font, fg, r.left + S(52), r.top, r.right - r.left - S(52), r.bottom - r.top, label,
             DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        if (focus) {
            RECT fr = pill;
            InflateRect(&fr, S(3), S(3));
            round_rect(dc, fr, S(12), C_CARD, C_ACCENT_HI, 0);
            round_rect(dc, pill, S(10), ci->state ? C_ACCENT : C_TRACK, ci->state ? C_ACCENT : C_INPUT_EDGE, 0);
            dot(dc, ci->state ? pill.right - S(10) : pill.left + S(10), cy, S(7), C_WHITE);
        }
        return;
    }
    case K_SEGMENT:
        round_rect(dc, r, S(6), ci->state ? C_ACCENT : pressed ? C_BTN_HI : C_BTN,
                   ci->state ? C_ACCENT : C_INPUT_EDGE, 0);
        text(dc, font_semi, ci->state ? C_WHITE : fg, r.left, r.top, r.right - r.left, r.bottom - r.top, label,
             DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        break;
    case K_NAV:
        if (ci->state) {
            round_rect(dc, r, S(8), C_ACCENT, C_ACCENT, 0);
        } else if (pressed) {
            round_rect(dc, r, S(8), C_BTN, C_BTN, 0);
        }
        glyph(dc, font_icon, ci->state ? C_WHITE : C_TEXT_DIM, r.left + S(12), r.top, S(24), r.bottom - r.top,
              ci->icon);
        text(dc, ci->state ? font_semi : ui_font, ci->state ? C_WHITE : C_TEXT_DIM, r.left + S(46), r.top,
             r.right - r.left - S(50), r.bottom - r.top, label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        break;
    default:
        break;
    }
    if (focus && ci->kind != K_NAV) {
        HPEN pen = CreatePen(PS_SOLID, 1, C_ACCENT_HI);
        HPEN op = (HPEN) SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH) SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, S(14), S(14));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }
}

/* --- volume slider ---------------------------------------------------------------- */

static void slider_notify(HWND h)
{
    SendMessageA(GetParent(h), WM_HSCROLL, 0, (LPARAM) h);
}

static void slider_from_x(HWND h, int x)
{
    RECT rc;
    int r, pos;
    GetClientRect(h, &rc);
    r = S(9);
    pos = MulDiv(x - r, 100, rc.right - 2 * r);
    pos = pos < 0 ? 0 : pos > 100 ? 100 : pos;
    SetWindowLongPtrA(h, GWLP_USERDATA, pos);
    InvalidateRect(h, NULL, FALSE);
    slider_notify(h);
}

static LRESULT CALLBACK slider_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    int pos = (int) GetWindowLongPtrA(h, GWLP_USERDATA);
    switch (m) {
    case SLM_SETPOS:
        pos = (int) wp;
        SetWindowLongPtrA(h, GWLP_USERDATA, pos < 0 ? 0 : pos > 100 ? 100 : pos);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case SLM_GETPOS:
        return pos;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, track;
        int r = S(9), cy, kx;
        GetClientRect(h, &rc);
        fill(dc, rc, C_CARD);
        cy = (rc.top + rc.bottom) / 2;
        kx = r + MulDiv(rc.right - 2 * r, pos, 100);
        track.left = r;
        track.right = rc.right - r;
        track.top = cy - S(3);
        track.bottom = cy + S(3);
        round_rect(dc, track, S(3), C_TRACK, C_TRACK, 0);
        track.right = kx;
        if (track.right > track.left) {
            round_rect(dc, track, S(3), C_ACCENT, C_ACCENT, 0);
        }
        dot(dc, kx, cy, r, GetFocus() == h ? C_ACCENT_HI : C_ACCENT);
        dot(dc, kx, cy, r - S(2), C_WHITE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        slider_from_x(h, (short) LOWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            slider_from_x(h, (short) LOWORD(lp));
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) {
            ReleaseCapture();
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN: {
        int np = pos;
        if (wp == VK_LEFT || wp == VK_DOWN) {
            np = pos - 5;
        } else if (wp == VK_RIGHT || wp == VK_UP) {
            np = pos + 5;
        } else if (wp == VK_HOME) {
            np = 0;
        } else if (wp == VK_END) {
            np = 100;
        } else {
            break;
        }
        SetWindowLongPtrA(h, GWLP_USERDATA, np < 0 ? 0 : np > 100 ? 100 : np);
        InvalidateRect(h, NULL, FALSE);
        slider_notify(h);
        return 0;
    }
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* --- controls -------------------------------------------------------------------- */

static HWND make_in(HWND parent, const char* cls, const char* label, DWORD style, int id, int card, int kind,
                    wchar_t icon)
{
    HWND c = CreateWindowExA(0, cls, label, WS_CHILD | style, 0, 0, 10, 10, parent, (HMENU) (INT_PTR) id, app, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM) ui_font, TRUE);
    INFO(id).kind = kind;
    INFO(id).card = card;
    INFO(id).icon = icon;
    return c;
}

static void button(const char* label, int id, int card, int kind, wchar_t icon)
{
    make_in(content_wnd, "BUTTON", label, BS_OWNERDRAW | WS_TABSTOP, id, card, kind, icon);
}

static void edit(int id, int card)
{
    make_in(content_wnd, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, id, card, K_NONE, 0);
}

static void create_fonts(void)
{
    HFONT* fonts[] = { &ui_font, &bold_font, &font_semi, &font_head, &font_title, &font_small,
                       &font_icon, &font_icon_lg, &font_play };
    int i;
    for (i = 0; i < (int) (sizeof(fonts) / sizeof(fonts[0])); i++) {
        if (*fonts[i] != NULL) {
            DeleteObject(*fonts[i]);
            *fonts[i] = NULL;
        }
    }
    ui_font = CreateFontA(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    bold_font = CreateFontA(-S(13), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    font_semi = CreateFontA(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe UI");
    font_head = CreateFontA(-S(16), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    font_title = CreateFontA(-S(22), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             "Segoe UI");
    font_small = CreateFontA(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             "Segoe UI");
    font_play = CreateFontA(-S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe UI");
    font_icon = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe MDL2 Assets");
    font_icon_lg = CreateFontA(-S(19), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               "Segoe MDL2 Assets");
    if (font_icon_sm != NULL) {
        DeleteObject(font_icon_sm);
    }
    font_icon_sm = CreateFontA(-S(10), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               "Segoe MDL2 Assets");
    {
        /* the icon font ships with Windows 10 and 11; without it the
         * glyphs are skipped rather than drawn as boxes */
        HDC dc = GetDC(NULL);
        char face[LF_FACESIZE];
        HFONT of = (HFONT) SelectObject(dc, font_icon);
        GetTextFaceA(dc, sizeof(face), face);
        have_icon_font = _stricmp(face, "Segoe MDL2 Assets") == 0;
        SelectObject(dc, of);
        ReleaseDC(NULL, dc);
    }
}

static BOOL CALLBACK apply_font(HWND h, LPARAM lp)
{
    SendMessageA(h, WM_SETFONT, (WPARAM) lp, TRUE);
    return TRUE;
}

static void build_ui(void)
{
    int i;
    static const char* scales[] = { "1x", "2x", "3x", "4x" };

    for (i = 0; i < PAGE_COUNT; i++) {
        make_in(main_wnd, "BUTTON", nav_items[i].name, BS_OWNERDRAW | WS_TABSTOP, IDC_NAV_FIRST + i, -1, K_NAV,
                nav_items[i].icon);
    }
    make_in(main_wnd, "BUTTON", "Play", BS_OWNERDRAW | WS_TABSTOP, IDC_PLAY, -1, K_PRIMARY, 0xE768);

    edit(IDC_ISO_EDIT, CARD_GAME);
    button("Browse...", IDC_ISO_BROWSE, CARD_GAME, K_SECONDARY, 0xF12B);
    button("Verify SHA-1", IDC_ISO_VERIFY, CARD_GAME, K_SECONDARY, 0);

    edit(IDC_SRC_EDIT, CARD_BUILD);
    button("Browse...", IDC_SRC_BROWSE, CARD_BUILD, K_SECONDARY, 0xF12B);
    button("Build", IDC_BUILD, CARD_BUILD, K_PRIMARY, 0);

    for (i = 0; i < 4; i++) {
        button(scales[i], IDC_SCALE_1 + i, CARD_DISPLAY, K_SEGMENT, 0);
    }
    {
        static const char* internal[] = { "Window", "1x", "2x", "3x", "4x" };
        static const char* msaa[] = { "Off", "2x", "4x", "8x" };
        static const char* aniso[] = { "Off", "4x", "16x" };
        for (i = 0; i < 5; i++) {
            button(internal[i], IDC_INTERNAL_0 + i, CARD_DISPLAY, K_SEGMENT, 0);
        }
        for (i = 0; i < 4; i++) {
            button(msaa[i], IDC_MSAA_0 + i, CARD_DISPLAY, K_SEGMENT, 0);
        }
        for (i = 0; i < 3; i++) {
            button(aniso[i], IDC_ANISO_0 + i, CARD_DISPLAY, K_SEGMENT, 0);
        }
    }
    button("Full screen (F11 or Alt+Enter in the game)", IDC_FULLSCREEN, CARD_DISPLAY, K_TOGGLE, 0);
    button("Hide the console window (output in melee.log)", IDC_NO_CONSOLE, CARD_DISPLAY, K_TOGGLE, 0);

    make_in(content_wnd, "MeleeSlider", "", WS_TABSTOP, IDC_VOLUME, CARD_AUDIO, K_NONE, 0);
    button("Mute", IDC_MUTE, CARD_AUDIO, K_TOGGLE, 0);

    button("Adapter setup...", IDC_ADAPTER, CARD_CONTROLS, K_SECONDARY, 0xE713);
    edit(IDC_KEYMAP_EDIT, CARD_CONTROLS);
    button("Browse...", IDC_KEYMAP_BROWSE, CARD_CONTROLS, K_SECONDARY, 0);
    button("Create / edit", IDC_KEYMAP_CREATE, CARD_CONTROLS, K_SECONDARY, 0xE70F);

    edit(IDC_SAVES_EDIT, CARD_SAVES);
    button("Browse...", IDC_SAVES_BROWSE, CARD_SAVES, K_SECONDARY, 0xF12B);
    button("Open folder", IDC_SAVES_OPEN, CARD_SAVES, K_SECONDARY, 0xED25);

    make_in(content_wnd, WC_LISTVIEWA, "", LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
            IDC_MODS_LIST, CARD_MODS, K_NONE, 0);
    {
        LVCOLUMNA col;
        HWND list = ctl(IDC_MODS_LIST);
        HMODULE ux = LoadLibraryA("uxtheme.dll");
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_WIDTH;
        col.cx = 300;
        SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM) &col);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(list, C_INPUT);
        ListView_SetTextBkColor(list, C_INPUT);
        ListView_SetTextColor(list, C_TEXT);
        if (ux != NULL) {
            typedef HRESULT(WINAPI * set_theme_t)(HWND, LPCWSTR, LPCWSTR);
            typedef int(WINAPI * set_mode_t)(int);
            typedef BOOL(WINAPI * allow_t)(HWND, BOOL);
            set_theme_t set_theme = (set_theme_t) GetProcAddress(ux, "SetWindowTheme");
            set_mode_t set_mode = (set_mode_t) GetProcAddress(ux, MAKEINTRESOURCEA(135));
            allow_t allow = (allow_t) GetProcAddress(ux, MAKEINTRESOURCEA(133));
            if (set_mode != NULL) {
                set_mode(1); /* AllowDark: dark scrollbars on the themed windows below */
            }
            if (allow != NULL) {
                allow(list, TRUE);
                allow(content_wnd, TRUE);
            }
            if (set_theme != NULL) {
                set_theme(list, L"DarkMode_Explorer", NULL);
                set_theme(content_wnd, L"DarkMode_Explorer", NULL);
            }
        }
    }
    button("Move up", IDC_MODS_UP, CARD_MODS, K_SECONDARY, 0xE74A);
    button("Move down", IDC_MODS_DOWN, CARD_MODS, K_SECONDARY, 0xE74B);
    button("Refresh", IDC_MODS_REFRESH, CARD_MODS, K_SECONDARY, 0xE72C);
    button("Open folder", IDC_MODS_OPEN, CARD_MODS, K_SECONDARY, 0xED25);
    button("Extract files...", IDC_EXTRACT, CARD_MODS, K_SECONDARY, 0xE896);
    button("Open extracted", IDC_EXTRACT_OPEN, CARD_MODS, K_SECONDARY, 0);

    edit(IDC_EXTRA_EDIT, CARD_ADVANCED);
}

/* --- layout ------------------------------------------------------------------------ */

static void place(int id, int x, int y, int w, int h)
{
    SetWindowPos(ctl(id), NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* an edit box (or the mod list) inside a painted frame */
static void place_input(int id, int x, int y, int w, int h)
{
    struct ctlinfo* ci = &INFO(id);
    ci->framed = 1;
    ci->frame.left = x;
    ci->frame.top = y;
    ci->frame.right = x + w;
    ci->frame.bottom = y + h;
    if (id == IDC_MODS_LIST) {
        place(id, x + 2, y + 2, w - 4, h - 4);
        ListView_SetColumnWidth(ctl(id), 0, LVSCW_AUTOSIZE_USEHEADER); /* fills the width */
        if (mods_count() * (int) S(20) < h - 4) {
            ShowScrollBar(ctl(id), SB_VERT, FALSE);
        }
    } else {
        int eh = S(18);
        place(id, x + S(12), y + (h - eh) / 2, w - S(24), eh);
    }
}

static void layout_card(int c, RECT rc)
{
    int pad = S(20), x = rc.left + pad, y = rc.top + pad, w = rc.right - rc.left - 2 * pad, right = rc.right - pad;
    int row = y + S(44);
    switch (c) {
    case CARD_GAME: {
        int tx = x + S(208) + S(24), bw = S(110);
        place_input(IDC_ISO_EDIT, tx, y + S(64), right - bw - S(10) - tx, S(34));
        place(IDC_ISO_BROWSE, right - bw, y + S(64), bw, S(34));
        place(IDC_ISO_VERIFY, right - bw, y + S(108), bw, S(30));
        break;
    }
    case CARD_BUILD:
        place_input(IDC_SRC_EDIT, x + S(100), row, right - S(120) - (x + S(100)), S(34));
        place(IDC_SRC_BROWSE, right - S(110), row, S(110), S(34));
        place(IDC_BUILD, right - S(130), row + S(46), S(130), S(40));
        break;
    case CARD_DISPLAY: {
        int i;
        for (i = 0; i < 4; i++) {
            place(IDC_SCALE_1 + i, x + S(100) + i * S(60), row, S(54), S(32));
        }
        /* the first choice is a word, the others a factor */
        place(IDC_INTERNAL_0, x + S(100), row + S(46), S(68), S(32));
        for (i = 1; i < 5; i++) {
            place(IDC_INTERNAL_0 + i, x + S(100) + S(72) + (i - 1) * S(48), row + S(46), S(44), S(32));
        }
        for (i = 0; i < 4; i++) {
            place(IDC_MSAA_0 + i, x + S(100) + i * S(60), row + S(92), S(54), S(32));
        }
        for (i = 0; i < 3; i++) {
            place(IDC_ANISO_0 + i, x + S(100) + i * S(60), row + S(138), S(54), S(32));
        }
        place(IDC_FULLSCREEN, x, row + S(184), w, S(24));
        place(IDC_NO_CONSOLE, x, row + S(216), w, S(24));
        break;
    }
    case CARD_AUDIO:
        place(IDC_VOLUME, x + S(80), row, right - S(60) - (x + S(80)), S(32));
        place(IDC_MUTE, x, row + S(46), w, S(24));
        break;
    case CARD_CONTROLS: {
        int half = (w - S(16)) / 2;
        place(IDC_ADAPTER, x + half - S(132), row + S(16), S(120), S(32));
        place_input(IDC_KEYMAP_EDIT, x + S(90), row + S(80), right - S(230) - (x + S(90)), S(34));
        place(IDC_KEYMAP_BROWSE, right - S(220), row + S(80), S(100), S(34));
        place(IDC_KEYMAP_CREATE, right - S(110), row + S(80), S(110), S(34));
        break;
    }
    case CARD_SAVES:
        place_input(IDC_SAVES_EDIT, x, row, w, S(34));
        place(IDC_SAVES_BROWSE, x, row + S(44), S(110), S(34));
        place(IDC_SAVES_OPEN, x + S(120), row + S(44), S(130), S(34));
        break;
    case CARD_MODS: {
        int bh = S(32), by2 = rc.bottom - pad - bh, by1 = by2 - bh - S(8), bw = (w - S(16)) / 3;
        int lh = by1 - S(12) - row;
        if (mods_count() > 0) {
            place_input(IDC_MODS_LIST, x, row, w, lh);
            SetRectEmpty(&mods_empty_rc);
        } else {
            ShowWindow(ctl(IDC_MODS_LIST), SW_HIDE);
            INFO(IDC_MODS_LIST).framed = 0;
            mods_empty_rc.left = x;
            mods_empty_rc.top = row;
            mods_empty_rc.right = x + w;
            mods_empty_rc.bottom = row + lh;
        }
        place(IDC_MODS_UP, x, by1, bw, bh);
        place(IDC_MODS_DOWN, x + bw + S(8), by1, bw, bh);
        place(IDC_MODS_REFRESH, x + 2 * (bw + S(8)), by1, bw, bh);
        place(IDC_MODS_OPEN, x, by2, bw, bh);
        place(IDC_EXTRACT, x + bw + S(8), by2, bw, bh);
        place(IDC_EXTRACT_OPEN, x + 2 * (bw + S(8)), by2, bw, bh);
        break;
    }
    case CARD_ADVANCED:
        place_input(IDC_EXTRA_EDIT, x + S(110), row, right - (x + S(110)), S(34));
        break;
    }
}

static void layout_content(void)
{
    RECT rc;
    int cw, ch, pad = S(24), gap = S(16), y, r, i, id, pass;
    unsigned mask = page_cards[page];
    SCROLLINFO si;
    if (content_wnd == NULL || in_layout) {
        return;
    }
    in_layout = 1;
    for (pass = 0; pass < 2; pass++) {
        GetClientRect(content_wnd, &rc);
        cw = rc.right;
        ch = rc.bottom;
        for (id = 100; id < IDC_LAST; id++) {
            INFO(id).framed = 0;
        }
        for (i = 0; i < CARD_COUNT; i++) {
            SetRectEmpty(&card_rc[i]);
        }
        y = pad - scroll_y;
        for (r = 0; r < (int) (sizeof(card_rows) / sizeof(card_rows[0])); r++) {
            int cards[2], n = 0, rowh = 0, k, cx = pad, each;
            for (k = 0; k < 2; k++) {
                int c = card_rows[r][k];
                if (c >= 0 && (mask & (1u << c))) {
                    cards[n++] = c;
                    if (S(card_info[c].h) > rowh) {
                        rowh = S(card_info[c].h);
                    }
                }
            }
            if (n == 0) {
                continue;
            }
            each = (cw - 2 * pad - (n - 1) * gap) / n;
            for (k = 0; k < n; k++) {
                RECT cr;
                cr.left = cx;
                cr.top = y;
                cr.right = cx + each;
                cr.bottom = y + rowh;
                card_rc[cards[k]] = cr;
                layout_card(cards[k], cr);
                cx += each + gap;
            }
            y += rowh + gap;
        }
        content_total = y + scroll_y - gap + pad;
        /* controls of the cards that are not on this page */
        for (id = 100; id < IDC_LAST; id++) {
            int c = INFO(id).card;
            if (c >= 0 && !(mask & (1u << c))) {
                ShowWindow(ctl(id), SW_HIDE);
            }
        }
        {
            int max = content_total - ch;
            int ns = max < 0 ? 0 : scroll_y > max ? max : scroll_y;
            if (ns == scroll_y) {
                break;
            }
            scroll_y = ns;
        }
    }
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content_total - 1;
    si.nPage = (UINT) ch;
    si.nPos = scroll_y;
    SetScrollInfo(content_wnd, SB_VERT, &si, TRUE);
    in_layout = 0;
    InvalidateRect(content_wnd, NULL, FALSE);
}

static void layout_main(void)
{
    RECT rc;
    int W, H, side = S(200), header = S(64), footer = S(76), i;
    if (main_wnd == NULL) {
        return;
    }
    GetClientRect(main_wnd, &rc);
    W = rc.right;
    H = rc.bottom;
    for (i = 0; i < PAGE_COUNT; i++) {
        place(IDC_NAV_FIRST + i, S(12), S(76) + i * S(48), side - S(24), S(42));
    }
    place(IDC_PLAY, side + S(24), H - footer + S(14), S(150), S(48));
    if (content_wnd != NULL) {
        SetWindowPos(content_wnd, NULL, side, header, W - side, H - header - footer,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        layout_content();
    }
    InvalidateRect(main_wnd, NULL, FALSE);
}

static void set_page(int p)
{
    int i;
    page = p;
    scroll_y = 0;
    for (i = 0; i < PAGE_COUNT; i++) {
        INFO(IDC_NAV_FIRST + i).state = (i == p);
        InvalidateRect(ctl(IDC_NAV_FIRST + i), NULL, FALSE);
    }
    layout_content();
}

static void scroll_to(int y)
{
    int max = content_total - (int) (S(1)) * 0;
    RECT rc;
    GetClientRect(content_wnd, &rc);
    max = content_total - rc.bottom;
    if (max < 0) {
        max = 0;
    }
    y = y < 0 ? 0 : y > max ? max : y;
    if (y != scroll_y) {
        scroll_y = y;
        layout_content();
    }
}

/* called by mods_refresh: the list gives way to a placeholder when empty */
static void ui_mods_changed(void)
{
    if (content_wnd != NULL) {
        layout_content();
    }
}

/* --- painting --------------------------------------------------------------------- */

static void pill_state(COLORREF* color, char* out, int size)
{
    char exe[MAX_PATH];
    if (child != NULL) {
        *color = C_ACCENT_HI;
        snprintf(out, size, "%s", child_kind == 2 ? "Building" : child_kind == 1 ? "Extracting" : "Game running");
    } else if (hash_running) {
        *color = C_ACCENT_HI;
        snprintf(out, size, "Verifying disc");
    } else if (!iso_ok) {
        *color = C_ORANGE;
        snprintf(out, size, "Choose the disc image");
    } else if (iso_verified < 0) {
        *color = C_RED;
        snprintf(out, size, "Wrong disc image");
    } else if (iso_verified == 0) {
        *color = C_ORANGE;
        snprintf(out, size, "Disc not verified");
    } else if (find_game_exe(exe, sizeof(exe))) {
        *color = C_GREEN;
        snprintf(out, size, "Ready to launch");
    } else {
        *color = C_ORANGE;
        snprintf(out, size, "Build the game first");
    }
}

static void paint_card_body(HDC dc, int c, RECT rc)
{
    int pad = S(20), x = rc.left + pad, y = rc.top + pad, w = rc.right - rc.left - 2 * pad, right = rc.right - pad;
    int row = y + S(44);
    char buf[64];
    if (card_info[c].icon != 0) {
        glyph(dc, font_icon_lg, C_TEXT, x, y, S(28), S(28), card_info[c].icon);
        text(dc, font_head, C_TEXT, x + (have_icon_font ? S(38) : 0), y, w, S(28), card_info[c].title,
             DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    switch (c) {
    case CARD_GAME: {
        RECT tile;
        int tx = x + S(208) + S(24);
        tile.left = x;
        tile.top = y;
        tile.right = x + S(208);
        tile.bottom = y + S(132);
        round_rect(dc, tile, S(10), C_INPUT, C_INPUT_EDGE, 0);
        if (banner != NULL) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP ob = (HBITMAP) SelectObject(mem, banner);
            int bw = S(192), bh = S(64);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, (tile.left + tile.right - bw) / 2, (tile.top + tile.bottom - bh) / 2, bw, bh, mem, 0, 0, 96,
                       32, SRCCOPY);
            SelectObject(mem, ob);
            DeleteDC(mem);
        } else {
            text(dc, font_semi, C_TEXT_DIM, tile.left + S(8), tile.top, S(192), S(132), "No disc image",
                 DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        text(dc, font_title, C_TEXT, tx, y - S(2), right - tx, S(32), card_info[c].title,
             DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        text(dc, font_semi, C_TEXT_DIM, tx, y + S(38), right - tx, S(20), "Game disc (ISO)",
             DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        {
            int cy = y + S(108) + S(15);
            int good = iso_ok && iso_verified > 0;
            COLORREF sc = good ? C_GREEN : C_RED;
            dot(dc, tx + S(9), cy, S(9), sc);
            glyph(dc, font_icon_sm, C_WHITE, tx, cy - S(9), S(18), S(18), good ? 0xE73E : 0xE711);
            text(dc, font_semi, sc, tx + S(26), y + S(108), right - S(130) - tx - S(26), S(30),
                 labels[IDC_ISO_STATUS - 100], DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        }
        text(dc, font_small, C_TEXT_DIM, tx, y + S(140), right - tx, S(36), labels[IDC_ISO_HASH - 100],
             DT_WORDBREAK | DT_END_ELLIPSIS);
        break;
    }
    case CARD_BUILD:
        text(dc, ui_font, C_TEXT, x, row, S(96), S(34), "Source folder", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        text(dc, font_small, C_TEXT_DIM, x, row + S(46), right - S(150) - x, S(44), labels[IDC_BUILD_STATUS - 100],
             DT_WORDBREAK | DT_END_ELLIPSIS);
        break;
    case CARD_DISPLAY: {
        static const char* res[] = { "640 x 480", "1280 x 960", "1920 x 1440", "2560 x 1920" };
        text(dc, ui_font, C_TEXT, x, row, S(96), S(32), "Window size", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        if (right - (x + S(100) + 4 * S(60)) >= S(72)) {
            text(dc, font_small, C_TEXT_DIM, x + S(100) + 4 * S(60), row, right - (x + S(100) + 4 * S(60)), S(32),
                 res[scale_get() - 1], DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
        text(dc, ui_font, C_TEXT, x, row + S(46), S(96), S(32), "Rendering", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        text(dc, ui_font, C_TEXT, x, row + S(92), S(96), S(32), "Anti-aliasing", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        text(dc, ui_font, C_TEXT, x, row + S(138), S(96), S(32), "Anisotropic", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        break;
    }
    case CARD_AUDIO:
        text(dc, ui_font, C_TEXT, x, row, S(76), S(32), "Volume", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        snprintf(buf, sizeof(buf), "%d%%", volume_get());
        text(dc, ui_font, C_TEXT, right - S(52), row, S(52), S(32), buf, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        break;
    case CARD_CONTROLS: {
        int half = (w - S(16)) / 2;
        RECT t1, t2;
        const char* title = adapter_st == 1 ? "Adapter detected" : adapter_st == 0 ? "Adapter needs a driver"
                                                                                    : "No adapter detected";
        const char* sub = adapter_st == 1 ? "Controllers ready (WinUSB)"
                          : adapter_st == 0 ? "Needs the WinUSB driver: see setup"
                                            : "Optional; XInput pads also work";
        t1.left = x;
        t1.top = row;
        t1.right = x + half;
        t1.bottom = row + S(64);
        t2 = t1;
        t2.left = x + half + S(16);
        t2.right = right;
        round_rect(dc, t1, S(8), C_INPUT, C_INPUT_EDGE, 0);
        round_rect(dc, t2, S(8), C_INPUT, C_INPUT_EDGE, 0);
        dot(dc, t1.left + S(20), (t1.top + t1.bottom) / 2, S(6),
            adapter_st == 1 ? C_GREEN : adapter_st == 0 ? C_ORANGE : C_TEXT_DIM);
        text(dc, font_semi, C_TEXT, t1.left + S(36), t1.top + S(12), half - S(180), S(20), title,
             DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
        text(dc, font_small, C_TEXT_DIM, t1.left + S(36), t1.top + S(34), half - S(180), S(20), sub,
             DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
        text(dc, font_small, C_TEXT_DIM, t2.left + S(12), t2.top + S(8), t2.right - t2.left - S(24), S(18),
             "Keyboard mapping (port 1):", DT_SINGLELINE | DT_LEFT);
        text(dc, font_small, C_TEXT, t2.left + S(12), t2.top + S(28), t2.right - t2.left - S(24), S(32),
             "Arrows = stick, IJKL = C stick, Z/X/C/V = A/B/X/Y, Q/E = L/R, Space = Z, Enter = Start",
             DT_WORDBREAK | DT_END_ELLIPSIS);
        text(dc, ui_font, C_TEXT, x, row + S(80), S(86), S(34), "Layout file", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        break;
    }
    case CARD_SAVES:
        text(dc, font_small, C_TEXT_DIM, x, row + S(92), w, S(80),
             "Leave the folder empty for the saves folder next to melee.exe. The game creates its save file on "
             "first boot into MemoryCardA.USA.raw, the format Dolphin uses: copy the file to or from "
             "Dolphin's card folder, or drop a .gci file here to import it. Slot B is always empty.",
             DT_WORDBREAK);
        break;
    case CARD_MODS:
        if (!IsRectEmpty(&mods_empty_rc)) {
            RECT e = mods_empty_rc;
            int cy = (e.top + e.bottom) / 2;
            round_rect(dc, e, S(8), C_CARD, C_INPUT_EDGE, 1);
            glyph(dc, font_icon_lg, C_TEXT_DIM, e.left + S(24), cy - S(16), S(32), S(32), 0xF12B);
            text(dc, font_semi, C_TEXT, e.left + S(68), cy - S(20), e.right - e.left - S(80), S(20), "No mods installed",
                 DT_SINGLELINE | DT_LEFT);
            text(dc, font_small, C_TEXT_DIM, e.left + S(68), cy + S(2), e.right - e.left - S(80), S(36),
                 "A mod is a folder under mods\\ holding the files it replaces, in the disc's layout "
                 "(mods\\MyMod\\files\\...). Checked mods are active; higher in the list wins.",
                 DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        break;
    case CARD_ADVANCED:
        text(dc, ui_font, C_TEXT, x, row, S(106), S(34), "Extra options", DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        text(dc, font_small, C_TEXT_DIM, x, row + S(44), w, S(20),
             "Passed to melee.exe as typed, e.g. --fast or --headless --frames 600 (see pc\\README.md).",
             DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
        break;
    }
}

static void paint_content(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    HDC dc;
    HBITMAP bmp, ob;
    int c, id;
    HWND focus = GetFocus();
    GetClientRect(h, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    ob = (HBITMAP) SelectObject(dc, bmp);
    fill(dc, rc, C_BG);
    for (c = 0; c < CARD_COUNT; c++) {
        if (IsRectEmpty(&card_rc[c])) {
            continue;
        }
        round_rect(dc, card_rc[c], S(12), C_CARD, C_CARD_EDGE, 0);
        paint_card_body(dc, c, card_rc[c]);
    }
    for (id = 100; id < IDC_LAST; id++) {
        if (INFO(id).framed) {
            round_rect(dc, INFO(id).frame, S(8), C_INPUT, focus == ctl(id) ? C_ACCENT : C_INPUT_EDGE, 0);
        }
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void paint_main(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc, r;
    HDC dc;
    HBITMAP bmp, ob;
    int side = S(200), header = S(64), footer = S(76), tw;
    COLORREF pill_c;
    char pill[64];
    GetClientRect(h, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    ob = (HBITMAP) SelectObject(dc, bmp);
    fill(dc, rc, C_BG);
    r = rc;
    r.right = side;
    fill(dc, r, C_SIDEBAR);
    /* sidebar: a small mark above the pages */
    {
        RECT m;
        m.left = S(20);
        m.top = S(22);
        m.right = S(20) + S(32);
        m.bottom = S(22) + S(32);
        round_rect(dc, m, S(8), C_ACCENT, C_ACCENT, 0);
        text(dc, font_head, C_WHITE, m.left, m.top, S(32), S(32), "M", DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        text(dc, font_semi, C_TEXT, m.right + S(10), m.top, side - m.right - S(12), S(32), "PC port",
             DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    /* header */
    tw = text_width(dc, font_title, PC_PORT_NAME);
    text(dc, font_title, C_TEXT, side + S(24), 0, tw + S(4), header, PC_PORT_NAME, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    text(dc, ui_font, C_TEXT_DIM, side + S(24) + tw + S(10), S(2), S(200), header, PC_PORT_VERSION,
         DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    pill_state(&pill_c, pill, sizeof(pill));
    tw = text_width(dc, font_semi, pill) + S(50);
    r.left = rc.right - S(24) - tw;
    r.right = rc.right - S(24);
    r.top = S(14);
    r.bottom = S(50);
    round_rect(dc, r, S(10), C_CARD, C_CARD_EDGE, 0);
    dot(dc, r.left + S(20), (r.top + r.bottom) / 2, S(5), pill_c);
    text(dc, font_semi, C_TEXT, r.left + S(34), r.top, tw - S(40), r.bottom - r.top, pill,
         DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    /* footer: the status line right of Play */
    text(dc, ui_font, C_TEXT_DIM, side + S(190), rc.bottom - footer + S(14), rc.right - S(24) - side - S(190), S(48),
         labels[IDC_STATUS - 100], DT_RIGHT | DT_WORDBREAK | DT_END_ELLIPSIS);
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

/* --- settings ---------------------------------------------------------------------- */

static void load_settings(void)
{
    char buf[MAX_PATH];
    int i;
    ini_get("iso", buf, sizeof(buf), "");
    if (buf[0] == '\0' || !file_exists(buf)) {
        find_default_iso(buf, sizeof(buf));
    }
    set_text(IDC_ISO_EDIT, buf);
    refresh_iso_status();
    i = ini_get_int("scale", 2);
    scale_set(i >= 1 && i <= 4 ? i : 2);
    i = ini_get_int("internal", 0);
    seg_set(IDC_INTERNAL_0, 5, i >= 0 && i <= 4 ? i : 0);
    i = ini_get_int("msaa", 0);
    seg_set(IDC_MSAA_0, 4, i == 2 ? 1 : i == 4 ? 2 : i == 8 ? 3 : 0);
    i = ini_get_int("aniso", 0);
    seg_set(IDC_ANISO_0, 3, i == 4 ? 1 : i == 16 ? 2 : 0);
    toggle_set(IDC_FULLSCREEN, ini_get_int("fullscreen", 0));
    toggle_set(IDC_NO_CONSOLE, ini_get_int("no_console", 0));
    i = ini_get_int("volume", 100);
    volume_set(i < 0 ? 0 : i > 100 ? 100 : i);
    toggle_set(IDC_MUTE, ini_get_int("mute", 0));
    ini_get("keymap", buf, sizeof(buf), "");
    set_text(IDC_KEYMAP_EDIT, buf);
    ini_get("saves", buf, sizeof(buf), "");
    set_text(IDC_SAVES_EDIT, buf);
    ini_get("extra", buf, sizeof(buf), "");
    set_text(IDC_EXTRA_EDIT, buf);
    ini_get("extract_dir", extract_dir, sizeof(extract_dir), "");
    ini_get("source", buf, sizeof(buf), "");
    if (buf[0] == '\0') {
        find_source_dir(buf, sizeof(buf));
    }
    set_text(IDC_SRC_EDIT, buf);
    refresh_build_status();
    EnableWindow(ctl(IDC_EXTRACT_OPEN), extract_dir[0] != '\0' && dir_exists(extract_dir));
    mods_refresh();
    refresh_adapter_status();
    {
        char exe[MAX_PATH];
        set_status(find_game_exe(exe, sizeof(exe)) ? "Ready." : "melee.exe was not found: build it first (Build card).");
    }
}

/* --- window procedures -------------------------------------------------------------- */

static void on_command(int id, int code)
{
    char buf[MAX_PATH];
    if (id < 100 || id >= IDC_LAST) {
        return;
    }
    if (id >= IDC_NAV_FIRST && id <= IDC_NAV_LAST) {
        set_page(id - IDC_NAV_FIRST);
        return;
    }
    if (id >= IDC_SCALE_1 && id <= IDC_SCALE_4) {
        scale_set(id - IDC_SCALE_1 + 1);
        return;
    }
    if (id >= IDC_INTERNAL_0 && id <= IDC_INTERNAL_4) {
        seg_set(IDC_INTERNAL_0, 5, id - IDC_INTERNAL_0);
        return;
    }
    if (id >= IDC_MSAA_0 && id <= IDC_MSAA_3) {
        seg_set(IDC_MSAA_0, 4, id - IDC_MSAA_0);
        return;
    }
    if (id >= IDC_ANISO_0 && id <= IDC_ANISO_2) {
        seg_set(IDC_ANISO_0, 3, id - IDC_ANISO_0);
        return;
    }
    if (INFO(id).kind == K_TOGGLE) {
        toggle_set(id, !toggle_get(id));
        return;
    }
    if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
        InvalidateRect(content_wnd, NULL, FALSE);
        return;
    }
    switch (id) {
    case IDC_ISO_EDIT:
        if (code == EN_CHANGE) {
            refresh_iso_status();
        }
        break;
    case IDC_ISO_BROWSE:
        get_text(IDC_ISO_EDIT, buf, sizeof(buf));
        if (browse_file("Choose the Melee disc image", "Disc images (*.iso;*.gcm)\0*.iso;*.gcm\0All files\0*.*\0",
                        buf, sizeof(buf)))
        {
            set_text(IDC_ISO_EDIT, buf);
        }
        break;
    case IDC_ISO_VERIFY:
        verify_iso();
        InvalidateRect(main_wnd, NULL, FALSE);
        break;
    case IDC_KEYMAP_BROWSE:
        get_text(IDC_KEYMAP_EDIT, buf, sizeof(buf));
        if (browse_file("Choose a keyboard layout file", "Text files (*.txt)\0*.txt\0All files\0*.*\0", buf,
                        sizeof(buf)))
        {
            set_text(IDC_KEYMAP_EDIT, buf);
        }
        break;
    case IDC_KEYMAP_CREATE:
        create_keymap();
        break;
    case IDC_ADAPTER:
        adapter_setup();
        break;
    case IDC_SAVES_BROWSE:
        get_text(IDC_SAVES_EDIT, buf, sizeof(buf));
        if (browse_folder("Choose the folder for save files", buf, sizeof(buf))) {
            set_text(IDC_SAVES_EDIT, buf);
        }
        break;
    case IDC_SAVES_OPEN:
        get_text(IDC_SAVES_EDIT, buf, sizeof(buf));
        if (buf[0] == '\0') {
            char dir[MAX_PATH];
            game_dir(dir, sizeof(dir));
            snprintf(buf, sizeof(buf), "%s\\saves", dir);
        }
        open_folder(buf);
        break;
    case IDC_MODS_UP:
        mods_move(-1);
        break;
    case IDC_MODS_DOWN:
        mods_move(1);
        break;
    case IDC_MODS_REFRESH:
        mods_save();
        mods_refresh();
        break;
    case IDC_MODS_OPEN:
        mods_dir(buf, sizeof(buf));
        open_folder(buf);
        break;
    case IDC_EXTRACT:
        extract();
        break;
    case IDC_EXTRACT_OPEN:
        if (extract_dir[0] != '\0') {
            open_folder(extract_dir);
        }
        break;
    case IDC_PLAY:
        play();
        break;
    case IDC_SRC_EDIT:
        if (code == EN_CHANGE) {
            refresh_build_status();
            InvalidateRect(main_wnd, NULL, FALSE);
        }
        break;
    case IDC_SRC_BROWSE:
        get_text(IDC_SRC_EDIT, buf, sizeof(buf));
        if (browse_folder("Choose the MeleeRecomp source folder", buf, sizeof(buf))) {
            set_text(IDC_SRC_EDIT, buf);
        }
        break;
    case IDC_BUILD:
        build_game();
        break;
    }
}

static LRESULT CALLBACK content_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*) lp;
        if (nm->idFrom == IDC_MODS_LIST && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* lv = (NMLISTVIEW*) lp;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
                mods_save();
            }
        }
        return 0;
    }
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT*) lp);
        return TRUE;
    case WM_HSCROLL:
        if ((HWND) lp == ctl(IDC_VOLUME)) {
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC) wp, C_TEXT);
        SetBkColor((HDC) wp, C_INPUT);
        return (LRESULT) br_input;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_content(h);
        return 0;
    case WM_SIZE:
        layout_content();
        return 0;
    case WM_VSCROLL: {
        RECT rc;
        int line = S(40);
        GetClientRect(h, &rc);
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            scroll_to(scroll_y - line);
            break;
        case SB_LINEDOWN:
            scroll_to(scroll_y + line);
            break;
        case SB_PAGEUP:
            scroll_to(scroll_y - rc.bottom);
            break;
        case SB_PAGEDOWN:
            scroll_to(scroll_y + rc.bottom);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si;
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &si);
            scroll_to(si.nTrackPos);
            break;
        }
        case SB_TOP:
            scroll_to(0);
            break;
        case SB_BOTTOM:
            scroll_to(content_total);
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to(scroll_y - GET_WHEEL_DELTA_WPARAM(wp) * S(48) / WHEEL_DELTA);
        return 0;
    case WM_SETFOCUS:
        /* clicks on the background take the focus away from an edit */
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT*) lp);
        return TRUE;
    case WM_HASH_DONE:
        set_text(IDC_ISO_HASH, hash_result);
        if (strncmp(hash_result, "Verified", 8) == 0 || strncmp(hash_result, "main.dol matches", 16) == 0) {
            char stamp[MAX_PATH + 64];
            iso_verified = 1;
            iso_stamp(hash_iso_path, stamp, sizeof(stamp));
            ini_set("verified", stamp);
        } else if (strncmp(hash_result, "Not the supported", 17) == 0) {
            iso_verified = -1;
        }
        iso_status_text();
        EnableWindow(ctl(IDC_ISO_VERIFY), TRUE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_PROCESS && child != NULL && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
            child_finished();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_main(h);
        return 0;
    case WM_SIZE:
        layout_main();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*) lp;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(600);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* r = (RECT*) lp;
        dpi = HIWORD(wp);
        create_fonts();
        EnumChildWindows(content_wnd, apply_font, (LPARAM) ui_font);
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_main();
        return 0;
    }
    case WM_CLOSE:
        save_settings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    INITCOMMONCONTROLSEX icc;
    RECT r;
    MSG msg;
    char* slash;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    (void) prev;
    (void) cmdline;

    app = inst;
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    slash = strrchr(exe_dir, '\\');
    if (slash != NULL) {
        *slash = '\0';
    }
    {
        /* the copy a previous build moved aside (see build_game) */
        char old[MAX_PATH];
        snprintf(old, sizeof(old), "%s\\melee-launcher.old.exe", exe_dir);
        DeleteFileA(old);
    }
    snprintf(ini_path, sizeof(ini_path), "%s\\%s", exe_dir, INI_NAME);

    {
        /* per-monitor DPI on Windows 10 1703+, system DPI before that */
        typedef BOOL(WINAPI * set_ctx_t)(HANDLE);
        typedef BOOL(WINAPI * set_aware_t)(void);
        set_ctx_t set_ctx = (set_ctx_t) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        set_aware_t set_aware = (set_aware_t) GetProcAddress(user32, "SetProcessDPIAware");
        if (set_ctx != NULL) {
            set_ctx((HANDLE) (INT_PTR) -4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        } else if (set_aware != NULL) {
            set_aware();
        }
    }

    CoInitialize(NULL);
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "MeleeLauncher";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    wc.lpfnWndProc = content_proc;
    wc.lpszClassName = "MeleeContent";
    wc.style = 0;
    RegisterClassA(&wc);
    wc.lpfnWndProc = slider_proc;
    wc.lpszClassName = "MeleeSlider";
    RegisterClassA(&wc);

    main_wnd = CreateWindowExA(0, "MeleeLauncher", APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                               CW_USEDEFAULT, 100, 100, NULL, NULL, inst, NULL);
    {
        typedef UINT(WINAPI * get_dpi_t)(HWND);
        get_dpi_t get_dpi = (get_dpi_t) GetProcAddress(user32, "GetDpiForWindow");
        if (get_dpi != NULL) {
            dpi = (int) get_dpi(main_wnd);
        } else {
            HDC dc = GetDC(NULL);
            dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
        if (dpi < 96) {
            dpi = 96;
        }
    }
    {
        /* dark title bar (Windows 10 1809+; the attribute id changed in 20H1) */
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm != NULL) {
            typedef HRESULT(WINAPI * set_attr_t)(HWND, DWORD, LPCVOID, DWORD);
            set_attr_t set_attr = (set_attr_t) GetProcAddress(dwm, "DwmSetWindowAttribute");
            BOOL on = TRUE;
            if (set_attr != NULL && set_attr(main_wnd, 20, &on, sizeof(on)) != S_OK) {
                set_attr(main_wnd, 19, &on, sizeof(on));
            }
        }
    }
    create_fonts();
    br_input = CreateSolidBrush(C_INPUT);
    content_wnd = CreateWindowExA(WS_EX_CONTROLPARENT, "MeleeContent", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                  0, 0, 10, 10, main_wnd, NULL, inst, NULL);
    build_ui();
    load_settings();
    set_page(PAGE_SETUP);

    r.left = 0;
    r.top = 0;
    r.right = S(1100);
    r.bottom = S(900);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    {
        RECT work;
        int w = r.right - r.left, hgt = r.bottom - r.top;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
        if (w > work.right - work.left) {
            w = work.right - work.left;
        }
        if (hgt > work.bottom - work.top) {
            hgt = work.bottom - work.top;
        }
        SetWindowPos(main_wnd, NULL, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - hgt) / 2, w, hgt, SWP_NOZORDER);
    }
    layout_main();
    ShowWindow(main_wnd, show);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(main_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (child != NULL) {
        CloseHandle(child);
    }
    return 0;
}
