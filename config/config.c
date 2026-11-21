/* config.exe -- Counter-Strike's settings program, next to the game.
 *
 * Drawn in the look of the game's manual (src/cs_ui.c), like the F1 menu, with
 * the same settings: the Video tab (display, mouse, controllers) saved in
 * %LOCALAPPDATA%\CounterStrikeXB\settings.ini (src/cs_settings.c, shared with
 * the game), and the Controls tab, the keyboard and mouse, saved in keymap.ini
 * (src/cs_keys.c, shared with the F1 menu). Nothing is written until Save.
 *
 * The mouse: pointing selects, left click changes or presses (or the "<" and
 * ">" step a setting), right click steps back or clears a key, the wheel moves
 * the selection. The keyboard: arrows choose and change, Enter presses, Tab or
 * Page Up/Down switch tabs, Delete clears a key. To change a key, click its slot
 * (or Enter), then press the key, mouse button or wheel; Esc cancels.
 * Esc, F1 and F11 belong to the game's own menus and fullscreen. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cs_settings.h"
#include "cs_keys.h"
#include "cs_ui.h"

/* ---- the Video tab: the F1 menu's settings ------------------------------------------------- */
enum { V_SCALE, V_FULLSCREEN, V_STRETCH, V_WIDE, V_VSYNC, V_RATE, V_MSAA, V_ANISO, V_HIRES,
       V_MOUSE, V_MSENS, V_MINV, V_PADS, V_COUNT };
#define V_FIRST_RIGHT V_MOUSE
static const char *const V_LABEL[V_COUNT] = {
    "Render scale", "Fullscreen", "Stretch image", "Widescreen", "VSync", "Frame rate cap", "Anti-aliasing",
    "Anisotropic filtering", "Hi-res textures", "Mouse aim", "Sensitivity", "Invert", "Other controllers" };
static const char *const T_OFFON[2] = { "Off", "On" };

static CsSettings s_set;
static CsKeyMap s_keys;

static int rate_top(void)
{
    static int top;
    if (!top) top = cs_settings_rate_max(NULL, NULL, NULL);
    return top;
}

static int v_count(int i)
{
    switch (i) {
    case V_SCALE: return CS_SET_SCALES;
    case V_RATE: return cs_settings_rate_choices(rate_top());
    case V_MSAA: return CS_SET_MSAAS;
    case V_ANISO: return CS_SET_ANISOS;
    case V_MSENS: return CS_SET_SENSES;
    case V_PADS: return 3;
    }
    return 2;
}

static int v_get(int i)
{
    switch (i) {
    case V_SCALE: return cs_settings_index(CS_SET_SCALE_X2, CS_SET_SCALES, s_set.scale_x2);
    case V_FULLSCREEN: return s_set.fullscreen;
    case V_STRETCH: return s_set.stretch;
    case V_WIDE: return s_set.widescreen;
    case V_VSYNC: return s_set.vsync;
    case V_RATE: return cs_settings_index(CS_SET_RATE_HZ, CS_SET_RATES, cs_settings_rate_effective(s_set.frame_rate, rate_top()));
    case V_MSAA: return cs_settings_index(CS_SET_MSAA_N, CS_SET_MSAAS, s_set.msaa);
    case V_ANISO: return cs_settings_index(CS_SET_ANISO_N, CS_SET_ANISOS, s_set.aniso);
    case V_HIRES: return s_set.hires;
    case V_MOUSE: return s_set.mouse_aim;
    case V_MSENS: return cs_settings_index(CS_SET_SENS_PCT, CS_SET_SENSES, s_set.mouse_sens);
    case V_MINV: return s_set.mouse_invert;
    case V_PADS: return s_set.other_pads;
    }
    return 0;
}

