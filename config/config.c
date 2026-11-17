/* config.exe -- Counter-Strike's settings program, next to the game.
 *
 * Video tab: the options of the game's F1 menu, so they can be set before it
 * starts. Saved in %LOCALAPPDATA%\CounterStrikeXB\settings.ini, [display]
 * (src/cs_settings.c, shared with the game).
 * Controls tab: the keyboard. Each Xbox control has a key and an alternate,
 * saved in %LOCALAPPDATA%\CounterStrikeXB\keymap.ini, [keys], one line per
 * control in the toolkit's names ("a = Z, Space"), which the game reads at
 * start (RECOMP_KEYMAP, toolkit src/input/keyboard.c). Controls the file does
 * not name keep the defaults; a controller works as it always does.
 * Esc, F1 and F11 belong to the game's own menus and fullscreen. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cs_settings.h"
#include "input_map.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

void xbox_InputKeyboardBindings(InputBindings *b);   /* toolkit keyboard.c: the default keys */

/* ---- the Xbox controls, in the toolkit's order (InputControl) ------------------------------ */
static const char *const CONTROL_LABEL[INPUT_CONTROL_COUNT] = {
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "START (pause menu)", "BACK", "Left stick click (buy menu)", "Right stick click",
    "A (select in menus)", "B (back in menus)", "X", "Y", "Black", "White",
    "Left trigger", "Right trigger",
    "Left stick up (move forward)", "Left stick down (move back)",
    "Left stick left (strafe left)", "Left stick right (strafe right)",
    "Right stick up (look up)", "Right stick down (look down)",
    "Right stick left (turn left)", "Right stick right (turn right)",
};

/* ---- video options: the F1 menu's ------------------------------------------------------------ */
enum { V_SCALE, V_FULLSCREEN, V_STRETCH, V_VSYNC, V_RATE, V_MSAA, V_ANISO, V_HIRES, NVIDEO };
static const char *const VIDEO_LABEL[NVIDEO] = {
    "Render scale", "Fullscreen", "Stretch image", "VSync", "Frame rate cap", "Anti-aliasing", "Anisotropic filtering", "Hi-res textures" };
static const char *const T_OFFON[2] = { "Off", "On" };
static const char *const T_STRETCH[2] = { "Off (keep 4:3)", "On (fill the window)" };

enum { ID_TAB = 100, ID_LIST, ID_CHANGE, ID_ALT, ID_CLEAR, ID_RESET, ID_SAVE, ID_CLOSE, ID_STATUS, ID_VIDEO0 = 200, ID_REPORT = 330, ID_OTHER_PADS = 331,
       ID_MOUSE_AIM = 332, ID_MOUSE_SENS = 333, ID_MOUSE_INV = 334 };
static HWND s_other_pads;      /* [input] other_controllers: PlayStation and other pads through SDL3 */
static HWND s_mouse_aim, s_mouse_sens, s_mouse_inv;   /* [input] mouse_aim, mouse_sensitivity, mouse_invert */
enum { PAGE_VIDEO, PAGE_CONTROLS };

static CsSettings s_set;
static int s_key[INPUT_CONTROL_COUNT][2];
static int s_def[INPUT_CONTROL_COUNT][2];
static int s_dirty, s_wait_row = -1, s_wait_slot, s_page;
static HWND s_main, s_tab, s_list, s_status, s_combo[NVIDEO];
static HWND s_vpage[32], s_cpage[16];
static int s_nv, s_nc;

/* The top frame-rate cap for the main monitor (the game checks the monitor its window is on). */
static int rate_top(void)
{
    static int top;
    if (!top) top = cs_settings_rate_max(NULL, NULL, NULL);
    return top;
}

static int video_count(int i)
{
    switch (i) {
    case V_SCALE: return CS_SET_SCALES;
    case V_RATE: return cs_settings_rate_choices(rate_top());   /* 72 only where it is smooth */
    case V_MSAA: return CS_SET_MSAAS;
    case V_ANISO: return CS_SET_ANISOS;
    }
    return 2;
}

