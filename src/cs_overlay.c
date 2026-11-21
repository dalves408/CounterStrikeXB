/*
 * cs_overlay.c -- the ESC quit prompt and the F1 settings menu.
 *
 * The mouse works in them too (game_proc): pointing selects, clicking changes.
 * Both are drawn with GDI, in the look of the game's manual (cs_ui.c), into one
 * layered popup over the game window's client area (owned by it, never activated, so the keys keep going to the game
 * window and arrive here through the toolkit's key filter).
 *
 * A host-side layer and nothing more: the game keeps running underneath --
 * clocks, sound, frames, and whatever network play is added later -- and the
 * menu never touches its state or timing. Its one contact with the game is
 * input: while a menu is open every key goes to the menu, and controller 1
 * (which drives it too: D-pad or left stick, A confirms, B or START closes)
 * reads as neutral to the game (xbox_InputSetHold), so a press meant for the
 * menu is not also a shot or a jump. The keys mapped to A and B confirm and
 * close as well. Holding BACK and pressing START opens the settings menu from
 * the controller; the game never sees that START (pad_filter).
 * F1 (or the menu's own lines) changes the settings of cs_settings.h; each
 * change is saved to settings.ini at once and applied while the game runs:
 * render scale, anti-aliasing, anisotropy and hi-res textures between frames, the rest at the
 * next present. config.exe offers the same options before the game starts.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <xinput.h>
#include "cs_overlay.h"
#include "cs_settings.h"
#include "cs_d3d11.h"
#include "cs_diag.h"
#include "cs_mouse.h"
#include "cs_wide.h"
#include "cs_ui.h"
#include "cs_keys.h"
#include "video/video_player.h"

extern void xbox_InputKeysClear(void);      /* toolkit input/keyboard.c */
extern void xbox_InputKeyboardReload(void); /* toolkit input/keyboard.c: keymap.ini again */
extern void xbox_InputSetMouseKeys(int on);  /* toolkit input/keyboard.c */
extern void mkb_set_enabled(int on);         /* toolkit input/mkb_input.c: mouse aim */
extern int  xbox_InputKeyFaceButton(int vk); /* toolkit input/keyboard.c: 0 A, 1 B, ... or -1 */
extern void xbox_InputSetHold(int on);      /* toolkit input/xinput_device.c */
extern int  xbox_FramebufferKeyDown(int vk); /* toolkit input/keyboard.c: a key or mouse button held */
extern DWORD xbox_InputHostPad(DWORD port, XINPUT_STATE *st);   /* Xbox pad, else an SDL3 one (PlayStation, ...) */

/* Port 0's state as the toolkit hands it to the title (input/xinput_xbox.h,
 * mirrored here: that header brings the Xbox's own Windows types). */
typedef struct CsPadState {
    DWORD dwPacketNumber;
    WORD  wButtons;
    BYTE  bAnalogButtons[8];
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} CsPadState;
#define PAD_START 0x0010
#define PAD_BACK  0x0020
extern void xbox_InputSetPort0Filter(void (*filter)(CsPadState *));

enum { MODE_QUIT, MODE_SETTINGS, MODE_LIVE };   /* MODE_LIVE: Xbox Live refused for mouse aim or widescreen */
#define VK_LIVE_NOTICE 0x97                     /* unassigned: posted to open MODE_LIVE */
enum { IT_SCALE, IT_FULLSCREEN, IT_STRETCH, IT_WIDE, IT_VSYNC, IT_RATE, IT_MSAA, IT_ANISO, IT_HIRES,
       IT_MOUSE, IT_MSENS, IT_MINV, IT_PADS, IT_CLOSE, IT_QUIT, IT_COUNT };
static const char *const ITEM_LABEL[IT_COUNT] = {
    "Render scale", "Fullscreen", "Stretch image", "Widescreen", "VSync", "Frame rate cap",
    "Anti-aliasing", "Anisotropic filtering", "Hi-res textures",
    "Mouse aim", "Sensitivity", "Invert", "Other controllers", "Back to the game", "Quit Counter-Strike" };
static const char *const MOUSE_AIM[2] = { "Off", "On" };
static const char *const OFFON[2] = { "Off", "On" };
static const char *const STRETCH[2] = { "Off (4:3)", "On (fill the window)" };
static const char *wide_text(int on)
{
    static char buf[64];
    float a = cs_wide_aspect();                  /* what cs_wide_apply found for this screen */
    if (!on) return "Off (4:3)";
    if (a < 4.0f / 3.0f + 0.01f) return "On (this screen is 4:3)";
    snprintf(buf, sizeof buf, "On (%s)", cs_settings_aspect_name(a));
    return buf;
}

static volatile LONG s_active;
static int s_mode, s_sel, s_quit_sel, s_armed;
/* The settings menu's two pages. s_sel -1 is the tab row; on CONTROLS, rows
 * 0 .. cs_key_action_count-1 are the actions (s_col the key slot) and the next
 * one the "Reset to defaults" button. s_listen: the selected slot waits for a key. */
enum { PAGE_VIDEO, PAGE_CONTROLS };
static int s_page, s_col, s_listen;
static CsKeyMap s_keys;
static char s_status[200];
static const char *const TABS[2] = { "Video", "Controls" };
static HWND s_ov, s_game;
static int s_cw, s_ch, s_ox, s_oy;          /* what was last drawn */
static RECT s_row[IT_COUNT], s_btn[2];
/* Where the last drawing put things, in the game window's client coordinates
 * (the menu covers it exactly), for the mouse. */
