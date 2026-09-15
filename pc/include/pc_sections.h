/**
 * @file pc_sections.h
 * Forced include for the runtime's own sources (pc/src, see CMakeLists.txt):
 * their globals go to sections of their own, so the determinism check in
 * determinism.c can hash the game's writable data without the host-side
 * state (renderer caches, the audio device, timers, file handles).
 */
#ifndef PC_SECTIONS_H
#define PC_SECTIONS_H

#pragma data_seg(".pcdata")
#pragma bss_seg(".pcbss")

#endif
