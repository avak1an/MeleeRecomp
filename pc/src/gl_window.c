/**
 * @file gl_window.c
 * Win32 window and OpenGL context for the renderer, plus the loader for the
 * post-1.1 GL entry points. No dependencies beyond the Windows SDK.
 */
#include "pc_gl.h"
#include "pc_runtime.h"

#include <stdio.h>

PFN_glCreateShader pc_glCreateShader;
PFN_glShaderSource pc_glShaderSource;
PFN_glCompileShader pc_glCompileShader;
PFN_glGetShaderiv pc_glGetShaderiv;
PFN_glGetShaderInfoLog pc_glGetShaderInfoLog;
PFN_glCreateProgram pc_glCreateProgram;
PFN_glAttachShader pc_glAttachShader;
PFN_glLinkProgram pc_glLinkProgram;
PFN_glGetProgramiv pc_glGetProgramiv;
PFN_glGetProgramInfoLog pc_glGetProgramInfoLog;
PFN_glUseProgram pc_glUseProgram;
PFN_glDeleteShader pc_glDeleteShader;
PFN_glGetUniformLocation pc_glGetUniformLocation;
PFN_glGetAttribLocation pc_glGetAttribLocation;
PFN_glUniform1i pc_glUniform1i;
PFN_glUniform1f pc_glUniform1f;
PFN_glUniform4fv pc_glUniform4fv;
PFN_glUniform3fv pc_glUniform3fv;
PFN_glUniform2fv pc_glUniform2fv;
PFN_glUniformMatrix4fv pc_glUniformMatrix4fv;
PFN_glEnableVertexAttribArray pc_glEnableVertexAttribArray;
PFN_glDisableVertexAttribArray pc_glDisableVertexAttribArray;
PFN_glVertexAttribPointer pc_glVertexAttribPointer;
PFN_glActiveTexture pc_glActiveTexture;
PFN_glBlendEquation pc_glBlendEquation;
PFN_glGenerateMipmap pc_glGenerateMipmap;
PFN_glGenFramebuffers pc_glGenFramebuffers;
PFN_glBindFramebuffer pc_glBindFramebuffer;
PFN_glFramebufferTexture2D pc_glFramebufferTexture2D;
PFN_glBlitFramebuffer pc_glBlitFramebuffer;
PFN_glDrawRangeElements pc_glDrawRangeElements;
PFN_glGenBuffers pc_glGenBuffers;
PFN_glBindBuffer pc_glBindBuffer;
PFN_glBufferStorage pc_glBufferStorage;
PFN_glMapBufferRange pc_glMapBufferRange;
PFN_glFenceSync pc_glFenceSync;
PFN_glClientWaitSync pc_glClientWaitSync;
PFN_glDeleteSync pc_glDeleteSync;
PFN_glDrawElementsBaseVertex pc_glDrawElementsBaseVertex;
PFN_glCheckFramebufferStatus pc_glCheckFramebufferStatus;

static HWND hwnd;
static HDC hdc;
static HGLRC hglrc;
static int ready;
static int closed;
static int win_w, win_h;
static int fullscreen;
static int vsync_hz; /* frame rate the swap chain enforces, 0 = none */

int pc_window_vsync_hz(void)
{
    return vsync_hz;
}
static RECT windowed_rect; /* window rectangle to restore after full screen */

/// Switches between the bordered window and a borderless window covering
/// the monitor (the GL context and swap chain are unaffected).
void pc_window_set_fullscreen(int on)
{
    if (hwnd == NULL || on == fullscreen) {
        return;
    }
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetWindowRect(hwnd, &windowed_rect);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, NULL, windowed_rect.left, windowed_rect.top,
                     windowed_rect.right - windowed_rect.left, windowed_rect.bottom - windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    fullscreen = on;
}

