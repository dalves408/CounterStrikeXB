/* cs_mouse.h -- mouse aim, kept off Xbox Live (cs_mouse.c). */
#ifndef CS_MOUSE_H
#define CS_MOUSE_H

#include <stdint.h>

void cs_mouse_init(void);    /* at start, after the settings are loaded */
void cs_mouse_apply(void);   /* after the mouse settings change */
void cs_mouse_look(void);    /* the client's look update's entry hook (recomp_manual.c) */
/* Outside play with mouse aim on: 1 when the left button is held (it is
 * START there), 2 the right (B). 0 in a match, where they are the key map's. */
int  cs_mouse_menu_clicks(void);
/* A UI command the game is about to run (guest address of its text): 1 to
 * refuse it (an Xbox Live page while mouse aim is on; the notice opens). */
int  cs_mouse_ui_command(uint32_t text_va);
/* sub_000CA3B0's wrapper (recomp_manual.c): 1 when the command in edx was
 * refused and the call already returned (esp past its return address). */
int  cs_mouse_ui_refuse(void);

#endif
