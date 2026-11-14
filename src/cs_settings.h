/*
 * cs_settings.h -- the player's settings, shared by the game (F1 menu) and
 * config.exe.
 *
 * %LOCALAPPDATA%\CounterStrikeXB\settings.ini, [display]:
 *   resolution_scale  1..4      internal resolution, a multiple of the Xbox's 640x480
 *   fullscreen        0/1       borderless fullscreen at start (F11 / Alt+Enter toggle)
 *   stretch           0/1       fill the window instead of keeping 4:3
 *   vsync             0/1
 *   frame_rate        30, 60, 120, 144, 240, 0 = uncapped
 *   msaa              1, 2, 4, 8 samples
 *   anisotropy        1, 2, 4, 8, 16
 * %LOCALAPPDATA%\CounterStrikeXB\keymap.ini, [keys]: the keyboard, one line
 * per Xbox control in the toolkit's names (src/input/input_map.h), read by the
 * toolkit at start through RECOMP_KEYMAP.
 *
 * No Windows types here: config.exe includes it too.
 */
#ifndef CS_SETTINGS_H
#define CS_SETTINGS_H

typedef struct {
    int scale;        /* 1..4 */
    int fullscreen;   /* 0/1 */
    int stretch;      /* 0/1 */
    int vsync;        /* 0/1 */
    int frame_rate;   /* Hz, 0 = uncapped */
    int msaa;         /* 1, 2, 4, 8 */
    int aniso;        /* 1, 2, 4, 8, 16 */
} CsSettings;

/* The choices each option offers, in menu order, and their names. */
#define CS_SET_SCALES 4
#define CS_SET_RATES 6
#define CS_SET_MSAAS 4
#define CS_SET_ANISOS 5
extern const int CS_SET_RATE_HZ[CS_SET_RATES];        /* 30, 60, 120, 144, 240, 0 */
extern const int CS_SET_MSAA_N[CS_SET_MSAAS];         /* 1, 2, 4, 8 */
extern const int CS_SET_ANISO_N[CS_SET_ANISOS];       /* 1, 2, 4, 8, 16 */
extern const char *const CS_SET_SCALE_TEXT[CS_SET_SCALES];
extern const char *const CS_SET_RATE_TEXT[CS_SET_RATES];
extern const char *const CS_SET_MSAA_TEXT[CS_SET_MSAAS];
extern const char *const CS_SET_ANISO_TEXT[CS_SET_ANISOS];

void cs_settings_defaults(CsSettings *s);
/* Read settings.ini (missing keys keep their defaults, odd values snap to the
 * nearest choice). */
void cs_settings_load(CsSettings *s);
/* Write every key; 1 on success. */
int cs_settings_save(const CsSettings *s);
/* %LOCALAPPDATA%\CounterStrikeXB\<name>; makes the folder when `make_dir`. */
int cs_settings_path(const char *name, char *out, unsigned n, int make_dir);
/* The index of `v` in a choice list, or of the nearest. */
int cs_settings_index(const int *list, int n, int v);

/* The game's live copy (cs_settings.c), loaded at start. */
extern CsSettings g_cs_settings;
/* At start, before the window and input exist: load the settings and turn the
 * ones read at start into the toolkit's variables (RECOMP_FULLSCREEN,
 * RECOMP_KEYMAP), unless the environment already sets them. */
void cs_settings_startup(void);

#endif