static RECT s_prev_r[IT_COUNT], s_next_r[IT_COUNT], s_tab_r[2], s_reset_r;
static RECT s_slot_r[INPUT_CONTROL_COUNT][CSK_SLOTS];      /* by action row */
static WNDPROC s_game_proc;                 /* the game window's own procedure (subclassed while open) */
static WORD s_pad_prev;
static DWORD s_pad_repeat;
static int s_pad_seen, s_pad_release;

int cs_overlay_active(void) { return s_active != 0; }

/* ---- the settings ----------------------------------------------------------- */
static int item_count(int i)
{
    switch (i) {
    case IT_SCALE: return CS_SET_SCALES;
    case IT_RATE:  return cs_settings_rate_choices(cs_d3d11_rate_max());   /* 72 only where it is smooth */
    case IT_MSAA:  return CS_SET_MSAAS;
    case IT_ANISO: return CS_SET_ANISOS;
    case IT_MSENS: return CS_SET_SENSES;
    case IT_WIDE:  return 2;
    case IT_FULLSCREEN: case IT_STRETCH: case IT_VSYNC: case IT_HIRES: case IT_MOUSE: case IT_MINV: return 2;
    case IT_PADS: return 3;
    }
    return 0;
}

static int item_get(int i)
{
    const CsSettings *s = &g_cs_settings;
    switch (i) {
    case IT_SCALE: return cs_settings_index(CS_SET_SCALE_X2, CS_SET_SCALES, s->scale_x2);
    case IT_FULLSCREEN: return xbox_FramebufferIsFullscreen() ? 1 : 0;
    case IT_STRETCH: return s->stretch;
    case IT_WIDE: return s->widescreen;
    case IT_VSYNC: return s->vsync;
    case IT_RATE: return cs_settings_index(CS_SET_RATE_HZ, CS_SET_RATES,
                                           cs_settings_rate_effective(s->frame_rate, cs_d3d11_rate_max()));
    case IT_MSAA: return cs_settings_index(CS_SET_MSAA_N, CS_SET_MSAAS, s->msaa);
    case IT_ANISO: return cs_settings_index(CS_SET_ANISO_N, CS_SET_ANISOS, s->aniso);
    case IT_HIRES: return s->hires;
    case IT_MOUSE: return s->mouse_aim;
    case IT_MSENS: return cs_settings_index(CS_SET_SENS_PCT, CS_SET_SENSES, s->mouse_sens);
    case IT_MINV: return s->mouse_invert;
    case IT_PADS: return s->other_pads;
    }
    return 0;
}

static const char *item_text(int i)
{
    int v = item_get(i);
    switch (i) {
    case IT_SCALE: return CS_SET_SCALE_TEXT[v];
    case IT_RATE: return CS_SET_RATE_TEXT[v];
    case IT_MSAA: return CS_SET_MSAA_TEXT[v];
    case IT_ANISO: return CS_SET_ANISO_TEXT[v];
    case IT_STRETCH: return STRETCH[v];
    case IT_WIDE: return wide_text(v);
    case IT_MOUSE: return MOUSE_AIM[v];
    case IT_MSENS: return CS_SET_SENS_TEXT[v];
    case IT_FULLSCREEN: case IT_VSYNC: case IT_HIRES: case IT_MINV: return OFFON[v];
    case IT_PADS: return v == 0 ? "Xbox only" : v == 1 ? "Auto" : "All (no check)";
    }
    return "";
}

static void item_set(int i, int v)
{
    CsSettings *s = &g_cs_settings;
    switch (i) {
    case IT_SCALE: s->scale_x2 = CS_SET_SCALE_X2[v]; cs_d3d11_settings_changed(); break;
    case IT_FULLSCREEN:
        if (v != item_get(IT_FULLSCREEN)) xbox_FramebufferToggleFullscreen();
        s->fullscreen = v;
        break;
    case IT_STRETCH: s->stretch = v; break;
    case IT_WIDE: s->widescreen = v; cs_wide_apply(); cs_d3d11_settings_changed(); break;
    case IT_VSYNC: s->vsync = v; break;
    case IT_RATE: s->frame_rate = CS_SET_RATE_HZ[v]; break;
    case IT_MSAA: s->msaa = CS_SET_MSAA_N[v]; cs_d3d11_settings_changed(); break;
    case IT_ANISO: s->aniso = CS_SET_ANISO_N[v]; cs_d3d11_settings_changed(); break;
    case IT_HIRES: s->hires = v; cs_d3d11_settings_changed(); break;
    case IT_MOUSE: s->mouse_aim = v; cs_mouse_apply(); break;
    case IT_MSENS: s->mouse_sens = CS_SET_SENS_PCT[v]; cs_mouse_apply(); break;
    case IT_MINV: s->mouse_invert = v; cs_mouse_apply(); break;
    case IT_PADS: s->other_pads = v; break;      /* read at start (cs_settings_startup): next start */
    }
    cs_settings_save(s);
    cs_diag_log_video("settings changed");      /* the log says what a report's picture was drawn with */
}

/* settings.ini changed while the game runs (config.exe, RecompLauncher): take
 * the new values the way the F1 menu does. Called between frames, at most
 * twice a second. */
void cs_settings_watch(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    CsSettings n;
    const char *mode;
    if (now - last < 500) return;
    last = now;
    if (!cs_settings_changed_on_disk(&n)) return;
    mode = getenv("RECOMPLAUNCHER_MODE");
    if (n.fullscreen != (xbox_FramebufferIsFullscreen() != 0) && !(mode && !_stricmp(mode, "bigpicture")))
        xbox_FramebufferToggleFullscreen();
    g_cs_settings = n;
    cs_wide_apply();
    cs_d3d11_settings_changed();
    cs_mouse_apply();
    cs_diag_log_video("settings.ini changed");
}

