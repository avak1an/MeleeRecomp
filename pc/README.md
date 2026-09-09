# Melee PC port

A native PC build of the decompiled game. This directory holds everything
that is specific to the PC target; the game and engine sources in `src/`
are shared with the matching GameCube build and stay byte-for-byte
compatible with it.

## Status

Version: **v1.0-beta** (`pc/include/pc_version.h`, shown by the launcher,
the game window title and `melee.exe --version`).

**Milestone 1 (build scaffold) - done.** All 907 game units, the 76 HSD
engine units, and three pure-C SDK units compile and link into
`build/pc/melee.exe`.

**Milestone 2 (boot) - done.** The game reads its files straight from a
disc image, the HSD engine parses and byte-swaps archives, and a headless
run with `--autoplay` boots through the memory-card prompt, the opening
movie, the title screen and the main menu into the character-select
screen. Controllers work through XInput or the keyboard.

**Milestone 3 (rendering) - menus done.** A GX-on-OpenGL layer draws the
memory-card prompt, the title screen, the main menu and the
character-select screen in a window, with correct text, textures, models
and layout. See "Renderer" below for what it covers and what it does not
yet.

**Milestone 3b (in-game rendering) - matches render.** The title
screen's attract demo (reached by the scripted route in
`pc/scripts/title-demo.txt`) plays complete four-player CPU matches: stages
(Great Bay, Corneria, Brinstar, Peach's Castle, Yoshi's Story, ... whatever
the demo picks), skinned and animated fighters, items, stage hazards, the
camera and the HUD all render; the demo returns to the title and starts the
next match. This needed the game-side data swaps for stages, fighters,
items and animation scripts (see "Changes to shared sources"), plus a
deterministic clock so headless runs repeat exactly. Fog, indirect
texturing, mipmaps and destination alpha are still missing from the
renderer, and a few in-match effects are only contained rather than fixed
(see "Known gaps").

**Milestone 4 (audio, video, saves, polish) - done.** A software AX
mixer plays the game's sound effects and streamed music through the
Windows audio device; THP movies (the Nintendo/HAL logo, the opening, the
how-to-play and ending videos) decode on the CPU into the game's YUV
textures; the memory card is a directory of files next to the executable,
so progress persists between runs; the window can go full screen or start
at an integer scale, the keyboard layout is remappable and the volume is
adjustable. See "Running" for the options.

**Milestone 5 (mods, launcher, coverage) - done.** Files in a mod folder
replace the disc's (`mods\<name>\files\...`, the layout `--extract`
writes); `melee-launcher.exe`, a plain Windows program built next to the
game, holds every setting, extracts the disc and manages mods; the
renderer gained fog and mipmapping; the remaining stage parameter blocks
are swapped and the demo route was pushed through more stages, which
found and fixed two compiler-layout differences (bit-field packing) and
one more adjacent-globals case (Mute City).

Planned milestones:

6. **Play-through coverage.** Scripted routes through the modes the
   attract demo never shows (1-P modes, Target Test, Home-Run Contest,
   the trophy scenes), indirect texturing and destination alpha in the
   renderer, save files convertible to and from real memory-card dumps.

## Building (Windows)

Requirements: Visual Studio 2022 or newer with the C++ workload and the
"C++ Clang tools for Windows" component (the build uses VS's bundled CMake
and Ninja if none are on `PATH`), and the game's disc image. Two sources
include byte tables of the font atlases that live in the original
`main.dol`; `pc\build.cmd` finds the image (`MELEE_ISO`, or `GALE01.iso`
in the repository, its parent directory, or `build\pc`) and a small host
tool, `pc/tools/extract_fonts.c`, pulls the tables out of it during the
build (byte-identical to what the GameCube build's `configure.py` +
`ninja` would extract, which remains an alternative). The launcher's
"Build the game" button runs the same script.

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
build\pc\melee.exe [--iso PATH] [--fullscreen] [--scale N] [--keymap FILE] [--volume N]
                    [--no-audio] [--saves DIR] [--fast] [--frames N] [--autoplay] [--input FILE]
                    [--seed N] [--quiet-stubs] [--headless] [--screenshots DIR]
