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
PFN_glUniformMatrix4fv pc_glUniformMatrix4fv;
PFN_glEnableVertexAttribArray pc_glEnableVertexAttribArray;
PFN_glDisableVertexAttribArray pc_glDisableVertexAttribArray;
PFN_glVertexAttribPointer pc_glVertexAttribPointer;
PFN_glActiveTexture pc_glActiveTexture;
PFN_glBlendEquation pc_glBlendEquation;
PFN_glGenerateMipmap pc_glGenerateMipmap;

static HWND hwnd;
static HDC hdc;
static HGLRC hglrc;
static int ready;
static int closed;
static int win_w, win_h;

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
        }
        return 0;
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
    LOAD(glUniformMatrix4fv);
    LOAD(glEnableVertexAttribArray);
    LOAD(glDisableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glActiveTexture);
    LOAD(glBlendEquation);
    LOAD(glGenerateMipmap);
    if (pc_glCreateShader == NULL || pc_glVertexAttribPointer == NULL) {
        fprintf(stderr, "[pc] GL: the driver does not provide OpenGL 2.0\n");
        return 0;
    }
    swap_interval = (PFN_wglSwapIntervalEXT) wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval != NULL) {
        swap_interval(pc_config.realtime ? 1 : 0);
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
    if (ready) {
        SwapBuffers(hdc);
    }
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