static const char *v_text(int i)
{
    static char buf[48];
    int v = v_get(i);
    switch (i) {
    case V_SCALE: return CS_SET_SCALE_TEXT[v];
    case V_RATE: return CS_SET_RATE_TEXT[v];
    case V_MSAA: return CS_SET_MSAA_TEXT[v];
    case V_ANISO: return CS_SET_ANISO_TEXT[v];
    case V_MSENS: return CS_SET_SENS_TEXT[v];
    case V_STRETCH: return v ? "On (fill the window)" : "Off (4:3)";
    case V_WIDE: {
        float a = cs_settings_wide_aspect(&s_set, NULL);
        if (!v) return "Off (4:3)";
        if (a <= 0.0f) return "On (this screen is 4:3)";
        snprintf(buf, sizeof buf, "On (%s)", cs_settings_aspect_name(a));
        return buf;
    }
    case V_PADS: return v == 0 ? "Xbox only" : v == 1 ? "Auto" : "All (no check)";
    }
    return T_OFFON[v];
}

static void v_set(int i, int v)
{
    switch (i) {
    case V_SCALE: s_set.scale_x2 = CS_SET_SCALE_X2[v]; break;
    case V_FULLSCREEN: s_set.fullscreen = v; break;
    case V_STRETCH: s_set.stretch = v; break;
    case V_WIDE: s_set.widescreen = v; break;
    case V_VSYNC: s_set.vsync = v; break;
    case V_RATE: s_set.frame_rate = CS_SET_RATE_HZ[v]; break;
    case V_MSAA: s_set.msaa = CS_SET_MSAA_N[v]; break;
    case V_ANISO: s_set.aniso = CS_SET_ANISO_N[v]; break;
    case V_HIRES: s_set.hires = v; break;
    case V_MOUSE: s_set.mouse_aim = v; break;
    case V_MSENS: s_set.mouse_sens = CS_SET_SENS_PCT[v]; break;
    case V_MINV: s_set.mouse_invert = v; break;
    case V_PADS: s_set.other_pads = v; break;
    }
}

/* What each setting is, shown in the note box while it is selected. */
static const char *v_help(int i)
{
    switch (i) {
    case V_SCALE: return "Render scale: pick your screen (480p is the Xbox's own 640x480); higher needs a faster graphics card.";
    case V_FULLSCREEN: return "Fullscreen: start without a border, filling the screen. F11 or Alt+Enter switch while playing.";
    case V_STRETCH: return "Stretch image: fill the window instead of keeping the picture's shape.";
    case V_WIDE: return "Widescreen: the picture takes your screen's shape (16:9, 16:10, up to 21:9) and shows more of the "
                        "world at the sides. Xbox Live stays 4:3.";
    case V_VSYNC: return "VSync: no tearing, at the cost of a little input delay.";
    case V_RATE: return "Frame rate cap: 72 is the game's own limit, offered on 144 Hz or FreeSync/G-Sync monitors; "
                        "elsewhere 60 is the smooth choice.";
    case V_MSAA: return "Anti-aliasing smooths jagged edges; it costs some graphics card time.";
    case V_ANISO: return "Anisotropic filtering keeps distant textures sharp; it costs little.";
    case V_HIRES: return "Hi-res textures: the downloadable texture pack, when it is installed.";
    case V_MOUSE: return "Mouse aim: the mouse turns the view and its buttons are keys (Controls tab). Not on Xbox Live, "
                         "which is controller only.";
    case V_MSENS: return "Sensitivity of mouse aim.";
    case V_MINV: return "Invert: moving the mouse forward looks down.";
    case V_PADS: return "Other controllers (PlayStation, Switch Pro, 8BitDo...). Auto: a pad Steam Input or DS4Windows also "
                        "shows as an Xbox one counts once. Xbox only, or All without that check.";
    }
    return "";
}

/* ---- the window ----------------------------------------------------------------------------- */
enum { PAGE_VIDEO, PAGE_CONTROLS };
enum { B_RESET, B_REPORT, B_SAVE, B_CLOSE, B_COUNT };
static const char *const B_TEXT[B_COUNT] = { "Reset to defaults", "Report a problem", "Save", "Close" };
static const char *const TABS[2] = { "Video", "Controls" };

static HWND s_main;
static float s_scale = 1.0f;
static int s_page, s_sel, s_col, s_listen, s_dirty;
static char s_status[300];
/* Where the last paint put things, for the mouse. */
static RECT s_tab_r[2], s_row_r[V_COUNT], s_prev_r[V_COUNT], s_next_r[V_COUNT], s_btn_r[B_COUNT];
static RECT s_slot_r[INPUT_CONTROL_COUNT][CSK_SLOTS];