```

Run from anywhere; with the disc image next to `melee.exe` (or in the
current or parent directory) no option is needed. `melee-launcher.exe`
next to it offers the same options in a window (see "Launcher").

A typical check that a change did not break the match:

```
build\pc\melee.exe --headless --quiet-stubs --input pc\scripts\title-demo.txt --seed 3 --frames 3000
```

Status 0 means two demo matches played out; a crash prints a symbolized
backtrace and a hang is reported by the watchdog (status 8).

- `--iso PATH`: the NTSC 1.02 disc image (`GALE01`). Without it the runtime
  reads `$MELEE_ISO`, then looks for `GALE01.iso` in the current and parent
  directory. Only the game's own files are read; nothing is extracted or
  written.
- `--frames N`: stop after N video frames (status 0).
- `--fast`: do not hold the game to 60 frames per second. Windowed runs
  are paced by default (`--realtime` is accepted and means the same);
  headless runs are never paced. Audio is mixed either way, but only
  sounds right when paced.
- `--fullscreen`: start in a borderless window covering the monitor. F11
  or Alt+Enter toggles at any time.
- `--scale N`: start with an N x 640x480 window (1-8).
- `--keymap FILE`: keyboard layout for port 1, see "Controllers" below.
- `--volume N`: audio volume in percent (default 100). `--no-audio` (or
  `MELEE_NO_AUDIO=1`) does not open the audio device at all.
- `--saves DIR`: where the memory-card files live (default: a `saves`
  directory next to `melee.exe`, created on the first save).
- `--mod DIR`: use the files under `DIR` (or `DIR\files`) instead of the
  disc's; repeatable, the first given wins. Without it the mods listed in
  `mods\enabled.txt` next to the executable are used (see "Mods").
- `--autoplay`: tap Start and A on port 1 every 150 frames, which pushes a
  headless run through prompts and menus.
- `--input FILE`: scripted controller input for port 1. Each line is
  `<first frame> <last frame> <buttons|-> [stick x y]`, buttons joined by
  `+` (`A B X Y Z L R START UP DOWN LEFT RIGHT`). `pc/scripts/title-demo.txt`
  skips the memory-card prompt and lets the title screen start its attract
  demo, which reaches a full match at frame 925: the standard in-game
  regression route. Scripted and headless runs ignore the host keyboard.
- `--seed N`: seed the game's random generator (the clock otherwise). With a
  script, the same seed replays the same match, stage and all.
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

The window starts at 640x480 (or `--scale` times that) and can be resized
or made full screen: the frame keeps its 4:3 shape, centred with black
bars. Escape or closing the window ends the run. The game runs at 60
frames per second: on a 60, 120, 180 or 240 Hz display the swap chain is
synchronized to every first, second, third or fourth refresh, on other
rates a 1 ms timer paces the loop (the startup log's `display:` line says
which). `--fast` removes the limit.

Audio: a 32 kHz stereo mix of every voice the game's sound engine starts
(sound effects from ARAM, music streamed from the disc), 5 ms at a time,
played through waveOut. Sound is generated in unpaced and headless runs
too, so `MELEE_AUDIO_DUMP=out.wav` records what a scripted run would have
played.

Mods: a mod is a folder holding the files it replaces in the disc's own
layout, `MyMod\files\GrCn.dat`, `MyMod\files\audio\us\...` and so on.
Every disc file that exists in the folder is read from there, with the
folder's file size; files the disc does not have cannot be added, because
the game finds files through the disc's table. Replacement archives must
be well-formed (the game checks an archive's size against its header).
`mods\enabled.txt` next to `melee.exe` lists the active mods, highest
priority first, one folder name (or full path) per line; the launcher
maintains it. `MELEE_TRACE_DVD=1` logs every file open and read with its
source. To make a mod, extract the disc (`--extract DIR` or the launcher's
button), copy the files you want to change into a new folder under
`mods\`, edit them, and enable the folder.

Launcher: `melee-launcher.exe` is built alongside the game and needs
nothing else; each build also copies it to `pc/dist`, where it is
committed so a fresh clone can start from the launcher (it links the C
runtime statically and contains nothing from the game; see
`pc/dist/README.md`). It picks the disc image (checks it is GALE01, and on
request hashes it: the whole image against the SHA-1 of the v1.02 disc
the decompilation targets, `d4e70c06...`, and its `main.dol` against the
README's `08e0bf20...`, so a differently dumped or modified image is told
apart from a wrong revision), window
size or full screen, volume and mute, the keyboard layout file (with a
button that writes the default layout and opens it for editing), the
saves folder, the mod list (checkboxes for enabled, buttons for priority),
extracts the disc's files, and starts the game with the matching options.
Settings persist in `launcher.ini` next to it; an "extra options" box
passes anything else through. Its "Build from source" section points at a
checkout of this repository (the launcher's own, two levels up from
`build\pc`, by default), reports whether Visual Studio with the C++
workload is installed, and builds the game from the chosen disc image in a
console window; afterwards it runs the `melee.exe` it built, so the
launcher alone plus a checkout and a disc is a complete setup.

Saves: slot A is a virtual 64 Mbit memory card whose files are
`<name>.sav` in the saves directory: a 96-byte header (the directory
entry: name, size, timestamp, icon and comment locations) followed by the
raw data the game wrote. The data is the game's own in-memory layout, so
the files are not interchangeable with real memory-card dumps, which are
big-endian. Slot B is always empty.

## Renderer

`pc/src/gx_render.c` implements the GX API on OpenGL 2.x. Geometry
arrives two ways and is decoded by the same code: immediate mode (the
`GXPosition3f32`-style inline functions in `GXVert.h` write to the
recorder on PC) and display lists from disc (the same command format,
big-endian). Vertices are decoded with the current vertex descriptor,
attribute formats and index arrays, then transformed on the CPU as the
console's XF unit would: position/normal matrices from the matrix memory,
per-vertex lighting from the channel controls, texture-coordinate
generation (source, texture matrix and the post-transform matrix HSD
uses for every texture's own translate/scale/rotate). The fragment side is a GLSL shader generated from the TEV
stage configuration (all inputs, compare ops, bias/scale, swap tables,
konstants, alpha compare). Textures are decoded from the console formats
(I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR and the C4/C8/C14X2 palette
formats) into a cache keyed by image pointer and palette contents, which
disc reads, ARAM transfers and cache flushes invalidate. EFB-to-texture
copies use a framebuffer copy. GX clip-space depth [-w, 0] is remapped to GL's [-w, w]
in the vertex shader; viewport and scissor are flipped from the console's
top-left origin.

Fog is applied from eye distance with the console's four curve types;
mip levels are generated from level 0 for textures that ask for them.
Not done yet: indirect texturing, Z textures, destination alpha, LOD
bias, GX line/point texture offsets, dithering.

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
L/R, Space = Z, Enter = Start, numpad 8/2/4/6 = D-pad). `--keymap FILE`
changes that layout; each line is `ACTION = KEY`, for example:

```
# actions: A B X Y Z L R START DPAD_UP/DOWN/LEFT/RIGHT
#          STICK_UP/DOWN/LEFT/RIGHT C_UP/DOWN/LEFT/RIGHT
A = K
B = J
STICK_UP = W
START = ENTER
```

Keys are letters, digits, `F1`-`F24`, or names: `ENTER SPACE TAB BACKSPACE
SHIFT LSHIFT RSHIFT CTRL LCTRL RCTRL ALT UP DOWN LEFT RIGHT INSERT DELETE
HOME END PAGEUP PAGEDOWN NUMPAD0`-`NUMPAD9 NUMPAD+ NUMPAD- NUMPAD* NUMPAD/
NUMPAD. COMMA PERIOD MINUS PLUS SEMICOLON SLASH BACKTICK LBRACKET
BACKSLASH RBRACKET QUOTE`. Actions not mentioned keep their default.

### Debugging aids

- `MELEE_GX_NOCULL=1` / `MELEE_GX_NOALPHA=1` disable face culling / the
  alpha test; `MELEE_GX_LOG_FRAME=N` logs every draw of frame N with its
  state and first vertex (draws issued while a fighter model is displayed
  are tagged, and each screenshot line reports how many).
- `--watch 0xADDR` (or `--watch symbol`, `--watch symbol+0x1c`, resolved
  through the debug symbols) reports every swap helper that touches the word at ADDR
  and every change of it seen at descriptor swaps, GObj processes and
  render callbacks, naming the callback before and after. The binary is
  linked with `/DYNAMICBASE:NO` so static addresses are the same from run
  to run; print an address in one run (`MELEE_GX_DEBUG=1`), watch it in the
  next. `MELEE_WATCH_PTCL=1` watches the stage particle bank automatically.
- `MELEE_WATCHDOG=N` (default 20) prints the main thread's stack and exits
  with status 8 when no frame completes for N seconds: the way to find
  where a run spins.
- `MELEE_TRACE_CARD=1` logs the memory-card command queue.
- `MELEE_AUDIO_DUMP=FILE` writes the mixed audio of the run as a 32 kHz
  stereo WAV; `MELEE_THP_DUMP=DIR` writes every decoded movie frame's luma
  plane as a PGM image and logs its mean brightness.
- `MELEE_GX_DUMP_TEX=DIR` writes every texture the renderer decodes as a
  PPM (colour) and PGM (alpha) pair named by image address, size and
  format; `MELEE_GX_NOCACHE=1` decodes textures on every use, to tell a
  stale cached upload from a decoding problem. The draw log names each
  draw's texture format, size and palette.
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
| `src/dvd.c` | Disc image access with the SDK's FST lookup, loose-file overrides for mods, extraction; reads complete from the pump. |
| `src/aram.c` | 16 MB auxiliary RAM and its DMA request queue. |
| `src/gx.c` | Fifo object, draw-done notification, texture/palette objects, texture buffer sizes. |
| `src/gx_render.c` | The GX-on-OpenGL renderer: state, vertex decoding, transform and lighting, texture decoding, TEV shader generation, frame copies. |
| `src/gl_window.c`, `src/pc_gl.h` | Win32 window, OpenGL context and entry-point loader. |
| `src/pc_gx.h` | Shared texture/palette object layout and the immediate-mode write interface. |
| `src/pad.c` | XInput and keyboard controllers, keyboard layout files, autoplay, scripted input. |
| `src/card.c` | Memory card: a directory of save files, with the SDK's asynchronous completion semantics. |
| `src/ax.c` | The AX sound driver: voice pool with priority stealing, ADPCM/PCM decoding from ARAM, sample-rate conversion, volume ramps, waveOut output, the AI interface. |
| `src/thp.c` | THP movie decoder (baseline JPEG without byte stuffing) writing GX-tiled Y/U/V planes. |
| `launcher/launcher.c` | The Win32 launcher: settings window, disc verification, disc extraction, mod list, build from source, Play. |
| `dist/melee-launcher.exe` | The committed launcher build (copied there by every build; `dist/.gitignore` un-ignores it). |
| `tools/extract_fonts.c` | Build-time host tool: the font atlas byte tables from the disc image's `main.dol`. |
| `src/hsd_swap.c` | Byte-swapping of HSD descriptors (see below). |
| `src/mtx.c`, `src/sdk_extra.c` | Matrix library; render-mode tables, thread stub, Metrowerks runtime helpers. |
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
for the renderer to decode. Non-HSD formats swapped: the `.ssm` sound-bank
header and sample table (the AX parameter blocks inside it are 16-bit
fields that the game also reads 32 bits at a time, so they are kept as
32-bit words and their halves exchanged: `pc_rotate32_range`), the `.sem`
sound-macro tables and command streams, `.hps` music stream headers and
block headers, the THP movie header and the size word before each frame.
JPEG data, sample data and movie frames are byte streams and stay as they
are.

**Audio is mixed in software, on the game thread.** The console's DSP
mixes 64 voices every 5 ms; here `VIWaitForRetrace` renders the 5 ms
frames that fall into each video frame (`pc_ax_frame`): the sound engine's
registered callback runs first, then every running voice is decoded from
the emulated ARAM (the same 4-bit ADPCM as the console, or 8/16-bit PCM),
resampled with the voice's 16.16 ratio, scaled by its envelope and left /
right mix and accumulated. Voice parameter blocks are the console's
structures, addressed by the same fields the game writes. The result goes
to waveOut through a few 50 ms buffers; when the device falls behind a
frame is dropped rather than queued.

**THP is a JPEG decoder with the console's output layout.** The SDK's
decoder is PowerPC assembly around the inverse DCT; `thp.c` is a fresh
baseline-JPEG decoder (Huffman tables, 4:2:0, restart intervals, no byte
stuffing in the entropy data, which is how the THP tools write it) that
stores each 8x8 block straight into the 8x4 tiles of the GX I8 textures
the movie player binds. The player's work area holds the decoder state.
Because the planes are rewritten in place, the renderer drops its cached
upload of any texture inside a range the game flushes (`DCStoreRange`,
`DCFlushRange`), reads from the disc or transfers from ARAM, or that the
decoder wrote; palettes are hashed into the cache key because the game
rewrites them in place (player colours).

**The memory card is a directory.** Files are held in memory while the
card is mounted and written through to disk on every create, write, status
change, rename or delete. The SDK completes asynchronous card requests
from interrupts after the call returned, and the game's state machines
depend on that ordering (they set their own "busy" flag after issuing the
request), so completions are queued and delivered from the pump; the
game's card state machine, which spins without reaching a wait point,
pumps on entry.

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
  header, sample table, stream header and command stream swaps.
- `synth.c`: the bank-compaction code stores the new bank end through the
  bank list array at index 0x20, which on the console is the array that
  follows it (`hsd_SynthSFXBank`); on PC the store names that array.
- `hsd_3A94.c`: the card state machine pumps completions on entry (the
  game spins on it while a request is in flight); the command queue
  `hsd_804D2348` is addressed as `hsd_804D1138 + 0x1210`, so it is part of
  the same block.
- Bit-field packing: Metrowerks places plain bytes that follow a group of
  `u32` bit-fields inside the bit-fields' 32-bit unit, MSVC starts a new
  unit. `mn/types.h` (`StartMeleeRules`, 48 bits then bytes) and
  `if/ifstatus.c` (`FlagsX`) declare those bit-fields byte-sized on PC,
  which restores the console offsets; `gm/types.h` packs the tournament
  menu entries on PC as the matching build does. All the settings-struct
  size asserts are active again. `pc/tools/scan_bitfields.py` finds this
  pattern.
- `grmutecity.c`: the car index array, the car array and the word before
  them are one block (`PC_ADJACENT`), because a sort reads one entry
  before the array and another routine views both arrays as one struct.
- `it_26B1.c`: the articles a fighter registers for its own items (PK
  Fire, ...) come from its Pl*.dat rather than the ItCo tables and are
  swapped at registration; `ftData` x54, declared `int`, is a pointer to a
  five-entry effect part table and is swapped as such.
- `grlast.c`: the untyped parameter block is four material indices,
  swapped on load; `grpstadium.c` got a generated swapper once the
  generator learned `u8 r, g, b;` declarations.
- Decomp quirks that only work on the console, each fixed under `TARGET_PC`:
  - `hsd_3A94.c`/`hsd_4D11.c`: three globals used as one contiguous buffer
    (`hsd_804D1138`/`hsd_804D1148`/`hsd_804D2348`) are one block on PC.
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
- Game-side data swaps (`pc/src/game_swap.c`, hooked where each table is
  loaded): stage `map_head`/`grGroundParam`/`coll_data`/`itemdata`, the
  per-stage `yakumono_param` blocks (swappers generated from the struct
  declarations by `pc/tools/gen_struct_swap.py` into
  `pc/generated/yakumono_swap.c`; rerun it after editing a stage struct),
  fighter data (`ftData`, the PlCo common tables, figatrees, wait-anim and
  part tables, per-character attributes at `PUSH_ATTRS`), item articles
  (ItCo tables and stage items; the item-specific attribute blocks have no
  recorded size and are bounded by the next referenced object in the file),
  bone dynamics and stage hazard descriptors.
- Animation command scripts (fighter subactions, item states, colour
  overlays) are streams of bit-packed 32-bit words: they are byte-swapped
  in place by a walker that knows each command's length (the tables the
  interpreters use), following subroutine and goto pointers, and the
  command structs in `lb/types.h` are declared in console bit order on PC.
  Raw half-word/byte reads inside a command word go through
  `CMD_HALF`/`CMD_BYTE`.
- Bit-fields that mirror disc data or are written through a byte/word view
  with console bit numbering are declared in reverse under `TARGET_PC`:
  `StageCallbacks::flags`, `LightOverrideEntry`, `ItemAttr`,
  `UnkFlagStruct`, `Fighter` x594, `ColorOverlay_x8_t`.
- `ASSERT_SIZE`/`ASSERT_OFFSET` are active on PC (they were compiled out
  before); the four settings structs in `gm/types.h` whose PC layout still
  differs are excluded with a `@todo`.
- Console-only idioms fixed under `TARGET_PC`: `granime.c` (a no-argument
  callback that relied on r3 still holding the object), `itspawn.c` (the
  second item pick table addressed as "the memory after the spawner"),
  `ftmaterial.c` (two templates read as the data following the class
  info), `Command_*`/`CmdUnion` (the union must stay 4 bytes: mixed
  bit-field base types are widened to `u32`), `lbarq.c`/`synth.c` (spin
  loops now pump completions), `item.c` (state tables with non-pointer
  words in the material/shape slots).
- `pad.c`: scripted and headless runs ignore the host keyboard and
  controllers; `runtime.c`: a frame-locked clock and calendar in unpaced
  runs, so the same `--seed` replays the same match.

## Known gaps (deliberate, for later milestones)

- Only the data the demo matches and the menus touch has been exercised
  (the demo picks stages at random; sweeping seeds covers most of them).
  The remaining untyped parameter blocks (`void*` in the target-test,
  Home-Run and trophy stages, Temple, Poke Floats) are never dereferenced
  by the decompiled code, so there is nothing to swap yet. Item-specific
  attribute blocks with sub-word fields and mode-specific tables are found
  the same way: run a route, fix the first bad read.
- Particle lists occasionally end up with a corrupt link after item hit
  effects; the walker drops the rest of the list and logs
  `particle list ... is corrupt` instead of crashing. Root cause not found.
- The mixer ignores the auxiliary effect buses (reverb, chorus, delay:
  the `AXFX*` functions are still stubs), interaural delay (`ITD`) and the
  low-pass filter; sample-rate conversion is linear rather than the DSP's
  4-tap filter. Volumes are not calibrated against the console.
- Save files are the PC layout of the game's structures; converting to or
  from real memory-card dumps (`.gci`) would need a byte swap of the game's
  save-data structs.
- Four more "index past a global" idioms exist (`gm_19EF.c`, `soundtest.c`)
  that assume console link order.
- `char` signedness and paired-single float rounding are not matched.
- Threads are stubs (the game creates none that matter on PC).
