/**
 * @file gx_pipe.c
 * The GameCube write-gather pipe (GXWGFifo) is a memory-mapped register that
 * the game writes vertex data and GX commands into. On PC it is a plain
 * variable: every write lands here and is discarded. Milestone 3 replaces
 * this with a command recorder feeding the renderer.
 */
#include <dolphin/gx/GXVert.h>

volatile PPCWGPipe GXWGFifo;
