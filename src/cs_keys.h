/*
 * cs_keys.h -- the keyboard and mouse controls, shared by the F1 menu's
 * Controls tab (cs_overlay.c) and config.exe.
 *
 * %LOCALAPPDATA%\CounterStrikeXB\keymap.ini, [keys]: one line per Xbox control
 * in the toolkit's names ("a = space"), read by the toolkit (RECOMP_KEYMAP,
 * src/input/keyboard.c) at start and again after a change here
 * (xbox_InputKeyboardReload). The controls are listed by what they do in
 * Counter-Strike, as the manual's Controls page names them, and the defaults
 * follow the PC game's keys as closely as the Xbox controls allow.
 *
 * No Windows types here: config.exe includes it too.
 */
#ifndef CS_KEYS_H
#define CS_KEYS_H

#include <stddef.h>
#include "input_map.h"

#define CSK_SLOTS 3                     /* keys per action shown and saved */

typedef struct {
    int vk[INPUT_CONTROL_COUNT][CSK_SLOTS];   /* 0: none */
} CsKeyMap;

typedef struct {
    int control;                        /* InputControl */
    const char *label;                  /* what it does, as the manual names it */
    const char *section;                /* non-NULL: a heading before this row */
} CsKeyAction;

extern const CsKeyAction cs_key_actions[];
extern const int cs_key_action_count;    /* INPUT_CONTROL_COUNT: every control, once */

void cs_keys_defaults(CsKeyMap *m);
/* keymap.ini, or the defaults where it says nothing; 1 if the file was read. */
int  cs_keys_load(CsKeyMap *m);
int  cs_keys_save(const CsKeyMap *m);
/* At start: a player without a keymap.ini gets the defaults written, so the
 * game and config.exe read the same keys. */
void cs_keys_startup(void);

/* A key's name for the screen ("W", "Space", "Left click", "Wheel down"). */
void cs_keys_name(int vk, char *out, size_t n);
/* Keys the game keeps for itself (Esc, F1, F11, Alt, Windows): never bound. */
int  cs_keys_reserved(int vk);
/* Put vk in control's slot; a key drives one control, so it leaves any other.
 * Returns the control it was taken from, or -1. */
int  cs_keys_assign(CsKeyMap *m, int control, int slot, int vk);

#endif
