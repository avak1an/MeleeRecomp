# Melee PC port

A native PC build of the decompiled game. This directory holds everything
that is specific to the PC target; the game and engine sources in `src/`
are shared with the matching GameCube build and stay byte-for-byte
compatible with it.

## Status

**Milestone 1 (build scaffold) — done.** All 907 game units, the 76 HSD
engine units, and three pure-C SDK units compile with MSVC and link into
`build/pc/melee.exe`. The Dolphin SDK (graphics, audio, disc, controllers,
memory cards, OS) is replaced by a small hand-written runtime plus generated
no-op stubs. The binary runs the game's real `main()`, gets through
hardware and engine initialisation, and stops at the first disc read.

Planned milestones:

2. **Boot to first frame.** Read assets from an extracted disc directory,
   byte-swap loaded structures, controllers via SDL, real alarms/threads.
3. **Rendering.** A GX emulation layer on OpenGL; menus first, then
   in-game.
4. **Audio, saves, polish.** AX mixing, memory-card files on disk, window
   and input options.

## Building (Windows)

Requirements: Visual Studio 2022 or newer with the C++ workload (the build
uses its bundled CMake and Ninja if none are on `PATH`), and a completed
GameCube build (`configure.py` + `ninja` at the repository root) because the
font tables in `build/GALE01/include` are extracted from the original DOL.

```
pc\build.cmd          configure and build
pc\build.cmd run      build, then run for one frame
pc\build.cmd clean    remove build\pc
```

Run options:

```
build\pc\melee.exe [--frames N] [--quiet-stubs]
```

By default the run ends when the game first asks to read from the disc,
with a backtrace of the requesting code and exit status 5. `--frames N`
ends it earlier, after N video retraces (status 0). A stub that is called
two million times in one run is treated as a spin on unimplemented hardware
and ends the run with status 4 and a backtrace. On exit the runtime prints
every SDK stub the game called and how often, which is the to-do list for
the next milestone.

A run today gets through OS, video, GX, audio-system, controller and heap
initialisation, the HSD engine setup, and stops in the sound-bank loader
when it waits for the first disc read.

## Layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | PC project. Globs the game sources; owns all PC-specific flags. |
| `build.cmd` | Sets up the 32-bit MSVC environment and drives CMake + Ninja. |
| `include/pc_platform.h` | Force-included into every unit: compiler shims, PowerPC intrinsics, math renames. |
| `include/stdbool.h`, `include/printf.h` | Shadow host headers so the game sees the MSL definitions it was written against (`bool` is `int`). |
| `src/main.c` | Process entry point; calls the game's `main()` (compiled as `melee_main`). |
| `src/runtime.c` | OS core: 24 MB arena, `OSReport`/`OSPanic`, timers, interrupt no-ops, frame-limited exit in `VIWaitForRetrace`. |
| `src/mtx.c` | C versions of the paired-single matrix/vector routines. |
| `src/dvd.c`, `src/card.c` | Disc and memory card: fail fast / report "no card" until later milestones. |
| `src/sdk_extra.c` | Render-mode tables, audio hooks, thread creation, Metrowerks runtime helpers. |
| `src/gx_pipe.c` | The write-gather pipe (`GXWGFifo`) as a plain variable; writes are discarded. |
| `generated/stubs.c` | No-op SDK stubs, produced by `tools/gen_stubs.py`. |
| `tools/gen_stubs.py` | Reads the link log, looks unresolved names up in the SDK headers, emits stubs. |

## Design decisions

**32-bit build.** HSD archive files store 32-bit offsets that the engine
relocates in place into pointers. Keeping `sizeof(void*) == 4` means the
loaded data keeps its GameCube layout, which avoids rewriting the archive
loader and every struct that mirrors on-disc data. The CMake file refuses
64-bit configurations.

**Host CRT, MSL semantics.** The game's `<math.h>`, `<string.h>` and so on
resolve to the MSVC C runtime; `src/MSL` is not on the include path. Where
MSL differs in a way the game depends on, a header in `include/` shadows
the host one (`bool` as `int`). The game's own trig and exp/pow
implementations are kept and reached through renames in `pc_platform.h`,
so results match the console.

**Stubs are generated, not written.** Every SDK function the game links
against but the runtime does not implement is a generated no-op that logs
its first call. To refresh after adding or removing runtime code:

```
pc\build.cmd > link.log
python pc\tools\gen_stubs.py --link-log link.log
```

Symbols the generator cannot find in the SDK headers (data objects,
function-pointer parameters, Metrowerks runtime helpers) are listed for
hand implementation in `src/sdk_extra.c`.

## Changes to shared sources

The PC build needed a few source-level changes. All are either guarded by
`TARGET_PC` or expand to the identical tokens in the GameCube build, so the
matching build is unaffected:

- `Runtime/platform.h`: new `STATIC_CONST(type, name, ...)` macro. MSVC's
  C front end does not fold `static T const` initialisers, so the
  motion-flag constants (about 600 across the fighter headers) use this
  macro; on PC they become enumerators.
- `Runtime/Gecko_setjmp.h`: on PC the Metrowerks `__jmp_buf` wraps the host
  `jmp_buf`; `__setjmp` is a macro, `longjmp` routes to `pc_longjmp`.
- `placeholder.h`: intrinsic fallbacks are `#ifndef`-guarded so the PC
  build can supply real ones (`__frsqrte` is 1/sqrt, not sqrt).
- `dolphin/types.h`, `dolphin/os.h`, `dolphin/gx/GXVert.h`: alignment
  attribute guard, bus-clock globals, and the write-gather pipe as an
  `extern` variable on PC.
- Eleven small `#ifdef TARGET_PC` blocks: variable-length arrays,
  const-from-const globals, the Metrowerks `__va_arg` in `efalt.c`, the
  MSL `FILE` internals in `debug.c`.

## Known gaps (deliberate, for later milestones)

- Big-endian data: nothing loaded from disc is byte-swapped yet.
- Bit-fields in structs mirroring disc data are laid out LSB-first by MSVC.
- `char` signedness and paired-single float rounding are not matched.
- `ATTRIBUTE_ALIGN` is compiled out; DMA alignment does not matter on PC.
- Only MSVC is supported. Clang would remove most of the source patches
  above (the repository's lint pass already uses clang), so adding
  clang-cl as a second toolchain is worth doing early in milestone 2.
