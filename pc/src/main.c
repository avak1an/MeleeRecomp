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
            "usage: melee [--frames N] [--quiet-stubs]\n"
            "  --frames N      exit after N video frames (default 0 = run until the\n"
            "                  game needs the disc)\n"
            "  --quiet-stubs   do not log the first call of each SDK stub\n");
}

int main(int argc, char** argv)
{
    int i;
    pc_config.max_frames = 0;
    pc_config.log_stubs = true;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            pc_config.max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--quiet-stubs") == 0) {
            pc_config.log_stubs = false;
        } else {
            usage();
            return 2;
        }
    }

    pc_runtime_init();
    return melee_main();
}
