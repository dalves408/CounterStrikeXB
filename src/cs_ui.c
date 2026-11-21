/*
 * cs_ui.c -- see cs_ui.h. Plain GDI, so the same code draws config.exe's
 * window and the game's F1 menu.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cs_ui.h"

#define C_BG_TOP   RGB(34, 70, 112)
#define C_BG_MID   RGB(18, 44, 82)
#define C_BG_BOT   RGB(8, 22, 46)
#define C_GRID     RGB(38, 74, 116)
#define C_RING     RGB(96, 112, 74)
#define C_OUTLINE  RGB(6, 16, 36)
#define C_DIAL     RGB(176, 178, 170)
#define C_DIAL_DK  RGB(96, 100, 100)
#define C_DIAL_YEL RGB(232, 222, 48)

int csui_px(const CsUiFonts *f, int v) { return (int)(v * f->s + 0.5f); }

float csui_fit(int w, int h, int *x, int *y)
{
    float s = (float)w / 960.0f;
    if ((float)h / 640.0f < s) s = (float)h / 640.0f;
    if (s < 0.4f) s = 0.4f;
    if (x) *x = (w - (int)(960 * s)) / 2;
    if (y) *y = (h - (int)(640 * s)) / 2;
    return s;
}

static int CALLBACK has_face_cb(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM l)
{
    (void)lf; (void)tm; (void)type;
    *(int *)l = 1;
    return 0;
}

static int has_face(const char *name)
{
    LOGFONTA lf;
    HDC dc = GetDC(NULL);
    int found = 0;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy(lf.lfFaceName, name, sizeof lf.lfFaceName - 1);
    EnumFontFamiliesExA(dc, &lf, has_face_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* Bahnschrift (Windows 10 and later) is close to the manual's type; the
 * headings are it at its heaviest, drawn wider, as the manual's extended face. */
static const char *face(void)
{
    static const char *pick;
    if (!pick) pick = has_face("Bahnschrift") ? "Bahnschrift" : "Segoe UI";
    return pick;
}

static HFONT mkfont(int px, int width, int weight)
{
    return CreateFontA(-px, width, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0,
                       CLEARTYPE_QUALITY, 0, face());
}

void csui_fonts_make(CsUiFonts *f, float scale)
{
    if (scale < 0.4f) scale = 0.4f;
    f->s = scale;
    f->title = mkfont(csui_px(f, 34), csui_px(f, 24), FW_BLACK);   /* wide, as the manual's headings */
    f->head = mkfont(csui_px(f, 19), csui_px(f, 12), FW_BLACK);
    f->body = mkfont(csui_px(f, 16), 0, FW_NORMAL);
    f->fine = mkfont(csui_px(f, 13), 0, FW_NORMAL);
    f->value = mkfont(csui_px(f, 15), 0, FW_SEMIBOLD);
}

void csui_fonts_free(CsUiFonts *f)
{
    DeleteObject(f->title); DeleteObject(f->head); DeleteObject(f->body);
    DeleteObject(f->fine); DeleteObject(f->value);
    memset(f, 0, sizeof *f);
}

static COLORREF lerp(COLORREF a, COLORREF b, int t, int n)
{
    if (n <= 0) return a;
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / n,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / n,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / n);
}

static void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ o = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, NULL);
    LineTo(dc, x1, y1);
    SelectObject(dc, o);
    DeleteObject(pen);
}

static void poly(HDC dc, const POINT *p, int n, COLORREF fill, COLORREF edge)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Polygon(dc, p, n);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}

