/**
 * @file pc_gl.h
 * Minimal OpenGL binding for the renderer: the GL 1.1 entry points come
 * from opengl32.lib, everything newer is loaded through wglGetProcAddress
 * by pc_gl_load(). Only what the GX layer uses is declared here.
 */
#ifndef PC_GL_H
#define PC_GL_H

#include <windows.h>
#include <GL/gl.h>

/* Tokens missing from the Windows GL 1.1 header. */
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_GENERATE_MIPMAP 0x8191

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

typedef GLuint(APIENTRY* PFN_glCreateShader)(GLenum);
typedef void(APIENTRY* PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void(APIENTRY* PFN_glCompileShader)(GLuint);
typedef void(APIENTRY* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint(APIENTRY* PFN_glCreateProgram)(void);
typedef void(APIENTRY* PFN_glAttachShader)(GLuint, GLuint);
typedef void(APIENTRY* PFN_glLinkProgram)(GLuint);
typedef void(APIENTRY* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void(APIENTRY* PFN_glUseProgram)(GLuint);
typedef void(APIENTRY* PFN_glDeleteShader)(GLuint);
typedef GLint(APIENTRY* PFN_glGetUniformLocation)(GLuint, const GLchar*);
typedef GLint(APIENTRY* PFN_glGetAttribLocation)(GLuint, const GLchar*);
typedef void(APIENTRY* PFN_glUniform1i)(GLint, GLint);
typedef void(APIENTRY* PFN_glUniform1f)(GLint, GLfloat);
typedef void(APIENTRY* PFN_glUniform4fv)(GLint, GLsizei, const GLfloat*);
typedef void(APIENTRY* PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void(APIENTRY* PFN_glEnableVertexAttribArray)(GLuint);
typedef void(APIENTRY* PFN_glDisableVertexAttribArray)(GLuint);
typedef void(APIENTRY* PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void(APIENTRY* PFN_glActiveTexture)(GLenum);
typedef void(APIENTRY* PFN_glBlendEquation)(GLenum);
typedef void(APIENTRY* PFN_glGenerateMipmap)(GLenum);
typedef BOOL(APIENTRY* PFN_wglSwapIntervalEXT)(int);

extern PFN_glCreateShader pc_glCreateShader;
extern PFN_glShaderSource pc_glShaderSource;
extern PFN_glCompileShader pc_glCompileShader;
extern PFN_glGetShaderiv pc_glGetShaderiv;
extern PFN_glGetShaderInfoLog pc_glGetShaderInfoLog;
extern PFN_glCreateProgram pc_glCreateProgram;
extern PFN_glAttachShader pc_glAttachShader;
extern PFN_glLinkProgram pc_glLinkProgram;
extern PFN_glGetProgramiv pc_glGetProgramiv;
extern PFN_glGetProgramInfoLog pc_glGetProgramInfoLog;
extern PFN_glUseProgram pc_glUseProgram;
extern PFN_glDeleteShader pc_glDeleteShader;
extern PFN_glGetUniformLocation pc_glGetUniformLocation;
extern PFN_glGetAttribLocation pc_glGetAttribLocation;
extern PFN_glUniform1i pc_glUniform1i;
extern PFN_glUniform1f pc_glUniform1f;
extern PFN_glUniform4fv pc_glUniform4fv;
extern PFN_glUniformMatrix4fv pc_glUniformMatrix4fv;
extern PFN_glEnableVertexAttribArray pc_glEnableVertexAttribArray;
extern PFN_glDisableVertexAttribArray pc_glDisableVertexAttribArray;
extern PFN_glVertexAttribPointer pc_glVertexAttribPointer;
extern PFN_glActiveTexture pc_glActiveTexture;
extern PFN_glBlendEquation pc_glBlendEquation;
extern PFN_glGenerateMipmap pc_glGenerateMipmap;

/// Create the window and GL context. Returns false (after printing why)
/// when no context can be made; the runtime then keeps running headless.
int pc_window_open(int width, int height, const char* title);

/// Process pending window messages. Returns false once the window closed.
int pc_window_pump(void);

/// Swap buffers.
void pc_window_present(void);

/// Current drawable size in pixels.
void pc_window_size(int* width, int* height);
/// The 4:3 rectangle inside the window that the frame is drawn into (GL
/// coordinates: y counts from the bottom).
void pc_window_viewport(int* x, int* y, int* width, int* height);
void pc_window_set_fullscreen(int on);
/// 60 when the swap chain itself holds the game to 60 frames per second.
int pc_window_vsync_hz(void);
int pc_window_is_fullscreen(void);

/// Whether a GL context exists (rendering calls must check this).
int pc_window_ready(void);

#endif
