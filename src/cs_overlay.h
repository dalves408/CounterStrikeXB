#ifndef CS_OVERLAY_H
#define CS_OVERLAY_H
/* ESC (quit prompt) and F1 (settings) menus over the game window (cs_overlay.c). */
void cs_overlay_init(void);     /* at start: takes the window's keys first */
int  cs_overlay_active(void);   /* a menu is open: the game is paused */
void cs_settings_watch(void);   /* between frames: apply settings.ini changed by config.exe or RecompLauncher */
void cs_overlay_live_refused(void); /* any thread: Xbox Live refused for mouse aim; F1 opens on it, saying why */
#endif