void csui_background(HDC dc, const RECT *r)
{
    int h = r->bottom - r->top, w = r->right - r->left, y, x, mid = h * 2 / 5;
    int step = h / 26 > 8 ? h / 26 : 8;
    HPEN pen;
    HGDIOBJ old;
    for (y = 0; y < h; y += 2) {
        RECT b;
        HBRUSH br = CreateSolidBrush(y < mid ? lerp(C_BG_TOP, C_BG_MID, y, mid)
                                             : lerp(C_BG_MID, C_BG_BOT, y - mid, h - mid));
        SetRect(&b, r->left, r->top + y, r->right, r->top + y + 2);
        FillRect(dc, &b, br);
        DeleteObject(br);
    }
    /* the blueprint grid */
    pen = CreatePen(PS_SOLID, 1, C_GRID);
    old = SelectObject(dc, pen);
    for (x = r->left + step / 2; x < r->right; x += step) { MoveToEx(dc, x, r->top, NULL); LineTo(dc, x, r->bottom); }
    for (y = r->top + step / 2; y < r->bottom; y += step) { MoveToEx(dc, r->left, y, NULL); LineTo(dc, r->right, y); }
    SelectObject(dc, old);
    DeleteObject(pen);
    /* the faint gold rings and a sweep behind the page */
    pen = CreatePen(PS_SOLID, 2, C_RING);
    old = SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, r->left + w / 2 - h * 6 / 10, r->top + h / 2 - h * 6 / 10, r->left + w / 2 + h * 6 / 10, r->top + h / 2 + h * 6 / 10);
    Ellipse(dc, r->left + w / 2 - h * 4 / 10, r->top + h / 2 - h * 4 / 10, r->left + w / 2 + h * 4 / 10, r->top + h / 2 + h * 4 / 10);
    Arc(dc, r->left - w / 3, r->top + h / 4, r->left + w * 4 / 3, r->top + h * 2,
        r->right, r->top + h / 2, r->left, r->top + h * 2 / 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void csui_dial(HDC dc, const CsUiFonts *f, const RECT *r, int right)
{
    int R = csui_px(f, 70), cx = right ? r->right : r->left, cy = r->bottom, i;
    HRGN clip = CreateRectRgn(r->left, r->top, r->right, r->bottom);
    HBRUSH grey = CreateSolidBrush(C_DIAL), yel = CreateSolidBrush(C_DIAL_YEL), dk = CreateSolidBrush(C_DIAL_DK);
    HPEN edge = CreatePen(PS_SOLID, csui_px(f, 2) > 0 ? csui_px(f, 2) : 1, C_DIAL_DK);
    HGDIOBJ ob, op;
    SelectClipRgn(dc, clip);
    op = SelectObject(dc, edge);
    ob = SelectObject(dc, dk);
    Ellipse(dc, cx - R - csui_px(f, 4), cy - R - csui_px(f, 4), cx + R + csui_px(f, 4), cy + R + csui_px(f, 4));
    SelectObject(dc, grey);
    Ellipse(dc, cx - R, cy - R, cx + R, cy + R);
    /* the dial's segments */
    for (i = 1; i < 4; i++) {
        double a = 3.14159265 / 2 * i / 4;
        int dx = (int)(R * cos(a)), dy = (int)(R * sin(a));
        line(dc, cx, cy, right ? cx - dx : cx + dx, cy - dy, C_DIAL_DK, csui_px(f, 3) > 0 ? csui_px(f, 3) : 1);
    }
    SelectObject(dc, yel);
    Ellipse(dc, cx - R * 45 / 100, cy - R * 45 / 100, cx + R * 45 / 100, cy + R * 45 / 100);
    SelectObject(dc, ob); SelectObject(dc, op);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip); DeleteObject(grey); DeleteObject(yel); DeleteObject(dk); DeleteObject(edge);
}

void csui_slant(HDC dc, const RECT *r, int slant, COLORREF fill, COLORREF edge)
{
    POINT p[4];
    p[0].x = r->left + slant; p[0].y = r->top;
    p[1].x = r->right - 1;    p[1].y = r->top;
    p[2].x = r->right - 1;    p[2].y = r->bottom - 1;
    p[3].x = r->left;         p[3].y = r->bottom - 1;
    poly(dc, p, 4, fill, edge);
}

