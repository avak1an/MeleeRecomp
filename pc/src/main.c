/**
 * @file main.c
 * PC entry point. Owns process setup, then hands control to the game's own
 * main() (compiled as melee_main, see pc/CMakeLists.txt).
 *
 * The game never returns from its main loop; the PC runtime exits from
 * VIWaitForRetrace() once the requested number of frames has elapsed, or
 * from OSPanic()/assertion failures.
 */
#include "pc_gl.h"
#include "pc_gx.h"
#include "pc_runtime.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int melee_main(void);

static void usage(void)
{
    fprintf(stderr,
            "usage: melee [--iso PATH] [--frames N] [--realtime] [--quiet-stubs]\n"
            "  --iso PATH      GameCube disc image (GALE01 .iso/.gcm). Default: $MELEE_ISO\n"
            "                  or GALE01.iso in the current or parent directory\n"
            "  --frames N      exit after N video frames (default 0 = run until a\n"
            "                  fatal error or Ctrl-C)\n"
            "  --realtime      pace the main loop to 60 Hz\n"
            "  --autoplay      press Start and A on port 1 every couple of seconds\n"
            "                  (pushes headless runs through prompts)\n"
            "  --input FILE    scripted port-1 input: lines of\n"
            "                  <first frame> <last frame> <buttons|-> [stickX stickY]\n"
            "                  buttons: A B X Y Z L R START UP DOWN LEFT RIGHT joined by +\n"
            "  --seed N        fixed RNG seed (default: the clock), for repeatable runs\n"
            "  --quiet-stubs   do not log the first call of each SDK stub\n"
            "  --headless      no window; run the game logic only\n"
            "  --screenshots DIR  save a BMP of every 60th frame into DIR\n"
            "  --extract DIR   write every file of the disc to DIR/files (and the\n"
            "                  system files to DIR/sys), then exit\n");
}

int main(int argc, char** argv)
{
    int i;
    const char* extract_dir = NULL;
    pc_config.max_frames = 0;
    pc_config.log_stubs = true;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            pc_config.max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iso") == 0 && i + 1 < argc) {
            pc_config.iso = argv[++i];
        } else if (strcmp(argv[i], "--extract") == 0 && i + 1 < argc) {
            extract_dir = argv[++i];
        } else if (strcmp(argv[i], "--realtime") == 0) {
            pc_config.realtime = true;
        } else if (strcmp(argv[i], "--autoplay") == 0) {
            pc_config.autoplay = true;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            pc_config.input_script = argv[++i];
        } else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            pc_debug_watch = (const unsigned int*) (uintptr_t) strtoul(argv[++i], NULL, 0);
            pc_debug_watch_install();
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            pc_config.seed = (unsigned) strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--quiet-stubs") == 0) {
            pc_config.log_stubs = false;
        } else if (strcmp(argv[i], "--headless") == 0) {
            pc_config.headless = true;
        } else if (strcmp(argv[i], "--screenshots") == 0 && i + 1 < argc) {
            pc_config.screenshot_dir = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    pc_runtime_init();
    pc_dvd_init(pc_config.iso);
    if (extract_dir != NULL) {
        pc_dvd_extract(extract_dir);
        return 0;
    }
    if (!pc_config.headless) {
        if (!pc_window_open(640, 480, "Super Smash Bros. Melee")) {
            fprintf(stderr, "[pc] continuing without rendering\n");
        }
    }
    pc_gx_render_init();
    return melee_main();
}