int pc_window_is_fullscreen(void)
{
    return fullscreen;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        closed = 1;
        return 0;
    case WM_SIZE:
        win_w = LOWORD(lp);
        win_h = HIWORD(lp);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            closed = 1;
        } else if (wp == VK_F11) {
            pc_window_set_fullscreen(!fullscreen);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { /* Alt+Enter */
            pc_window_set_fullscreen(!fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) {
            return 0; /* no beep for Alt+Enter */
        }
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void* load(const char* name)
{
    void* p = (void*) wglGetProcAddress(name);
    if (p == NULL || p == (void*) 1 || p == (void*) 2 || p == (void*) 3 || p == (void*) -1) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        p = gl ? (void*) GetProcAddress(gl, name) : NULL;
    }
    if (p == NULL) {
        fprintf(stderr, "[pc] GL: missing entry point %s\n", name);
    }
    return p;
}

#define LOAD(fn) pc_##fn = (PFN_##fn) load(#fn)

int pc_window_open(int width, int height, const char* title)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    RECT r;
    int pf;
    PFN_wglSwapIntervalEXT swap_interval;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "MeleePC";
    RegisterClassA(&wc);

    if (pc_config.scale > 1) {
        width *= pc_config.scale;
        height *= pc_config.scale;
    }
    r.left = 0;
    r.top = 0;
    r.right = width;
    r.bottom = height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("MeleePC", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                         CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                         wc.hInstance, NULL);
    if (hwnd == NULL) {
        fprintf(stderr, "[pc] GL: CreateWindow failed (%lu)\n", GetLastError());
        return 0;
    }
    win_w = width;
    win_h = height;
    hdc = GetDC(hwnd);
    if (pc_config.fullscreen) {
        pc_window_set_fullscreen(1);
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pf = ChoosePixelFormat(hdc, &pfd);
    if (pf == 0 || !SetPixelFormat(hdc, pf, &pfd)) {
        fprintf(stderr, "[pc] GL: no suitable pixel format\n");
        return 0;
    }
    hglrc = wglCreateContext(hdc);
    if (hglrc == NULL || !wglMakeCurrent(hdc, hglrc)) {
        fprintf(stderr, "[pc] GL: cannot create an OpenGL context (%lu)\n", GetLastError());
        return 0;
    }

    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram);
    LOAD(glDeleteShader);
    LOAD(glGetUniformLocation);
    LOAD(glGetAttribLocation);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glUniform4fv);
    LOAD(glUniform3fv);
    LOAD(glUniform2fv);
    LOAD(glUniformMatrix4fv);
    LOAD(glEnableVertexAttribArray);
    LOAD(glDisableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glActiveTexture);
    LOAD(glBlendEquation);
    LOAD(glGenerateMipmap);
    /* optional (OpenGL 3.0): the EFB copy blits on the GPU with them and
     * reads the pixels back without */
    LOAD(glGenFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glBlitFramebuffer);
    LOAD(glDrawRangeElements);
    LOAD(glGenBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferStorage);
    LOAD(glMapBufferRange);
    LOAD(glFenceSync);
    LOAD(glClientWaitSync);
    LOAD(glDeleteSync);
    LOAD(glDrawElementsBaseVertex);
    LOAD(glCheckFramebufferStatus);
    if (pc_glCreateShader == NULL || pc_glVertexAttribPointer == NULL) {
        fprintf(stderr, "[pc] GL: the driver does not provide OpenGL 2.0\n");
        return 0;
    }
    swap_interval = (PFN_wglSwapIntervalEXT) wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval != NULL) {
        /* vsync only when the refresh rate divides into 60 Hz frames
         * exactly (60, 120, 180, 240 Hz); other rates are paced by a
         * timer in VIWaitForRetrace instead */
        int hz = GetDeviceCaps(hdc, VREFRESH);
        int interval = 0;
        if (pc_config.realtime && hz >= 60 && hz % 60 == 0) {
            interval = hz / 60;
            vsync_hz = 60;
        }
        swap_interval(interval);
        fprintf(stderr, "[pc] display: %d Hz, %s\n", hz,
                interval ? "vsync" : pc_config.realtime ? "timer paced" : "unpaced");
    }
    fprintf(stderr, "[pc] GL: %s / %s\n", (const char*) glGetString(GL_RENDERER),
            (const char*) glGetString(GL_VERSION));
    ready = 1;
    return 1;
}

int pc_window_pump(void)
{
    MSG msg;
    if (hwnd == NULL) {
        return 1;
    }
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return !closed;
}

void pc_window_present(void)
{
    int x, y, w, h;
    if (!ready) {
        return;
    }
    SwapBuffers(hdc);
    pc_window_viewport(&x, &y, &w, &h);
    if (w != win_w || h != win_h) {
        /* the frame is letterboxed: keep the bars black */
        glPushAttrib(GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glPopAttrib();
    }
}

/// The largest 4:3 rectangle centred in the window: where the frame goes.
void pc_window_viewport(int* x, int* y, int* w, int* h)
{
    int ww = win_w > 0 ? win_w : 1, wh = win_h > 0 ? win_h : 1;
    if (ww * 3 >= wh * 4) {
        *h = wh;
        *w = wh * 4 / 3;
    } else {
        *w = ww;
        *h = ww * 3 / 4;
    }
    *x = (ww - *w) / 2;
    *y = (wh - *h) / 2;
}

void pc_window_size(int* width, int* height)
{
    *width = win_w > 0 ? win_w : 1;
    *height = win_h > 0 ? win_h : 1;
}

int pc_window_ready(void)
{
    return ready;
}