void csui_text(HDC dc, HFONT font, COLORREF color, const RECT *r, const char *s, UINT fmt)
{
    RECT t = *r;
    HGDIOBJ o = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    if ((fmt & DT_WORDBREAK) && (fmt & DT_VCENTER)) {   /* DrawText centres only single lines */
        RECT c = t;
        DrawTextA(dc, s, -1, &c, fmt | DT_NOPREFIX | DT_CALCRECT);
        if (c.bottom - c.top < t.bottom - t.top) t.top += ((t.bottom - t.top) - (c.bottom - c.top)) / 2;
    }
    DrawTextA(dc, s, -1, &t, fmt | DT_NOPREFIX);
    SelectObject(dc, o);
}

void csui_heading(HDC dc, HFONT font, const RECT *r, const char *s, UINT fmt)
{
    static const int d[8][2] = { { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 }, { -2, -2 }, { 2, 2 }, { -2, 2 }, { 2, -2 } };
    int i;
    for (i = 0; i < 8; i++) {
        RECT t = *r;
        OffsetRect(&t, d[i][0], d[i][1]);
        csui_text(dc, font, C_OUTLINE, &t, s, fmt);
    }
    csui_text(dc, font, CSUI_WHITE, r, s, fmt);
}

static int text_w(HDC dc, HFONT font, const char *s)
{
    SIZE sz;
    HGDIOBJ o = SelectObject(dc, font);
    GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

int csui_title(HDC dc, const CsUiFonts *f, const RECT *band, const char *title)
{
    POINT p[3];
    RECT t;
    int h = band->bottom - band->top, rule = band->bottom - csui_px(f, 6);
    /* the yellow notch at the page edge */
    p[0].x = band->left;                    p[0].y = band->top + h / 8;
    p[1].x = band->left + csui_px(f, 26);   p[1].y = band->top + h / 2;
    p[2].x = band->left;                    p[2].y = band->bottom - h / 8;
    poly(dc, p, 3, CSUI_GOLD, CSUI_GOLD);
    /* the gold double rule under the title */
    line(dc, band->left, rule, band->right, rule, CSUI_GOLD, csui_px(f, 4) > 0 ? csui_px(f, 4) : 1);
    line(dc, band->left, rule + csui_px(f, 6), band->right, rule + csui_px(f, 6), RGB(150, 132, 30), 1);
    SetRect(&t, band->left + csui_px(f, 40), band->top, band->right, rule - csui_px(f, 4));
    csui_heading(dc, f->title, &t, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return t.left + text_w(dc, f->title, title);
}

int csui_text_w(HDC dc, HFONT font, const char *s) { return text_w(dc, font, s); }

void csui_tab(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, int active, int focused)
{
    COLORREF fill = active ? (focused ? CSUI_GOLD : CSUI_NAVY_LIT) : CSUI_NAVY;
    csui_slant(dc, r, csui_px(f, 10), fill, active ? CSUI_GOLD : CSUI_EDGE);
    csui_text(dc, f->head, active && focused ? RGB(16, 22, 34) : (active ? CSUI_WHITE : CSUI_DIM), r, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void csui_keyslot(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, int selected, int listening)
{
    COLORREF fill = listening ? RGB(92, 64, 8) : (selected ? CSUI_NAVY_LIT : CSUI_NAVY);
    COLORREF edge = listening || selected ? CSUI_GOLD : RGB(70, 98, 136);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Rectangle(dc, r->left, r->top, r->right, r->bottom);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
    csui_text(dc, f->fine, listening ? CSUI_CREAM : (text[0] == '-' && !text[1] ? CSUI_DIM : CSUI_WHITE), r,
              listening ? "Press a key" : text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void csui_section(HDC dc, const CsUiFonts *f, const RECT *row, const char *text)
{
    RECT box = *row, t;
    int w = text_w(dc, f->head, text) + csui_px(f, 44), mid = (row->top + row->bottom) / 2;
    if (box.left + w < box.right) box.right = box.left + w;
    csui_slant(dc, &box, csui_px(f, 14), CSUI_NAVY, CSUI_EDGE);
    line(dc, box.right, mid, row->right, mid, CSUI_GOLD, 1);
    t = box; t.left += csui_px(f, 20);
    csui_heading(dc, f->head, &t, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void tri(HDC dc, int cx, int cy, int s, int dir, COLORREF c)
{
    POINT p[3];
    p[0].x = cx + dir * s; p[0].y = cy;
    p[1].x = cx - dir * s; p[1].y = cy - s;
    p[2].x = cx - dir * s; p[2].y = cy + s;
    poly(dc, p, 3, c, c);
}

void csui_option(HDC dc, const CsUiFonts *f, const RECT *row, const char *label, const char *value,
                 int selected, RECT *left, RECT *right)
{
    RECT lr = *row, box, l, rr, v;
    int bw = (row->right - row->left) * 46 / 100, a;
    csui_slant(dc, row, csui_px(f, 12), selected ? CSUI_NAVY_LIT : CSUI_NAVY, selected ? CSUI_GOLD : CSUI_EDGE);
    lr.left += csui_px(f, 22);
    lr.right = row->right - bw - csui_px(f, 6);
    csui_text(dc, f->body, selected ? CSUI_WHITE : CSUI_TEXT, &lr, label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetRect(&box, row->right - bw, row->top + csui_px(f, 4), row->right - csui_px(f, 6), row->bottom - csui_px(f, 4));
    a = box.bottom - box.top;
    SetRect(&l, box.left, box.top, box.left + a, box.bottom);
    SetRect(&rr, box.right - a, box.top, box.right, box.bottom);
    tri(dc, (l.left + l.right) / 2, (l.top + l.bottom) / 2, csui_px(f, 5), -1, selected ? CSUI_GOLD : CSUI_DIM);
    tri(dc, (rr.left + rr.right) / 2, (rr.top + rr.bottom) / 2, csui_px(f, 5), 1, selected ? CSUI_GOLD : CSUI_DIM);
    SetRect(&v, l.right, box.top, rr.left, box.bottom);
    csui_text(dc, f->value, selected ? CSUI_CREAM : CSUI_WHITE, &v, value,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (left) *left = l;
    if (right) *right = rr;
}

void csui_button(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, int selected)
{
    csui_slant(dc, r, csui_px(f, 12), selected ? CSUI_GOLD : CSUI_NAVY, selected ? CSUI_GOLD : CSUI_EDGE);
    csui_text(dc, f->head, selected ? RGB(16, 22, 34) : CSUI_WHITE, r, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void csui_note(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, COLORREF color)
{
    RECT t = *r;
    HBRUSH br = CreateSolidBrush(CSUI_NAVY);
    FillRect(dc, r, br);
    DeleteObject(br);
    InflateRect(&t, -csui_px(f, 12), -csui_px(f, 6));
    csui_text(dc, f->fine, color, &t, text, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
}

void csui_panel(HDC dc, const CsUiFonts *f, const RECT *r)
{
    POINT p[5];
    int c = csui_px(f, 22);
    p[0].x = r->left;          p[0].y = r->top;
    p[1].x = r->right - 1 - c; p[1].y = r->top;
    p[2].x = r->right - 1;     p[2].y = r->top + c;
    p[3].x = r->right - 1;     p[3].y = r->bottom - 1;
    p[4].x = r->left;          p[4].y = r->bottom - 1;
    {   /* the manual's blueprint page inside it */
        HRGN rg = CreatePolygonRgn(p, 5, WINDING);
        SelectClipRgn(dc, rg);
        csui_background(dc, r);
        SelectClipRgn(dc, NULL);
        DeleteObject(rg);
    }
    {
        HPEN pen = CreatePen(PS_SOLID, 1, CSUI_EDGE);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Polygon(dc, p, 5);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen);
    }
}
