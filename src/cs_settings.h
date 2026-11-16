/*
 * cs_settings.h -- the player's settings, shared by the game (F1 menu) and
 * config.exe.
 *
 * %LOCALAPPDATA%\CounterStrikeXB\settings.ini, [display]:
 *   resolution_scale  1..3      internal resolution: Low 1x (the Xbox's 640x480, default),
 *                               Medium 2x, High 3x
 *   fullscreen        0/1       borderless fullscreen at start (F11 / Alt+Enter toggle)
 *   stretch           0/1       fill the window instead of keeping 4:3
 *   vsync             0/1
 *   frame_rate        30, 60 (default), 72. The game's own limit (its fps_max)
 *                               is 72, and 72 is only offered on a monitor it
 *                               divides evenly (144 Hz) or one with adaptive sync
 *                               (FreeSync / G-Sync Compatible); elsewhere 60 is the top.
 *                               Older values (120..240, 0 = uncapped) read as 72.
 *   msaa              1, 2, 4, 8 samples
 *   anisotropy        1, 2, 4, 8, 16
 *   hires_textures    0/1       use the BC7 texture pack when one is installed (default 1)
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
    int frame_rate;   /* Hz: 30, 60, 72 */
    int msaa;         /* 1, 2, 4, 8 */
    int aniso;        /* 1, 2, 4, 8, 16 */
    int hires;        /* 0/1: hi-res texture pack */
} CsSettings;

/* The choices each option offers, in menu order, and their names. */
#define CS_SET_SCALES 3                               /* Low 1x (default), Medium 2x, High 3x */
#define CS_SET_RATES 3
#define CS_SET_MSAAS 4
#define CS_SET_ANISOS 5
extern const int CS_SET_RATE_HZ[CS_SET_RATES];        /* 30, 60, 72 */
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
/* settings.ini changed since this program last read or wrote it (config.exe or
 * RecompLauncher while the game runs): loads it into *out and returns 1. */
int cs_settings_changed_on_disk(CsSettings *out);
void cs_settings_mark_seen(void);
/* %LOCALAPPDATA%\CounterStrikeXB\<name>; makes the folder when `make_dir`. */
int cs_settings_path(const char *name, char *out, unsigned n, int make_dir);
/* The index of `v` in a choice list, or of the nearest. */
int cs_settings_index(const int *list, int n, int v);

/* The highest frame-rate cap that looks smooth on a monitor (an HMONITOR; NULL
 * for the main one): 72 when its EDID advertises adaptive sync (AMD FreeSync
 * data block, HDMI 2.1 VRR, DisplayID Adaptive-Sync) and it runs at 72 Hz or
 * more (120 Hz included), or when its refresh rate is a multiple of 72
 * (144 Hz); else 60. *hz and *vrr, when given, say why. */
int cs_settings_rate_max(void *monitor, int *hz, int *vrr);
/* How many of CS_SET_RATE_HZ a monitor with that top offers, and a setting
 * held to it. */
int cs_settings_rate_choices(int rate_max);
int cs_settings_rate_effective(int frame_rate, int rate_max);

/* The game's live copy (cs_settings.c), loaded at start. */
extern CsSettings g_cs_settings;
/* At start, before the window and input exist: load the settings and turn the
 * ones read at start into the toolkit's variables (RECOMP_FULLSCREEN,
 * RECOMP_KEYMAP), unless the environment already sets them. */
void cs_settings_startup(void);

#endif
