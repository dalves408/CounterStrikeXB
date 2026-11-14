/* cs_settings.c -- settings.ini for the game and config.exe (see cs_settings.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cs_settings.h"

const int CS_SET_RATE_HZ[CS_SET_RATES] = { 30, 60, 120, 144, 240, 0 };
const int CS_SET_MSAA_N[CS_SET_MSAAS] = { 1, 2, 4, 8 };
const int CS_SET_ANISO_N[CS_SET_ANISOS] = { 1, 2, 4, 8, 16 };
const char *const CS_SET_SCALE_TEXT[CS_SET_SCALES] = {
    "Low  (1x, 640x480)", "Medium  (2x, 1280x960)", "High  (3x, 1920x1440)" };
const char *const CS_SET_RATE_TEXT[CS_SET_RATES] = {
    "30", "60", "120", "144", "240", "Uncapped" };
const char *const CS_SET_MSAA_TEXT[CS_SET_MSAAS] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
const char *const CS_SET_ANISO_TEXT[CS_SET_ANISOS] = { "Off", "2x", "4x", "8x", "16x" };

CsSettings g_cs_settings;

void cs_settings_defaults(CsSettings *s)
{
    s->scale = 1;
    s->fullscreen = 0;
    s->stretch = 0;
    s->vsync = 0;
    s->frame_rate = 60;
    s->msaa = 1;
    s->aniso = 1;
}

int cs_settings_path(const char *name, char *out, unsigned n, int make_dir)
{
    char base[MAX_PATH];
    DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!got || got >= sizeof base - 48)
        return 0;
    snprintf(out, n, "%s\\CounterStrikeXB", base);
    if (make_dir)
        CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\CounterStrikeXB\\%s", base, name);
    return 1;
}

int cs_settings_index(const int *list, int n, int v)
{
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < n; i++) {
        int d = abs(list[i] - v);
        if (list[i] == 0 && v != 0) d = 1 << 29;     /* 0 (uncapped) only matches itself */
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int get(const char *path, const char *key, int def)
{
    return (int)GetPrivateProfileIntA("display", key, def, path);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cs_settings_load(CsSettings *s)
{
    char path[MAX_PATH];
    cs_settings_defaults(s);
    if (!cs_settings_path("settings.ini", path, sizeof path, 0))
        return;
    s->scale = clampi(get(path, "resolution_scale", s->scale), 1, CS_SET_SCALES);
    s->fullscreen = get(path, "fullscreen", s->fullscreen) != 0;
    s->stretch = get(path, "stretch", s->stretch) != 0;
    s->vsync = get(path, "vsync", s->vsync) != 0;
    s->frame_rate = CS_SET_RATE_HZ[cs_settings_index(CS_SET_RATE_HZ, CS_SET_RATES, get(path, "frame_rate", s->frame_rate))];
    s->msaa = CS_SET_MSAA_N[cs_settings_index(CS_SET_MSAA_N, CS_SET_MSAAS, get(path, "msaa", s->msaa))];
    s->aniso = CS_SET_ANISO_N[cs_settings_index(CS_SET_ANISO_N, CS_SET_ANISOS, get(path, "anisotropy", s->aniso))];
}

static int put(const char *path, const char *key, int v)
{
    char val[16];
    snprintf(val, sizeof val, "%d", v);
    return WritePrivateProfileStringA("display", key, val, path) != 0;
}

int cs_settings_save(const CsSettings *s)
{
    char path[MAX_PATH];
    int ok;
    if (!cs_settings_path("settings.ini", path, sizeof path, 1))
        return 0;
    ok = put(path, "resolution_scale", s->scale);
    ok &= put(path, "fullscreen", s->fullscreen);
    ok &= put(path, "stretch", s->stretch);
    ok &= put(path, "vsync", s->vsync);
    ok &= put(path, "frame_rate", s->frame_rate);
    ok &= put(path, "msaa", s->msaa);
    ok &= put(path, "anisotropy", s->aniso);
    return ok;
}

void cs_settings_startup(void)
{
    char path[MAX_PATH];
    cs_settings_load(&g_cs_settings);
    /* _putenv_s: the toolkit reads these with getenv, the CRT's own copy. */
    if (!getenv("RECOMP_FULLSCREEN"))
        _putenv_s("RECOMP_FULLSCREEN", g_cs_settings.fullscreen ? "1" : "0");
    if (!getenv("RECOMP_KEYMAP") && cs_settings_path("keymap.ini", path, sizeof path, 0)
        && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        _putenv_s("RECOMP_KEYMAP", path);
    fprintf(stderr, "[CS-SET] scale %dx, fullscreen %d, stretch %d, vsync %d, frame rate %d, msaa %d, aniso %d\n",
            g_cs_settings.scale, g_cs_settings.fullscreen, g_cs_settings.stretch, g_cs_settings.vsync,
            g_cs_settings.frame_rate, g_cs_settings.msaa, g_cs_settings.aniso);
}