/* ---- drawing --------------------------------------------------------------- */
/* In the look of the game's manual (cs_ui.c). The settings are one page over
 * the whole window -- Display on the left; Mouse and the game's two buttons on
 * the right -- and the quit prompt and the Xbox Live notice small panels over
 * the dimmed game. */
#define FIRST_RIGHT IT_MOUSE                   /* the right column starts here */

/* The CONTROLS page: the actions in two columns as the manual's Controls page
 * groups them (Movement and Combat on the left; Look, Menus and radio on the
 * right), each with CSK_SLOTS keys, then "Reset to defaults" and a note. */
#define CONTROLS_SPLIT 14                      /* the right column's first action */
static void draw_controls(HDC dc, const CsUiFonts *f, int x, int y)
{
    RECT sec, row, slot, note;
    int col, i, s, rh = csui_px(f, 25), gap = csui_px(f, 30);
    int area_l = x + csui_px(f, 40), area_r = x + csui_px(f, 920), cw = (area_r - area_l - gap) / 2;
    char name[48];
    for (col = 0; col < 2; col++) {
        int x0 = area_l + col * (cw + gap), x1 = x0 + cw, top = y + csui_px(f, 94);
        int first = col ? CONTROLS_SPLIT : 0, last = col ? cs_key_action_count : CONTROLS_SPLIT;
        int labw = cw * 40 / 100, sw = (cw - labw - csui_px(f, 4) * CSK_SLOTS) / CSK_SLOTS;
        for (i = first; i < last; i++) {
            const CsKeyAction *a = &cs_key_actions[i];
            int sel = s_sel == i;
            if (a->section) {
                SetRect(&sec, x0, top + csui_px(f, 2), x1, top + rh + csui_px(f, 2));
                csui_section(dc, f, &sec, a->section);
                top += rh + csui_px(f, 6);
            }
            SetRect(&row, x0, top, x1, top + rh - csui_px(f, 3));
            csui_slant(dc, &row, csui_px(f, 8), sel ? CSUI_NAVY_LIT : CSUI_NAVY, sel ? CSUI_GOLD : RGB(60, 88, 128));
            {
                RECT lr = row;
                lr.left += csui_px(f, 14); lr.right = x0 + labw;
                csui_text(dc, f->fine, sel ? CSUI_WHITE : CSUI_TEXT, &lr, a->label,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            for (s = 0; s < CSK_SLOTS; s++) {
                int sx = x0 + labw + s * (sw + csui_px(f, 4));
                SetRect(&slot, sx, row.top + csui_px(f, 2), sx + sw, row.bottom - csui_px(f, 2));
                cs_keys_name(s_keys.vk[a->control][s], name, sizeof name);
                s_slot_r[i][s] = slot;
                csui_keyslot(dc, f, &slot, name, sel && s_col == s, sel && s_col == s && s_listen);
            }
            top += rh;
        }
        if (col == 1) {                            /* the reset button under the right column */
            top += csui_px(f, 14);
            SetRect(&row, x0 + cw / 4, top, x1, top + csui_px(f, 34));
            s_reset_r = row;
            csui_button(dc, f, &row, "Reset to defaults", s_sel == cs_key_action_count);
        }
    }
    SetRect(&note, x + csui_px(f, 110), y + csui_px(f, 548), x + csui_px(f, 850), y + csui_px(f, 610));
    csui_note(dc, f, &note, s_listen ? "Press the new key or mouse button for it (the mouse counts while mouse aim is on). "
                                       "Esc cancels."
                         : s_status[0] ? s_status
                         : "Choose a key with the arrows and press Enter, then the new key or mouse button. Delete clears it."
                           " A key does one thing: giving it to another action moves it there. Changes work at once.",
              s_listen ? CSUI_GOLD : CSUI_CREAM);
}

static void draw_settings(HDC dc, int cw, int ch)
{
    CsUiFonts f;
    RECT all, band, sec, row, note;
    int x, y, i;
    float sc = csui_fit(cw, ch, &x, &y);
    char hint[400];

    csui_fonts_make(&f, sc);
    SetRect(&all, 0, 0, cw, ch);
    csui_background(dc, &all);
    SetRect(&band, 0, y + csui_px(&f, 14), cw, y + csui_px(&f, 80));
    {   /* the title, then the two tabs */
        int tx = csui_title(dc, &f, &band, "Settings") + csui_px(&f, 40);
        for (i = 0; i < 2; i++) {
            RECT tr;
            int tw = csui_text_w(dc, f.head, TABS[i]) + csui_px(&f, 44);
            SetRect(&tr, tx, band.top + csui_px(&f, 14), tx + tw, band.bottom - csui_px(&f, 18));
            s_tab_r[i] = tr;
            csui_tab(dc, &f, &tr, TABS[i], i == s_page, s_sel < 0);
            tx += tw + csui_px(&f, 8);
        }
    }
    if (s_page == PAGE_CONTROLS) {
        draw_controls(dc, &f, x, y);
        csui_dial(dc, &f, &all, 0);
        csui_dial(dc, &f, &all, 1);
        csui_fonts_free(&f);
        return;
    }

    SetRect(&sec, x + csui_px(&f, 40), y + csui_px(&f, 98), x + csui_px(&f, 470), y + csui_px(&f, 130));
    csui_section(dc, &f, &sec, "Display");
    SetRect(&sec, x + csui_px(&f, 500), y + csui_px(&f, 98), x + csui_px(&f, 920), y + csui_px(&f, 130));
    csui_section(dc, &f, &sec, "Mouse");
    for (i = 0; i < IT_COUNT; i++) {
        int right = i >= FIRST_RIGHT, k = right ? i - FIRST_RIGHT : i;
        int x0 = x + csui_px(&f, right ? 500 : 40), x1 = x + csui_px(&f, right ? 920 : 470);
        int top = y + csui_px(&f, 142 + k * 40);
        if (i == IT_PADS) {                        /* below the mouse rows, under a heading of its own */
            top = y + csui_px(&f, 142 + 3 * 40 + 50);
            SetRect(&sec, x0, top - csui_px(&f, 44), x1, top - csui_px(&f, 12));
            csui_section(dc, &f, &sec, "Controllers");
        }
        if (i >= IT_CLOSE) {                       /* then the game's two buttons, under theirs */
            top = y + csui_px(&f, 142 + 3 * 40 + 50 + 40 + 50 + (i - IT_CLOSE) * 50);
            if (i == IT_CLOSE) {
                SetRect(&sec, x0, top - csui_px(&f, 44), x1, top - csui_px(&f, 12));
                csui_section(dc, &f, &sec, "Game");
            }
            SetRect(&row, x0, top, x1, top + csui_px(&f, 40));
            s_row[i] = row;
            csui_button(dc, &f, &row, i == IT_QUIT && s_armed ? "Press again to quit"
                                     : i == IT_CLOSE ? "Back to the game" : "Quit Counter-Strike", i == s_sel);
            continue;
        }
        SetRect(&row, x0, top, x1, top + csui_px(&f, 34));
        s_row[i] = row;
        csui_option(dc, &f, &row, ITEM_LABEL[i], item_text(i), i == s_sel, &s_prev_r[i], &s_next_r[i]);
    }
    snprintf(hint, sizeof hint, "The arrows or the mouse choose and change; Tab: Controls. Saved as you go, and F1, Esc"
             " or B closes. Xbox Live plays with a controller in 4:3, so it stays closed while mouse aim or widescreen"
             " is on. Other controllers (PlayStation, Switch...): Auto sees a Steam Input or DS4Windows pad once; changes apply"
             " at the next start.");
    SetRect(&note, x + csui_px(&f, 110), y + csui_px(&f, 536), x + csui_px(&f, 850), y + csui_px(&f, 606));
    csui_note(dc, &f, &note, hint, CSUI_CREAM);
    {   /* the page dials, in the window's bottom corners */
        csui_dial(dc, &f, &all, 0);
        csui_dial(dc, &f, &all, 1);
    }
    csui_fonts_free(&f);
}

/* The quit prompt and the Xbox Live notice: a panel in the middle, its title,
 * a line or two, then two buttons. *box: the panel. */
static void draw_prompt(HDC dc, int cw, int ch, RECT *box)
{
    CsUiFonts f;
    RECT band, msg;
    int live = s_mode == MODE_LIVE, bw, bh, i;
    float sc = csui_fit(cw, ch, NULL, NULL);
    static const char *const quit_btn[2] = { "No", "Yes" }, *const live_btn[2] = { "Off", "On" };
    /* which settings keep Xbox Live closed: mouse aim, widescreen or both */
    int m = g_cs_settings.mouse_aim != 0, wd = g_cs_settings.widescreen != 0, k = (m && wd) ? 2 : (wd ? 1 : 0);
    static const char *const line[3] = {
        "Xbox Live is shared with players on Xbox and xemu, so it is controller only: it needs mouse aim off. "
        "Mouse aim (also in F1):",
        "Xbox Live is shared with players on Xbox and xemu, so it plays in 4:3: it needs widescreen off. "
        "Widescreen (also in F1):",
        "Xbox Live is shared with players on Xbox and xemu: it needs mouse aim off, and plays in 4:3. "
        "Mouse aim and widescreen (also in F1):" };

    csui_fonts_make(&f, sc);
    bw = csui_px(&f, live ? 600 : 470);
    bh = csui_px(&f, live ? 270 : 220);
    SetRect(box, (cw - bw) / 2, (ch - bh) / 2, (cw - bw) / 2 + bw, (ch - bh) / 2 + bh);
    csui_panel(dc, &f, box);
    SetRect(&band, box->left + csui_px(&f, 2), box->top + csui_px(&f, 10), box->right - csui_px(&f, 2), box->top + csui_px(&f, 70));
    csui_title(dc, &f, &band, live ? "Xbox Live" : "Quit Game");
    SetRect(&msg, box->left + csui_px(&f, 30), band.bottom + csui_px(&f, 8), box->right - csui_px(&f, 30),
            box->bottom - csui_px(&f, 74));
    csui_text(dc, live ? f.body : f.head, CSUI_TEXT, &msg, live ? line[k] : "Quit Counter-Strike?",
              DT_CENTER | DT_VCENTER | (live ? DT_WORDBREAK : DT_SINGLELINE));
    for (i = 0; i < 2; i++) {
        int cx = (box->left + box->right) / 2 + (i ? csui_px(&f, 14) : -csui_px(&f, 14) - csui_px(&f, 150));
        SetRect(&s_btn[i], cx, box->bottom - csui_px(&f, 62), cx + csui_px(&f, 150), box->bottom - csui_px(&f, 22));
        csui_button(dc, &f, &s_btn[i], live ? live_btn[i] : quit_btn[i], i == s_quit_sel);
    }
    csui_fonts_free(&f);
}

static void draw(void)
{
    RECT cr, box;
    POINT o = { 0, 0 }, p0 = { 0, 0 };
    SIZE sz;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, dc;
    HBITMAP bm;
    HGDIOBJ oldbm;
    BLENDFUNCTION bf;
    uint32_t *px;
    int cw, ch, x, y;

    if (!s_ov || !s_game || IsIconic(s_game)) return;
    GetClientRect(s_game, &cr);
    ClientToScreen(s_game, &o);
    cw = cr.right; ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(NULL);
    dc = CreateCompatibleDC(sdc);
    bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(dc); ReleaseDC(NULL, sdc); return; }
    oldbm = SelectObject(dc, bm);
    px = (uint32_t *)bits;
    memset(px, 0, (size_t)cw * ch * 4);
    SetBkMode(dc, TRANSPARENT);

    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    if (s_mode == MODE_SETTINGS) {
        /* the whole window: the manual's page, the game faintly behind it */
        draw_settings(dc, cw, ch);
        GdiFlush();
        bf.SourceConstantAlpha = 242; bf.AlphaFormat = 0;
    } else {
        /* the panel solid, the game around it dimmed (premultiplied: black at 63%) */
        draw_prompt(dc, cw, ch, &box);
        GdiFlush();
        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++) {
                uint32_t *p = &px[(size_t)y * cw + x];
                if (*p && x >= box.left && x < box.right && y >= box.top && y < box.bottom) *p |= 0xFF000000u;
                else *p = 0xA0000000u;
            }
        bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    }
    {   /* CS_OVERLAY_DUMP=<file.bmp> (testing): each drawn menu also goes to that file */
        const char *dump = getenv("CS_OVERLAY_DUMP");
        FILE *fp = dump && *dump ? fopen(dump, "wb") : NULL;
        if (fp) {
            BITMAPFILEHEADER fh;
            memset(&fh, 0, sizeof fh);
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
            fh.bfSize = fh.bfOffBits + (DWORD)cw * ch * 4;
            fwrite(&fh, sizeof fh, 1, fp);
            fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, fp);
            fwrite(px, 4, (size_t)cw * ch, fp);
            fclose(fp);
        }
    }
    sz.cx = cw; sz.cy = ch;
    UpdateLayeredWindow(s_ov, sdc, &o, &sz, dc, &p0, 0, &bf, ULW_ALPHA);
    s_cw = cw; s_ch = ch; s_ox = o.x; s_oy = o.y;
    SelectObject(dc, oldbm);
    DeleteObject(bm); DeleteDC(dc); ReleaseDC(NULL, sdc);
}

