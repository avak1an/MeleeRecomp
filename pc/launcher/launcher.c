/**
 * @file launcher.c
 * Graphical launcher for the PC port: a plain Win32 window (no runtime
 * dependencies) that lives next to melee.exe. It picks the disc image,
 * display, audio, keyboard, save and mod settings, remembers them in
 * launcher.ini, extracts the disc's files for modding, keeps
 * mods/enabled.txt in step with the mod list, and starts the game.
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
#include <string.h>

#include "../include/pc_version.h"

#define APP_TITLE "Super Smash Bros. Melee PC " PC_PORT_VERSION
#define INI_NAME "launcher.ini"

enum {
    IDC_ISO_EDIT = 100,
    IDC_ISO_BROWSE,
    IDC_ISO_STATUS,
    IDC_ISO_VERIFY,
    IDC_ISO_HASH,
    IDC_SIZE_COMBO,
    IDC_FULLSCREEN,
    IDC_NO_CONSOLE,
    IDC_VOLUME,
    IDC_VOLUME_LABEL,
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
    IDC_LAST
};

#define TIMER_PROCESS 1
#define WM_HASH_DONE (WM_APP + 1)

/* Super Smash Bros. Melee (USA) v1.02: the image the decompilation targets,
 * and its main.dol (the hash in the repository README). */
#define KNOWN_ISO_SHA1 "d4e70c064cc714ba8400a849cf299dbd1aa326fc"
#define KNOWN_DOL_SHA1 "08e0bf20134dfcb260699671004527b2d6bb1a45"

static HINSTANCE app;
static HWND main_wnd;
static HFONT ui_font, bold_font;
static char exe_dir[MAX_PATH];
static char exe_path[MAX_PATH]; /* this program's own file */
static char ini_path[MAX_PATH];
static char extract_dir[MAX_PATH];
static HANDLE child; /* running game, extraction or build */
static int child_kind;  /* 0 game, 1 extraction, 2 build */

static int find_game_exe(char* out, int size);

static HWND ctl(int id)
{
    return GetDlgItem(main_wnd, id);
}

