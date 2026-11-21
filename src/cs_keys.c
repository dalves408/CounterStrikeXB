/*
 * cs_keys.c -- see cs_keys.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_keys.h"
#include "cs_settings.h"

/* The manual's Controls page, in its order: what each Xbox control does. */
const CsKeyAction cs_key_actions[] = {
    { INPUT_LSTICK_UP,     "Move forward",               "Movement" },
    { INPUT_LSTICK_DOWN,   "Move back",                  NULL },
    { INPUT_LSTICK_LEFT,   "Strafe left",                NULL },
    { INPUT_LSTICK_RIGHT,  "Strafe right",               NULL },
    { INPUT_A,             "Jump",                       NULL },
    { INPUT_LEFT_TRIGGER,  "Crouch",                     NULL },
    { INPUT_LEFT_THUMB,    "Walk (hold) / Buy menu",     NULL },
    { INPUT_RIGHT_TRIGGER, "Fire weapon",                "Combat" },
    { INPUT_B,             "Alternate fire",             NULL },
    { INPUT_X,             "Reload / use",               NULL },
    { INPUT_Y,             "Cycle weapons",              NULL },
    { INPUT_BLACK,         "Cycle grenades",             NULL },
    { INPUT_WHITE,         "Drop weapon",                NULL },
    { INPUT_RIGHT_THUMB,   "Fast look (hold)",           NULL },
    { INPUT_RSTICK_UP,     "Look up",                    "Look" },
    { INPUT_RSTICK_DOWN,   "Look down",                  NULL },
    { INPUT_RSTICK_LEFT,   "Turn left",                  NULL },
    { INPUT_RSTICK_RIGHT,  "Turn right",                 NULL },
    { INPUT_START,         "In-game menu / team",        "Menus and radio" },
    { INPUT_BACK,          "Current scores",             NULL },
    { INPUT_DPAD_UP,       "Radio: Follow me",           NULL },
    { INPUT_DPAD_DOWN,     "Radio: Hold position",       NULL },
    { INPUT_DPAD_LEFT,     "Radio: Report in",           NULL },
    { INPUT_DPAD_RIGHT,    "Radio: Taking fire",         NULL },
};
const int cs_key_action_count = (int)(sizeof cs_key_actions / sizeof cs_key_actions[0]);

/* The PC game's keys where an Xbox control does the same thing. The d-pad
 * keeps the arrows (the game's menus move with it); the right stick keeps
 * I/J/K/L for playing without mouse aim. The mouse counts only while mouse aim
 * is on. PC keys with no Xbox control (flashlight F, night vision N, chat
 * Y/U) are not here: the Xbox game has no such action. */
#define VK_MOUSE1 0x01
#define VK_MOUSE2 0x02
#define VK_MOUSE3 0x04
#define VK_WHEELUP 0x0E
#define VK_WHEELDOWN 0x0F
void cs_keys_defaults(CsKeyMap *m)
{
    static const struct { int control; int vk[CSK_SLOTS]; } d[] = {
        { INPUT_LSTICK_UP,     { 'W' } },
        { INPUT_LSTICK_DOWN,   { 'S' } },
        { INPUT_LSTICK_LEFT,   { 'A' } },
        { INPUT_LSTICK_RIGHT,  { 'D' } },
        { INPUT_A,             { 0x20 } },                    /* Space: jump */
        { INPUT_LEFT_TRIGGER,  { 0x11 } },                    /* Ctrl: duck */
        { INPUT_LEFT_THUMB,    { 0x10, 'B' } },               /* Shift: walk; B: buy menu */
        { INPUT_RIGHT_TRIGGER, { VK_MOUSE1 } },               /* fire */
        { INPUT_B,             { VK_MOUSE2, 0x08 } },         /* alternate fire; Backspace: back in menus */
        { INPUT_X,             { 'R', 'E' } },                /* reload; use */
        { INPUT_Y,             { 'Q', VK_WHEELDOWN, VK_WHEELUP } },   /* last / next / previous weapon */
        { INPUT_BLACK,         { '4' } },                     /* grenades, the PC's slot 4 */
        { INPUT_WHITE,         { 'G' } },                     /* drop weapon */
        { INPUT_RIGHT_THUMB,   { VK_MOUSE3 } },
        { INPUT_RSTICK_UP,     { 'I' } },
        { INPUT_RSTICK_DOWN,   { 'K' } },
        { INPUT_RSTICK_LEFT,   { 'J' } },
        { INPUT_RSTICK_RIGHT,  { 'L' } },
        { INPUT_START,         { 0x0D, 'M' } },               /* Enter; M: choose team */
        { INPUT_BACK,          { 0x09 } },                    /* Tab: scores */
        { INPUT_DPAD_UP,       { 0x26, 'Z' } },               /* radio: the PC's Z, X, C menus */
        { INPUT_DPAD_DOWN,     { 0x28, 'X' } },
        { INPUT_DPAD_LEFT,     { 0x25, 'C' } },
        { INPUT_DPAD_RIGHT,    { 0x27, 'V' } },
    };
    size_t i;
    memset(m, 0, sizeof *m);
    for (i = 0; i < sizeof d / sizeof d[0]; i++)
        memcpy(m->vk[d[i].control], d[i].vk, sizeof d[i].vk);
}

