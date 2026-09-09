@echo off
rem Build the PC port (32-bit, MSVC ABI).
rem   pc\build.cmd              configure (if needed) and build with clang-cl
rem                             (falls back to cl.exe if clang is not installed)
rem   pc\build.cmd msvc         force cl.exe (build\pc-msvc)
rem   pc\build.cmd clang        force clang-cl (build\pc)
rem   pc\build.cmd [msvc|clang] run    build, then run the game
rem   pc\build.cmd [msvc|clang] clean  delete the build directory
rem The disc image supplies the font tables the sources include: set
rem MELEE_ISO=<path> or keep GALE01.iso in the repository or its parent
rem directory. Without it the GameCube build (configure.py + ninja) must have
rem produced build\GALE01\include.
setlocal
set ROOT=%~dp0..
set TOOLCHAIN=auto
set ACTION=%1
if /i "%1"=="clang" (
    set TOOLCHAIN=clang
    set ACTION=%2
)
if /i "%1"=="msvc" (
    set TOOLCHAIN=msvc
    set ACTION=%2
)

rem vswhere lives under "Program Files (x86)"; the parentheses in that path
rem break cmd blocks and for /f, so keep it out of any ( ) and use a temp file.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if not exist "%VSWHERE%" goto :vs_done
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\melee_vspath.txt"
set /p VSPATH=<"%TEMP%\melee_vspath.txt"
del "%TEMP%\melee_vspath.txt" 2>nul
:vs_done
if not defined VSPATH (
    echo Visual Studio with the C++ toolset was not found.
    exit /b 1
)

set "CLANGDIR=%VSPATH%\VC\Tools\Llvm\x64\bin"
if "%TOOLCHAIN%"=="auto" (
    if exist "%CLANGDIR%\clang-cl.exe" (set TOOLCHAIN=clang) else (set TOOLCHAIN=msvc)
)
if "%TOOLCHAIN%"=="clang" (
    if not exist "%CLANGDIR%\clang-cl.exe" (
        echo clang-cl was not found. Install "C++ Clang tools for Windows" in the Visual Studio Installer.
        exit /b 1
    )
    set BUILD=%ROOT%\build\pc
    set "PATH=%CLANGDIR%;%PATH%"
    set "CMAKE_EXTRA=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_C_FLAGS=-m32"
) else (
    set BUILD=%ROOT%\build\pc-msvc
    set "CMAKE_EXTRA="
)

if /i "%ACTION%"=="clean" (
    rmdir /s /q "%BUILD%" 2>nul
    echo cleaned %BUILD%
    exit /b 0
)

rem vcvars can print a harmless vswhere warning when run from a non-VS shell.
call "%VSPATH%\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>&1
if errorlevel 1 exit /b 1

where cmake >nul 2>nul || set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

if not defined MELEE_ISO (
    if exist "%ROOT%\GALE01.iso" set "MELEE_ISO=%ROOT%\GALE01.iso"
)
if not defined MELEE_ISO (
    if exist "%ROOT%\..\GALE01.iso" set "MELEE_ISO=%ROOT%\..\GALE01.iso"
)
if not defined MELEE_ISO (
    if exist "%ROOT%\build\pc\GALE01.iso" set "MELEE_ISO=%ROOT%\build\pc\GALE01.iso"
)
set "ISO_ARG="
if defined MELEE_ISO (
    set "ISO_ARG=-DMELEE_ISO=%MELEE_ISO%"
    echo font tables from %MELEE_ISO%
)

if not exist "%BUILD%\build.ninja" (
    cmake -S "%ROOT%\pc" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Debug %CMAKE_EXTRA% %ISO_ARG% || exit /b 1
)
ninja -C "%BUILD%" || exit /b 1

if /i "%ACTION%"=="run" (
    "%BUILD%\melee.exe"
)
endlocal
