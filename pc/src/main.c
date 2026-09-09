/**
 * @file main.c
 * PC entry point. Owns process setup, then hands control to the game's own
 * main() (compiled as melee_main, see pc/CMakeLists.txt).
 *
 * The game never returns from its main loop; the PC runtime exits from
 * VIWaitForRetrace() once the requested number of frames has elapsed, or
 * from OSPanic()/assertion failures.
 */
#include "pc_runtime.h"

#include <stdio.h>
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
            "  --quiet-stubs   do not log the first call of each SDK stub\n"
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
        } else if (strcmp(argv[i], "--quiet-stubs") == 0) {
            pc_config.log_stubs = false;
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
    return melee_main();
}
