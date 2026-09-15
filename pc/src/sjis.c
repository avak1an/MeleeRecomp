/**
 * @file sjis.c
 * String literals in the game's sources in the console's encoding.
 *
 * The game's text renderer (sislib) reads Shift-JIS: the two-byte codes
 * select glyphs in the font. The sources keep their Japanese and full-width
 * text as UTF-8 and the GameCube build converts every literal with sjiswrap
 * on the way into the compiler; this build compiles them as they are, so a
 * string that reaches the renderer from a source literal (the character
 * names on the character select, for one) is UTF-8. Strings from the disc
 * are Shift-JIS already. The renderer's encoder calls this on its input:
 * a buffer that is valid UTF-8 with at least one multi-byte sequence is
 * converted in place, anything else is left alone. A Shift-JIS string is
 * practically never valid UTF-8 as a whole, since its second bytes go down
 * to 0x40 where UTF-8 needs 0x80-0xBF.
 */
#include "pc_runtime.h"

#include <pc_text.h>

#include <string.h>
#include <windows.h>

void pc_utf8_to_sjis(u8* buf, int cap)
{
    int i, multibyte = 0;
    wchar_t wide[256];
    char out[256];
    int n, m;
    for (i = 0; i < cap && buf[i] != 0; i++) {
        if (buf[i] >= 0x80) {
            multibyte = 1;
        }
    }
    if (!multibyte || i >= cap) {
        return;
    }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*) buf, i, wide, 256);
    if (n <= 0) {
        return; /* not UTF-8: the console's encoding already */
    }
    m = WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide, n, out, (int) sizeof(out), NULL, NULL);
    if (m <= 0 || m >= cap) {
        return;
    }
    memcpy(buf, out, (size_t) m);
    buf[m] = 0;
}
