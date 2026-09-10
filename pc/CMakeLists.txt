# PC port of the Melee decompilation (milestone 1: build scaffold).
#
# This is a separate project root so the matching GameCube build
# (configure.py + ninja at the repository root) is untouched.
#
#   cmake -S pc -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   ninja -C build/pc
#
# Use pc/build.cmd on Windows, which sets up the 32-bit MSVC environment.
cmake_minimum_required(VERSION 3.21)
project(melee_pc C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "The PC build must be 32-bit (pointer size 4). HSD archives relocate "
        "32-bit pointers in place. Run pc/build.cmd or configure with the x86 "
        "MSVC toolchain.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Only MSVC is supported for now (see pc/include/pc_platform.h).")
endif()

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GAME_VERSION GALE01)

# CMake's default Debug flags add /RTC1 (run-time checks). The decompiled
# code relies on behaviour those checks flag, such as reading a local the
# console left as zero, so they are removed.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

# --- Sources ----------------------------------------------------------------
# Game and engine: everything the GameCube build links, minus the few units
# that are not part of the DOL.
file(GLOB_RECURSE MELEE_SOURCES CONFIGURE_DEPENDS ${REPO}/src/melee/*.c)
file(GLOB_RECURSE HSD_SOURCES CONFIGURE_DEPENDS ${REPO}/src/sysdolphin/baselib/*.c)
list(FILTER MELEE_SOURCES EXCLUDE REGEX "ftCo_BuryWait\.c$") # not in the DOL

# SDK sources that are pure C and hardware-free; compiled as-is.
set(SDK_SOURCES
    ${REPO}/extern/dolphin/src/dolphin/os/OSAlloc.c
    ${REPO}/extern/dolphin/src/dolphin/pad/PadClamp.c
    ${REPO}/extern/dolphin/src/dolphin/mtx/mtx44.c
)

# PC runtime: hand-written replacements for the SDK plus generated stubs.
file(GLOB PC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
set(PC_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated/stubs.c
                 ${CMAKE_CURRENT_SOURCE_DIR}/generated/yakumono_swap.c)

add_executable(melee
    ${MELEE_SOURCES}
    ${HSD_SOURCES}
    ${SDK_SOURCES}
    ${PC_SOURCES}
    ${PC_GENERATED}
)

# The game defines int main(); the PC runtime owns the real entry point and
# calls the game's as melee_main().
set_source_files_properties(${REPO}/src/melee/gm/gmmain.c
    PROPERTIES COMPILE_DEFINITIONS main=melee_main)

# --- Include paths ----------------------------------------------------------
# pc/include first so it can shadow headers; src/MSL is deliberately NOT on
# the path: the game's <math.h>, <string.h> etc. resolve to the host CRT.
# --- Font tables --------------------------------------------------------------
# Two sources include byte tables of the game's font atlases. The GameCube
# build extracts them from the original DOL into build/GALE01/include; with
# MELEE_ISO set (pc/build.cmd finds the disc image) a small host tool pulls
# the same bytes straight out of the disc image, so the PC build stands alone.
set(MELEE_ISO "" CACHE FILEPATH "GALE01 disc image to extract the font tables from")
set(FONT_INCLUDE_DIR ${REPO}/build/${GAME_VERSION}/include)
if(MELEE_ISO)
    add_executable(melee-extract-fonts ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_fonts.c)
    target_compile_options(melee-extract-fonts PRIVATE /D_CRT_SECURE_NO_WARNINGS)
    set(FONT_INCLUDE_DIR ${CMAKE_BINARY_DIR}/include)
    add_custom_command(
        OUTPUT ${FONT_INCLUDE_DIR}/sysdolphin/baselib/sislib_font.inc
               ${FONT_INCLUDE_DIR}/sysdolphin/baselib/debug_font.inc
        COMMAND melee-extract-fonts "${MELEE_ISO}" "${FONT_INCLUDE_DIR}"
        DEPENDS melee-extract-fonts "${MELEE_ISO}"
        COMMENT "Extracting the font tables from ${MELEE_ISO}")
    add_custom_target(melee-fonts DEPENDS
        ${FONT_INCLUDE_DIR}/sysdolphin/baselib/sislib_font.inc
        ${FONT_INCLUDE_DIR}/sysdolphin/baselib/debug_font.inc)
    add_dependencies(melee melee-fonts)
elseif(NOT EXISTS ${FONT_INCLUDE_DIR}/sysdolphin/baselib/sislib_font.inc)
    message(FATAL_ERROR
        "The font tables are missing. Either pass -DMELEE_ISO=<path to GALE01.iso> "
        "(pc/build.cmd does this when it finds the disc image) or run the GameCube "
        "build first (configure.py + ninja at the repository root).")
endif()

target_include_directories(melee PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${REPO}/src
    ${REPO}/extern/dolphin/include
    ${FONT_INCLUDE_DIR}
)

# --- Compiler flags ---------------------------------------------------------
target_compile_definitions(melee PRIVATE
    TARGET_PC=1
    VERSION_${GAME_VERSION}
    BUILD_VERSION=2
)

target_compile_options(melee PRIVATE
    /std:c11
    /Zc:preprocessor        # standard-conforming preprocessor (GCC-like __VA_ARGS__)
    /FI${CMAKE_CURRENT_SOURCE_DIR}/include/pc_platform.h
    /fp:precise
    /W3
    /wd4068     # unknown pragma (Metrowerks pragmas)
    /wd4244     # conversion, possible loss of data
    /wd4305     # truncation from double to float
    /wd4018     # signed/unsigned mismatch
    /wd4101     # unreferenced local variable
    /wd4102     # unreferenced label
    /wd4716     # function must return a value (matches the CW build's -Wno-return-type)
    /wd4715     # not all control paths return a value
    /wd4700     # uninitialized local variable used
    /wd4133     # incompatible pointer types (warning in C; keep the build quiet for now)
    /wd4047     # differs in levels of indirection
    /wd4267     # size_t conversion
    /wd4146     # unary minus on unsigned
    /wd4293     # shift count negative or too big
    /wd4090     # different const qualifiers
    /wd4113     # parameter lists differ
    /wd4996     # deprecated CRT names
)

# Implicit function declarations are how missing prototypes hide; make them
# errors so every call site is checked against the SDK headers.
target_compile_options(melee PRIVATE /we4013)

# clang-cl (Visual Studio's "C++ Clang tools" component). Configure with
#   -DCMAKE_C_COMPILER=clang-cl -DCMAKE_C_FLAGS=-m32
# or run `pc\build.cmd clang`. Warning choices mirror the repository's own
# clang lint pass (configure.py); the two -Wno-error lines cover idioms the
# decomp relies on that recent clang promotes to errors.
# Function-entry tracing (clang only): every function records itself in a
# ring buffer that the crash handler prints. Slow; for finding crashes that
# leave no usable stack. Configure with -DMELEE_TRACE_FUNCS=ON.
option(MELEE_TRACE_FUNCS "Record the last function entries for crash reports" OFF)
if(MELEE_TRACE_FUNCS AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(melee PRIVATE -Xclang -finstrument-functions)
    target_compile_definitions(melee PRIVATE MELEE_TRACE_FUNCS=1)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(melee PRIVATE
        # The decompiled code reads locals it never wrote in places where the
        # console's stack happened to hold zeros; make that deterministic.
        -ftrivial-auto-var-init=zero
        -Wno-error=return-mismatch
        -Wno-error=incompatible-function-pointer-types
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-value
        -Wno-unused-function
        -Wno-unused-parameter
        -Wno-unused-command-line-argument
        -Wno-missing-braces
        -Wno-missing-field-initializers
        -Wno-array-parameter
        -Wno-sometimes-uninitialized
        -Wno-return-type
        -Wno-absolute-value
        -Wno-format
        -Wno-compare-distinct-pointer-types
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
        -Wno-pointer-sign
        -Wno-parentheses
        -Wno-switch
        -Wno-microsoft-anon-tag
        -Wno-deprecated-non-prototype
        -Wno-unknown-pragmas
    )
endif()

# /LARGEADDRESSAWARE lets the 32-bit process map the console's main-memory
# range (0x80000000+), which the game uses to tell main memory from ARAM.
target_link_options(melee PRIVATE /SUBSYSTEM:CONSOLE /STACK:0x400000 /LARGEADDRESSAWARE /DYNAMICBASE:NO)
target_link_libraries(melee PRIVATE dbghelp xinput9_1_0 winmm winusb setupapi opengl32 gdi32 user32)
set_target_properties(melee PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    C_STANDARD 11)

# --- Launcher ---------------------------------------------------------------
# A plain Win32 window next to melee.exe: disc, display, audio, keyboard,
# saves and mod settings, disc extraction, and a Play button.
add_executable(melee-launcher WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.c)
target_compile_options(melee-launcher PRIVATE /W3 /O2 /D_CRT_SECURE_NO_WARNINGS)
# The launcher binary is committed (pc/dist) and runs on machines without a
# development setup: static release C runtime, no debug CRT dependency.
set_property(TARGET melee-launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
# Runs on every build (a relink alone would skip it when the launcher is
# up to date but the dist copy was moved aside, see build_game in launcher.c).
add_custom_target(melee-launcher-dist ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:melee-launcher>
            -DDST=${CMAKE_CURRENT_SOURCE_DIR}/dist/melee-launcher.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_launcher.cmake
    DEPENDS melee-launcher
    COMMENT "Copying the launcher to pc/dist")
target_link_libraries(melee-launcher PRIVATE comctl32 comdlg32 shell32 ole32 user32 gdi32 advapi32)
target_link_options(melee-launcher PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
set_target_properties(melee-launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    C_STANDARD 11)