/* ---- open, close, keys ---------------------------------------------------- */
static void menu_close(void);
static void pad_tick(void);

static LRESULT CALLBACK ov_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;      /* the mouse goes through */
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_TIMER && !s_active && s_pad_release) {
        /* closed: give the pad back once the button that closed the menu is up */
        XINPUT_STATE st;
        if (xbox_InputHostPad(0, &st) != ERROR_SUCCESS || (st.Gamepad.wButtons == 0
                && st.Gamepad.sThumbLX > -16000 && st.Gamepad.sThumbLX < 16000
                && st.Gamepad.sThumbLY > -16000 && st.Gamepad.sThumbLY < 16000)) {
            s_pad_release = 0;
            xbox_InputSetHold(0);
            KillTimer(h, 1);
        }
        return 0;
    }
    if (m == WM_TIMER && s_active) {
        RECT cr;
        POINT o = { 0, 0 };
        pad_tick();
        if (s_active && s_game && !IsIconic(s_game)) {      /* follow the game window */
            GetClientRect(s_game, &cr);
            ClientToScreen(s_game, &o);
            if (cr.right != s_cw || cr.bottom != s_ch || o.x != s_ox || o.y != s_oy) draw();
            if (!IsWindowVisible(s_ov)) ShowWindow(s_ov, SW_SHOWNOACTIVATE);
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ---- the mouse ------------------------------------------------------------ */
/* While a menu is open the mouse is the menu's: mouse aim lets go of it (the
 * pointer is free and shown), and the game window's mouse messages come here
 * instead of reaching the game (game_proc, a subclass of the toolkit's window
 * procedure). Pointing selects; left click changes or presses, right click
 * steps a setting back or clears a key (elsewhere it closes the menu), the
 * wheel moves the selection. */
static LRESULT CALLBACK game_proc(HWND h, UINT m, WPARAM w, LPARAM l);

static void mouse_take(HWND game)
{
    if (!s_game_proc && game)
        s_game_proc = (WNDPROC)SetWindowLongPtrA(game, GWLP_WNDPROC, (LONG_PTR)game_proc);
    if (g_cs_settings.mouse_aim) {               /* aim stops pinning and hiding the pointer */
        mkb_set_enabled(0);
        xbox_InputSetMouseKeys(0);
    }
}

static void mouse_give_back(void)
{
    cs_mouse_apply();                            /* mouse aim as the settings say */
}

static int in(const RECT *r, int x, int y)
{
    return r->right > r->left && x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

/* What is under the pointer: *what 1 tab, 2 setting row, 3 its "<", 4 its ">",
 * 5 key slot, 6 reset, 7 prompt button; *a the index, *b the slot. 0: nothing. */
static int hit(int x, int y, int *a, int *b)
{
    int i, k;
    *a = *b = 0;
    if (s_mode != MODE_SETTINGS) {
        for (i = 0; i < 2; i++) if (in(&s_btn[i], x, y)) { *a = i; return 7; }
        return 0;
    }
    for (i = 0; i < 2; i++) if (in(&s_tab_r[i], x, y)) { *a = i; return 1; }
    if (s_page == PAGE_VIDEO) {
        for (i = 0; i < IT_COUNT; i++) {
            if (in(&s_prev_r[i], x, y)) { *a = i; return 3; }
            if (in(&s_next_r[i], x, y)) { *a = i; return 4; }
            if (in(&s_row[i], x, y)) { *a = i; return 2; }
        }
        return 0;
    }
    for (i = 0; i < cs_key_action_count; i++)
        for (k = 0; k < CSK_SLOTS; k++)
            if (in(&s_slot_r[i][k], x, y)) { *a = i; *b = k; return 5; }
    if (in(&s_reset_r, x, y)) return 6;
    return 0;
}

static void controls_key(WPARAM w, int repeat);
static void listen_key(int vk);
static void page_switch(void);
static void settings_change(int dir);
static void menu_key(WPARAM w, int repeat);

/* The pointer moved: select what it is over. */
static void mouse_move(int x, int y)
{
    int a, b, what = hit(x, y, &a, &b), sel = s_sel, col = s_col, q = s_quit_sel;
    if (s_listen || !what) return;
    if (what == 7) s_quit_sel = a;
    else if (what == 1) sel = -1;
    else if (what == 5) { sel = a; col = b; }
    else if (what == 6) sel = cs_key_action_count;
    else sel = a;
    if (sel != s_sel || col != s_col || q != s_quit_sel) {
        if (sel != s_sel) s_armed = 0;
        s_sel = sel; s_col = col;
        draw();
    }
}

static void mouse_click(int x, int y, int right)
{
    int a, b, what = hit(x, y, &a, &b);
    if (!what) {
        if (right) menu_close();                 /* right click away from everything: close */
        return;
    }
    mouse_move(x, y);
    switch (what) {
    case 1: if (a != s_page && !right) page_switch(); break;
    case 2:
        if (item_count(a)) settings_change(right ? -1 : +1);
        else if (!right) settings_change(0);    /* Back to the game, Quit (press again) */
        break;
    case 3: settings_change(-1); break;
    case 4: settings_change(right ? -1 : +1); break;
    case 5: controls_key(right ? VK_DELETE : VK_RETURN, 0); break;
    case 6: if (!right) controls_key(VK_RETURN, 0); break;
    case 7: if (!right) menu_key(VK_RETURN, 0); break;
    }
}

static LRESULT CALLBACK game_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (s_active) {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        switch (m) {
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorA(NULL, IDC_ARROW)); return TRUE; }
            break;
        case WM_MOUSEMOVE:
            mouse_move(x, y);
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            if (s_listen && s_mode == MODE_SETTINGS) {   /* a mouse button is the new key */
                listen_key(m == WM_LBUTTONDOWN ? 0x01 : m == WM_RBUTTONDOWN ? 0x02 : m == WM_MBUTTONDOWN ? 0x04
                           : HIWORD(w) == XBUTTON1 ? 0x05 : 0x06);
                return m == WM_XBUTTONDOWN ? TRUE : 0;
            }
            if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) mouse_click(x, y, m == WM_RBUTTONDOWN);
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
            return m == WM_XBUTTONUP ? TRUE : 0;
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(w);
            if (s_listen && s_mode == MODE_SETTINGS) { listen_key(d > 0 ? 0x0E : 0x0F); return 0; }
            if (s_mode == MODE_SETTINGS) menu_key(d > 0 ? VK_UP : VK_DOWN, 0);
            return 0;
        }
        }
    }
    return CallWindowProcA(s_game_proc, h, m, w, l);
}

