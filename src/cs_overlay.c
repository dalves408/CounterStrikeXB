/*
 * cs_overlay.c -- the ESC quit prompt and the F1 settings menu.
 *
 * Both are drawn with GDI into one layered popup over the game window's client
 * area (owned by it, never activated, so the keys keep going to the game
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
#include "video/video_player.h"

extern void xbox_InputKeysClear(void);      /* toolkit input/keyboard.c */
extern int  xbox_InputKeyFaceButton(int vk); /* toolkit input/keyboard.c: 0 A, 1 B, ... or -1 */
extern void xbox_InputSetHold(int on);      /* toolkit input/xinput_device.c */
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

enum { MODE_QUIT, MODE_SETTINGS };
enum { IT_SCALE, IT_FULLSCREEN, IT_STRETCH, IT_VSYNC, IT_RATE, IT_MSAA, IT_ANISO, IT_HIRES, IT_CLOSE, IT_QUIT, IT_COUNT };
static const char *const ITEM_LABEL[IT_COUNT] = {
    "Render scale", "Fullscreen", "Stretch image", "VSync", "Frame rate cap",
    "Anti-aliasing", "Anisotropic filtering", "Hi-res textures", "Back to the game", "Quit Counter-Strike" };
static const char *const OFFON[2] = { "Off", "On" };
static const char *const STRETCH[2] = { "Off (4:3)", "On (fill the window)" };

static volatile LONG s_active;
static int s_mode, s_sel, s_quit_sel, s_armed;
static HWND s_ov, s_game;
static int s_cw, s_ch, s_ox, s_oy;          /* what was last drawn */
static RECT s_row[IT_COUNT], s_btn[2];
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
    case IT_FULLSCREEN: case IT_STRETCH: case IT_VSYNC: case IT_HIRES: return 2;
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
    case IT_VSYNC: return s->vsync;
    case IT_RATE: return cs_settings_index(CS_SET_RATE_HZ, CS_SET_RATES,
                                           cs_settings_rate_effective(s->frame_rate, cs_d3d11_rate_max()));
    case IT_MSAA: return cs_settings_index(CS_SET_MSAA_N, CS_SET_MSAAS, s->msaa);
    case IT_ANISO: return cs_settings_index(CS_SET_ANISO_N, CS_SET_ANISOS, s->aniso);
    case IT_HIRES: return s->hires;
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
    case IT_FULLSCREEN: case IT_VSYNC: case IT_HIRES: return OFFON[v];
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
    case IT_VSYNC: s->vsync = v; break;
    case IT_RATE: s->frame_rate = CS_SET_RATE_HZ[v]; break;
    case IT_MSAA: s->msaa = CS_SET_MSAA_N[v]; cs_d3d11_settings_changed(); break;
    case IT_ANISO: s->aniso = CS_SET_ANISO_N[v]; cs_d3d11_settings_changed(); break;
    case IT_HIRES: s->hires = v; cs_d3d11_settings_changed(); break;
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
    cs_d3d11_settings_changed();
    cs_diag_log_video("settings.ini changed");
}

/* ---- drawing --------------------------------------------------------------- */
#define C_FRAME  RGB(222, 190, 62)     /* the title's menu yellow */
#define C_PANEL  RGB(22, 24, 26)
#define C_ROW    RGB(52, 56, 60)
#define C_TEXT   RGB(224, 226, 214)
#define C_DIM    RGB(150, 154, 140)