/* Rows of the page: the settings or the actions, then the buttons' row. */
static int page_rows(void) { return s_page == PAGE_VIDEO ? V_COUNT : cs_key_action_count; }
#define BTN_ROW page_rows()

static void status(const char *s) { snprintf(s_status, sizeof s_status, "%s", s); InvalidateRect(s_main, NULL, FALSE); }
static void redraw(void) { InvalidateRect(s_main, NULL, FALSE); }

#define CONTROLS_SPLIT 14

static void paint(HDC dc, int w, int h)
{
    CsUiFonts f;
    RECT all, band, sec, row, note, slot;
    int x = 0, y = 0, i, k;
    const char *hint;
    char name[48];

    csui_fonts_make(&f, csui_fit(w, h, &x, &y));
    SetRect(&all, 0, 0, w, h);
    csui_background(dc, &all);
    SetRect(&band, 0, y + csui_px(&f, 14), w, y + csui_px(&f, 80));
    {
        int tx = csui_title(dc, &f, &band, "Settings") + csui_px(&f, 40);
        for (i = 0; i < 2; i++) {
            int tw = csui_text_w(dc, f.head, TABS[i]) + csui_px(&f, 44);
            SetRect(&s_tab_r[i], tx, band.top + csui_px(&f, 14), tx + tw, band.bottom - csui_px(&f, 18));
            csui_tab(dc, &f, &s_tab_r[i], TABS[i], i == s_page, s_sel < 0);
            tx += tw + csui_px(&f, 8);
        }
    }
    memset(s_row_r, 0, sizeof s_row_r); memset(s_prev_r, 0, sizeof s_prev_r); memset(s_next_r, 0, sizeof s_next_r);
    memset(s_slot_r, 0, sizeof s_slot_r);
    if (s_page == PAGE_VIDEO) {
        SetRect(&sec, x + csui_px(&f, 40), y + csui_px(&f, 94), x + csui_px(&f, 470), y + csui_px(&f, 124));
        csui_section(dc, &f, &sec, "Display");
        SetRect(&sec, x + csui_px(&f, 500), y + csui_px(&f, 94), x + csui_px(&f, 920), y + csui_px(&f, 124));
        csui_section(dc, &f, &sec, "Mouse");
        for (i = 0; i < V_COUNT; i++) {
            int right = i >= V_FIRST_RIGHT, kk = right ? i - V_FIRST_RIGHT : i;
            int x0 = x + csui_px(&f, right ? 500 : 40), x1 = x + csui_px(&f, right ? 920 : 470);
            int top = y + csui_px(&f, 134 + kk * 40);
            if (i == V_PADS) {
                top = y + csui_px(&f, 134 + 3 * 40 + 50);
                SetRect(&sec, x0, top - csui_px(&f, 42), x1, top - csui_px(&f, 12));
                csui_section(dc, &f, &sec, "Controllers");
            }
            SetRect(&row, x0, top, x1, top + csui_px(&f, 34));
            s_row_r[i] = row;
            csui_option(dc, &f, &row, V_LABEL[i], v_text(i), i == s_sel, &s_prev_r[i], &s_next_r[i]);
        }
        hint = s_status[0] ? s_status : (s_sel >= 0 && s_sel < V_COUNT) ? v_help(s_sel)
             : "The same settings as the game's F1 menu. Click a setting, or use the arrows, then Save.";
    } else {
        int rh = csui_px(&f, 25), gap = csui_px(&f, 30), area_l = x + csui_px(&f, 40), area_r = x + csui_px(&f, 920);
        int cw = (area_r - area_l - gap) / 2, col;
        for (col = 0; col < 2; col++) {
            int x0 = area_l + col * (cw + gap), x1 = x0 + cw, top = y + csui_px(&f, 92);
            int first = col ? CONTROLS_SPLIT : 0, last = col ? cs_key_action_count : CONTROLS_SPLIT;
            int labw = cw * 40 / 100, sw = (cw - labw - csui_px(&f, 4) * CSK_SLOTS) / CSK_SLOTS;
            for (i = first; i < last; i++) {
                const CsKeyAction *a = &cs_key_actions[i];
                int sel = s_sel == i;
                if (a->section) {
                    SetRect(&sec, x0, top + csui_px(&f, 2), x1, top + rh + csui_px(&f, 2));
                    csui_section(dc, &f, &sec, a->section);
                    top += rh + csui_px(&f, 6);
                }
                SetRect(&row, x0, top, x1, top + rh - csui_px(&f, 3));
                csui_slant(dc, &row, csui_px(&f, 8), sel ? CSUI_NAVY_LIT : CSUI_NAVY, sel ? CSUI_GOLD : RGB(60, 88, 128));
                {
                    RECT lr = row;
                    lr.left += csui_px(&f, 14); lr.right = x0 + labw;
                    csui_text(dc, f.fine, sel ? CSUI_WHITE : CSUI_TEXT, &lr, a->label,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                for (k = 0; k < CSK_SLOTS; k++) {
                    int sx = x0 + labw + k * (sw + csui_px(&f, 4));
                    SetRect(&slot, sx, row.top + csui_px(&f, 2), sx + sw, row.bottom - csui_px(&f, 2));
                    s_slot_r[i][k] = slot;
                    cs_keys_name(s_keys.vk[a->control][k], name, sizeof name);
                    csui_keyslot(dc, &f, &slot, name, sel && s_col == k, sel && s_col == k && s_listen);
                }
                top += rh;
            }
        }
        hint = s_listen ? "Press the new key, mouse button or wheel for it. Esc cancels."
             : s_status[0] ? s_status
             : "Click a key (or use the arrows and Enter), then press the new key or mouse button; right-click or Delete "
               "clears it. A key does one thing: giving it to another action moves it. The mouse counts while mouse aim is on.";
    }
    SetRect(&note, x + csui_px(&f, 40), y + csui_px(&f, 512), x + csui_px(&f, 920), y + csui_px(&f, 566));
    csui_note(dc, &f, &note, hint, s_listen ? CSUI_GOLD : CSUI_CREAM);
    for (i = 0; i < B_COUNT; i++) {             /* Reset and Report at the left, Save and Close at the right */
        static const int bx[B_COUNT] = { 40, 250, 600, 770 }, bw[B_COUNT] = { 200, 200, 160, 150 };
        SetRect(&s_btn_r[i], x + csui_px(&f, bx[i]), y + csui_px(&f, 580), x + csui_px(&f, bx[i] + bw[i]), y + csui_px(&f, 620));
        csui_button(dc, &f, &s_btn_r[i], i == B_SAVE && s_dirty ? "Save *" : B_TEXT[i], s_sel == BTN_ROW && s_col == i);
    }
    csui_fonts_free(&f);
}

/* ---- actions -------------------------------------------------------------------------------- */
static void changed(const char *what)
{
    s_dirty = 1;
    status(what ? what : "");
}

static void step(int row, int dir)
{
    int n = v_count(row);
    v_set(row, (v_get(row) + dir + n) % n);
    s_status[0] = 0;
    s_dirty = 1;
    redraw();
}

static void do_reset(void)
{
    if (s_page == PAGE_VIDEO) {
        cs_settings_defaults(&s_set);
        changed("Default settings restored. Click Save to keep them.");
    } else {
        cs_keys_defaults(&s_keys);
        changed("Default keys restored (the PC game's). Click Save to keep them.");
    }
}

static int do_save(void)
{
    if (!cs_settings_save(&s_set) || !cs_keys_save(&s_keys)) {
        MessageBoxA(s_main, "The settings could not be saved.", "Counter-Strike", MB_ICONERROR);
        return 0;
    }
    s_dirty = 0;
    status("Saved. A game that is running takes the new settings now; keys and other controllers at its next start.");
    return 1;
}

#include "report.inc"

static void listen_start(void)
{
    s_listen = 1;
    s_status[0] = 0;
    SetCapture(s_main);                          /* a click anywhere is the new key */
    redraw();
}

static void listen_key(int vk)
{
    const CsKeyAction *a = &cs_key_actions[s_sel];
    char n[48], m[220];
    int from;
    s_listen = 0;
    ReleaseCapture();
    if (vk == VK_ESCAPE) { status("Cancelled."); return; }
    if (cs_keys_reserved(vk)) {
        status("That key belongs to the game itself (Esc, F1 settings, F11 or Alt+Enter fullscreen). Choose another.");
        return;
    }
    from = cs_keys_assign(&s_keys, a->control, s_col, vk);
    cs_keys_name(vk, n, sizeof n);
    if (from >= 0 && from != a->control) {
        int k;
        for (k = 0; k < cs_key_action_count && cs_key_actions[k].control != from; k++) ;
        snprintf(m, sizeof m, "%s is now \"%s\" (it was \"%s\"). Click Save to keep it.", n, a->label,
                 k < cs_key_action_count ? cs_key_actions[k].label : "another action");
    } else
        snprintf(m, sizeof m, "%s is now \"%s\". Click Save to keep it.", n, a->label);
    changed(m);
}

static void press(int row, int col)
{
    if (row == BTN_ROW) {
        switch (col) {
        case B_RESET: do_reset(); break;
        case B_REPORT: do_report(s_main); break;
        case B_SAVE: do_save(); break;
        case B_CLOSE: PostMessageA(s_main, WM_CLOSE, 0, 0); break;
        }
        return;
    }
    if (row < 0) return;
    if (s_page == PAGE_VIDEO) step(row, +1);
    else listen_start();
}

static void page_switch(int page)
{
    s_page = page;
    s_col = 0; s_status[0] = 0;
    if (s_sel >= 0) s_sel = 0;
    redraw();
}

static void key(int vk)
{
    int n = page_rows();
    switch (vk) {
    case VK_UP: s_sel = s_sel > -1 ? s_sel - 1 : -1; if (s_sel == BTN_ROW - 1 && s_col >= CSK_SLOTS) s_col = 0; break;
    case VK_DOWN: if (s_sel < BTN_ROW) s_sel++; if (s_sel == BTN_ROW && s_col >= B_COUNT) s_col = 0; break;
    case VK_LEFT: case VK_RIGHT: {
        int dir = vk == VK_RIGHT ? 1 : -1;
        if (s_sel < 0) { page_switch(!s_page); return; }
        if (s_sel == BTN_ROW) { s_col = (s_col + dir + B_COUNT) % B_COUNT; break; }
        if (s_page == PAGE_VIDEO) { step(s_sel, dir); return; }
        s_col += dir;
        if (s_col < 0) s_col = 0;
        if (s_col >= CSK_SLOTS) s_col = CSK_SLOTS - 1;
        break;
    }
    case VK_RETURN: case VK_SPACE:
        if (s_sel < 0) { s_sel = 0; break; }
        press(s_sel, s_col);
        return;
    case VK_TAB: case VK_PRIOR: case VK_NEXT: page_switch(!s_page); return;
    case VK_DELETE: case VK_BACK:
        if (s_page == PAGE_CONTROLS && s_sel >= 0 && s_sel < n) {
            s_keys.vk[cs_key_actions[s_sel].control][s_col] = 0;
            changed("Cleared. Click Save to keep it.");
        }
        return;
    case VK_ESCAPE: PostMessageA(s_main, WM_CLOSE, 0, 0); return;
    default: return;
    }
    s_status[0] = 0;
    redraw();
}

/* What is under the pointer: 1 tab, 2 setting row, 3 its "<", 4 its ">", 5 key slot, 6 button. */
static int in(const RECT *r, int x, int y) { return r->right > r->left && x >= r->left && x < r->right && y >= r->top && y < r->bottom; }
static int hit(int x, int y, int *a, int *b)
{
    int i, k;
    *a = *b = 0;
    for (i = 0; i < 2; i++) if (in(&s_tab_r[i], x, y)) { *a = i; return 1; }
    for (i = 0; i < B_COUNT; i++) if (in(&s_btn_r[i], x, y)) { *a = i; return 6; }
    if (s_page == PAGE_VIDEO) {
        for (i = 0; i < V_COUNT; i++) {
            if (in(&s_prev_r[i], x, y)) { *a = i; return 3; }
            if (in(&s_next_r[i], x, y)) { *a = i; return 4; }
            if (in(&s_row_r[i], x, y)) { *a = i; return 2; }
        }
    } else
        for (i = 0; i < cs_key_action_count; i++)
            for (k = 0; k < CSK_SLOTS; k++)
                if (in(&s_slot_r[i][k], x, y)) { *a = i; *b = k; return 5; }
    return 0;
}

static void mouse_move(int x, int y)
{
    int a, b, what = hit(x, y, &a, &b), sel = s_sel, col = s_col;
    if (s_listen || !what) return;
    if (what == 1) sel = -1;
    else if (what == 6) { sel = BTN_ROW; col = a; }
    else if (what == 5) { sel = a; col = b; }
    else sel = a;
    if (sel != s_sel || col != s_col) {
        if (sel != s_sel) s_status[0] = 0;
        s_sel = sel; s_col = col;
        redraw();
    }
}

static void mouse_click(int x, int y, int right)
{
    int a, b, what = hit(x, y, &a, &b);
    if (!what) return;
    mouse_move(x, y);
    switch (what) {
    case 1: if (a != s_page) page_switch(a); break;
    case 2: step(a, right ? -1 : +1); break;
    case 3: step(a, -1); break;
    case 4: step(a, right ? -1 : +1); break;
    case 5:
        if (right) { s_keys.vk[cs_key_actions[a].control][b] = 0; changed("Cleared. Click Save to keep it."); }
        else listen_start();
        break;
    case 6: if (!right) press(BTN_ROW, a); break;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        s_main = h;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT cr;
        HDC dc = BeginPaint(h, &ps), mem;
        HBITMAP bm, old;
        GetClientRect(h, &cr);
        mem = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        old = (HBITMAP)SelectObject(mem, bm);
        paint(mem, cr.right, cr.bottom);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (s_listen) {
            if (!(l & (1L << 30))) listen_key((int)w);
            return 0;
        }
        if (m == WM_KEYDOWN) key((int)w);
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (s_listen) return 0;
        break;
    case WM_MOUSEMOVE:
        mouse_move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (s_listen) {                          /* a mouse button as the key (keymap.ini mouse1..mouse5) */
            listen_key(m == WM_LBUTTONDOWN ? 0x01 : m == WM_RBUTTONDOWN ? 0x02 : m == WM_MBUTTONDOWN ? 0x04
                       : HIWORD(w) == XBUTTON1 ? 0x05 : 0x06);
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        }
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) mouse_click(GET_X_LPARAM(l), GET_Y_LPARAM(l), m == WM_RBUTTONDOWN);
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    case WM_MOUSEWHEEL:
        if (s_listen) { listen_key(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 0x0E : 0x0F); return 0; }
        key(GET_WHEEL_DELTA_WPARAM(w) > 0 ? VK_UP : VK_DOWN);
        return 0;
    case WM_CAPTURECHANGED:
        if (s_listen && (HWND)l != h) { s_listen = 0; status("Cancelled."); }
        break;
    case WM_SIZE:
        redraw();
        return 0;
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
    RECT r, work;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    UINT dpi = 96;
    (void)prev; (void)cmd;

    {   /* sharp text on high-DPI screens: draw at the screen's own scale */
        typedef BOOL (WINAPI *SetCtx)(HANDLE);
        SetCtx set = u32 ? (SetCtx)(void (*)(void))GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
        if (!set || !set((HANDLE)-4)) SetProcessDPIAware();      /* -4: per-monitor v2 */
    }
    {
        HDC dc = GetDC(NULL);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    s_scale = dpi / 96.0f;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) SetRect(&work, 0, 0, 1280, 720);
    if (960 * s_scale > (work.right - work.left) * 0.95f) s_scale = (work.right - work.left) * 0.95f / 960.0f;
    if (640 * s_scale > (work.bottom - work.top) * 0.90f) s_scale = (work.bottom - work.top) * 0.90f / 640.0f;

    cs_settings_load(&s_set);
    cs_keys_load(&s_keys);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));     /* the game's icon (resources/config.rc) */
    wc.lpszClassName = "CounterStrikeXBConfig";
    RegisterClassA(&wc);
    SetRect(&r, 0, 0, (int)(960 * s_scale), (int)(640 * s_scale));
    AdjustWindowRect(&r, style, FALSE);
    CreateWindowExA(0, wc.lpszClassName, "Counter-Strike - Settings", style,
                    work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                    work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                    r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(s_main, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