static void menu_open(HWND game, int mode)
{
    s_game = game;
    if (!s_ov) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = ov_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "CounterStrikeXBMenu";
        RegisterClassA(&wc);
        s_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                               wc.lpszClassName, "", WS_POPUP, 0, 0, 1, 1, game, NULL, wc.hInstance, NULL);
        if (!s_ov) return;
    }
    s_mode = mode; s_sel = 0; s_quit_sel = 0; s_armed = 0; s_col = 0; s_listen = 0; s_status[0] = 0;
    if (mode == MODE_SETTINGS) cs_keys_load(&s_keys);
    memset(s_row, 0, sizeof s_row); memset(s_prev_r, 0, sizeof s_prev_r); memset(s_next_r, 0, sizeof s_next_r);
    memset(s_tab_r, 0, sizeof s_tab_r); memset(s_slot_r, 0, sizeof s_slot_r); memset(&s_reset_r, 0, sizeof s_reset_r);
    if (!s_active) {
        xbox_InputKeysClear();                   /* nothing the game saw held stays held */
        xbox_InputSetHold(1);
        mouse_take(game);
        s_pad_seen = 0; s_pad_release = 0;
        InterlockedExchange(&s_active, 1);
        SetTimer(s_ov, 1, 16, NULL);
    }
    draw();
    ShowWindow(s_ov, SW_SHOWNOACTIVATE);
}

