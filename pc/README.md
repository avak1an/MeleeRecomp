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

**Milestone 6 (play-through coverage) - done.** Scripted routes
now go through the VS character select (port 1 human, port 2 CPU), the
stage select, a full timed match and its results screen, and through
Classic mode's first seven stages including the stage-clear bonus screen,
Break the Targets and Grab the Trophies (the `--kill` option ends matches
without a player). These found and fixed the
VS crash after Start (a shared vertex-attribute list tail re-swapped by
the stage-select model load), the 1-P crash after a win (a missing return
value and a stack-layout trick in the blur renderer), Onett's animated
textures (the stage code's own animation walk), and the results-screen
camera tables (another struct laid over neighbouring globals).

**Milestone 7 (every mode) - done.** Routes for Adventure, Event Match,
Target Test, Home-Run Contest, Multi-Man Melee and Training
(`pc/scripts/*-play.txt`), a `--stage N` sweep of every VS stage and
`--char`, `--item`, `--kill` to steer them without a player. They found
the tables and bit-fields those modes read (event and Multi-Man
definitions, the Home-Run block, the menu strings), the toon texgen and
the GX indirect-texturing and emboss-bump paths the renderer still lacked,
and the particle-script float order behind the giant flat triangles.

**Milestone 8 (performance and audio) - done.** Vertices are transformed
and lit on the GPU from parameter blocks in a float texture, uploaded
through a persistently mapped ring buffer, draws with identical state are
merged, and the shader and texture caches are hashed; the four-player
results screen went from 20 ms to under 9 ms a frame. The AX mixer gained
the console's effect buses: the SDK's reverb and delay effects, ported
from their PowerPC code, run on the two aux sends Melee uses.

**Milestone 9 (resolution scaling and Dolphin-compatible saves) - done.**
The scene renders into an off-screen framebuffer at the window's 4:3 size
or an `--internal` scale (1x to 8x the console's 640x480) with multisample
anti-aliasing and anisotropic filtering, exposed on the launcher's Display
card. The memory card is a raw image in Dolphin's format with the game's
data in the console's byte order, so saves move between the port,
Dolphin and a console without conversion, and `.gci` files import.

Planned:

- Adventure mode beyond its first stage, All-Star and the ending sequence
  (no routes yet); the open renderer questions under "Known gaps".
- Native peer-to-peer netplay between copies of the port (the game already
  replays deterministically from a seed).

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
                    [--screenshot-every N] [--kill SLOTS@FRAME[/N]]
                    [--internal N] [--msaa N] [--aniso N]
```

Run from anywhere; with the disc image next to `melee.exe` (or in the
current or parent directory) no option is needed. `melee-launcher.exe`
next to it offers the same options in a window (see "Launcher").

A typical check that a change did not break the match:

```
build\pc\melee.exe --headless --quiet-stubs --input pc\scripts\title-demo.txt --seed 3 --frames 3000
```

Status 0 means two demo matches played out; a crash prints a symbolized
backtrace and a hang is reported by the watchdog (status 8). Two more
routes cover what the demo never shows (the frame numbers assume a save
exists, so run the demo route once first; they also assume a save without
records, because after the first finished match the title screen's Start
leads to the game's achievement pop-up before the menu, which shifts every
later frame: keep a copy of the freshly created card image for the
routes):

```
build\pc\melee.exe --headless --quiet-stubs --input pc\scripts\vs-match.txt --seed 1 --frames 9500 --kill 1@700
build\pc\melee.exe --headless --quiet-stubs --input pc\scripts\classic.txt --seed 1 --frames 40000 --kill 1,2,3@900/750
```

The first picks Mario for port 1 and a CPU for port 2 in VS mode, chooses
Brinstar and plays the two-minute match to its results screen (scene 5
at about frame 8000); the second plays Classic mode, winning each stage
by KO and pressing Start through the stage-clear screens (a Start that
lands inside a match pauses it, which is why the KOs repeat twice per
Start period). Different seeds give different stages and opponents.
`pc\scripts\vs-greens.txt` sets ports 2-4 to CPU and picks Green Greens
(a four-player match, run it with `--frames 9500`).
`pc\scripts\classic-play.txt` is the Classic route without the later Start
presses: the first match runs unpaused, for watching a CPU on a stage
(combine with `--stage`). The other modes have routes too:
`adventure-play.txt` (Adventure, Mushroom Kingdom; press Start around
frame 700 to skip the intro, then hold right), `event-play.txt` (Event
Match 1), `target-play.txt` (Target Test), `homerun-play.txt` (Home-Run
Contest), `multiman-play.txt` (10-Man Melee) and `training-play.txt`
(Training, up to the match with its menu open). All-Star is locked on a
fresh save and has no route yet. A sweep of every stage is the VS route with
`--stage N` for N = 2..32, checking the exit status, the `corrupt` /
`archive check` lines and the motion trace.

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
- `--internal N`: render the scene at N x 640x480 (1-8) whatever the window
  size; the default (0) renders at the size of the window's 4:3 area, so
  a 2x window is drawn at 1280x960 and full screen at the monitor's
  height. Screenshots are saved at the rendering size.
- `--msaa N`: multisample anti-aliasing with N samples (2, 4, 8; 0 off),
  `--aniso N`: anisotropic texture filtering up to N (2-16; 0 off, the
  console's trilinear filtering). `MELEE_GX_NOFBO=1` draws straight into
  the window instead of the off-screen scene framebuffer (the window size
  is then the rendering size and the three options are ignored), for
  driver trouble.
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
  draw and vertex counts of that frame on stderr; `--screenshot-every N`
  changes the interval (every frame with 1, for calibrating scripted
  cursor moves).
- `--log FILE`: write everything the game prints to FILE instead of the
  console. The launcher always passes `melee.log` next to `melee.exe`, so
  that file is the log to send with a bug report.
- `--trace motion,cpu`: turns on the corresponding `MELEE_TRACE_*` logs
  from the command line, so a bug report can be recorded from the
  launcher: put `--trace motion,cpu` in Extra options, reproduce, and send
  `melee.log`.
- `--char N[,M]`: port 1 always picks character N at the character select
  (and a CPU on port 2 gets character M instead of a random one)
  (`CharacterKind` from `src/melee/ft/forward.h`, e.g. 6 Link, 8 Mario,
  9 Marth); `pc\scripts\vs-link.txt` is the VS route plus two forward
  smashes for it.
- `--item KIND@FRAME[:P]`: the item appears next to player P (default 1).
- `--screenshot-from N`: with `--screenshots`, start saving at frame N.
- `MELEE_POKEMON=N`: every Poke Ball releases item kind N (165 Weezing);
  `MELEE_CPU_LEVEL=N`: ports 2-4 play at CPU level N.
- `MELEE_GX_PROFILE=1` (or `--profile`): every 300 frames, where the
  frame's wall time went (vertex processing, shader and texture lookups,
  EFB copies, the present, and the rest, which is the game logic and any
  pacing wait) and how many GL draw calls a frame took. In a vsync'd run
  the driver blocks inside draw calls once the swap queue is full, so the
  draws figure includes waiting; profile with `--fast` to see the work.
- `MELEE_GX_NOBLIT=1`: read EFB copies back through the CPU instead of
  blitting them on the GPU (for driver trouble); `MELEE_GX_CPU=1` transforms
  vertices on the CPU instead of in the vertex shader, `MELEE_GX_NORING=1`
  uses client-side vertex arrays instead of the mapped ring buffer,
  `MELEE_GX_DUMP_VS=1` prints the generated vertex shaders,
  `MELEE_AX_NOAUX=1` mutes the effect buses; `MELEE_GX_NOSHADOW=1`
  skips the fighter shadow-map passes; `MELEE_TRACE_EFFECT=N` logs effect
  N's joint descriptors and animated scales when it spawns.
- `MELEE_TRACE_ANIM=1` also logs texture blend and konst animation
  updates, the fighter's animated-texture table, and TEV constant
  compilation; `MELEE_TRACE_MOTION` also logs item spawns and the star's
  timed status.
- `--stage N`: play every VS and Classic match on stage N (the `StKind`
  number from `src/melee/gr/forward.h`: 2 Fountain, 3 Stadium, 4 Peach's
  Castle, 5 Kongo Jungle, 6 Brinstar, 7 Corneria, 8 Yoshi's Story, 9 Onett,
  10 Mute City, 11 Rainbow Cruise, 12 Jungle Japes, 13 Great Bay, 14 Temple,
  15 Brinstar Depths, 16 Yoshi's Island, 17 Green Greens, 18 Fourside, 19/20
  Mushroom Kingdom I/II, 22 Venom, 23 Poke Floats, 24 Big Blue, 25 Icicle
  Mountain, 27 Flat Zone, 28-30 the N64 stages, 31 Battlefield, 32 Final
  Destination). Only the stage file changes: the rules keep the stage the
  menu chose, which is fine for testing.
- `--item KIND@FRAME`: spawn item KIND (the number from
  `src/melee/it/forward.h`, e.g. 24 for the fan) next to player 1 at FRAME.
- `--kill SLOTS@FRAME[/N]`: drop the fighters of player slots SLOTS (`1`,
  or `1,2,3`) below the stage at FRAME and every N frames after it (300 by
  default). A debugging aid: with it a scripted match ends with a KO and
  reaches the results screen or the next 1-P stage without anyone playing.
- `--extract DIR`: write every file of the disc to `DIR/files`, in the
  disc's own folder layout, and the boot header, FST, apploader and
  `main.dol` to `DIR/sys` (the same layout the decomp's `orig/` uses), then
  exit. This is the starting point for modding: a later milestone adds a
  loose-file override so files in such a folder take precedence over the
  image.

Performance: the game code is compiled unoptimized (it does not survive
the optimizer), but the PC runtime sources under `pc/src` get `/O2` in
every configuration, since the renderer transforms and lights every vertex
on the CPU. The renderer also keeps the GL state it last applied (viewport,
scissor, depth, blend, program, uniforms, vertex attributes) and only
issues the calls whose values changed, and EFB copies are blitted on the
GPU through a framebuffer object instead of being read back. The shader
cache is looked up through a hash of the TEV/texgen key and the texture
cache through a hash of the image pointer (a four-player match compares a
thousand draws a frame against them; the linear searches were the largest
single cost), the key is only rebuilt when a GX call changed something in
it, a palette is hashed once per frame, and consecutive draws whose GL
state is identical are merged into one indexed draw call (strips and fans
become triangle lists; every state change, copy, clear, readback or present
flushes the pending batch first). The four-player results screen, the
heaviest scene at 6800 draws and 118,000 vertices a frame, went from 20 ms
to 13 ms per frame with 640 GL draws; Temple in a two-player match from 13
ms to under 7 ms and a four-player Green Greens from 14.5 to under 8;
before that the port could not hold 60 frames per second
and the audio starved.

The window starts at 640x480 (or `--scale` times that) and can be resized
or made full screen: the frame keeps its 4:3 shape, centred with black
bars. The scene is rendered into an off-screen framebuffer of the window's
4:3 size or the `--internal` scale (multisampled with `--msaa`), which the
EFB copies, the screenshots and the present read from, so the game's
640x480 output scales cleanly to any window; the console's own resolution
is the 1x setting. Escape or closing the window ends the run. The game runs at 60
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

Disc check: the game itself hashes the disc's `main.dol` when it opens
the image and refuses anything but the NTSC 1.02 executable (SHA-1
`08e0bf20...`, the one the decompilation matches): other revisions have
different data layouts and boot into garbage. `MELEE_UNVERIFIED=1` skips
the check for experiments. This is a compatibility check, not copy
protection: anyone can edit it out of the source, and nothing in the
repository or the build contains the game's data, which is the point.

Launcher: `melee-launcher.exe` is built alongside the game and needs
nothing else; each build also copies it to `pc/dist`, where it is
committed so a fresh clone can start from the launcher (it links the C
runtime statically and contains nothing from the game; see
`pc/dist/README.md`). It is a dark, hand-drawn Win32 window: a sidebar of
pages (Setup shows every card, the others one group each), a state pill
in the header, cards for the settings, and a Play button. The cards, in
the order a new user needs them: the game disc (the image is checked to
be GALE01 and its banner is shown; the status line is red with a cross
until "Verify SHA-1" has matched the image, then green with a check mark,
and a matched image is remembered by size and date in `launcher.ini` so
the check is not repeated; "Verify SHA-1" hashes the whole image
against the v1.02 disc the decompilation targets, `d4e70c06...`, and its
`main.dol` against the README's `08e0bf20...`, so a differently dumped or
modified image is told apart from a wrong revision); Build (a checkout of
this repository, the launcher's own two levels up from `build\pc` by
default, whether Visual Studio with the C++ workload is installed, and a
Build button that builds the game from the chosen disc in a console
window; afterwards Play runs the `melee.exe` it built); Display (window
size, the rendering resolution, anti-aliasing and anisotropic filtering,
full screen, and whether the game's console window is shown; hidden, the
game still writes everything to `melee.log`); Audio (volume, mute);
Controls (adapter state and setup, the keyboard layout file with a button
that writes the default layout and opens it for editing); Saves (the
folder); Mods (the list with checkboxes for enabled and buttons for
priority, plus extracting the disc's files); and Advanced (extra options
passed through as typed). Settings persist in `launcher.ini` next to it.
The launcher scales with the monitor's DPI and uses the Segoe MDL2 icon
font that ships with Windows 10 and 11.

Saves: slot A is a virtual 64 Mbit memory card kept as one raw image,
`MemoryCardA.USA.raw` in the saves directory, in the format Dolphin uses
for its own cards (`GC\USA\Card A\MemoryCardA.USA.raw` under Dolphin's
user folder): the card header, the two directory copies and the two
block-allocation tables with their checksums and update counters, and the
data blocks. The game's own card library (`hsd_3A94.c`) writes Melee's
file into it sector by sector exactly as on the console, with its
per-sector digests and byte cipher, and the save payloads (the main save
block and the seven name-tag banks) are converted to the console's byte
order on the way in and out (`pc/src/save_swap.c`, field by field:
records, unlock masks, trophy flags, fighter statistics, including the
one 16-bit bit-field group the console packs from the other end). So the
image is byte-for-byte what a GameCube writes: copy it into Dolphin's
card folder (or the other way round) and the progress carries over, and
a `.gci` file dropped into the saves folder is imported into the card at
the next start (then renamed `.gci.imported`). Every write updates the
directory and allocation table the way the SDK does, into the other copy
first, so an interrupted write leaves the older copy valid. `.sav` files
from earlier versions of the port are not read any more (a startup line
says so); their data was the PC's in-memory layout and does not convert.
Slot B is always empty. `MELEE_TRACE_CARD=1` logs every card call and
the conversions.

## Renderer

`pc/src/gx_render.c` implements the GX API on OpenGL 2.x. Geometry
arrives two ways and is decoded by the same code: immediate mode (the
`GXPosition3f32`-style inline functions in `GXVert.h` write to the
recorder on PC) and display lists from disc (the same command format,
big-endian). Vertices are decoded with the current vertex descriptor,
attribute formats and index arrays, then transformed as the console's XF
unit would: position/normal matrices from the matrix memory, per-vertex
lighting from the channel controls (ambient and material registers, up to
eight lights with the spot and specular attenuation functions: a specular
light's position is the direction to it and its direction the half-angle
vector, as the hardware defines them), texture-coordinate generation
(source, texture matrix and the post-transform matrix HSD uses for every
texture's own translate/scale/rotate). By default that transform runs on
the GPU: the raw vertex goes up with its matrix slot and a generated
vertex shader (one per texgen/channel configuration, cached with the
fragment shader) reads the matrices, lights and material colours from
blocks in a float texture. A block is appended only when its GX state
changed since the last draw, so a batch spans any number of matrix loads.
`MELEE_GX_CPU=1` selects the CPU transform (the same code, kept as the
reference; the two paths render the same pixels). Vertices are written
into a persistently mapped ring buffer when the driver has GL 4.4 (three
regions guarded by fences), so a draw call copies nothing;
`MELEE_GX_NORING=1` falls back to client-side arrays. The fragment side is a GLSL shader generated from the TEV
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

Controllers: GameCube controllers on the official Wii U / Switch USB
adapter map to ports 1-4 (see below); otherwise XInput devices map to
ports 1-4. With no gamepad, the keyboard drives port 1 (arrows = stick, IJKL = C stick, Z/X/C/V = A/B/X/Y, Q/E =
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

GameCube adapter: the official Nintendo adapter (USB id 057E:0337, the
Wii U one and the Switch one are the same device) is supported directly,
with rumble, and controllers can be plugged in or the adapter connected
while the game runs. Windows has no driver for it, so, exactly as for
Dolphin, install the WinUSB driver once with
[Zadig](https://zadig.akeo.ie/): plug the adapter in, in Zadig choose
"Options > List All Devices", select "WUP-028", pick "WinUSB" and click
"Replace Driver". The launcher's "GameCube adapter setup..." button shows the same steps,
reports whether an adapter is present and whether it already has the
WinUSB driver, and opens the Zadig page. The startup log prints
`GameCube adapter connected` when the game finds it (`MELEE_TRACE_PAD=1`
logs the port states and a controller's raw values); a port with a controller takes precedence over an
XInput pad on the same port. Third-party adapters that emulate the
official one (Mayflash in "Wii U" mode) work the same way.

Keys are letters, digits, `F1`-`F24`, or names: `ENTER SPACE TAB BACKSPACE
SHIFT LSHIFT RSHIFT CTRL LCTRL RCTRL ALT UP DOWN LEFT RIGHT INSERT DELETE
HOME END PAGEUP PAGEDOWN NUMPAD0`-`NUMPAD9 NUMPAD+ NUMPAD- NUMPAD* NUMPAD/
NUMPAD. COMMA PERIOD MINUS PLUS SEMICOLON SLASH BACKTICK LBRACKET
BACKSLASH RBRACKET QUOTE`. Actions not mentioned keep their default.

EFB copies (`GXCopyTex`) are read back from the GL framebuffer and
flipped, because GX textures start at the top row and GL framebuffers at
the bottom; the 1-P pause panel and the results screens render text into
such copies and drew it upside down before.

### Debugging aids

- `MELEE_GX_NOCULL=1` / `MELEE_GX_NOALPHA=1` disable face culling / the
  alpha test; `MELEE_GX_LITONLY=1` samples every texture as white, so a
  screenshot shows the vertex lighting alone; `MELEE_GX_LOG_FRAME=N` logs
  every draw of frame N with its state, TEV stages, both colour channels
  with their lights and the first vertex's normal and lit colours (draws
  issued while a fighter model is displayed are tagged, and each
  screenshot line reports how many). `MELEE_TRACE_LIGHT=N` lists the
  scene's light objects (flags, colour, position) as they are set up
  during frame N.
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
- `MELEE_TRACE_MOTION=1` logs every fighter's motion-state changes with
  position and velocity, the animation lengths read at spawn, every hit a
  fighter takes (source, damage, knockback, angle) and the animation
  archives loaded; `MELEE_TRACE_CPU=1` logs every command the CPU logic
  queues and every destination it picks, with the routine that decided it;
  `MELEE_TRACE_PAD=1` also logs every motor command; `MELEE_TRACE_CSS=1`
  logs the character-select cursor and tag bounds on each A press (for
  calibrating scripted routes).
- `MELEE_TRACE_NAN=1` names the first joint per frame whose matrix went
  NaN, with its transform and a backtrace; `MELEE_TRACE_SHIELD=1`,
  `MELEE_TRACE_MOVIE=1`, `MELEE_TRACE_ANIM=1` and `MELEE_TRACE_SWAP=1` log
  the shield size inputs, the movie player's frame counters, texture
  animation image selection and the swapped attribute/pose/trophy tables.
- The log always ends with an `exit after N frame(s)` line, an abort or
  hardware exception prints a backtrace, and C runtime assertions go to
  the log instead of a dialog; a log that stops without any of these means
  the process was killed from outside. A paced run reports every five
  seconds how many frames took longer than a retrace and whether the
  audio device ran dry (the stutter) or was fed too fast (an unpaced run).
- `MELEE_ARCHIVE_CHECK=1` verifies once per frame (and before every joint
  load) that every relocated pointer slot of the parsed archives still
  holds what the parser wrote, and names the first slots that changed:
  the way to catch something overwriting a loaded file.
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
| `src/gcadapter.c` | The official GameCube controller adapter over WinUSB: report reader thread, rumble, hot-plug. |
| `src/card.c` | Memory card: a directory of save files, with the SDK's asynchronous completion semantics. |
| `src/ax.c` | The AX sound driver: voice pool with priority stealing, ADPCM/PCM decoding from ARAM, sample-rate conversion, volume ramps, the two auxiliary effect buses, waveOut output, the AI interface. |
| `src/axfx.c` | The AX auxiliary effects the game registers: the standard reverb (the SDK's per-sample core, PowerPC assembly in the reference source, written out in C) and the delay. |
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

### Swap registry

Every descriptor is swapped at most once, tracked by address in a hash
set (`once()` in `hsd_swap.c`); the archive pre-pass forgets the records of
a buffer when a new file is loaded into it, and so do `HSD_Free` and the
archive free for a buffer that held a parsed file. Lists are checked per
entry, not per list head, because files share list tails: the
stage-select model's polygons point into the middle of another polygon's
vertex-attribute list, and a walk keyed on the head alone re-swapped the
shared entries, ran past the terminator and corrupted the descriptors
after it (the VS-mode crash after pressing Start). A vertex-attribute walk
that meets an attribute out of range now stops and reports it.

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
  the same block. The four places that copy a save entry into a sector
  or a sector into a save entry call `pc_card_swap_payload`, so the card
  holds the console's byte order (a whole-file write copies a sector's
  worth from the entry's registered buffer, a single-entry update such as
  the results screen's save passes the entry itself at its own size; both
  are recognised and converted at the entry's size). `lbcardgame.c` keeps
  the banner/icon descriptor words in console order, because the library
  reads them as bytes.
- `if/ifstatus.c`: the damage display applies its digit textures the
  moment it requests them. The console applies a requested texture frame
  at the next animation pass while the digit positions and the tens
  digit's visibility change at once, so the one frame on which the
  number changes draws the old digits in the new layout (a "0" in the
  tens slot, the "%" over the ones digit); a display-only, one-frame
  difference.
- `hsd_3A64.c`: the text encoder converts UTF-8 input to Shift-JIS first
  (`pc/src/sjis.c`). The sources keep full-width text such as the
  character names as UTF-8 and the GameCube build converts every literal
  with sjiswrap; this build compiles them as they are, and without the
  conversion the name box on the character select stayed empty.
- Bit-field packing: Metrowerks places plain bytes that follow a group of
  `u32` bit-fields inside the bit-fields' 32-bit unit, MSVC starts a new
  unit. `mn/types.h` (`StartMeleeRules`, 48 bits then bytes) and
  `if/ifstatus.c` (`FlagsX`) declare those bit-fields byte-sized on PC,
  which restores the console offsets; `gm/types.h` packs the tournament
  menu entries on PC as the matching build does. All the settings-struct
  size asserts are active again. `pc/tools/scan_bitfields.py` finds this
  pattern.
- `pl/player.c`: the character-to-fighter mapping is read through a struct
  laid over two filename strings and the mapping table (original link
  order); on PC the view is a copy filled from the three globals. Without
  it the 1-P and VS intros picked the wrong fighters' animation archives
  and crashed at the first match after the character select.
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
- `granime.c`: the stage code has its own copy of the HSD "add animation"
  walk; it got the texture-animation swap hook the HSD one has (Onett's
  animated textures come through it and crashed the second Classic stage).
- `gm_1798.c`: the results-screen camera reads its tables through a struct
  laid over the statics that follow `gmResultPlayerColors` in link order;
  on PC the members are macros naming the statics (`RES_*`).
- `lbspdisplay.c`: the blur renderers write `GXColor` temporaries at
  negative offsets from a `GXTexObj` local (the console's stack layout);
  on PC the object sits in a struct with scratch space below it. The
  function that creates the blur object also returns it now (the console
  returned it through r3 by accident of register allocation; the 1-P
  stage-clear screen uses the value). A few other functions that fell off
  their end got the return the console produced by the same accident
  (`gm_1601.c`, `gm_1798.c`, `itkyasarinegg.c`); `-Wreturn-type` lists the
  remaining ones, whose callers ignore the value.
- `gobj.h`: `HSD_GObjGetUserData(NULL)` returns NULL on PC; several
  matching tricks evaluate `GET_FIGHTER(0)` for their stack layout, which
  reads address 0x2C (mapped on the console).
- `gmclassic.c`: Classic mode reads its matchup tables as the data after
  the scene table (`(gmClassicSceneData*) gm_Mode_Classic_States`) and the
  matchup order state as the bytes after the intro buffer
  (`gm_804908A0`); on PC the tables are named directly and the intro
  buffer and order state are one object. With the wrong bytes the stage
  of the fourth Classic match came out as kind 0 (no file, no music).
- `toy.c`/`toy.h`: the trophy code views `_Toy_804A26B8`, two text
  buffers and `Toy_804A284C` as one 0x3F0-byte object (`toy + 0x194` is
  the 1-P trophy flag table); on PC they are one struct. The trophy tables
  of `TyDatai.dat` (init tables, sort table, exception lists, display
  tables) are swapped when loaded (`pc_swap_trophy_tables`). Without both,
  the "Grab the Trophies" bonus stage spun forever looking for a trophy
  to place.
- `tydisplay.c`: the trophy display code reads its three name tables
  (joint names, material-animation names, archive names) as one
  `TyDspNameTables` starting at the first; on PC they are one object.
- `gm_1601.c`: the character-to-texture index function leaves its
  result uninitialized for the regular characters (the console returns
  the character kind because both share a register); on PC it starts from
  the character kind. Without it the results screen named every fighter
  after texture 0 (Captain Falcon).
- `particle.c`/`particle.h`: the particle system reads its tables as one
  struct starting at `hsd_804D08E8` (JObj slots, list heads, the six
  per-bank tables and the particle allocator, in link order); on PC they
  are one object (`pc_particle_block`). Before this the list heads came
  from whatever the linker placed next, which is what the
  "particle list ... is corrupt" messages (and the slowdown on Onett)
  were.
- `lb_00B0.c`: the joint-copy and blend helpers that read pose
  descriptors straight from fighter data (the guard pose, special-move
  blends) swap the descriptor first; the shield made the whole fighter
  vanish because its joints took big-endian floats.
- `sobjlib.c`: sprite descriptors (the opening's "Nintendo's All-Stars
  in" caption, how-to-play captions) get their image and palette
  descriptors swapped; the caption drew as a white rectangle before.
- `rumble.c` + `lb_013B.c`: rumble patterns are u16 word lists loaded from
  `LbRb.dat` (three command bits, a count below); they are swapped when
  loaded and the interpreter takes the command from the host-order word.
  Unswapped, the first fight rumble never reached its stop word, which is
  why the controller vibrated for the whole match.
- `ef/eflib.c`: an effect descriptor's lifetime is a float in the effect
  data file; unswapped it read as 0, "never expires", and the entry beam
  stayed around the fighter for the whole match.
- `ft/types.h`: the motion-state tables initialize a word as
  `(move_id << 24) | (flag << 23) | ...` and read it through byte and
  bit-field views; on PC the view is declared in the console's order.
- `pc_swap_ft_common`: the item swing speed table (`float[type][5]`) and
  the float list after it from `PlCo.dat` are swapped; unswapped the fan
  swing ran at a denormal speed and the fighter never left the state
  (Fox "unresponsive after grabbing the fan").
- `grvenom.c`: the stage callbacks are read as the data 0x44 bytes past
  the stage's data struct; the three objects are kept adjacent with
  `PC_ADJACENT` (sections l-n). Otherwise the stage's init recursed until
  the heap was gone.
- `gcadapter.c`: `PAD_MOTOR_STOP_HARD` stops the motor too; the adapter's
  own "brake" value kept the official adapter rumbling for the whole match.
- `itmasterhandlaser.c`: a finger-beam laser that already died leaves a
  GObj without item data; the console's write through it lands in low
  memory, PC skips it.
- `ftbosslib.c`: Master Hand's attack timer divides by its CPU level,
  which is 0 in Classic mode; the console's integer division by zero
  yields 0 without trapping, x86 traps. Guarded on PC.
- `ftCo_Bury.c`: the floor's hazard description (Mute City's road) is
  swapped where the bury state reads it, as the hazard path already did.
- `itsonans.c`: Wobbuffet's counter damage decays below zero between
  hits and is converted to `u32`; the console's float-to-unsigned
  conversion saturates negatives to 0, x86's returns 0xFFFFFFFF (which the
  game rejects as "attack power over 500"). Clamped on PC. Other
  `(u32) float` conversions of possibly negative values would differ the
  same way; none has shown up yet.
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
- `ftdata.c`: the second figatree loader (the one that only reads an
  animation's frame count for the landing and jump lengths) swaps the tree
  too; unswapped, LandingFallSpecial ran at a speed near zero and a fighter
  stood still for good after landing from an up special (the "Mario
  stopped and never moved again" report; CPUs died the same way on
  Rainbow Cruise).
- `pc_swap_ft_common`: the CPU attack-selection lists, distance thresholds
  and weapon reach floats (PlCo table 22) and the crowd reaction
  thresholds (table 21) are swapped; every per-kind array is walked to the
  next relocated object instead of a fixed count, and the entry for kind
  33 ("no kind", used for a thrown fighter's animation) is swapped when it
  is first used (`pc_swap_ft_kind_entry`). With CPUs now grabbing and
  throwing, the demo reached several paths for the first time: a
  fighter's own items (the Ice Climbers' blizzard and rope) get their
  item-specific attribute block swapped like a stage item's, and Kirby's
  copy hats (`PlKb*.dat`: parts descriptor, and the bone dynamics of
  Kirby's and Pichu's hats) are swapped when a hat is put on.
- `lb/types.h`: `spawn_hitbox_skip` reads its flag from the word the
  console compiler packs it into; misread, the damage-fly collision
  hitbox was created for fighters that were merely launched, and two
  fighters hit by the same hazard (Onett's car, a Green Greens block)
  juggled each other every frame for the rest of the match.
- `grdatfiles.c`: a stage's extra `map_head` (Pokemon Stadium's
  transformations) is swapped before its table is read (crash on the
  first transformation).
- `pc/tools/gen_struct_swap.py`: stages that declare their parameter
  block inline (`static struct { ... }* yakumono_param`) get a swapper for
  that struct; before, a header's declaration of the same name won, and
  the Yoshi's Island (N64) block was swapped as Kongo Jungle's 0xBC bytes,
  running into the light descriptors behind it (the stage asserted on a
  missing light).
- `gx_render.c`: `GX_VA_NBT` vertices (items with environment maps: the
  capsule, the shells, the barrel) supply their normal; without it they
  were lit by the ambient term only and looked grey and flat. `GX_VA_NBT`
  is the normal attribute with nine components, so it shares the normal's
  descriptor, format and array and sits between the position and the
  colours in the vertex stream; read after the texture coordinates (its
  own attribute number) the indices were garbage, the binormal and tangent
  were random and the emboss stages subtracted a random amount of the red
  bump map (the capsule came out olive instead of beige and pink).
- `particle.c`: the particle bytecode's float operands (position, velocity,
  size targets) are big-endian bytes and were assembled in stream order on
  the little-endian host, so every one came out byte-reversed: a size of
  6.0 read as a denormal and 5.2 as 2.7e23. Fox's Fire Fox charge sets its
  flame sprites' size that way, and a sprite 1e23 units wide is the flat
  yellow triangle that covered the screen during the move.
- `mtx.c`: `MTXRotRad`, `MTXLightPerspective`, `MTXLightFrustum` and
  `MTXLightOrtho` were SDK stubs, so everything built from them had a zero
  matrix: the fighter shadow projections, the refraction texture matrix
  (cloaking device, heat haze; a cloaked fighter was a black silhouette)
  and rotated billboards (rotated effect sprites).
- `gx_render.c`: indirect texturing (`GXSetTevIndirect`, `GXSetIndTexOrder`,
  `GXSetIndTexMtx`): a stage's texture coordinate is offset by the
  indirect texture's (a, b, g) values through the indirect matrix, in
  texels of the stage's texture. The refraction material (cloaking device,
  fire effects) warps a copy of the screen with it.
- `gx_render.c`: a texture coordinate generated with the identity matrix
  still goes through the post-transform matrix; toon-shaded items (tomato,
  Poke Ball, barrel, crate, food) map their normal through it and were
  drawn unshaded. Emboss bump mapping (`GX_TG_BUMP0-7`): the barrel and
  other NBT items sample their height map twice, at the plain coordinate
  and at one shifted along the eye-space binormal and tangent by the
  direction to a light, and subtract; without it the shifted sample was
  texel (0,0) and the barrel came out black.
- `inlines.h` / `game_swap.c`: Link's, Young Link's, Marth's and Roy's
  attribute blocks end in the sword-trail block whose alpha and colour are
  bytes; the word swap now leaves those three words in file order (the
  trail was a solid green sail instead of a soft fading arc).
- `mnevent.c`: the event menu addresses its strings as offsets from an
  animation-settings struct that precedes them in the console's data
  layout; on PC the string block is addressed directly (the menu looked
  up garbage symbol names and asserted).
- `gmevent.c`: the event level tables from `GmEvent.dat` are swapped, and
  `gm_evinit` declares its two flag bytes as byte-sized bit-fields so the
  fields after them stay at their console offsets (every event had stage
  0 and no music).
- `gm_181A.c`: the Multi-Man spawn tables from `GmKumite.dat` are swapped
  (the wireframes' attack ratio was a denormal; the first hit sent the
  player to infinity). `mnhyaku.c`: the Multi-Man menu passes the port to
  `gm_801677E8` explicitly (the console build left it in r3 from the call
  before), so the character select stores the pick under the right port;
  before, the match started with no character and a second Mario.
- `gm_180A.c`: the four ints that `fn_80181708` clears past
  `lbl_80472E48` are that struct's own storage (`lbl_80472EC8`) on PC;
  they used to land on the Home-Run Contest's archive pointers.
- `ground.c`: the stage's light override table (which lights are diffuse,
  specular or shadow-only for this stage) is applied to the light
  descriptors' flags before `HSD_LObjLoadDesc` swaps them; on PC the
  descriptor is swapped there first. Unswapped, the type test failed on
  every light and the overrides were skipped, so Temple's shadow-only
  white light lit the fighters as a second diffuse light and washed the
  shading out (the "characters look flat and pale" report).
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
- The particle list walker still drops a list and logs
  `particle list ... is corrupt` if a link is ever bad; the cause found so
  far (the table block above) is fixed.
- The mixer ignores interaural delay (`ITD`) and the low-pass filter;
  sample-rate conversion is linear rather than the DSP's 4-tap filter.
  Volumes are not calibrated against the console. Of the auxiliary
  effects only the two the game uses exist (the standard reverb on bus A,
  the delay on bus B; `src/axfx.c`); the chorus and the "hi" reverb are
  still stubs.
- Save files are the PC layout of the game's structures; converting to or
  from real memory-card dumps (`.gci`) would need a byte swap of the game's
  save-data structs.
- Four more "index past a global" idioms exist (`gm_19EF.c`, `soundtest.c`)
  that assume console link order.
- The console converts negative floats to `u32` as 0; x86 does not. Only
  the site that mattered so far (Wobbuffet) is clamped.
- Pokemon Stadium's screen renders its text through an EFB copy; the copy
  is now the right way up, but whether the paused 1-P layout ("P1 Pause"
  drawn over "Combatants") matches the console has not been checked.
- Classic mode has been driven to and through the Master Hand fight with
  `--kill`; the ending sequence after his defeat has not been reached (the
  option cannot KO him).
- Stages 1 (Test), 21 (Akaneia) and 26 (Icetop) are unfinished in the
  game and crash or hang when forced with `--stage`; the menus never
  select them.
- Adventure mode has only been driven through the first stage's opening
  (the scripted walker stops at the first tall wall); the later stages
  and their cutscenes are unverified. All-Star mode is locked on a fresh
  save and untested.
- Flat Zone (`--stage 27`): after about a minute a falling tool item
  crashes while flashing before it vanishes (`it_80273670` with state
  index `x0 + 5` = 9, whose animation joint has a garbage child). Not
  found yet; every other selectable stage runs 5000 frames of a VS match
  clean.
- Specular lighting: sysdolphin builds the half-angle vector as
  `light vector + (joint direction from the eye)`, which is the negative
  of the half-angle the GX SDK's `GXInitSpecularDir` stores, so with the
  hardware's `N . H` term (as Dolphin implements it) the specular channel
  comes out zero for fighters here. Whether the console shows those
  highlights, and with which sign, has not been checked against hardware.
- `char` signedness and paired-single float rounding are not matched.
- Threads are stubs (the game creates none that matter on PC).