static const char *video_text(int i, int v)
{
    switch (i) {
    case V_SCALE: return CS_SET_SCALE_TEXT[v];
    case V_RATE: return CS_SET_RATE_TEXT[v];
    case V_MSAA: return CS_SET_MSAA_TEXT[v];
    case V_ANISO: return CS_SET_ANISO_TEXT[v];
    case V_STRETCH: return T_STRETCH[v];
    }
    return T_OFFON[v];
}

static int video_get(int i)
{
    switch (i) {
    case V_SCALE: return cs_settings_index(CS_SET_SCALE_X2, CS_SET_SCALES, s_set.scale_x2);
    case V_FULLSCREEN: return s_set.fullscreen;
    case V_STRETCH: return s_set.stretch;
    case V_VSYNC: return s_set.vsync;
    case V_RATE: return cs_settings_index(CS_SET_RATE_HZ, CS_SET_RATES, cs_settings_rate_effective(s_set.frame_rate, rate_top()));
    case V_MSAA: return cs_settings_index(CS_SET_MSAA_N, CS_SET_MSAAS, s_set.msaa);
    case V_ANISO: return cs_settings_index(CS_SET_ANISO_N, CS_SET_ANISOS, s_set.aniso);
    case V_HIRES: return s_set.hires;
    }
    return 0;
}

static void video_set(int i, int v)
{
    switch (i) {
    case V_SCALE: s_set.scale_x2 = CS_SET_SCALE_X2[v]; break;
    case V_FULLSCREEN: s_set.fullscreen = v; break;
    case V_STRETCH: s_set.stretch = v; break;
    case V_VSYNC: s_set.vsync = v; break;
    case V_RATE: s_set.frame_rate = CS_SET_RATE_HZ[v]; break;
    case V_MSAA: s_set.msaa = CS_SET_MSAA_N[v]; break;
    case V_ANISO: s_set.aniso = CS_SET_ANISO_N[v]; break;
    case V_HIRES: s_set.hires = v; break;
    }
}

/* ---- keys ------------------------------------------------------------------------------------- */
static void key_label(int vk, char *out, size_t n)
{
    const char *name = vk ? xbox_InputMap_KeyName(vk) : NULL;
    static const struct { int vk; const char *label; } mouse[] = {
        { 0x01, "Left click" }, { 0x02, "Right click" }, { 0x04, "Middle click" },
        { 0x05, "Mouse button 4" }, { 0x06, "Mouse button 5" },
        { 0x0E, "Wheel up" }, { 0x0F, "Wheel down" },
    };
    size_t m;
    if (!vk) { snprintf(out, n, "(none)"); return; }
    for (m = 0; m < sizeof mouse / sizeof mouse[0]; m++)
        if (mouse[m].vk == vk) { snprintf(out, n, "%s", mouse[m].label); return; }
    if (name) {
        size_t i;
        snprintf(out, n, "%s", name);
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
        for (i = 1; out[i]; i++) if (out[i] == '_') out[i] = ' ';
        return;
    }
    snprintf(out, n, "Key %d", vk);
}

static void load_keys(void)
{
    InputBindings def, cur;
    int c;
    _putenv("RECOMP_KEYMAP=");                  /* the toolkit's defaults, not a file */
    xbox_InputKeyboardBindings(&def);
    for (c = 0; c < INPUT_CONTROL_COUNT; c++) {
        s_def[c][0] = def.count[c] > 0 ? def.key[c][0] : 0;
        s_def[c][1] = def.count[c] > 1 ? def.key[c][1] : 0;
        s_key[c][0] = s_def[c][0]; s_key[c][1] = s_def[c][1];
    }
    {   /* the player's file, read with the toolkit's own parser */
        char path[MAX_PATH], line[512];
        FILE *f;
        int in_keys = 0;
        if (!cs_settings_path("keymap.ini", path, sizeof path, 0) || !(f = fopen(path, "r"))) return;
        memset(&cur, 0, sizeof cur);
        while (fgets(line, sizeof line, f)) {
            char *p = line, *eq, *e;
            while (*p == ' ' || *p == '\t') p++;
            for (e = p + strlen(p); e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) ;
            *e = 0;
            if (*p == '[') { in_keys = !strncmp(p, "[keys]", 6); continue; }
            if (!in_keys || !(eq = strchr(p, '='))) continue;
            for (e = eq; e > p && (e[-1] == ' ' || e[-1] == '\t'); e--) ;
            *e = 0;
            c = xbox_InputMap_ControlFromName(p);
            if (c < 0) continue;
            for (p = eq + 1; *p == ' ' || *p == '\t'; p++) ;
            xbox_InputMap_BindingsParse(&cur, c, p);
            s_key[c][0] = cur.count[c] > 0 ? cur.key[c][0] : 0;
            s_key[c][1] = cur.count[c] > 1 ? cur.key[c][1] : 0;
        }
        fclose(f);
    }
}

