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
#include <pc_version.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int melee_main(void);

static void usage(void)
{
    fprintf(stderr,
            "melee " PC_PORT_NAME " " PC_PORT_VERSION "\n"
            "usage: melee [--iso PATH] [--frames N] [--realtime] [--quiet-stubs]\n"
            "  --version       print the port's version and exit\n"
            "  --iso PATH      GameCube disc image (GALE01 .iso/.gcm). Default: $MELEE_ISO\n"
            "                  or GALE01.iso in the current or parent directory\n"
            "  --frames N      exit after N video frames (default 0 = run until a\n"
            "                  fatal error or Ctrl-C)\n"
            "  --fast          do not pace to 60 Hz (windowed runs are paced by default;\n"
            "                  headless runs never are). --realtime is accepted and is\n"
            "                  the default\n"
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
            "  --mod DIR       use the files under DIR (or DIR/files) instead of the disc's;\n"
            "                  repeatable, the first given wins. Without it, the mods listed\n"
            "                  in mods/enabled.txt next to the exe are used\n"
            "  --fullscreen    start full screen (F11 or Alt+Enter toggles)\n"
            "  --scale N       window size N x 640x480 (default 1)\n"
            "  --keymap FILE   keyboard layout: lines of ACTION = KEY (see pc/README.md)\n"
            "  --volume N      audio volume in percent (default 100); --no-audio mutes\n"
            "  --extract DIR   write every file of the disc to DIR/files (and the\n"
            "                  system files to DIR/sys), then exit\n");
}

/* mods/enabled.txt next to the executable: one mod per line, highest
 * priority first; a name is a folder under mods/, or a full path. */
static void load_enabled_mods(void)
{
    char path[1024];
    char line[1024];
    FILE* f;
    snprintf(path, sizeof(path), "%s/mods/enabled.txt", pc_exe_dir());
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
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
        if (p[1] == ':' || p[0] == '\\' || p[0] == '/') {
            pc_dvd_add_mod(p);
        } else {
            char full[1024];
            snprintf(full, sizeof(full), "%s/mods/%s", pc_exe_dir(), p);
            pc_dvd_add_mod(full);
        }
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    int i;
    const char* extract_dir = NULL;
    bool mods_given = false;
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
        } else if (strcmp(argv[i], "--fast") == 0) {
            pc_config.fast = true;
        } else if (strcmp(argv[i], "--autoplay") == 0) {
            pc_config.autoplay = true;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            pc_config.input_script = argv[++i];
        } else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            const char* spec = argv[++i];
            uintptr_t addr = (spec[0] >= '0' && spec[0] <= '9') ? (uintptr_t) strtoul(spec, NULL, 0)
                                                                : pc_symbol_address(spec);
            if (addr == 0) {
                fprintf(stderr, "[pc] --watch: cannot resolve %s\n", spec);
                return 2;
            }
            fprintf(stderr, "[pc] watching %s at %p\n", spec, (void*) addr);
            pc_debug_watch = (const unsigned int*) addr;
            pc_debug_watch_install();
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            pc_config.seed = (unsigned) strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--quiet-stubs") == 0) {
            pc_config.log_stubs = false;
            pc_config.quiet_stubs = true;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", PC_PORT_NAME, PC_PORT_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--headless") == 0) {
            pc_config.headless = true;
        } else if (strcmp(argv[i], "--screenshots") == 0 && i + 1 < argc) {
            pc_config.screenshot_dir = argv[++i];
        } else if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) {
            pc_config.save_dir = argv[++i];
        } else if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
            pc_dvd_add_mod(argv[++i]);
            mods_given = true;
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
    if (!mods_given) {
        load_enabled_mods();
    }
    pc_dvd_init(pc_config.iso);
    if (extract_dir != NULL) {
        pc_dvd_extract(extract_dir);
        return 0;
    }
    if (!pc_config.headless) {
        if (pc_config.keymap != NULL) {
            pc_pad_load_keymap(pc_config.keymap);
        }
        if (!pc_config.fast) {
            pc_config.realtime = true; /* a window runs at the game's speed */
        }
        if (!pc_window_open(640, 480, "Super Smash Bros. Melee (" PC_PORT_NAME " " PC_PORT_VERSION ")")) {
            fprintf(stderr, "[pc] continuing without rendering\n");
        }
    }
    pc_gx_render_init();
    return melee_main();
}
