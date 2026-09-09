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
            "  --saves DIR     memory card files (default: saves next to the exe)\n"
            "  --fullscreen    start full screen (F11 or Alt+Enter toggles)\n"
            "  --scale N       window size N x 640x480 (default 1)\n"
            "  --keymap FILE   keyboard layout: lines of ACTION = KEY (see pc/README.md)\n"
            "  --volume N      audio volume in percent (default 100); --no-audio mutes\n"
            "  --extract DIR   write every file of the disc to DIR/files (and the\n"
            "                  system files to DIR/sys), then exit\n");
}

int main(int argc, char** argv)
{
    int i;
    const char* extract_dir = NULL;
    pc_config.max_frames = 0;
    pc_config.log_stubs = true;
    pc_config.volume = 100;
    pc_config.scale = 1;

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
            pc_config.quiet_stubs = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            pc_config.headless = true;
        } else if (strcmp(argv[i], "--screenshots") == 0 && i + 1 < argc) {
            pc_config.screenshot_dir = argv[++i];
        } else if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) {
            pc_config.save_dir = argv[++i];
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            pc_config.fullscreen = true;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            pc_config.scale = atoi(argv[++i]);
            if (pc_config.scale < 1 || pc_config.scale > 8) {
                pc_config.scale = 1;
            }
        } else if (strcmp(argv[i], "--keymap") == 0 && i + 1 < argc) {
            pc_config.keymap = argv[++i];
        } else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            pc_config.volume = atoi(argv[++i]);
            if (pc_config.volume < 0) {
                pc_config.volume = 0;
            }
            if (pc_config.volume > 100) {
                pc_config.volume = 100;
            }
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            pc_config.no_audio = true;
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
        if (pc_config.keymap != NULL) {
            pc_pad_load_keymap(pc_config.keymap);
        }
        if (!pc_window_open(640, 480, "Super Smash Bros. Melee")) {
            fprintf(stderr, "[pc] continuing without rendering\n");
        }
    }
    pc_gx_render_init();
    return melee_main();
}