static int save_keys(void)
{
    char path[MAX_PATH];
    FILE *f;
    int c;
    if (!cs_settings_path("keymap.ini", path, sizeof path, 1) || !(f = fopen(path, "w"))) return 0;
    fprintf(f, "; Counter-Strike keyboard (config.exe). One line per Xbox control: up to two keys.\n[keys]\n");
    for (c = 0; c < INPUT_CONTROL_COUNT; c++) {
        const char *k0 = s_key[c][0] ? xbox_InputMap_KeyName(s_key[c][0]) : NULL;
        const char *k1 = s_key[c][1] ? xbox_InputMap_KeyName(s_key[c][1]) : NULL;
        fprintf(f, "%s = ", xbox_InputMap_ControlName(c));
        if (k0 && k1) fprintf(f, "%s, %s\n", k0, k1);
        else if (k0 || k1) fprintf(f, "%s\n", k0 ? k0 : k1);
        else fprintf(f, "none\n");
    }
    return fclose(f) == 0;
}

/* ---- the Controls tab ------------------------------------------------------------------------- */
static void status(const char *s) { SetWindowTextA(s_status, s); }

static void refresh_row(int i)
{
    char t[64];
    key_label(s_key[i][0], t, sizeof t); ListView_SetItemText(s_list, i, 1, t);
    key_label(s_key[i][1], t, sizeof t); ListView_SetItemText(s_list, i, 2, t);
}

static void refresh_all(void)
{
    int i;
    for (i = 0; i < INPUT_CONTROL_COUNT; i++) refresh_row(i);
}

static void begin_wait(int slot)
{
    char m[200];
    int row = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
    if (row < 0) { status("Select a control in the list first."); return; }
    s_wait_row = row; s_wait_slot = slot;
    snprintf(m, sizeof m, "Press the new %s for \"%s\", or click or turn the wheel for a mouse button "
             "(Esc cancels).", slot ? "alternate key" : "key", CONTROL_LABEL[row]);
    status(m);
    SetFocus(s_main);
    SetCapture(s_main);                          /* a click anywhere is the answer, not a click on a control */
}

static void assign(int vk)
{
    char m[220], n[64];
    int i, s, moved = 0;
    if (vk == VK_ESCAPE) { s_wait_row = -1; ReleaseCapture(); status("Cancelled."); return; }
    if (vk == VK_F1 || vk == VK_F11 || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        status("That key belongs to the game itself (F1 settings, F11 or Alt+Enter fullscreen, Esc quit). Press another key.");
        return;
    }
    if (!xbox_InputMap_KeyName(vk)) { status("That key can't be used. Press another key."); return; }
    for (i = 0; i < INPUT_CONTROL_COUNT; i++)          /* one key drives one control: take it from the old one */
        for (s = 0; s < 2; s++)
            if (s_key[i][s] == vk && !(i == s_wait_row && s == s_wait_slot)) {
                s_key[i][s] = 0;
                refresh_row(i);
                key_label(vk, n, sizeof n);
                snprintf(m, sizeof m, "%s moved here from \"%s\".", n, CONTROL_LABEL[i]);
                status(m);
                moved = 1;
            }
    s_key[s_wait_row][s_wait_slot] = vk;
    refresh_row(s_wait_row);
    if (!moved) {
        key_label(vk, n, sizeof n);
        snprintf(m, sizeof m, "\"%s\" is now %s.", CONTROL_LABEL[s_wait_row], n);
        status(m);
    }
    s_wait_row = -1;
    ReleaseCapture();
    s_dirty = 1;
}

