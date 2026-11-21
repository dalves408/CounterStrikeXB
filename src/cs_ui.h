/*
 * cs_ui.h -- GDI drawing in the look of the Counter-Strike (Xbox) manual:
 * a navy blueprint page (grid, faint gold rings), heavy white headings after a
 * yellow arrow notch over a gold double rule, slanted navy label boxes with a
 * pale edge, gold accents, and the round page dial in the bottom corners.
 * Shared by the in-game F1 menu and quit prompt (cs_overlay.c) and config.exe.
 * Layouts are made for a 960x640 page and scaled (CsUiFonts.s).
 */
#ifndef CS_UI_H
#define CS_UI_H

#include <windows.h>

typedef struct {
    float s;                         /* scale: 1.0 at a 960x640 page */
    HFONT title, head, body, fine, value;
} CsUiFonts;

/* The manual's palette. */
#define CSUI_NAVY      RGB(10, 26, 56)     /* label boxes */
#define CSUI_NAVY_LIT  RGB(24, 56, 104)    /* the selected one */
#define CSUI_EDGE      RGB(132, 166, 206)  /* their pale edge */
#define CSUI_GOLD      RGB(226, 198, 42)   /* rules, leader lines, arrows */
#define CSUI_WHITE     RGB(244, 246, 250)
#define CSUI_TEXT      RGB(220, 228, 238)
#define CSUI_CREAM     RGB(240, 232, 176)  /* the note boxes' text */
#define CSUI_DIM       RGB(140, 162, 192)

void csui_fonts_make(CsUiFonts *f, float scale);
void csui_fonts_free(CsUiFonts *f);
int  csui_px(const CsUiFonts *f, int v);
/* The scale and top-left of a 960x640 page centred in a w x h area. */
float csui_fit(int w, int h, int *x, int *y);

void csui_background(HDC dc, const RECT *r);
/* The page dial in a bottom corner of r (right != 0: the right one). */
void csui_dial(HDC dc, const CsUiFonts *f, const RECT *r, int right);
/* A box with its left edge slanted (bottom-left further out), as the manual's labels. */
void csui_slant(HDC dc, const RECT *r, int slant, COLORREF fill, COLORREF edge);
void csui_text(HDC dc, HFONT font, COLORREF color, const RECT *r, const char *s, UINT fmt);
/* A heading with the manual's dark outline. */
void csui_heading(HDC dc, HFONT font, const RECT *r, const char *s, UINT fmt);

/* The page title: the yellow notch at the left of `band`, the title, the gold
 * double rule. Returns the x where the title ends (tabs go after it). */
int  csui_title(HDC dc, const CsUiFonts *f, const RECT *band, const char *title);
/* A tab in the title band: active (the page shown), focused (the tab row has
 * the selection). */
void csui_tab(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, int active, int focused);
/* A key slot on the Controls page: the key's name, selected, or waiting for a key. */
void csui_keyslot(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, int selected, int listening);
/* The width text takes in a font. */
int  csui_text_w(HDC dc, HFONT font, const char *s);
/* A section heading on a slanted box, with a gold leader line to the right. */
void csui_section(HDC dc, const CsUiFonts *f, const RECT *row, const char *text);
/* A setting: label, then "<  value  >" (the arrows' rectangles in *left, *right). */
void csui_option(HDC dc, const CsUiFonts *f, const RECT *row, const char *label, const char *value,
                 int selected, RECT *left, RECT *right);
void csui_button(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, int selected);
/* The manual's note box: navy, cream text, wrapped. */
void csui_note(HDC dc, const CsUiFonts *f, const RECT *r, const char *text, COLORREF color);
/* A framed panel (the quit prompt): navy, pale edge, the top-right corner cut. */
void csui_panel(HDC dc, const CsUiFonts *f, const RECT *r);

#endif
