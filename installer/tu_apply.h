/* tu_apply: turn the disc's default.xbe into the title update's with cs_tu.xbepatch
 * (scripts/tu_patch.py describes the format). Shared by cs_import and the game. */
#ifndef TU_APPLY_H
#define TU_APPLY_H

#include <windows.h>

enum {
    TU_ALREADY = 0,         /* default.xbe is already the title update's */
    TU_PATCHED = 1,         /* it was the disc's and is now the update's */
    TU_ERR_NO_PATCH = -1,   /* the patch file is missing or unreadable */
    TU_ERR_BAD_PATCH = -2,  /* not a CSTU patch, or damaged */
    TU_ERR_WRONG_XBE = -3,  /* default.xbe is neither the disc's nor the update's */
    TU_ERR_IO = -4          /* default.xbe could not be read or replaced */
};

/* xbe: the game folder's default.xbe; patch: cs_tu.xbepatch. Replaces xbe only
 * after the result matches the update's SHA-256. */
int tu_apply_file(const WCHAR *xbe, const WCHAR *patch);
const char *tu_error_text(int code);

#endif
