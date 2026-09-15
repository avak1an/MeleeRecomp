#ifndef PC_TEXT_H
#define PC_TEXT_H

#include <dolphin/types.h>

/// Convert a NUL-terminated UTF-8 string of at most `cap` bytes to
/// Shift-JIS in place; a string that is not UTF-8 is left as it is.
void pc_utf8_to_sjis(u8* buf, int cap);

#endif
