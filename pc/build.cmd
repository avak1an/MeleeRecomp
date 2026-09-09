@echo off
rem Build the PC port with the 32-bit MSVC toolchain.
rem   pc\build.cmd            configure (if needed) and build
rem   pc\build.cmd run        build, then run until the game needs the disc
rem   pc\build.cmd clean      delete build\pc
setlocal
set ROOT=%~dp0..
set BUILD=%ROOT%\build\pc

if /i "%1"=="clean" (
    rmdir /s /q "%BUILD%" 2>nul
    echo cleaned %BUILD%
    exit /b 0
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
rem vcvars can print a harmless vswhere warning when run from a non-VS shell.
call "%VSPATH%\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>&1
if errorlevel 1 exit /b 1

where cmake >nul 2>nul || set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

if not exist "%BUILD%\build.ninja" (
    cmake -S "%ROOT%\pc" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Debug || exit /b 1
)
ninja -C "%BUILD%" || exit /b 1

if /i "%1"=="run" (
    "%BUILD%\melee.exe"
)
endlocal