static void menu_close(void)
{
    if (!s_active) return;
    s_listen = 0;
    mouse_give_back();
    ShowWindow(s_ov, SW_HIDE);                   /* the timer runs on until the pad is given back */
    xbox_InputKeysClear();
    InterlockedExchange(&s_active, 0);
    s_pad_release = 1;                           /* the pad stays held until its buttons are let go */
}

static void quit_game(void)
{
    menu_close();
    if (s_game) PostMessageA(s_game, WM_CLOSE, 0, 0);   /* the window's own exit path */
}

/* Left/Right/Enter on a settings line. */
static void settings_change(int dir)
{
    int n = item_count(s_sel);
    if (n) {
        item_set(s_sel, (item_get(s_sel) + dir + n) % n);
        s_armed = 0;
    } else if (dir == 0) {
        if (s_sel == IT_CLOSE) { menu_close(); return; }
        if (s_sel == IT_QUIT) { if (s_armed) { quit_game(); return; } s_armed = 1; }
    }
    draw();
}

/* The CONTROLS page: keymap.ini written, and the toolkit reads it again now. */
static void keys_changed(void)
{
    if (!cs_keys_save(&s_keys)) snprintf(s_status, sizeof s_status, "keymap.ini could not be saved.");
    xbox_InputKeyboardReload();
}