int cs_keys_load(CsKeyMap *m)
{
    char path[260], line[512];
    FILE *f;
    int in_keys = 0;
    cs_keys_defaults(m);
    if (!cs_settings_path("keymap.ini", path, sizeof path, 0) || !(f = fopen(path, "r"))) return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *e;
        InputBindings b;
        int c, s;
        while (*p == ' ' || *p == '\t') p++;
        for (e = p + strlen(p); e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); e--) ;
        *e = 0;
        if (*p == '[') { in_keys = !strncmp(p, "[keys]", 6); continue; }
        if (!in_keys || !(eq = strchr(p, '='))) continue;
        for (e = eq; e > p && (e[-1] == ' ' || e[-1] == '\t'); e--) ;
        *e = 0;
        if ((c = xbox_InputMap_ControlFromName(p)) < 0) continue;
        for (p = eq + 1; *p == ' ' || *p == '\t'; p++) ;
        memset(&b, 0, sizeof b);
        xbox_InputMap_BindingsParse(&b, c, p);       /* the toolkit's own reading */
        for (s = 0; s < CSK_SLOTS; s++) m->vk[c][s] = s < b.count[c] ? b.key[c][s] : 0;
    }
    fclose(f);
    return 1;
}

int cs_keys_save(const CsKeyMap *m)
{
    char path[260];
    FILE *f;
    int c, s;
    if (!cs_settings_path("keymap.ini", path, sizeof path, 1) || !(f = fopen(path, "w"))) return 0;
    fprintf(f, "; Counter-Strike keyboard and mouse (F1 menu, config.exe). One line per Xbox control.\n[keys]\n");
    for (c = 0; c < INPUT_CONTROL_COUNT; c++) {
        int any = 0;
        fprintf(f, "%s = ", xbox_InputMap_ControlName(c));
        for (s = 0; s < CSK_SLOTS; s++) {
            const char *n = m->vk[c][s] ? xbox_InputMap_KeyName(m->vk[c][s]) : NULL;
            if (!n) continue;
            fprintf(f, "%s%s", any ? ", " : "", n);
            any = 1;
        }
        fprintf(f, "%s\n", any ? "" : "none");
    }
    return fclose(f) == 0;
}

void cs_keys_startup(void)
{
    char path[260];
    FILE *f;
    CsKeyMap m;
    if (!cs_settings_path("keymap.ini", path, sizeof path, 0)) return;
    if ((f = fopen(path, "r")) != NULL) { fclose(f); return; }
    cs_keys_defaults(&m);
    cs_keys_save(&m);
}

void cs_keys_name(int vk, char *out, size_t n)
{
    static const struct { int vk; const char *label; } special[] = {
        { 0x01, "Left click" }, { 0x02, "Right click" }, { 0x04, "Middle click" },
        { 0x05, "Mouse 4" }, { 0x06, "Mouse 5" }, { 0x0E, "Wheel up" }, { 0x0F, "Wheel down" },
        { 0x10, "Shift" }, { 0x11, "Ctrl" }, { 0x20, "Space" }, { 0x0D, "Enter" }, { 0x08, "Backspace" },
        { 0x09, "Tab" }, { 0x26, "Up" }, { 0x28, "Down" }, { 0x25, "Left" }, { 0x27, "Right" },
    };
    const char *name;
    size_t i;
    if (!vk) { snprintf(out, n, "-"); return; }
    for (i = 0; i < sizeof special / sizeof special[0]; i++)
        if (special[i].vk == vk) { snprintf(out, n, "%s", special[i].label); return; }
    if ((name = xbox_InputMap_KeyName(vk)) != NULL) {
        snprintf(out, n, "%s", name);
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
        for (i = 1; out[i]; i++) if (out[i] == '_') out[i] = ' ';
        return;
    }
    snprintf(out, n, "Key %d", vk);
}

int cs_keys_reserved(int vk)
{
    return vk == 0x1B || vk == 0x70 || vk == 0x7A || vk == 0x12 || vk == 0xA4 || vk == 0xA5 ||
           vk == 0x5B || vk == 0x5C || !xbox_InputMap_KeyName(vk);
}

int cs_keys_assign(CsKeyMap *m, int control, int slot, int vk)
{
    int c, s, from = -1;
    for (c = 0; c < INPUT_CONTROL_COUNT; c++)
        for (s = 0; s < CSK_SLOTS; s++)
            if (m->vk[c][s] == vk && !(c == control && s == slot)) { m->vk[c][s] = 0; from = c; }
    m->vk[control][slot] = vk;
    return from;
}