/* ---- pages, reset, save ----------------------------------------------------------------------- */
static void show_page(int page)
{
    int i;
    s_page = page; s_wait_row = -1;
    for (i = 0; i < s_nv; i++) ShowWindow(s_vpage[i], page == PAGE_VIDEO ? SW_SHOW : SW_HIDE);
    for (i = 0; i < s_nc; i++) ShowWindow(s_cpage[i], page == PAGE_CONTROLS ? SW_SHOW : SW_HIDE);
    status(page == PAGE_VIDEO
           ? "Video: the same options as the game's F1 menu. Saved changes apply the next time the game starts."
           : "Controls: select a control, click Change key (or double-click it), then press the key you want. "
             "Controllers need no setup.");
}

static void do_reset(void)
{
    int i;
    if (s_page == PAGE_VIDEO) {
        cs_settings_defaults(&s_set);
        for (i = 0; i < NVIDEO; i++) SendMessageA(s_combo[i], CB_SETCURSEL, (WPARAM)video_get(i), 0);
        status("Default video settings restored. Click Save to keep them.");
    } else {
        for (i = 0; i < INPUT_CONTROL_COUNT; i++) { s_key[i][0] = s_def[i][0]; s_key[i][1] = s_def[i][1]; }
        refresh_all();
        status("Default keys restored. Click Save to keep them.");
    }
    s_dirty = 1;
}

static int do_save(void)
{
    char ini[MAX_PATH];
    int other = SendMessageA(s_other_pads, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!cs_settings_save(&s_set) || !save_keys() || !cs_settings_path("settings.ini", ini, sizeof ini, 1)
        || !WritePrivateProfileStringA("input", "other_controllers", other ? "1" : "0", ini)) {
        MessageBoxA(s_main, "The settings could not be saved.", "Counter-Strike", MB_ICONERROR);
        return 0;
    }
    s_dirty = 0;
    status("Saved. The new settings apply the next time the game starts.");
    return 1;
}