static HFONT mkfont(int px, int weight)
{
    HFONT f = CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Bahnschrift");
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void draw(void)
{
    RECT cr, r, panel;
    POINT o = { 0, 0 }, p0 = { 0, 0 };
    SIZE sz;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc, dc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldf;
    HFONT ft, fr, fs;
    BLENDFUNCTION bf;
    uint32_t *px;
    int cw, ch, i, x, y, rh, pw, ph;

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
    for (i = 0; i < cw * ch; i++) px[i] = 0xA0000000u;           /* the game dimmed behind */
    SetBkMode(dc, TRANSPARENT);

    if (s_mode == MODE_SETTINGS) {
        rh = ch * 6 / 100; if (rh < 22) rh = 22;
        pw = rh * 15; if (pw > cw * 94 / 100) { pw = cw * 94 / 100; rh = pw / 15; }
        ph = rh * (IT_COUNT + 3);
    } else {
        rh = ch * 9 / 100; if (rh < 26) rh = 26;
        pw = rh * 9; if (pw > cw * 90 / 100) { pw = cw * 90 / 100; rh = pw / 9; }
        ph = rh * 4;
    }
    SetRect(&panel, (cw - pw) / 2, (ch - ph) / 2, (cw - pw) / 2 + pw, (ch - ph) / 2 + ph);
    fill(dc, &panel, C_FRAME);
    r = panel; InflateRect(&r, -(rh / 12 + 2), -(rh / 12 + 2));
    fill(dc, &r, C_PANEL);
    ft = mkfont(rh * 62 / 100, FW_BOLD);
    fr = mkfont(rh * 46 / 100, FW_NORMAL);
    fs = mkfont(rh * 32 / 100, FW_NORMAL);
    oldf = SelectObject(dc, ft);
    SetTextColor(dc, C_FRAME);

    if (s_mode == MODE_SETTINGS) {
        SetRect(&r, panel.left, panel.top + rh / 5, panel.right, panel.top + rh * 6 / 5);
        DrawTextA(dc, "SETTINGS", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, fr);
        for (i = 0; i < IT_COUNT; i++) {
            int sel = i == s_sel;
            char buf[96];
            x = panel.left + rh / 3;
            y = panel.top + rh * 13 / 10 + i * rh;
            SetRect(&s_row[i], x, y, panel.right - rh / 3, y + rh * 84 / 100);
            fill(dc, &s_row[i], sel ? C_FRAME : C_ROW);
            SetTextColor(dc, sel ? RGB(16, 16, 16) : C_TEXT);
            r = s_row[i]; r.left += rh / 3;
            DrawTextA(dc, ITEM_LABEL[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (item_count(i)) snprintf(buf, sizeof buf, "<  %s  >", item_text(i));
            else if (i == IT_QUIT && s_armed) snprintf(buf, sizeof buf, "press again to quit");
            else buf[0] = 0;
            r = s_row[i]; r.right -= rh / 3;
            DrawTextA(dc, buf, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, fs);
        SetTextColor(dc, C_DIM);
        SetRect(&r, panel.left + rh / 3, panel.top + rh * 13 / 10 + IT_COUNT * rh + rh / 8,
                panel.right - rh / 3, panel.bottom - rh / 8);
        DrawTextA(dc, "Up/Down choose, Left/Right change, Enter confirms. Saved as you go. "
                      "F1, Esc or B closes. The game keeps running underneath.",
                  -1, &r, DT_LEFT | DT_WORDBREAK);
    } else {
        int bw = pw * 3 / 10, bh = rh * 9 / 10, by = panel.top + rh * 2;
        SetRect(&r, panel.left, panel.top + rh / 3, panel.right, panel.top + rh * 3 / 2);
        DrawTextA(dc, "Quit Counter-Strike?", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetRect(&s_btn[0], panel.left + pw / 2 - bw - pw / 24, by, panel.left + pw / 2 - pw / 24, by + bh);
        SetRect(&s_btn[1], panel.left + pw / 2 + pw / 24, by, panel.left + pw / 2 + pw / 24 + bw, by + bh);
        SelectObject(dc, fr);
        for (i = 0; i < 2; i++) {
            fill(dc, &s_btn[i], s_quit_sel == i ? C_FRAME : C_ROW);
            SetTextColor(dc, s_quit_sel == i ? RGB(16, 16, 16) : C_TEXT);
            DrawTextA(dc, i ? "YES" : "NO", -1, &s_btn[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(dc, oldf);
    DeleteObject(ft); DeleteObject(fr); DeleteObject(fs);
    GdiFlush();
    for (y = panel.top; y < panel.bottom; y++)                    /* the panel itself is opaque */
        for (x = panel.left; x < panel.right; x++)
            px[(size_t)y * cw + x] |= 0xFF000000u;

    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
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
    s_mode = mode; s_sel = 0; s_quit_sel = 0; s_armed = 0;
    if (!s_active) {
        xbox_InputKeysClear();                   /* nothing the game saw held stays held */
        xbox_InputSetHold(1);
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

/* A key for an open menu. */
static void menu_key(WPARAM w, int repeat)
{
    switch (w == VK_RETURN || w == VK_SPACE ? -1 : xbox_InputKeyFaceButton((int)w)) {
    case 0: w = VK_RETURN; break;                    /* the player's A key confirms */
    case 1: if (!repeat) menu_close(); return;       /* and their B key closes */
    }
    if (s_mode == MODE_SETTINGS) {
        if (w == VK_UP || w == VK_DOWN) {
            s_sel = (s_sel + (w == VK_UP ? IT_COUNT - 1 : 1)) % IT_COUNT;
            s_armed = 0;
            draw();
        } else if (w == VK_LEFT) settings_change(-1);
        else if (w == VK_RIGHT) settings_change(+1);
        else if ((w == VK_RETURN || w == VK_SPACE) && !repeat)
            settings_change(item_count(s_sel) ? +1 : 0);
        return;
    }
    if (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB) {
        s_quit_sel = w == VK_LEFT ? 0 : w == VK_RIGHT ? 1 : !s_quit_sel;
        draw();
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
    if (w == VK_F1) {
        if (down && !repeat) {
            if (!s_active) menu_open((HWND)hwnd, MODE_SETTINGS);
            else if (s_mode == MODE_SETTINGS) menu_close();
            else { s_mode = MODE_SETTINGS; s_sel = 0; draw(); }
        }
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

void cs_overlay_init(void)
{
    xbox_FramebufferSetKeyFilter(key_filter);
    xbox_InputSetPort0Filter(pad_filter);
}
