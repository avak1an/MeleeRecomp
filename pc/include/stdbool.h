/**
 * @file stdbool.h
 * Shadows the host CRT's <stdbool.h>. The game was written against the
 * Metrowerks MSL headers, where bool is a plain int (see src/MSL/stdbool.h);
 * keeping that on PC preserves struct layouts and the int/bool prototype
 * mixing found throughout the sources.
 */
#ifndef PC_STDBOOL_H
#define PC_STDBOOL_H

typedef int bool;
#define true 1
#define false 0

#endif