/* The waiting slot gets vk (a key, a mouse button or a wheel notch). */
static void listen_key(int vk)
{
    char n[48];
    const CsKeyAction *a = &cs_key_actions[s_sel];
    int from;
    if (vk == VK_ESCAPE) { s_listen = 0; snprintf(s_status, sizeof s_status, "Cancelled."); draw(); return; }
    if (cs_keys_reserved(vk)) {
        snprintf(s_status, sizeof s_status, "That key belongs to the game itself (Esc, F1 settings, F11 or Alt+Enter "
                 "fullscreen). Press another key.");
        s_listen = 0;
        draw();
        return;
    }
    from = cs_keys_assign(&s_keys, a->control, s_col, vk);
    cs_keys_name(vk, n, sizeof n);
    if (from >= 0 && from != a->control) {
        int k;
        for (k = 0; k < cs_key_action_count && cs_key_actions[k].control != from; k++) ;
        snprintf(s_status, sizeof s_status, "%s is now \"%s\" (it was \"%s\").", n, a->label,
                 k < cs_key_action_count ? cs_key_actions[k].label : "another action");
    } else
        snprintf(s_status, sizeof s_status, "%s is now \"%s\".", n, a->label);
    s_listen = 0;
    keys_changed();
    draw();
}

static void page_switch(void)
{
    s_page = !s_page;
    s_col = 0; s_armed = 0; s_status[0] = 0;
    if (s_sel >= 0) s_sel = 0;
    draw();
}

/* A key on the CONTROLS page. */
static void controls_key(WPARAM w, int repeat)
{
    int n = cs_key_action_count + 1;             /* the actions, then the reset button */
    if (w == VK_UP || w == VK_DOWN) {
        s_sel += w == VK_UP ? -1 : 1;
        if (s_sel < -1) s_sel = -1;
        if (s_sel >= n) s_sel = n - 1;
        s_status[0] = 0;
    } else if (w == VK_LEFT || w == VK_RIGHT) {
        if (s_sel < 0) { page_switch(); return; }
        s_col += w == VK_LEFT ? -1 : 1;
        if (s_col < 0) s_col = 0;
        if (s_col >= CSK_SLOTS) s_col = CSK_SLOTS - 1;
    } else if ((w == VK_RETURN || w == VK_SPACE) && !repeat) {
        if (s_sel < 0) s_sel = 0;
        else if (s_sel == cs_key_action_count) {
            cs_keys_defaults(&s_keys);
            snprintf(s_status, sizeof s_status, "The default keys are back.");
            keys_changed();
        } else {
            s_listen = 1;
        }
    } else if (w == VK_DELETE && s_sel >= 0 && s_sel < cs_key_action_count) {
        s_keys.vk[cs_key_actions[s_sel].control][s_col] = 0;
        snprintf(s_status, sizeof s_status, "Cleared.");
        keys_changed();
    } else
        return;
    draw();
}

/* A key for an open menu. */
static void menu_key(WPARAM w, int repeat)
{
    if (s_mode == MODE_SETTINGS && s_listen) {       /* every key is the new key */
        if (!repeat) listen_key((int)w);
        return;
    }
    if (s_mode == MODE_SETTINGS && !repeat && (w == VK_TAB || w == VK_PRIOR || w == VK_NEXT)) {
        page_switch();
        return;
    }
    switch (w == VK_RETURN || w == VK_SPACE ? -1 : xbox_InputKeyFaceButton((int)w)) {
    case 0: w = VK_RETURN; break;                    /* the player's A key confirms */
    case 1: if (!repeat) menu_close(); return;       /* and their B key closes */
    }
    if (s_mode == MODE_SETTINGS && s_page == PAGE_CONTROLS) {
        controls_key(w, repeat);
        return;
    }
    if (s_mode == MODE_SETTINGS) {
        if (w == VK_UP || w == VK_DOWN) {
            s_sel += w == VK_UP ? -1 : 1;              /* up from the first row: the tabs */
            if (s_sel < -1) s_sel = -1;
            if (s_sel >= IT_COUNT) s_sel = IT_COUNT - 1;
            s_armed = 0;
            draw();
        } else if (s_sel < 0) {
            if (w == VK_LEFT || w == VK_RIGHT) page_switch();
            else if ((w == VK_RETURN || w == VK_SPACE) && !repeat) { s_sel = 0; draw(); }
        } else if (w == VK_LEFT) settings_change(-1);
        else if (w == VK_RIGHT) settings_change(+1);
        else if ((w == VK_RETURN || w == VK_SPACE) && !repeat)
            settings_change(item_count(s_sel) ? +1 : 0);
        return;
    }
    if (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB) {
        s_quit_sel = w == VK_LEFT ? 0 : w == VK_RIGHT ? 1 : !s_quit_sel;
        draw();
    } else if (s_mode == MODE_LIVE) {
        if (!repeat && (w == VK_RETURN || w == VK_SPACE)) {
            if (s_quit_sel == 0) {                   /* OFF: mouse aim off and back to 4:3, saved; ON keeps them */
                g_cs_settings.mouse_aim = 0;
                g_cs_settings.widescreen = 0;
                cs_mouse_apply();
                cs_wide_apply();
                cs_d3d11_settings_changed();
                cs_settings_save(&g_cs_settings);
            }
            menu_close();
        }
    } else if (!repeat && (w == VK_RETURN || w == VK_SPACE)) {
        if (s_quit_sel) quit_game(); else menu_close();
    } else if (!repeat && w == 'Y') quit_game();
    else if (!repeat && w == 'N') menu_close();
}

