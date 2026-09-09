# Melee PC port

A native PC build of the decompiled game. This directory holds everything
that is specific to the PC target; the game and engine sources in `src/`
are shared with the matching GameCube build and stay byte-for-byte
compatible with it.

## Status

**Milestone 1 (build scaffold) — done.** All 907 game units, the 76 HSD
engine units, and three pure-C SDK units compile and link into
`build/pc/melee.exe`.

**Milestone 2 (boot) — done.** The game reads its files straight from a
disc image, the HSD engine parses and byte-swaps archives, and a headless
run with `--autoplay` boots through the memory-card prompt, the opening
movie, the title screen and the main menu into the character-select
screen. Controllers work through XInput or the keyboard.

**Milestone 3 (rendering) — menus done.** A GX-on-OpenGL layer draws the
memory-card prompt, the title screen, the main menu and the
character-select screen in a window, with correct text, textures, models
and layout. See "Renderer" below for what it covers and what it does not
yet.

Planned milestones:

3b. **In-game rendering.** Stages and fighters (skinning, shape animation,
   fog, EFB effects), plus the remaining game-side data swaps those need.
4. **Audio, saves, polish.** AX mixing, THP video decoding, memory-card
   files on disk, window and input options.

## Building (Windows)

Requirements: Visual Studio 2022 or newer with the C++ workload and the
"C++ Clang tools for Windows" component (the build uses VS's bundled CMake
and Ninja if none are on `PATH`), and a completed GameCube build
(`configure.py` + `ninja` at the repository root) because the font tables
in `build/GALE01/include` are extracted from the original DOL.

```
pc\build.cmd          configure and build with clang-cl  -> build\pc
pc\build.cmd run      build, then run the game
pc\build.cmd msvc     build with cl.exe instead          -> build\pc-msvc
pc\build.cmd clean    remove the build directory
```

clang-cl is the primary toolchain: it honours the alignment attributes the
engine asserts on, accepts the GNU idioms the decomp uses, and matches the
repository's own clang lint. The cl.exe build still links and boots but
trips DMA-alignment asserts sooner or later.

## Running

```
build\pc\melee.exe [--iso PATH] [--frames N] [--realtime] [--autoplay] [--quiet-stubs]
```

- `--iso PATH`: the NTSC 1.02 disc image (`GALE01`). Without it the runtime
  reads `$MELEE_ISO`, then looks for `GALE01.iso` in the current and parent
  directory. Only the game's own files are read; nothing is extracted or
  written.
- `--frames N`: stop after N video frames (status 0).
- `--realtime`: pace the loop to 60 Hz (default: as fast as possible).
- `--autoplay`: tap Start and A on port 1 every 150 frames, which pushes a
  headless run through prompts and menus.
- `--quiet-stubs`: don't log the first call of each SDK stub.
- `--headless`: no window; run the game logic only (what the regression
  runs use).
- `--screenshots DIR`: save a BMP of every 60th frame into DIR, with the
  draw and vertex counts of that frame on stderr.
