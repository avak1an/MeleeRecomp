# PC port of the Melee decompilation (milestone 1: build scaffold).
#
# This is a separate project root so the matching GameCube build
# (configure.py + ninja at the repository root) is untouched.
#
#   cmake -S pc -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Debug
#   ninja -C build/pc
#
# Use pc/build.cmd on Windows, which sets up the 32-bit MSVC environment.
cmake_minimum_required(VERSION 3.20)
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
set(PC_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated/stubs.c)

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
target_include_directories(melee PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${REPO}/src
    ${REPO}/extern/dolphin/include
    ${REPO}/build/${GAME_VERSION}/include  # font tables extracted by configure.py
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

target_link_options(melee PRIVATE /SUBSYSTEM:CONSOLE /STACK:0x400000)
target_link_libraries(melee PRIVATE dbghelp)
set_target_properties(melee PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    C_STANDARD 11)