/* The toolkit's key filter (window thread): 1 keeps the key from the game. */
static int key_filter(void *hwnd, unsigned msg, uintptr_t w, intptr_t l)
{
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    int repeat = (l & (1L << 30)) != 0;
    if (s_active && s_listen && w != VK_F1) {      /* a slot waits: the key is the new key (Esc cancels) */
        if (down && !repeat) listen_key((int)w);
        return 1;
    }
    if (w == VK_F1) {
        if (down && !repeat) {
            if (!s_active) menu_open((HWND)hwnd, MODE_SETTINGS);
            else if (s_mode == MODE_SETTINGS) menu_close();
            else { s_mode = MODE_SETTINGS; s_sel = 0; draw(); }
        }
        return 1;
    }
    if (w == VK_LIVE_NOTICE) {                   /* cs_overlay_live_refused, posted */
        if (down && !s_active) menu_open((HWND)hwnd, MODE_LIVE);
        return 1;
    }
    if (w == VK_ESCAPE) {
        if (down && !repeat) {
            if (s_active) menu_close();
            else menu_open((HWND)hwnd, MODE_QUIT);
        }
        return 1;
    }
    if (!s_active) return 0;
    if (down) menu_key((WPARAM)w, repeat);
    return 1;                                    /* while a menu is open, every key is the menu's */
}

/* Controller 1 drives an open menu like the keyboard: D-pad or left stick,
 * A confirms, B or START closes. Buttons held when it opened are not presses. */
static void pad_tick(void)
{
    XINPUT_STATE st;
    WORD b, press;
    DWORD now = GetTickCount();
    if (s_listen) {                              /* the mouse is game_proc's; B on the pad cancels */
        if (xbox_InputHostPad(0, &st) == ERROR_SUCCESS) {
            WORD pb = st.Gamepad.wButtons, pp = (WORD)(pb & ~s_pad_prev);
            s_pad_prev = pb;
            if (pp & XINPUT_GAMEPAD_B) listen_key(VK_ESCAPE);
        }
        return;
    }
    if (xbox_InputHostPad(0, &st) != ERROR_SUCCESS) { s_pad_prev = 0; s_pad_seen = 1; return; }
    b = st.Gamepad.wButtons;
    if (st.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (!s_pad_seen) { s_pad_prev = b; s_pad_seen = 1; return; }
    press = (WORD)(b & ~s_pad_prev);
    s_pad_prev = b;
    if (press & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) { menu_close(); return; }
    if (s_mode == MODE_SETTINGS && (press & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
        page_switch();
        return;
    }
    if (s_mode == MODE_SETTINGS && s_page == PAGE_CONTROLS && (press & XINPUT_GAMEPAD_X)) {   /* X clears */
        menu_key(VK_DELETE, 0);
        return;
    }
    if (press & XINPUT_GAMEPAD_A) { menu_key(VK_RETURN, 0); if (!s_active) return; }
    {
        WORD dirs = (WORD)(b & 0x000F), fire = 0;
        if (press & 0x000F) { fire = (WORD)(press & 0x000F); s_pad_repeat = now + 350; }
        else if (dirs && now >= s_pad_repeat) { fire = dirs; s_pad_repeat = now + 110; }
        if (fire & XINPUT_GAMEPAD_DPAD_UP) menu_key(VK_UP, 0);
        else if (fire & XINPUT_GAMEPAD_DPAD_DOWN) menu_key(VK_DOWN, 0);
        else if (fire & XINPUT_GAMEPAD_DPAD_LEFT) menu_key(VK_LEFT, 0);
        else if (fire & XINPUT_GAMEPAD_DPAD_RIGHT) menu_key(VK_RIGHT, 0);
    }
}

/* Port 0 on its way to the game (the game's input thread): BACK held and START
 * pressed opens the settings menu. Both buttons are taken out of what the game
 * sees until they are let go, so the game's own pause menu does not open too.
 * The menu itself opens on the window thread, through the F1 key's own path. */
static void pad_filter(CsPadState *s)
{
    static int chord, start_was;
    if (!s_active) {
        /* Mouse aim on, outside play (menus, intros): left click is START (it
         * passes the title screen and the prompts), right click B, instead of
         * the triggers they are in a match. */
        int c = cs_mouse_menu_clicks();
        if (c & 1) { s->wButtons |= PAD_START; s->bAnalogButtons[7] = 0; s->dwPacketNumber++; }
        if (c & 2) { s->bAnalogButtons[1] = 255; s->bAnalogButtons[6] = 0; s->dwPacketNumber++; }
    }
    WORD b = s->wButtons;
    int start = (b & PAD_START) != 0, back = (b & PAD_BACK) != 0;
    if (chord) {
        if (!start && !back) chord = 0;
        s->wButtons &= (WORD)~(PAD_START | PAD_BACK);
    } else if (back && start && !start_was && !s_active) {
        HWND h = (HWND)xbox_FramebufferWindowHwnd();
        chord = 1;
        s->wButtons &= (WORD)~(PAD_START | PAD_BACK);
        if (h) {
            PostMessageA(h, WM_KEYDOWN, VK_F1, 1);
            PostMessageA(h, WM_KEYUP, VK_F1, (LPARAM)(1u | 3u << 30));
        }
    }
    start_was = start;
}

/* Xbox Live was refused because mouse aim is on (cs_mouse.c, on the NIC's
 * thread): a notice like the quit prompt naming the setting, with a button
 * that turns it off. Opened on the window thread through a posted key, as
 * BACK+START opens the settings. */
void cs_overlay_live_refused(void)
{
    HWND h = (HWND)xbox_FramebufferWindowHwnd();
    if (h && !s_active) {
        PostMessageA(h, WM_KEYDOWN, VK_LIVE_NOTICE, 1);
        PostMessageA(h, WM_KEYUP, VK_LIVE_NOTICE, (LPARAM)(1u | 3u << 30));
    }
}

void cs_overlay_init(void)
{
    xbox_FramebufferSetKeyFilter(key_filter);
    xbox_InputSetPort0Filter(pad_filter);
}