static void set_text(int id, const char* text)
{
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
    snprintf(status, size, "Super Smash Bros. Melee (NTSC, GALE01): %.32s", hdr + 0x20);
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
    get_text(IDC_ISO_EDIT, path, sizeof(path));
    probe_iso(path, status, sizeof(status));
    set_text(IDC_ISO_STATUS, status);
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

static void refresh_adapter_status(void)
{
    switch (adapter_state()) {
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

static void save_settings(void)
{
    char buf[MAX_PATH];
    get_text(IDC_ISO_EDIT, buf, sizeof(buf));
    ini_set("iso", buf);
    ini_set_int("scale", (int) SendMessageA(ctl(IDC_SIZE_COMBO), CB_GETCURSEL, 0, 0) + 1);
    ini_set_int("fullscreen", IsDlgButtonChecked(main_wnd, IDC_FULLSCREEN) == BST_CHECKED);
    ini_set_int("no_console", IsDlgButtonChecked(main_wnd, IDC_NO_CONSOLE) == BST_CHECKED);
    ini_set_int("volume", (int) SendMessageA(ctl(IDC_VOLUME), TBM_GETPOS, 0, 0));
    ini_set_int("mute", IsDlgButtonChecked(main_wnd, IDC_MUTE) == BST_CHECKED);
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
                    "Build the game first (see \"Build from source\").",
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
    scale = (int) SendMessageA(ctl(IDC_SIZE_COMBO), CB_GETCURSEL, 0, 0) + 1;
    volume = (int) SendMessageA(ctl(IDC_VOLUME), TBM_GETPOS, 0, 0);
    game_dir(buf, sizeof(buf));
    n = (size_t) snprintf(args, sizeof(args), "--iso \"%s\" --scale %d --volume %d --log \"%s\\melee.log\"", iso,
                          scale, volume, buf);
    if (IsDlgButtonChecked(main_wnd, IDC_FULLSCREEN) == BST_CHECKED) {
        n += (size_t) snprintf(args + n, sizeof(args) - n, " --fullscreen");
    }
    if (IsDlgButtonChecked(main_wnd, IDC_MUTE) == BST_CHECKED) {
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
    if (IsDlgButtonChecked(main_wnd, IDC_NO_CONSOLE) == BST_CHECKED) {
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

/* --- window ------------------------------------------------------------------- */

static HWND make(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, main_wnd, (HMENU) (INT_PTR) id,
                             app, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM) ui_font, TRUE);
    return c;
}

static HWND make_ex(DWORD ex, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, main_wnd,
                             (HMENU) (INT_PTR) id, app, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM) ui_font, TRUE);
    return c;
}

static void heading(const char* text, int y)
{
    HWND c = make("STATIC", text, 0, 16, y, 400, 18, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM) bold_font, TRUE);
}

static void build_ui(void)
{
    char buf[MAX_PATH];
    int y = 12, i;
    static const char* sizes[] = { "640 x 480 (1x)", "1280 x 960 (2x)", "1920 x 1440 (3x)", "2560 x 1920 (4x)" };

    heading("Game disc", y);
    y += 20;
    make("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 16, y, 420, 22, IDC_ISO_EDIT);
    make("BUTTON", "Browse...", 0, 444, y, 80, 22, IDC_ISO_BROWSE);
    y += 26;
    make("STATIC", "", 0, 16, y, 420, 16, IDC_ISO_STATUS);
    make("BUTTON", "Verify (SHA-1)", 0, 444, y - 3, 80, 22, IDC_ISO_VERIFY);
    y += 20;
    make("STATIC", "", 0, 16, y, 508, 32, IDC_ISO_HASH);
    y += 36;

    heading("Display", y);
    y += 20;
    make("STATIC", "Window size", 0, 16, y + 3, 80, 16, 0);
    make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 100, y, 150, 120, IDC_SIZE_COMBO);
    for (i = 0; i < 4; i++) {
        SendMessageA(ctl(IDC_SIZE_COMBO), CB_ADDSTRING, 0, (LPARAM) sizes[i]);
    }
    make("BUTTON", "Full screen (F11 or Alt+Enter in the game)", BS_AUTOCHECKBOX, 270, y + 2, 254, 18,
         IDC_FULLSCREEN);
    y += 24;
    make("BUTTON", "Hide the console window (the game's output still goes to melee.log)", BS_AUTOCHECKBOX, 16,
         y + 2, 508, 18, IDC_NO_CONSOLE);
    y += 30;

    heading("Audio", y);
    y += 20;
    make("STATIC", "Volume", 0, 16, y + 3, 80, 16, 0);
    make(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_AUTOTICKS, 100, y, 150, 24, IDC_VOLUME);
    SendMessageA(ctl(IDC_VOLUME), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageA(ctl(IDC_VOLUME), TBM_SETTICFREQ, 10, 0);
    make("STATIC", "100%", 0, 256, y + 3, 40, 16, IDC_VOLUME_LABEL);
    make("BUTTON", "Mute", BS_AUTOCHECKBOX, 300, y + 2, 80, 18, IDC_MUTE);
    y += 32;

    heading("Controls", y);
    y += 20;
    make("STATIC", "GameCube controllers on the official USB adapter (WinUSB driver via Zadig) or XInput", 0, 16, y,
         508, 16, 0);
    y += 16;
    make("STATIC", "gamepads are ports 1-4. Without a gamepad the keyboard is port 1:", 0, 16, y, 508, 16, 0);
    y += 16;
    make("STATIC", "arrows = stick, IJKL = C stick, Z/X/C/V = A/B/X/Y, Q/E = L/R, Space = Z, Enter = Start.", 0,
         16, y, 508, 16, 0);
    y += 20;
    make("STATIC", "Layout file", 0, 16, y + 3, 80, 16, 0);
    make("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, y, 250, 22, IDC_KEYMAP_EDIT);
    make("BUTTON", "Browse...", 0, 356, y, 80, 22, IDC_KEYMAP_BROWSE);
    make("BUTTON", "Create / edit", 0, 444, y, 80, 22, IDC_KEYMAP_CREATE);
    y += 26;
    make("BUTTON", "GameCube adapter setup...", 0, 16, y, 170, 22, IDC_ADAPTER);
    make("STATIC", "", 0, 194, y + 3, 330, 16, IDC_ADAPTER_STATUS);
    y += 32;

    heading("Saves", y);
    y += 20;
    make("STATIC", "Folder", 0, 16, y + 3, 80, 16, 0);
    make("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, y, 250, 22, IDC_SAVES_EDIT);
    make("BUTTON", "Browse...", 0, 356, y, 80, 22, IDC_SAVES_BROWSE);
    make("BUTTON", "Open", 0, 444, y, 80, 22, IDC_SAVES_OPEN);
    y += 26;
    make("STATIC", "Empty = the saves folder next to melee.exe. The game creates its save file on first boot.", 0,
         16, y, 508, 16, 0);
    y += 26;

    heading("Mods", y);
    y += 20;
    make_ex(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            16, y, 420, 96, IDC_MODS_LIST);
    {
        LVCOLUMNA col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_WIDTH;
        col.cx = 390;
        SendMessageA(ctl(IDC_MODS_LIST), LVM_INSERTCOLUMNA, 0, (LPARAM) &col);
        ListView_SetExtendedListViewStyle(ctl(IDC_MODS_LIST), LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    }
    make("BUTTON", "Move up", 0, 444, y, 80, 22, IDC_MODS_UP);
    make("BUTTON", "Move down", 0, 444, y + 26, 80, 22, IDC_MODS_DOWN);
    make("BUTTON", "Refresh", 0, 444, y + 52, 80, 22, IDC_MODS_REFRESH);
    make("BUTTON", "Open folder", 0, 444, y + 78, 80, 22, IDC_MODS_OPEN);
    y += 100;
    make("STATIC", "Each mod is a folder under mods\\ holding the files it replaces, in the disc's layout", 0, 16, y,
         508, 16, 0);
    y += 16;
    make("STATIC", "(mods\\MyMod\\files\\...). Checked mods are active; higher in the list wins.", 0, 16, y, 508,
         16, 0);
    y += 24;

    heading("Game data", y);
    y += 20;
    make("BUTTON", "Extract game files...", 0, 16, y, 150, 24, IDC_EXTRACT);
    make("BUTTON", "Open extracted folder", 0, 174, y, 150, 24, IDC_EXTRACT_OPEN);
    make("STATIC", "Writes the disc's files to a folder: the starting point for a mod.", 0, 332, y + 4, 200, 32, 0);
    y += 34;

    heading("Build from source", y);
    y += 20;
    make("STATIC", "Source folder", 0, 16, y + 3, 80, 16, 0);
    make("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, y, 250, 22, IDC_SRC_EDIT);
    make("BUTTON", "Browse...", 0, 356, y, 80, 22, IDC_SRC_BROWSE);
    make("BUTTON", "Build", 0, 444, y, 80, 22, IDC_BUILD);
    y += 26;
    make("STATIC", "", 0, 16, y, 508, 32, IDC_BUILD_STATUS);
    y += 36;

    heading("Advanced", y);
    y += 20;
    make("STATIC", "Extra options", 0, 16, y + 3, 80, 16, 0);
    make("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, y, 424, 22, IDC_EXTRA_EDIT);
    y += 26;
    make("STATIC", "Passed to melee.exe as typed, e.g. --fast, --headless --frames 600 (see pc\\README.md).", 0, 16, y,
         508, 16, 0);
    y += 28;

    make("BUTTON", "Play", BS_DEFPUSHBUTTON, 16, y, 120, 32, IDC_PLAY);
    SendMessageA(ctl(IDC_PLAY), WM_SETFONT, (WPARAM) bold_font, TRUE);
    make("STATIC", "", 0, 148, y + 8, 236, 32, IDC_STATUS);
    make("STATIC", PC_PORT_NAME " " PC_PORT_VERSION, SS_RIGHT, 384, y + 8, 140, 16, 0);
    y += 44;

    /* settings */
    ini_get("iso", buf, sizeof(buf), "");
    if (buf[0] == '\0' || !file_exists(buf)) {
        find_default_iso(buf, sizeof(buf));
    }
    set_text(IDC_ISO_EDIT, buf);
    refresh_iso_status();
    i = ini_get_int("scale", 2);
    SendMessageA(ctl(IDC_SIZE_COMBO), CB_SETCURSEL, (WPARAM) (i >= 1 && i <= 4 ? i - 1 : 1), 0);
    CheckDlgButton(main_wnd, IDC_FULLSCREEN, ini_get_int("fullscreen", 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(main_wnd, IDC_NO_CONSOLE, ini_get_int("no_console", 0) ? BST_CHECKED : BST_UNCHECKED);
    i = ini_get_int("volume", 100);
    SendMessageA(ctl(IDC_VOLUME), TBM_SETPOS, TRUE, (LPARAM) (i < 0 ? 0 : i > 100 ? 100 : i));
    snprintf(buf, sizeof(buf), "%d%%", i);
    set_text(IDC_VOLUME_LABEL, buf);
    CheckDlgButton(main_wnd, IDC_MUTE, ini_get_int("mute", 0) ? BST_CHECKED : BST_UNCHECKED);
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
        set_status(find_game_exe(exe, sizeof(exe)) ? "Ready." : "melee.exe was not found: build it (see \"Build from source\").");
    }
    (void) y;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        char buf[MAX_PATH];
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
        return 0;
    }
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
    case WM_HSCROLL:
        if ((HWND) lp == ctl(IDC_VOLUME)) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", (int) SendMessageA(ctl(IDC_VOLUME), TBM_GETPOS, 0, 0));
            set_text(IDC_VOLUME_LABEL, buf);
        }
        return 0;
    case WM_HASH_DONE:
        set_text(IDC_ISO_HASH, hash_result);
        EnableWindow(ctl(IDC_ISO_VERIFY), TRUE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_PROCESS && child != NULL && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
            child_finished();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC) wp, TRANSPARENT);
        return (LRESULT) GetSysColorBrush(COLOR_BTNFACE);
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

    CoInitialize(NULL);
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ui_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    bold_font = CreateFontA(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = "MeleeLauncher";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    r.left = 0;
    r.top = 0;
    r.right = 540;
    r.bottom = 894;
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    main_wnd = CreateWindowExA(0, "MeleeLauncher", APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst,
                               NULL);
    build_ui();
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
    CoUninitialize();
    return 0;
}
