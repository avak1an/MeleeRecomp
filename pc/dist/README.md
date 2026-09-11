# Prebuilt launcher

`melee-launcher.exe` is the Windows launcher for the PC port, built from
`pc/launcher/launcher.c` by `pc\build.cmd`, which copies each build here.
It contains nothing from the game: it is a settings window that verifies
and extracts a disc image you own, builds the game from this repository
with Visual Studio, manages mods and saves, and starts `melee.exe`.

It links the C runtime statically, so it runs on a plain Windows 10 or 11
installation. It is a 32-bit program like the game.

To use it without building anything first:

1. Clone this repository and install Visual Studio (Community is free)
   with "Desktop development with C++" and the "C++ Clang tools for
   Windows" component.
2. Run `pc\dist\melee-launcher.exe`. Point the game disc card at your
   Super Smash Bros. Melee (USA) v1.02 disc image and click "Verify SHA-1".
3. In the Build card the source folder is this checkout; click Build.
   A console shows the build; it takes a few minutes the first time.
4. Click Play. Saves and mods live next to the built `melee.exe`, under
   `build\pc\`.

`melee.exe` itself is not distributed: it embeds tables taken from the
disc's `main.dol`, so every copy is built from the owner's own disc.