/* ---- window ----------------------------------------------------------------------------------- */
static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | style, x, y, w, h, s_main, (HMENU)(INT_PTR)id, NULL, NULL);
    if (f) SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void make_ui(HWND h)
{
    LVCOLUMNA col;
    LVITEMA it;
    TCITEMA ti;
    int i, j;
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    s_tab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                            10, 10, 580, 500, h, (HMENU)(INT_PTR)ID_TAB, NULL, NULL);
    SendMessageA(s_tab, WM_SETFONT, (WPARAM)f, TRUE);
    memset(&ti, 0, sizeof ti);
    ti.mask = TCIF_TEXT;
    ti.pszText = "Video";    SendMessageA(s_tab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
    ti.pszText = "Controls"; SendMessageA(s_tab, TCM_INSERTITEMA, 1, (LPARAM)&ti);

    s_vpage[s_nv++] = mk("STATIC", "These are the same as the game's F1 menu (F1 while playing changes them live).",
                         SS_LEFT, 30, 52, 540, 30, 0, f);
    for (i = 0; i < NVIDEO; i++) {
        int y = 92 + i * 40;
        s_vpage[s_nv++] = mk("STATIC", VIDEO_LABEL[i], SS_LEFT, 40, y + 4, 180, 22, 0, f);
        s_combo[i] = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 230, y, 320, 200, ID_VIDEO0 + i, f);
        for (j = 0; j < video_count(i); j++) SendMessageA(s_combo[i], CB_ADDSTRING, 0, (LPARAM)video_text(i, j));
        SendMessageA(s_combo[i], CB_SETCURSEL, (WPARAM)video_get(i), 0);
        s_vpage[s_nv++] = s_combo[i];
    }
    s_vpage[s_nv++] = mk("STATIC",
        "Render scale: pick your screen (480p is the Xbox's own 640x480); higher needs a faster graphics card. Anti-aliasing smooths edges and anisotropic "
        "filtering keeps distant textures sharp; both cost a little GPU time. Fullscreen fills the screen without a border "
        "(F11 or Alt+Enter switch while playing). Frame rate cap: 72 is the game's own limit and is offered on 144 Hz or FreeSync/G-Sync monitors; elsewhere 60 is the smooth choice. Hi-res textures uses the "
        "texture_pack folder beside the game when it is there.",
        SS_LEFT, 30, 416, 540, 84, 0, f);

    s_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             20, 48, 560, 400, h, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
    SendMessageA(s_list, WM_SETFONT, (WPARAM)f, TRUE);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = "Xbox control"; col.cx = 250; SendMessageA(s_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = "Key"; col.cx = 145; SendMessageA(s_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.pszText = "Alternate key"; col.cx = 145; SendMessageA(s_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    for (i = 0; i < INPUT_CONTROL_COUNT; i++) {
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (char *)CONTROL_LABEL[i];
        SendMessageA(s_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }
    refresh_all();
    ListView_SetItemState(s_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    s_cpage[s_nc++] = s_list;
    s_cpage[s_nc++] = mk("BUTTON", "Change key", WS_TABSTOP | BS_PUSHBUTTON, 20, 458, 100, 28, ID_CHANGE, f);
    s_cpage[s_nc++] = mk("BUTTON", "Change alternate", WS_TABSTOP | BS_PUSHBUTTON, 126, 458, 120, 28, ID_ALT, f);
    s_cpage[s_nc++] = mk("BUTTON", "Clear", WS_TABSTOP | BS_PUSHBUTTON, 252, 458, 70, 28, ID_CLEAR, f);
    s_other_pads = mk("BUTTON", "PlayStation and other controllers", WS_TABSTOP | BS_AUTOCHECKBOX, 340, 461, 240, 22, ID_OTHER_PADS, f);
    {
        char ini[MAX_PATH];
        int on = !cs_settings_path("settings.ini", ini, sizeof ini, 0) || GetPrivateProfileIntA("input", "other_controllers", 1, ini);
        SendMessageA(s_other_pads, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    s_cpage[s_nc++] = s_other_pads;
    s_mouse_aim = mk("BUTTON", "Mouse aim (not on Xbox Live)", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 490, 200, 22, ID_MOUSE_AIM, f);
    SendMessageA(s_mouse_aim, BM_SETCHECK, s_set.mouse_aim ? BST_CHECKED : BST_UNCHECKED, 0);
    s_cpage[s_nc++] = s_mouse_aim;
    s_cpage[s_nc++] = mk("STATIC", "Sensitivity", SS_LEFT, 236, 493, 66, 20, 0, f);
    s_mouse_sens = mk("COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 304, 489, 76, 200, ID_MOUSE_SENS, f);
    for (i = 0; i < CS_SET_SENSES; i++) SendMessageA(s_mouse_sens, CB_ADDSTRING, 0, (LPARAM)CS_SET_SENS_TEXT[i]);
    SendMessageA(s_mouse_sens, CB_SETCURSEL, (WPARAM)cs_settings_index(CS_SET_SENS_PCT, CS_SET_SENSES, s_set.mouse_sens), 0);
    s_cpage[s_nc++] = s_mouse_sens;
    s_mouse_inv = mk("BUTTON", "Invert mouse", WS_TABSTOP | BS_AUTOCHECKBOX, 396, 490, 110, 22, ID_MOUSE_INV, f);
    SendMessageA(s_mouse_inv, BM_SETCHECK, s_set.mouse_invert ? BST_CHECKED : BST_UNCHECKED, 0);
    s_cpage[s_nc++] = s_mouse_inv;

    s_status = mk("STATIC", "", SS_LEFT | WS_VISIBLE, 10, 520, 580, 20, ID_STATUS, f);
    mk("BUTTON", "Reset to defaults", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 10, 548, 130, 28, ID_RESET, f);
    mk("BUTTON", "Report a problem...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 150, 548, 140, 28, ID_REPORT, f);
    mk("BUTTON", "Save", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 444, 548, 70, 28, ID_SAVE, f);
    mk("BUTTON", "Close", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 520, 548, 70, 28, ID_CLOSE, f);
    show_page(PAGE_VIDEO);
}

#include "report.inc"

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        s_main = h;
        make_ui(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= ID_VIDEO0 && LOWORD(w) < ID_VIDEO0 + NVIDEO) {
            if (HIWORD(w) == CBN_SELCHANGE) {
                int i = LOWORD(w) - ID_VIDEO0;
                video_set(i, (int)SendMessageA(s_combo[i], CB_GETCURSEL, 0, 0));
                s_dirty = 1;
                status("Changed. Click Save to keep it.");
            }
            return 0;
        }
        switch (LOWORD(w)) {
        case ID_CHANGE: begin_wait(0); return 0;
        case ID_ALT:    begin_wait(1); return 0;
        case ID_CLEAR: {
            int row = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
            if (row < 0) { status("Select a control in the list first."); return 0; }
            s_key[row][0] = s_key[row][1] = 0;
            refresh_row(row);
            s_dirty = 1;
            status("Cleared. That control has no key until you give it one.");
            return 0;
        }
        case ID_MOUSE_AIM:
            s_set.mouse_aim = SendMessageA(s_mouse_aim, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s_dirty = 1;
            status(s_set.mouse_aim
                   ? "Mouse aim on: the mouse aims and its buttons fire. Xbox Live will not sign in while it is on "
                     "(it is controller only there); single player and System Link keep it. Click Save to keep it."
                   : "Mouse aim off. Click Save to keep it.");
            return 0;
        case ID_MOUSE_SENS:
            if (HIWORD(w) == CBN_SELCHANGE) {
                int v = (int)SendMessageA(s_mouse_sens, CB_GETCURSEL, 0, 0);
                if (v >= 0 && v < CS_SET_SENSES) s_set.mouse_sens = CS_SET_SENS_PCT[v];
                s_dirty = 1;
            }
            return 0;
        case ID_MOUSE_INV:
            s_set.mouse_invert = SendMessageA(s_mouse_inv, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s_dirty = 1;
            return 0;
        case ID_OTHER_PADS:
            s_dirty = 1;
            status("PlayStation, Switch Pro and other controllers work beside Xbox ones. Turn this off if Steam Input "
                   "or DS4Windows already makes your pad an Xbox one (it would count twice). Click Save to keep it.");
            return 0;
        case ID_RESET: do_reset(); return 0;
        case ID_REPORT: do_report(h); return 0;
        case ID_SAVE:  do_save(); return 0;
        case ID_CLOSE: PostMessageA(h, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        /* labels on a tab page take the page's white, not the dialog grey */
        if (GetDlgCtrlID((HWND)l) != ID_STATUS) {
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)l)->idFrom == ID_TAB && ((NMHDR *)l)->code == TCN_SELCHANGE) {
            show_page((int)SendMessageA(s_tab, TCM_GETCURSEL, 0, 0));
        } else if (((NMHDR *)l)->idFrom == ID_LIST && ((NMHDR *)l)->code == NM_DBLCLK) {
            NMITEMACTIVATE *a = (NMITEMACTIVATE *)l;
            LVHITTESTINFO ht;
            memset(&ht, 0, sizeof ht);
            ht.pt = a->ptAction;
            if (ListView_SubItemHitTest(s_list, &ht) >= 0 && ht.iItem >= 0) {
                ListView_SetItemState(s_list, ht.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                begin_wait(ht.iSubItem == 2 ? 1 : 0);
            }
        }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (s_wait_row >= 0) {
            if (!(l & (1L << 30)))
                assign((int)w);
            return 0;
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (s_wait_row >= 0) return 0;
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (s_wait_row >= 0) {                   /* a mouse button as the key (keymap.ini mouse1..mouse5) */
            assign(m == WM_LBUTTONDOWN ? 0x01 : m == WM_RBUTTONDOWN ? 0x02 : m == WM_MBUTTONDOWN ? 0x04
                   : HIWORD(w) == XBUTTON1 ? 0x05 : 0x06);
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (s_wait_row >= 0) {                   /* mwheelup / mwheeldown */
            assign(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0x0E : 0x0F);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (s_wait_row >= 0 && (HWND)l != h) { s_wait_row = -1; status("Cancelled."); }
        break;
    case WM_CLOSE:
        if (s_dirty) {
            int r = MessageBoxA(h, "Save your changes before closing?", "Counter-Strike", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !do_save()) return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    RECT r = { 0, 0, 600, 590 };
    (void)prev; (void)cmd;

    InitCommonControlsEx(&ic);
    cs_settings_load(&s_set);
    load_keys();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));     /* the game's icon (resources/config.rc) */
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CounterStrikeXBConfig";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowExA(0, wc.lpszClassName, "Counter-Strike - Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(s_main, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (s_wait_row < 0 && IsDialogMessageA(s_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