- `--extract DIR`: write every file of the disc to `DIR/files`, in the
  disc's own folder layout, and the boot header, FST, apploader and
  `main.dol` to `DIR/sys` (the same layout the decomp's `orig/` uses), then
  exit. This is the starting point for modding: a later milestone adds a
  loose-file override so files in such a folder take precedence over the
  image.

The window is 640x480 and can be resized (the frame is scaled); Escape or
closing it ends the run. Rendering is as fast as the machine allows
unless `--realtime` is given, which also enables vsync.

## Renderer

`pc/src/gx_render.c` implements the GX API on OpenGL 2.x. Geometry
arrives two ways and is decoded by the same code: immediate mode (the
`GXPosition3f32`-style inline functions in `GXVert.h` write to the
recorder on PC) and display lists from disc (the same command format,
big-endian). Vertices are decoded with the current vertex descriptor,
attribute formats and index arrays, then transformed on the CPU as the
console's XF unit would: position/normal matrices from the matrix memory,
per-vertex lighting from the channel controls, texture-coordinate
generation. The fragment side is a GLSL shader generated from the TEV
stage configuration (all inputs, compare ops, bias/scale, swap tables,
konstants, alpha compare). Textures are decoded from the console formats
(I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR and the C4/C8/C14X2 palette
formats) into a cache keyed by image pointer. EFB-to-texture copies use a
framebuffer copy. GX clip-space depth [-w, 0] is remapped to GL's [-w, w]
in the vertex shader; viewport and scissor are flipped from the console's
top-left origin.

Not done yet: fog, indirect texturing, Z textures, destination alpha,
mipmaps and LOD bias, GX line/point texture offsets, dithering. These
matter in-game more than in the menus.

Debugging: `MELEE_GX_DEBUG=1` logs the first draws of the run (vertex
descriptor, first vertex in view and clip space, TEV/texture state, the
current position matrix) and the joint display entries; `MELEE_GX_FLAT=1`
replaces every fragment with magenta and disables the alpha test, which
separates geometry problems from shading problems.

Exit status: 0 frame limit, 3 game assertion/`OSPanic`, 4 spin on an
unimplemented SDK function (a stub called two million times), 5 unsupported
disc request, 6 runtime error (disc/ARAM), 7 hardware exception, 9 stack
smash detected by the tracer. Every abnormal exit prints a symbolized
backtrace, `[pc] scene:` lines mark game-mode transitions, and the stub
call counts at the end are the to-do list.

Controllers: XInput devices map to ports 1-4. With no gamepad, the keyboard
drives port 1 (arrows = stick, IJKL = C stick, Z/X/C/V = A/B/X/Y, Q/E =
L/R, Space = Z, Enter = Start, numpad 8/2/4/6 = D-pad).

### Debugging aids

- `MELEE_TRACE_CARD=1` logs the memory-card command queue.
- A build configured with `-DMELEE_TRACE_FUNCS=ON` (clang only) records every
  function entry in a ring buffer and prints the last 96 on a crash, and
  verifies each function's return address as it returns, naming the frame
  a stack overrun smashed. It is slow; use a separate build directory:

  ```
  cmake -S pc -B build\pc-trace -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_C_FLAGS=-m32 -DMELEE_TRACE_FUNCS=ON
  ```

## Layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | PC project. Globs the game sources; owns all PC-specific flags. |
| `build.cmd` | Toolchain selection, 32-bit VS environment, CMake + Ninja. |
| `include/pc_platform.h` | Force-included into every unit: compiler shims, PowerPC intrinsics, math renames. |
| `include/pc_endian.h`, `include/pc_hsd_swap.h` | Byte-swap helpers and the descriptor swap API used from `#ifdef TARGET_PC` blocks. |
| `include/stdbool.h`, `include/printf.h` | Shadow host headers so the game sees the MSL definitions it was written against (`bool` is `int`). |
| `src/main.c` | Process entry point and options; calls the game's `main()` (compiled as `melee_main`). |
| `src/runtime.c` | OS core: 24 MB arena at the console's address, `OSReport`/`OSPanic`, timers, alarms, the completion pump, crash handler and backtraces. |
| `src/vi.c` | Frame boundary: retrace callbacks, pacing, frame limit. |
| `src/dvd.c` | Disc image access with the SDK's FST lookup; reads complete from the pump. |
| `src/aram.c` | 16 MB auxiliary RAM and its DMA request queue. |
| `src/gx.c` | Fifo object, draw-done notification, texture/palette objects, texture buffer sizes. |
| `src/gx_render.c` | The GX-on-OpenGL renderer: state, vertex decoding, transform and lighting, texture decoding, TEV shader generation, frame copies. |
| `src/gl_window.c`, `src/pc_gl.h` | Win32 window, OpenGL context and entry-point loader. |
| `src/pc_gx.h` | Shared texture/palette object layout and the immediate-mode write interface. |
| `src/pad.c` | XInput and keyboard controllers, autoplay. |
| `src/card.c` | Memory card: reports "no card". |
| `src/hsd_swap.c` | Byte-swapping of HSD descriptors (see below). |
| `src/mtx.c`, `src/sdk_extra.c` | Matrix library; render-mode tables, voice pool, thread stub, Metrowerks runtime helpers. |
| `src/trace.c` | Function-entry ring buffer and return-address check (`MELEE_TRACE_FUNCS`). |
| `generated/stubs.c` | No-op SDK stubs, produced by `tools/gen_stubs.py`. |
| `tools/gen_stubs.py` | Reads the link log, looks unresolved names up in the SDK headers, emits stubs. |

## Design decisions

**32-bit build, main memory at the console's address.** HSD archive files
store 32-bit offsets that the engine relocates in place into pointers, so
`sizeof(void*)` must stay 4. The game also tells main memory from ARAM by
address (main memory is 0x80000000 and up), so the 24 MB arena is mapped
exactly there; the executable is linked `/LARGEADDRESSAWARE` to make that
range available to a 32-bit process.

**Completions come from a pump, not interrupts.** Disc reads, ARAM
transfers, GX draw-done and alarms complete inside `pc_pump()`, which every
wait point calls (`VIWaitForRetrace`, `DVDGetDriveStatus`, ...). Reads are
performed synchronously but their callbacks are queued, so callback chains
never recurse.

**Endianness.** Files are big-endian. `HSD_ArchiveParse` (archive.c,
`TARGET_PC` block) swaps the archive header, its tables, and every pointer
slot the relocation table names. Scalar fields of the descriptors those
pointers lead to are swapped on entry to the engine loader that consumes
them (`pc/src/hsd_swap.c`, hooked into `JObjLoad`, `CObjLoad`, `TObjLoad`,
... and the `AddAnim` functions). Each descriptor is swapped once,
tracked by address; a freshly parsed archive clears the records inside its
memory. Only descriptors inside the emulated main memory are touched, so
static descriptors compiled into the game are left alone. Raw GX payloads
(vertex arrays, display lists, textures, palettes) stay in console order
for the renderer to decode. Non-HSD formats swapped so far: the `.ssm`
sound-bank header and sample table, the `.sem` sound-macro tables, the THP
movie header and frame headers.

**Stubs are generated, not written.** Every SDK function the game links
against but the runtime does not implement is a generated no-op that logs
its first call. After adding runtime code, regenerate with the names to
drop:

```
python pc\tools\gen_stubs.py --remove NameA NameB
```

## Changes to shared sources

All guarded by `TARGET_PC` or token-identical on GameCube:

- `Runtime/platform.h`: `STATIC_CONST` macro for the ~600 `static T const`
  motion-flag constants (MSVC cannot fold them; they become enumerators).
- `Runtime/Gecko_setjmp.h`: the Metrowerks `__jmp_buf` wraps the host
  `jmp_buf`.
- `placeholder.h`, `dolphin/types.h`, `dolphin/os.h`, `dolphin/gx/GXVert.h`:
  intrinsic guards, alignment attribute guard, bus-clock globals, the
  write-gather pipe as a variable.
- Engine loaders (`jobj.c`, `cobj.c`, `wobj.c`, `lobj.c`, `fog.c`, `aobj.c`,
  `fobj.c`, `mobj.c`, `tobj.c`, `pobj.c`, `robj.c`): one-line swap hooks.
- `archive.c`: endian pre-pass. `synth.c`, `axdriver.c`, `lbmthp.c`: file
  header swaps.
- Decomp quirks that only work on the console, each fixed under `TARGET_PC`:
  - `hsd_3A94.c`/`hsd_4D11.c`: two globals used as one contiguous buffer
    (`hsd_804D1138`/`hsd_804D1148`) are one block on PC.
  - `mnmain.c`: a matching trick writes 0x14 bytes past a 12-byte local,
    which on x86 hits the return address.
  - `lbcardnew.c`: the card work area is zeroed (the code reads it before
    writing it and expects fresh memory).
  - `devcom.c`: DMA alignment asserts on source/destination are skipped.
  - `debug.c`: `HSD_Panic` goes straight to `OSPanic` (the crash screen
    needs a renderer); the MSL `FILE` hook is compiled out.
  - `efalt.c`, `hsd_3B2B.c`, `ground.c`, `grheal.c`, `lbfile.c`,
    `ftyoshispecialn.c`: `__va_arg`, variable-length arrays,
    const-from-const globals.
- `gm_1A3F.c`: scene-transition log line. `jobj.c`: joint display log
  (`MELEE_GX_DEBUG`).
- `dolphin/gx/GXVert.h`: on PC the vertex inline functions write to the
  renderer; direct pipe writes in `gm_1832.c`, `hsd_3915.c`, `psdisp.c`
  go through `GX_FIFO_F32`/`GX_FIFO_U8` (token-identical on GameCube).
- `hsd_3A76.c`: SIS text streams are big-endian byte streams both on disc
  and as built at runtime, so their 16-bit token reads go through
  `SIS_U16`/`SIS_S16`; pointer tokens are stored in host order on PC.
- `wobj.c`, `fog.c`, `jobj.c`: the `*Init` and recursive helpers that copy
  descriptor fields without going through the hooked loaders got the same
  swap hooks.
- `mncharsel.c` + `Runtime/platform.h`: `PC_ADJACENT(k)` places the six
  statics that `CSS_ALL` views as one block into a linker-ordered section;
  the scene entry verifies the layout.

## Known gaps (deliberate, for later milestones)

- Game-specific data structures loaded from disc (character select, stages,
  fighters, items, menus) are not swapped yet; the first one is what stops
  the current autoplay run.
- Four more "index past a global" idioms exist (`gm_19EF.c`, `soundtest.c`)
  that assume console link order.
- Bit-fields in structs mirroring disc data are laid out LSB-first.
- `char` signedness and paired-single float rounding are not matched.
- THP video, AX audio mixing, memory-card saves and threads are stubs.
