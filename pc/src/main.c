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
            "  --log FILE      write everything the game prints to FILE instead of the\n"
            "                  console (the launcher uses melee.log next to melee.exe)\n"
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
            "  --screenshot-every N  with --screenshots: every Nth frame instead\n"
            "  --screenshot-from N   with --screenshots: start at frame N\n"
            "  --stage N       play every VS and Classic match on stage N (StKind, see\n"
            "                  src/melee/gr/forward.h: 2 Fountain ... 11 Rainbow Cruise ... 31 Battlefield)\n"
            "  --item KIND@FRAME[:P]  spawn item KIND (number, see src/melee/it/forward.h)\n"
            "                  next to player P (default 1) at FRAME\n"
            "  --char N        port 1 plays character N (CharacterKind, see src/melee/ft/forward.h:\n"
            "                  0 Falcon 1 DK 2 Fox 3 G&W 4 Kirby 5 Bowser 6 Link 7 Luigi 8 Mario\n"
            "                  9 Marth 10 Mewtwo 11 Ness 12 Peach 13 Pikachu 14 ICs 15 Puff 16 Samus\n"
            "                  17 Yoshi 18 Zelda 19 Sheik 20 Falco 21 YLink 22 Dr.M 23 Roy 24 Pichu 25 Ganon)\n"
            "  --kill SLOTS@FRAME[/N]  KO the fighters of player SLOTS (e.g. 1 or 1,2,3) at\n"
            "                  FRAME and every N (300) frames after it, to reach results\n"
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
    pc_config.screenshot_every = 60;
    pc_config.item_kind = -1;
    pc_config.p1_char = -1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            pc_config.max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            /* everything the game prints goes to this file (the launcher
             * passes melee.log next to the game) */
            const char* path = argv[++i];
            if (freopen(path, "w", stderr) == NULL) {
                fprintf(stdout, "[pc] cannot write the log file %s\n", path);
            } else {
                setvbuf(stderr, NULL, _IOLBF, 4096);
                freopen(path, "a", stdout);
                setvbuf(stdout, NULL, _IOLBF, 4096);
                fprintf(stderr, "[pc] %s %s log\n", PC_PORT_NAME, PC_PORT_VERSION);
            }
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
        } else if (strcmp(argv[i], "--kill") == 0 && i + 1 < argc) {
            /* SLOTS@FRAME/N: KO those slots' fighters at FRAME and every N
             * frames after it (debugging aid to reach match results) */
            const char* spec = argv[++i];
            const char* at = strchr(spec, '@');
            const char* every = strchr(spec, '/');
            const char* p = spec;
            pc_config.kill_slots = 0;
            while (*p >= '0' && *p <= '9') {
                pc_config.kill_slots |= 1 << atoi(p);
                while (*p >= '0' && *p <= '9') {
                    p++;
                }
                if (*p == ',') {
                    p++;
                }
            }
            pc_config.kill_frame = at != NULL ? atoi(at + 1) : 0;
            pc_config.kill_every = every != NULL ? atoi(every + 1) : 300;
            if (pc_config.kill_every <= 0) {
                pc_config.kill_every = 300;
            }
        } else if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            pc_config.stage = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--item") == 0 && i + 1 < argc) {
            /* KIND@FRAME: spawn item KIND (see it/forward.h) next to player 1 */
            const char* spec = argv[++i];
            const char* at = strchr(spec, '@');
            const char* colon = at != NULL ? strchr(at, ':') : NULL;
            pc_config.item_kind = atoi(spec);
            pc_config.item_frame = at != NULL ? atoi(at + 1) : 0;
            pc_config.item_slot = colon != NULL ? atoi(colon + 1) - 1 : 0;
        } else if (strcmp(argv[i], "--char") == 0 && i + 1 < argc) {
            pc_config.p1_char = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-every") == 0 && i + 1 < argc) {
            pc_config.screenshot_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-from") == 0 && i + 1 < argc) {
            pc_config.screenshot_from = atoi(argv[++i]);
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
