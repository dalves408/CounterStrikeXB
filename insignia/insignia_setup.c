/*
 * insignia_setup.exe -- puts a player's own Xbox console identity and Xbox
 * Live (Insignia) account into the Counter-Strike recompilation.
 *
 * Xbox Live knows a console by its EEPROM and its hard-drive key, and keeps
 * the console's accounts in a hidden area of its drive (partition 0, sectors
 * 8 and up: sector 8 is the network settings, 9.. the machine account and the
 * players' accounts, each signed with a key from the hard-drive key). The game
 * reads the same things here:
 *
 *   %LOCALAPPDATA%\CounterStrikeXB\eeprom.bin               the console's EEPROM
 *   %LOCALAPPDATA%\CounterStrikeXB\settings.ini             [insignia] hdd_key=
 *   %LOCALAPPDATA%\CounterStrikeXB\drive_config.bin         the drive's config sectors 8-23
 *
 * This program checks what the player gives it and writes those. An account
 * comes either from Insignia (Recover from Insignia: the gamertag and the
 * 16-digit number on the insignia.live Web Dashboard; the game does the
 * recovery itself, cs_account.c) or from a dump of the console's drive (the
 * first sectors are enough), a raw drive image, or an xemu drive (.qcow2).
 * From a drive only sectors 9..19 (accounts) are copied: the network settings
 * stay this PC's (the game's router hands them out). Creating a new account
 * needs the dashboard's sign-up and is not here yet. Nothing secret is shown:
 * the hard-drive key and the recovery number are masked, and the serial
 * number shows only its last digits.
 */
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_settings.h"
#include "cs_ui.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

enum { ID_EE_EDIT = 100, ID_EE_BROWSE, ID_EE_STATUS, ID_KEY_EDIT, ID_KEY_SHOW, ID_KEY_FILE, ID_KEY_BIOS, ID_KEY_STATUS,
       ID_ACC_EDIT, ID_ACC_BROWSE, ID_ACC_STATUS, ID_CREATE, ID_SAVE, ID_CLOSE, ID_STATUS,
       ID_LAN_KEY, ID_LAN_STATUS };

static HWND s_main, s_ee, s_ee_st, s_key, s_key_st, s_acc, s_acc_st, s_status;
static uint8_t s_eeprom[256];
static int s_ee_ok, s_acc_ok;
static uint8_t s_bios_key_sum[20];          /* SHA-1 of a key decrypted from the BIOS (verified) */
static int s_have_bios_key;
static uint8_t s_lan_key[16];               /* the game's system-link key, from the BIOS (lankey.bin) */
static int s_have_lan_key;
#define ACCOUNT_SECTORS 11                  /* 9 machine account, 10, 11 user directory, 12-19 user slots */
static uint8_t s_sectors[ACCOUNT_SECTORS][512];   /* the drive's sectors 9..19 */
static char s_gamertags[256];
static void say(HWND w, const char *t);
static void check_key(void);
static int key_state(void);
static int s_cli;                           /* run from the command line: no message boxes */

/* ---- the look: the game's manual (src/cs_ui.c), as config.exe and the F1 menu ----
 * The window keeps Windows' own edit boxes (the masked key and number) and
 * buttons; they are drawn here: the page behind them, the steps' headings where
 * group boxes were, buttons in the manual's style, navy edit boxes. */
#define TOP 70                              /* the title band above the controls */
#define K(v) ((v) * 5 / 4)                  /* the layout below, made for the old dialog font, a size up */
typedef struct { RECT r; char text[64]; } Head;
static Head s_heads[8];
static int s_nheads;
static CsUiFonts s_f;
static HFONT s_label_font, s_button_font;
static HBRUSH s_edit_brush;
static int s_key_shown;                     /* the hard-drive key shown instead of masked */

static void ui_init(void)
{
    if (s_label_font) return;
    csui_fonts_make(&s_f, 1.0f);
    s_label_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0,
                               CLEARTYPE_QUALITY, 0, "Bahnschrift");
    s_edit_brush = CreateSolidBrush(CSUI_NAVY);
    s_button_font = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0,
                                CLEARTYPE_QUALITY, 0, "Bahnschrift");
}

/* The page: background, title, the steps' headings. */
static void ui_paint(HWND h, const char *title)
{
    PAINTSTRUCT ps;
    RECT cr, band;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bm, old;
    int i;
    GetClientRect(h, &cr);
    mem = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    old = (HBITMAP)SelectObject(mem, bm);
    csui_background(mem, &cr);
    SetRect(&band, 0, 6, cr.right, TOP - 6);
    csui_title(mem, &s_f, &band, title);
    if (h == s_main)
        for (i = 0; i < s_nheads; i++) csui_section(mem, &s_f, &s_heads[i].r, s_heads[i].text);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

/* Labels on the page, light; edit boxes navy. 0: not ours. */
static LRESULT ui_ctlcolor(UINT m, HDC dc, HWND ctl)
{
    char cls[16];
    GetClassNameA(ctl, cls, sizeof cls);
    SetBkMode(dc, TRANSPARENT);
    if (m == WM_CTLCOLOREDIT || !_stricmp(cls, "Edit")) {
        SetTextColor(dc, CSUI_WHITE);
        SetBkColor(dc, CSUI_NAVY);
        SetBkMode(dc, OPAQUE);
        return (LRESULT)s_edit_brush;
    }
    SetTextColor(dc, GetWindowLongPtrA(ctl, GWLP_ID) >= 100 ? CSUI_CREAM : CSUI_TEXT);   /* status lines cream */
    return (LRESULT)GetStockObject(NULL_BRUSH);
}

/* A button in the manual's style, on the page behind it. */
static void ui_drawitem(HWND parent, const DRAWITEMSTRUCT *d)
{
    RECT pr, r = d->rcItem;
    char t[96];
    POINT o = { 0, 0 };
    int id = (int)d->CtlID;
    GetClientRect(parent, &pr);
    MapWindowPoints(d->hwndItem, parent, &o, 1);
    SetViewportOrgEx(d->hDC, -o.x, -o.y, NULL);              /* the page as it is behind the button */
    csui_background(d->hDC, &pr);
    SetViewportOrgEx(d->hDC, 0, 0, NULL);
    GetWindowTextA(d->hwndItem, t, sizeof t);
    if (id == ID_KEY_SHOW) snprintf(t, sizeof t, "%s", s_key_shown ? "Hide" : "Show");
    {
        CsUiFonts f = s_f;                                     /* the buttons take the body font */
        f.head = s_button_font;
        csui_button(d->hDC, &f, &r, t, (d->itemState & (ODS_SELECTED | ODS_FOCUS)) != 0);
    }
    if (d->itemState & ODS_DISABLED) {
        HBRUSH b = CreateSolidBrush(RGB(10, 20, 40));
        (void)b; DeleteObject(b);
    }
}

/* A static's text changed: the page behind it is drawn again (it is transparent). */
static void ui_refresh(HWND w)
{
    RECT r;
    HWND p = GetParent(w);
    if (!p) return;
    GetWindowRect(w, &r);
    MapWindowPoints(NULL, p, (POINT *)&r, 2);
    InvalidateRect(p, &r, TRUE);
}

/* The child controls' kinds: a group box is a heading on the page, push buttons
 * and the Show switch are drawn by ui_drawitem. */
static DWORD ui_style(const char *cls, DWORD style, int id)
{
    if (!_stricmp(cls, "BUTTON") && ((style & BS_TYPEMASK) <= BS_DEFPUSHBUTTON || id == ID_KEY_SHOW))
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    return style;
}

/* A message box, or with arguments a line on the calling console. */
static void tell(const char *text, UINT icon)
{
    if (s_cli) { fprintf(stderr, "%s\n", text); return; }
    MessageBoxA(s_main, text, "Insignia Setup", icon);
}

/* ---- checks ----------------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t ee_sum(const uint8_t *p, unsigned n)   /* the EEPROM checksum */
{
    uint32_t hi = 0, lo = 0;
    unsigned i;
    for (i = 0; i + 4 <= n; i += 4) {
        uint64_t s = ((uint64_t)hi << 32 | lo) + rd32(p + i);
        hi = (uint32_t)(s >> 32); lo = (uint32_t)s;
    }
    return ~(hi + lo);
}

/* A config-area sector: magic, footer, and a sum-with-carry checksum over the
 * sector with its checksum field zero (XOnline's own check). */
static int config_sector_ok(const uint8_t *s)
{
    uint8_t t[512];
    uint64_t sum = 0;
    uint32_t v;
    int i;
    if (rd32(s) != 0x79132568u || rd32(s + 0x1FC) != 0xAA550000u) return 0;
    memcpy(t, s, 512);
    memset(t + 0x1F8, 0, 4);
    for (i = 0; i < 512; i += 4) sum += rd32(t + i);
    v = (uint32_t)sum + (uint32_t)(sum >> 32);
    if (v < (uint32_t)sum) v++;                      /* the end-around carry */
    return (~v) == rd32(s + 0x1F8);
}

static int hex_key(const char *s, uint8_t out[16])
{
    int n = 0;
    for (; *s && n < 32; s++) {
        int v;
        if (*s == ' ' || *s == ':' || *s == '-') continue;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else return 0;
        if (n & 1) out[n / 2] |= (uint8_t)v; else out[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    return n == 32 && !*s;
}

/* ---- the hard-drive key from the console's own BIOS and MCPX ROM ------------------------------ */
/* The EEPROM's hard-drive key is encrypted with the "EEPROM key", which every
 * console keeps in its BIOS's second-stage loader (2BL), itself encrypted:
 *   MCPX 1.0 (Xbox 1.0): RC4 with the 16-byte key at MCPX ROM offset 0x1A5,
 *     over the 0x6000-byte 2BL at flash 0xFFFF9E00;
 *   MCPX 1.1 (Xbox 1.1-1.6): RC4 with SHA-1(MCPX key at ROM 0x19C | flash key
 *     at 0xFFFFFDF0 | MCPX key ^ 0x5C), 20 bytes, over the 2BL up to the flash
 *     boot loader at 0xFFFFD400.
 * (xboxdevwiki, Boot Process.) The EEPROM key is then whichever 16 bytes of
 * the decrypted 2BL reproduce the EEPROM's own HMAC-SHA1 -- found by trying
 * each, so no offset is assumed, and a mismatched BIOS, ROM or EEPROM simply
 * finds nothing. Player-supplied files only; no key is built in. */
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static BCRYPT_ALG_HANDLE s_hmac_alg, s_sha_alg;

static int hmac_sha1(const uint8_t *key, ULONG kl, const uint8_t *d, ULONG n, uint8_t out[20])
{
    if (!s_hmac_alg && BCryptOpenAlgorithmProvider(&s_hmac_alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return 0;
    return BCryptHash(s_hmac_alg, (PUCHAR)key, kl, (PUCHAR)d, n, out, 20) == 0;
}

static int sha1(const uint8_t *d, ULONG n, uint8_t out[20])
{
    if (!s_sha_alg && BCryptOpenAlgorithmProvider(&s_sha_alg, BCRYPT_SHA1_ALGORITHM, NULL, 0))
        return 0;
    return BCryptHash(s_sha_alg, NULL, 0, (PUCHAR)d, n, out, 20) == 0;
}

static void rc4(const uint8_t *key, unsigned kl, uint8_t *d, unsigned n)
{
    uint8_t s[256], t;
    unsigned i, j = 0, k;
    for (i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) { j = (j + s[i] + key[i % kl]) & 255; t = s[i]; s[i] = s[j]; s[j] = t; }
    i = j = 0;
    for (k = 0; k < n; k++) {
        i = (i + 1) & 255; j = (j + s[i]) & 255;
        t = s[i]; s[i] = s[j]; s[j] = t;
        d[k] ^= s[(s[i] + s[j]) & 255];
    }
}

/* With candidate EEPROM key k: 1 and the hard-drive key when the EEPROM's
 * encrypted section decrypts and its HMAC checks. */
static int try_eeprom_key(const uint8_t *k, uint8_t hd[16])
{
    uint8_t rk[20], sec[28], check[20];
    if (!hmac_sha1(k, 16, s_eeprom, 20, rk)) return 0;
    memcpy(sec, s_eeprom + 0x14, sizeof sec);
    rc4(rk, 20, sec, sizeof sec);
    if (!hmac_sha1(k, 16, sec, sizeof sec, check) || memcmp(check, s_eeprom, 20)) return 0;
    memcpy(hd, sec + 8, 16);
    SecureZeroMemory(sec, sizeof sec);
    return 1;
}

/* The EEPROM key's offset in the decrypted 2BL plus one, or 0. */
static unsigned search_2bl(const uint8_t *dec, unsigned n, uint8_t hd[16])
{
    unsigned p;
    for (p = 0; p + 16 <= n; p++)
        if (try_eeprom_key(dec + p, hd)) return p + 1;
    return 0;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024 || !(b = (uint8_t *)malloc((size_t)sz))) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

/* The game's system-link key, XboxLANKey: what the kernel makes of the title
 * certificate's LAN key when the game starts, HMAC-SHA1 keyed with its CERT
 * key, first 16 bytes. The retail 2BL keeps the CERT key in the 16 bytes
 * after the EEPROM key (checked: with it a real Xbox's system link -- xemu's
 * -- finds the game). Only this per-game result is kept, as lankey.bin; the
 * game uses it when settings.ini has [network] real_xbox=1. */
static int lan_key_from_2bl(const uint8_t *cert_key, uint8_t out[16])
{
    char path[MAX_PATH], *sl;
    size_t n = 0;
    uint8_t *x, d[20];
    uint32_t c;
    int ok = 0;
    if (getenv("CS_GAME_CWD")) snprintf(path, sizeof path, "%s\\game_files\\default.xbe", getenv("CS_GAME_CWD"));
    else {
        if (!GetModuleFileNameA(NULL, path, sizeof path) || !(sl = strrchr(path, '\\'))) return 0;
        strcpy_s(sl + 1, sizeof path - (size_t)(sl + 1 - path), "game_files\\default.xbe");
    }
    if (!(x = read_file(path, &n)) || n < 0x200 || memcmp(x, "XBEH", 4)) { free(x); return 0; }
    c = rd32(x + 0x118) - rd32(x + 0x104);
    if (c + 0xC0 <= n && hmac_sha1(cert_key, 16, x + c + 0xB0, 16, d)) {
        memcpy(out, d, 16);
        ok = 1;
    }
    SecureZeroMemory(d, sizeof d);
    free(x);
    return ok;
}

/* 0: decrypted (hd filled; *rev names the MCPX revision that worked);
 * 1: files unreadable or the wrong size; 2: they do not unlock this EEPROM. */
static int hdkey_from_bios(const char *bios_path, const char *mcpx_path, uint8_t hd[16], const char **rev)
{
    size_t bn = 0, mn = 0;
    uint8_t *bios = read_file(bios_path, &bn), *mcpx = read_file(mcpx_path, &mn);
    const uint8_t *im;
    uint8_t *dec = NULL;
    int r = 1;
    unsigned at = 0;
    if (!bios || !mcpx || bn < 0x40000 || mn != 512) goto out;
    im = bios + bn - 0x40000;                    /* flash is mirrored up to the top of memory */
    dec = (uint8_t *)malloc(0x6000);
    if (!dec) goto out;
    r = 2;
    {   /* MCPX 1.1 */
        uint8_t blk[48], rk[20];
        int i;
        memcpy(blk, mcpx + 0x19C, 16);
        memcpy(blk + 16, im + 0x3FDF0, 16);
        for (i = 0; i < 16; i++) blk[32 + i] = (uint8_t)(mcpx[0x19C + i] ^ 0x5C);
        if (sha1(blk, sizeof blk, rk)) {
            memcpy(dec, im + 0x39E00, 0x3600);
            rc4(rk, 20, dec, 0x3600);
            if ((at = search_2bl(dec, 0x3600, hd)) != 0) { *rev = "1.1"; r = 0; goto out; }
        }
        SecureZeroMemory(blk, sizeof blk); SecureZeroMemory(rk, sizeof rk);
    }
    {   /* MCPX 1.0 */
        memcpy(dec, im + 0x39E00, 0x6000);
        rc4(mcpx + 0x1A5, 16, dec, 0x6000);
        if ((at = search_2bl(dec, 0x6000, hd)) != 0) { *rev = "1.0"; r = 0; goto out; }
    }
out:
    s_have_lan_key = r == 0 && at + 31 < 0x6000 && lan_key_from_2bl(dec + at - 1 + 16, s_lan_key);
    if (dec) { SecureZeroMemory(dec, 0x6000); free(dec); }
    if (bios) { SecureZeroMemory(bios, bn); free(bios); }
    if (mcpx) { SecureZeroMemory(mcpx, mn); free(mcpx); }
    return r;
}

static void key_from_bios(const char *bios_path, const char *mcpx_path)
{
    uint8_t hd[16];
    const char *rev = "";
    char hex[33], msg[200];
    int i, r;
    if (!s_ee_ok) { say(s_key_st, "Choose your console's EEPROM first: the key is checked against it."); return; }
    say(s_key_st, "Decrypting...");
    UpdateWindow(s_key_st);
    r = hdkey_from_bios(bios_path, mcpx_path, hd, &rev);
    if (r == 1) { say(s_key_st, "Those are not a BIOS image (256 KB-1 MB) and an MCPX boot ROM (512 bytes)."); return; }
    if (r == 2) {
        say(s_key_st, "That BIOS and MCPX ROM do not unlock this EEPROM. Use your own console's BIOS, and MCPX 1.0 "
                      "for an Xbox 1.0 (kernels 3944-4627) or MCPX 1.1 for 1.1-1.6 (4817-5838).");
        return;
    }
    for (i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02X", hd[i]);
    s_have_bios_key = sha1(hd, 16, s_bios_key_sum);
    SetWindowTextA(s_key, hex);
    SecureZeroMemory(hd, sizeof hd); SecureZeroMemory(hex, sizeof hex);
    check_key();
    if (!s_acc_ok) {
        snprintf(msg, sizeof msg, "OK: hard-drive key decrypted from your BIOS (MCPX %s) and verified against the EEPROM.", rev);
        say(s_key_st, msg);
    }
}

/* ---- 4. System Link with xemu: only the game's LAN key, from the BIOS -------------------------- */
/* For players who set up Insignia before this existed, or never will: the
 * BIOS and MCPX are checked against the EEPROM already loaded (the saved
 * eeprom.bin), and only lankey.bin is written -- identity, hard-drive key and
 * account stay as they are. The switch that uses it is separate and off by
 * default: with it on, System Link no longer finds this game's other players
 * who leave it off. */
static const char k_real_xbox_warning[] =
    "Turn on System Link with xemu now?\n\n"
    "With it on, the game uses the real Xbox System Link key, so System Link only finds:\n"
    "    - players of this game who turned it on too\n"
    "    - xemu (set to UDP Tunnel, see the README)\n\n"
    "It no longer finds players of this game who leave it off (the default), whether or not they "
    "set up an EEPROM. Real Xbox consoles cannot join yet.\n\n"
    "You can turn it off again in settings.ini: [network] real_xbox=0.";

/* 0 saved; 1 no EEPROM; 2 files unreadable; 3 they do not unlock it; 4 not saved. */
static int lan_key_from_bios(const char *bios_path, const char *mcpx_path)
{
    uint8_t hd[16];
    const char *rev = "";
    char path[MAX_PATH];
    FILE *f;
    int r;
    if (!s_ee_ok) return 1;
    s_have_lan_key = 0;
    r = hdkey_from_bios(bios_path, mcpx_path, hd, &rev);
    SecureZeroMemory(hd, sizeof hd);
    if (r == 1) return 2;
    if (r != 0 || !s_have_lan_key) return 3;
    if (!cs_settings_path("lankey.bin", path, sizeof path, 1) || !(f = fopen(path, "wb"))) return 4;
    r = fwrite(s_lan_key, 1, 16, f) == 16;
    if (fclose(f) != 0 || !r) return 4;
    return 0;
}

static int set_real_xbox(int on)
{
    char ini[MAX_PATH];
    return cs_settings_path("settings.ini", ini, sizeof ini, 1) &&
           WritePrivateProfileStringA("network", "real_xbox", on ? "1" : "0", ini);
}

static const char *const k_lan_why[] = {
    "",
    "Choose your console's EEPROM in step 1 first: the BIOS is checked against it.",
    "Those are not a BIOS image (256 KB-1 MB) and an MCPX boot ROM (512 bytes).",
    "That BIOS and MCPX ROM do not unlock this EEPROM. Use your own console's BIOS, with MCPX 1.0 "
    "for an Xbox 1.0 or MCPX 1.1 for an Xbox 1.1-1.6.",
    "The System Link key could not be saved.",
};

static HWND s_lan_st;

static void lan_key_ui(const char *bios_path, const char *mcpx_path)
{
    int r = lan_key_from_bios(bios_path, mcpx_path);
    if (r) { say(s_lan_st, k_lan_why[r]); return; }
    if (MessageBoxA(s_main, k_real_xbox_warning, "Insignia Setup", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES &&
        set_real_xbox(1))
        say(s_lan_st, "Key saved. System Link with xemu is on.");
    else
        say(s_lan_st, "Key saved. System Link with xemu stays off until [network] real_xbox=1.");
}

/* ---- reading a drive: raw, or xemu's qcow2 ---------------------------------------------------- */
static uint64_t be64(const uint8_t *p) { uint64_t v = 0; int i; for (i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int read_at(HANDLE f, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
}

/* Sector `lba` of the drive in file f (512 bytes). 1 on success; a sector
 * the image never wrote reads as zeros. */
static int read_sector(HANDLE f, uint64_t lba, uint8_t out[512])
{
    uint8_t h[48];
    if (!read_at(f, 0, h, sizeof h)) return 0;
    if (memcmp(h, "QFI\xfb", 4) == 0) {
        uint32_t cbits = be32(h + 20), l2bits = cbits - 3;
        uint64_t off = lba * 512, l1off = be64(h + 40), l1e, l2e, cl;
        uint32_t l1size = be32(h + 36);
        uint64_t l1i = off >> (cbits + l2bits), l2i = (off >> cbits) & ((1ull << l2bits) - 1);
        uint8_t e[8];
        if (be32(h + 32) != 0 || cbits < 9 || cbits > 21 || l1i >= l1size) return 0;   /* encrypted or odd */
        if (!read_at(f, l1off + l1i * 8, e, 8)) return 0;
        l1e = be64(e) & 0x00FFFFFFFFFFFE00ull;
        if (!l1e) { memset(out, 0, 512); return 1; }
        if (!read_at(f, l1e + l2i * 8, e, 8)) return 0;
        l2e = be64(e);
        if (l2e & (1ull << 62)) return 0;                  /* compressed cluster: not handled */
        cl = l2e & 0x00FFFFFFFFFFFE00ull;
        if (!cl) { memset(out, 0, 512); return 1; }
        return read_at(f, cl + (off & ((1ull << cbits) - 1)), out, 512);
    }
    return read_at(f, lba * 512, out, 512);
}

/* ---- UI helpers ------------------------------------------------------------------------------- */
static void say(HWND w, const char *t) { SetWindowTextA(w, t); ui_refresh(w); }

static int browse(const char *filter, char *path, DWORD n)
{
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = s_main;
    o.lpstrFilter = filter;
    o.lpstrFile = path;
    o.nMaxFile = n;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    path[0] = 0;
    return GetOpenFileNameA(&o);
}

static void check_eeprom(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    char msg[160];
    s_ee_ok = 0;
    if (f) { n = fread(s_eeprom, 1, sizeof s_eeprom, f); fclose(f); }
    if (n != 256) { say(s_ee_st, "Not an Xbox EEPROM (it must be a 256-byte eeprom.bin)."); return; }
    if (ee_sum(s_eeprom + 0x34, 0x2C) != rd32(s_eeprom + 0x30) || ee_sum(s_eeprom + 0x64, 0x5C) != rd32(s_eeprom + 0x60)) {
        say(s_ee_st, "This EEPROM's checksums are wrong: it is damaged or not an Xbox EEPROM.");
        return;
    }
    s_ee_ok = 1;
    snprintf(msg, sizeof msg, "OK: console serial number ending ...%.4s, network address %02X:%02X:%02X:..",
             (const char *)s_eeprom + 0x34 + 8, s_eeprom[0x40], s_eeprom[0x41], s_eeprom[0x42]);
    say(s_ee_st, msg);
}

/* ---- checking a hard-drive key against the drive's accounts ----------------------------------- */
/* XOnline signs each account record with the hard-drive key (the game's own
 * signer, cs_account.c, 0x0027A7BE): the 16-byte account key at +0x50 is
 * encrypted with 3DES-CBC, its IV the 8-byte confounder in the account's
 * directory entry and its key HMAC-SHA1(K1, hd)[0..3] | HMAC-SHA1(K2, hd), and
 * +0x64 holds the first 8 bytes of HMAC-SHA1(hd, record[0..0x63]) taken over
 * the record with the account key in the clear. K1 and K2 are XOnline
 * constants read from the game's own default.xbe (never built in here), so a
 * key that reproduces a stored signature is this drive's console's key. */
#define XBE_PATH   "game_files\\default.xbe"
#define XO_K1_VA   0x007DE13Cu
#define XO_K2_VA   0x007DE128u
#define REC_SIZE   0x6C
#define SECTOR_HDR 0x0C                     /* config-sector data starts after magic and header */

/* 16 bytes at virtual address va from the game's XBE. */
static int xbe_read(uint32_t va, uint8_t out[16])
{
    char path[MAX_PATH], *sl;
    size_t n = 0;
    uint8_t *x;
    uint32_t base, nsec, sec, i;
    int ok = 0;
    if (getenv("CS_GAME_CWD")) snprintf(path, sizeof path, "%s\\" XBE_PATH, getenv("CS_GAME_CWD"));
    else {
        if (!GetModuleFileNameA(NULL, path, sizeof path) || !(sl = strrchr(path, '\\'))) return 0;
        strcpy_s(sl + 1, sizeof path - (size_t)(sl + 1 - path), XBE_PATH);
    }
    if (!(x = read_file(path, &n)) || n < 0x200 || memcmp(x, "XBEH", 4)) { free(x); return 0; }
    base = rd32(x + 0x104); nsec = rd32(x + 0x11C); sec = rd32(x + 0x120) - base;
    for (i = 0; i < nsec && sec + (i + 1) * 0x38 <= n; i++) {
        const uint8_t *h = x + sec + i * 0x38;
        uint32_t v = rd32(h + 4), raw = rd32(h + 12), rs = rd32(h + 16);
        if (va >= v && va + 16 <= v + rs && raw + (va - v) + 16 <= n) {
            memcpy(out, x + raw + (va - v), 16);
            ok = 1;
            break;
        }
    }
    free(x);
    return ok;
}

static int des3_cbc_decrypt(const uint8_t key[24], const uint8_t iv_in[8], uint8_t *d, ULONG n)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE k = NULL;
    uint8_t iv[8];
    ULONG got = 0;
    int ok = 0;
    memcpy(iv, iv_in, 8);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_3DES_ALGORITHM, NULL, 0) == 0 &&
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof BCRYPT_CHAIN_MODE_CBC, 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 24, 0) == 0)
        ok = BCryptDecrypt(k, d, n, NULL, iv, 8, d, n, &got, 0) == 0 && got == n;
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* 1: the key signed an account on the chosen drive; 0: it signed none of
 * them; -1: nothing to check against (no drive, no accounts, no game XBE). */
static int key_matches_accounts(const uint8_t hd[16])
{
    uint8_t k1[16], k2[16], h1[20], h2[20], dk[24], rec[REC_SIZE], sig[20];
    const uint8_t *dir;
    int i, tried = 0, match = 0;
    if (!s_acc_ok || !xbe_read(XO_K1_VA, k1) || !xbe_read(XO_K2_VA, k2)) return -1;
    if (!hmac_sha1(k1, 16, hd, 16, h1) || !hmac_sha1(k2, 16, hd, 16, h2)) return -1;
    memcpy(dk, h1, 4); memcpy(dk + 4, h2, 20);
    dir = s_sectors[11 - 9] + SECTOR_HDR;                    /* sector 11: the account directory */
    for (i = 0; i < 8 && !match; i++) {
        const uint8_t *e = dir + 12 * i;
        if (!rd32(e)) continue;                              /* slot not in use */
        memcpy(rec, s_sectors[12 + i - 9] + SECTOR_HDR, REC_SIZE);
        tried = 1;
        if (des3_cbc_decrypt(dk, e + 4, rec + 0x50, 16) && hmac_sha1(hd, 16, rec, 0x64, sig) && !memcmp(sig, rec + 0x64, 8))
            match = 1;
    }
    SecureZeroMemory(dk, sizeof dk); SecureZeroMemory(h1, sizeof h1); SecureZeroMemory(h2, sizeof h2);
    SecureZeroMemory(rec, sizeof rec); SecureZeroMemory(k1, sizeof k1); SecureZeroMemory(k2, sizeof k2);
    return tried ? match : -1;
}

/* 1 verified (it signed the drive's accounts, or came from the BIOS), 0 it
 * did not sign the drive's accounts, 3 it came from the BIOS (so it is the
 * EEPROM's) but the drive's accounts are another console's, -1 not
 * verifiable here, -2 not a key. */
static int key_state(void)
{
    char t[128];
    uint8_t k[16], sum[20];
    int acc, bios;
    GetWindowTextA(s_key, t, sizeof t);
    if (!hex_key(t, k)) { SecureZeroMemory(t, sizeof t); return -2; }
    SecureZeroMemory(t, sizeof t);
    acc = key_matches_accounts(k);
    bios = s_have_bios_key && sha1(k, 16, sum) && !memcmp(sum, s_bios_key_sum, 20);
    SecureZeroMemory(k, sizeof k);
    if (bios) return acc == 0 ? 3 : 1;
    return acc;
}

static void check_key(void)
{
    char t[128];
    GetWindowTextA(s_key, t, sizeof t);
    if (!t[0]) say(s_key_st, "Needed: your console's hard-drive key (32 hex digits).");
    else switch (key_state()) {
    case -2: say(s_key_st, "Not a hard-drive key: it must be 32 hex digits (0-9, A-F)."); break;
    case 1:  say(s_key_st, s_acc_ok ? "OK: hard-drive key verified: it signed the accounts on your drive."
                                    : "OK: hard-drive key decrypted from your BIOS and verified against the EEPROM."); break;
    case 0:  say(s_key_st, "This key did not sign the accounts on that drive: it is not that console's key."); break;
    case 3:  say(s_key_st, "The key is this EEPROM's, but the accounts on that drive are another console's."); break;
    default: say(s_key_st, "16-byte key, not verified: add your drive in step 3, or use From BIOS + MCPX, to check it.");
    }
    SecureZeroMemory(t, sizeof t);
}

/* EvoX/UnleashX hddinfo.txt: the "XboxHDKey" line (not "HDD Key", which is
 * the drive's lock password). */
static void key_from_hddinfo(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512], out[40];
    int n = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "XboxHDKey");
        if (!p) continue;
        p += 9;
        for (; *p && n < 32; p++)
            if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) out[n++] = *p;
        break;
    }
    fclose(f);
    out[n] = 0;
    if (n == 32) SetWindowTextA(s_key, out);
    else say(s_key_st, "No XboxHDKey line in that file.");
    SecureZeroMemory(out, sizeof out);
    SecureZeroMemory(line, sizeof line);
    check_key();
}

static void check_accounts(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    int i, valid = 0, users = 0;
    char msg[400];
    s_acc_ok = 0;
    s_gamertags[0] = 0;
    if (f == INVALID_HANDLE_VALUE) { say(s_acc_st, "That file could not be opened."); return; }
    for (i = 0; i < ACCOUNT_SECTORS; i++)
        if (!read_sector(f, 9 + (uint64_t)i, s_sectors[i])) { CloseHandle(f); say(s_acc_st, "That file is too short, or a drive image this program cannot read."); return; }
    CloseHandle(f);
    for (i = 0; i < ACCOUNT_SECTORS; i++) {
        const uint8_t *s = s_sectors[i];
        int k, empty = 1;
        for (k = 0; k < 512; k++) if (s[k]) { empty = 0; break; }
        if (empty) continue;
        if (!config_sector_ok(s)) { say(s_acc_st, "Those sectors are not an Xbox drive's account area (wrong file?)."); return; }
        valid++;
        if (i >= 3 && s[24] && memcmp(s + 24, "SN.", 3) != 0) {   /* a user slot (12-19) with a gamertag */
            char tag[17];
            memcpy(tag, s + 24, 16); tag[16] = 0;
            for (k = 0; tag[k]; k++) if (tag[k] < 32 || tag[k] > 126) { tag[k] = 0; break; }
            if (tag[0]) {
                if (users++) strncat(s_gamertags, ", ", sizeof s_gamertags - strlen(s_gamertags) - 1);
                strncat(s_gamertags, tag, sizeof s_gamertags - strlen(s_gamertags) - 1);
            }
        }
    }
    if (!valid) { say(s_acc_st, "No Xbox Live accounts in that file (sectors 9-19 are empty)."); return; }
    s_acc_ok = 1;
    snprintf(msg, sizeof msg, users ? "OK: account(s) %s" : "Found the console's machine account but no player accounts.",
             s_gamertags);
    say(s_acc_st, msg);
    check_key();                                    /* the key can be checked against these accounts now */
}

/* ---- saving ----------------------------------------------------------------------------------- */
/* drive_config.bin holds the drive's config sectors 8-23 (the game emulates
 * the raw disk and keeps only these). Sector 8, the network settings, is kept
 * as it is; the accounts go in 9-19. */
static int write_partition0(void)
{
    char path[MAX_PATH], bak[MAX_PATH];
    uint8_t cfg[16 * 512];
    FILE *f;
    int i;
    if (!cs_settings_path("drive_config.bin", path, sizeof path, 1)) return 0;
    memset(cfg, 0, sizeof cfg);
    if ((f = fopen(path, "rb")) != NULL) {
        fread(cfg, 1, sizeof cfg, f);
        fclose(f);
        snprintf(bak, sizeof bak, "%s.before-insignia", path);
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES)
            CopyFileA(path, bak, TRUE);                    /* the first time only */
    }
    for (i = 0; i < ACCOUNT_SECTORS; i++)
        memcpy(cfg + (1 + i) * 512, s_sectors[i], 512);
    if (!(f = fopen(path, "wb"))) return 0;
    i = fwrite(cfg, 1, sizeof cfg, f) == sizeof cfg;
    return fclose(f) == 0 && i;
}

/* Steps 1 and 2: the console's EEPROM and hard-drive key. */
static int save_identity(void)
{
    char t[128], path[MAX_PATH], ini[MAX_PATH];
    uint8_t k[16];
    FILE *f;
    GetWindowTextA(s_key, t, sizeof t);
    if (!s_ee_ok) { tell("Choose your console's EEPROM first.", MB_ICONWARNING); return 0; }
    if (!hex_key(t, k)) { tell("Enter your console's hard-drive key (32 hex digits).", MB_ICONWARNING); return 0; }
    SecureZeroMemory(k, sizeof k);
    switch (key_state()) {
    case 0:
        SecureZeroMemory(t, sizeof t);
        tell("That hard-drive key did not sign the accounts on your drive, so it is not your console's key.\n\n"
             "Check the key (UnleashX, EvoX hddinfo.txt, XBlast or xemu show it), or use From BIOS + MCPX.", MB_ICONWARNING);
        return 0;
    case 3:
        SecureZeroMemory(t, sizeof t);
        tell("The hard-drive key belongs to this EEPROM, but the accounts on that drive were made on another "
             "console and will not work with it.\n\n"
             "Choose the drive of the console this EEPROM came from, or "
             "recover your account from Insignia instead.", MB_ICONWARNING);
        return 0;
    }
    if (!cs_settings_path("eeprom.bin", path, sizeof path, 1) || !(f = fopen(path, "wb")) ||
        fwrite(s_eeprom, 1, 256, f) != 256 || fclose(f) != 0) {
        tell("The EEPROM could not be saved.", MB_ICONERROR);
        return 0;
    }
    if (!cs_settings_path("settings.ini", ini, sizeof ini, 1) || !WritePrivateProfileStringA("insignia", "hdd_key", t, ini)) {
        tell("The hard-drive key could not be saved.", MB_ICONERROR);
        return 0;
    }
    SecureZeroMemory(t, sizeof t);
    if (s_have_lan_key) {                    /* from the BIOS step: for [network] real_xbox=1 */
        if (!cs_settings_path("lankey.bin", path, sizeof path, 1) || !(f = fopen(path, "wb")) ||
            fwrite(s_lan_key, 1, 16, f) != 16 || fclose(f) != 0) {
            tell("The system-link key could not be saved.", MB_ICONERROR);
            return 0;
        }
    }
    return 1;
}

/* Step 3 is optional: an account from a drive dump if one was chosen; else
 * the account comes from Insignia (Recover from Insignia...). */
static void save(void)
{
    if (!save_identity()) return;
    if (s_acc_ok && !write_partition0()) {
        tell("The account could not be written to the game's drive.", MB_ICONERROR);
        return;
    }
    say(s_status, s_acc_ok ? "Saved. Start Counter-Strike, choose Xbox Live, and sign in."
                           : "Saved. Now add your account: Recover from Insignia...");
    tell(s_acc_ok ? "Your console identity and Xbox Live account are set up.\n\n"
                    "Start Counter-Strike, choose Xbox Live, and pick your gamertag to sign in to Insignia."
                  : "Your console identity is set up.\n\n"
                    "To add your Xbox Live account, use Recover from Insignia... with your gamertag and the "
                    "16-digit number from your insignia.live web dashboard.", MB_ICONINFORMATION);
}

/* ---- Recover from Insignia ----------------------------------------------------------------------- */
/* Insignia's account recovery (insignia.live/guide/recover): the gamertag and
 * the 16-digit number its web dashboard shows. The game does the recovery
 * itself (cs_account.c, CS_ACCT=recover): it signs this console in, asks
 * Insignia for the account, and saves it to the drive the way the Xbox
 * Dashboard does. The two values go to it on a pipe, never on a command line
 * or in a file, and are wiped here once written. */
enum { ID_REC_TAG = 300, ID_REC_NUM, ID_REC_OK, ID_REC_CANCEL };
static HWND s_rec, s_rec_tag, s_rec_num;
static int s_rec_done;

/* Insignia shows the number as 0000-0000-0000-0000: keep only its digits as it
 * is typed or pasted, so the dashes (and spaces) never count. */
static void rec_digits_only(HWND e)
{
    static int busy;
    char t[64], d[17];
    int i, n = 0;
    if (busy) return;
    GetWindowTextA(e, t, sizeof t);
    for (i = 0; t[i] && n < 16; i++) if (t[i] >= '0' && t[i] <= '9') d[n++] = t[i];
    d[n] = 0;
    if (strcmp(t, d)) {
        busy = 1;
        SetWindowTextA(e, d);
        SendMessageA(e, EM_SETSEL, (WPARAM)n, (LPARAM)n);
        busy = 0;
    }
    SecureZeroMemory(t, sizeof t);
    SecureZeroMemory(d, sizeof d);
}

static LRESULT CALLBACK rec_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND && LOWORD(w) == ID_REC_NUM && HIWORD(w) == EN_CHANGE) {
        rec_digits_only((HWND)l);
        return 0;
    }
    if (m == WM_COMMAND && (LOWORD(w) == ID_REC_OK || LOWORD(w) == ID_REC_CANCEL)) {
        s_rec_done = LOWORD(w) == ID_REC_OK ? 1 : -1;
        return 0;
    }
    if (m == WM_CLOSE) { s_rec_done = -1; return 0; }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) { ui_paint(h, "Recover Account"); return 0; }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT) return ui_ctlcolor(m, (HDC)w, (HWND)l);
    if (m == WM_DRAWITEM) { ui_drawitem(h, (const DRAWITEMSTRUCT *)l); return TRUE; }
    return DefWindowProcA(h, m, w, l);
}

static HWND rec_mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c;
    ui_init();
    c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | ui_style(cls, style, id) | (cls[0] == 'E' ? WS_BORDER : 0),
                        K(x), K(y) + TOP, K(w), K(h), s_rec, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)(cls[0] == 'S' ? s_label_font : s_button_font), TRUE);
    return c;
}

/* The two values, or 0 if the player cancelled. */
static int ask_recover(char *tag, unsigned tagn, char *num, unsigned numn)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, K(460), K(262) + TOP };
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = rec_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CounterStrikeXBRecover";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    s_rec = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName, "Recover your account from Insignia",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, s_main, NULL, wc.hInstance, NULL);
    rec_mk("STATIC", "Sign in to your Web Dashboard at insignia.live and press Reveal to see your account's "
           "16-digit number. Don't share it: it lets anyone recover your account.", SS_LEFT, 14, 10, 432, 44, 0);
    rec_mk("STATIC", "Gamertag:", SS_LEFT, 14, 66, 120, 20, 0);
    s_rec_tag = rec_mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, 140, 62, 306, 23, ID_REC_TAG);
    rec_mk("STATIC", "16-digit number:", SS_LEFT, 14, 102, 120, 20, 0);
    s_rec_num = rec_mk("EDIT", "", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 140, 98, 306, 23, ID_REC_NUM);
    rec_mk("STATIC", "Paste it as shown (0000-0000-0000-0000): the dashes are dropped.",
           SS_LEFT, 140, 126, 306, 32, 0);
    rec_mk("STATIC", "Recover starts Counter-Strike in a small window. Once the game reaches its title screen "
           "(about half a minute) it signs this console in to Insignia, fetches your account and closes by itself.",
           SS_LEFT, 14, 164, 432, 44, 0);
    SendMessageA(s_rec_tag, EM_LIMITTEXT, 15, 0);
    SendMessageA(s_rec_num, EM_LIMITTEXT, 32, 0);   /* room for a pasted 0000-0000-0000-0000; trimmed to 16 digits */
    rec_mk("BUTTON", "Recover", BS_DEFPUSHBUTTON | WS_TABSTOP, 252, 222, 94, 28, ID_REC_OK);
    rec_mk("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 352, 222, 94, 28, ID_REC_CANCEL);
    EnableWindow(s_main, FALSE);
    ShowWindow(s_rec, SW_SHOW);
    SetFocus(s_rec_tag);
    s_rec_done = 0;
    while (!s_rec_done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { s_rec_done = 1; continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { s_rec_done = -1; continue; }
        if (IsDialogMessageA(s_rec, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    GetWindowTextA(s_rec_tag, tag, (int)tagn);
    GetWindowTextA(s_rec_num, num, (int)numn);
    SetWindowTextA(s_rec_num, "");
    EnableWindow(s_main, TRUE);
    DestroyWindow(s_rec);
    SetForegroundWindow(s_main);
    return s_rec_done == 1;
}

/* The game's exit code (cs_account.h) as a sentence. */
static const char *recover_result(DWORD code)
{
    switch (code) {
    case 0:  return NULL;
    case 21: return "This console could not sign in to Xbox Live. Check the EEPROM and hard-drive key "
                    "(the console must be registered with Insignia) and your internet connection.";
    case 22: return "Insignia did not answer. Check your internet connection and try again.";
    case 23: return "Insignia did not accept that gamertag and number. Check both on your insignia.live "
                    "Web Dashboard (the number changes if you reveal a new one).";
    case 24: return "The account could not be saved to the game's drive.";
    case 25: return "All eight account slots on this console are in use.";
    default: return "The recovery did not finish. The game's log (CounterStrikeXB\\logs) says why.";
    }
}

static void recover(void)
{
    char tag[32], num[40], exe[MAX_PATH], dir[MAX_PATH], line[96], *sl;
    unsigned i, digits = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 26, n;
    const char *why;
    if (!save_identity()) return;
    if (!ask_recover(tag, sizeof tag, num, sizeof num)) { SecureZeroMemory(num, sizeof num); return; }
    for (i = 0; num[i]; i++) if (num[i] >= '0' && num[i] <= '9') digits++;
    if (!tag[0] || digits != 16) {
        SecureZeroMemory(num, sizeof num);
        tell("Enter your gamertag and the full 16-digit number from your insignia.live Web Dashboard.", MB_ICONWARNING);
        return;
    }
    if (!GetModuleFileNameA(NULL, exe, sizeof exe) || !(sl = strrchr(exe, '\\'))) { SecureZeroMemory(num, sizeof num); return; }
    strcpy_s(sl + 1, sizeof exe - (size_t)(sl + 1 - exe), "cs2_xbox_recomp.exe");
    strcpy_s(dir, sizeof dir, exe);
    *strrchr(dir, '\\') = 0;
    if (getenv("CS_GAME_CWD")) strcpy_s(dir, sizeof dir, getenv("CS_GAME_CWD"));   /* a development build */
    if (!CreatePipe(&rd, &wr, &sa, 0)) { SecureZeroMemory(num, sizeof num); tell("Could not start the recovery.", MB_ICONERROR); return; }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    snprintf(line, sizeof line, "%llu", (unsigned long long)(uintptr_t)rd);
    SetEnvironmentVariableA("CS_ACCT", "recover");
    SetEnvironmentVariableA("CS_ACCT_INPUT", line);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    if (!CreateProcessA(exe, NULL, NULL, NULL, TRUE, 0, NULL, dir, &si, &pi)) {
        SetEnvironmentVariableA("CS_ACCT", NULL); SetEnvironmentVariableA("CS_ACCT_INPUT", NULL);
        CloseHandle(rd); CloseHandle(wr); SecureZeroMemory(num, sizeof num);
        tell("Could not start Counter-Strike (cs2_xbox_recomp.exe beside this program).", MB_ICONERROR);
        return;
    }
    SetEnvironmentVariableA("CS_ACCT", NULL);
    SetEnvironmentVariableA("CS_ACCT_INPUT", NULL);
    CloseHandle(rd);
    snprintf(line, sizeof line, "%s\n%s\n", tag, num);
    WriteFile(wr, line, (DWORD)strlen(line), &n, NULL);
    CloseHandle(wr);
    SecureZeroMemory(line, sizeof line);
    SecureZeroMemory(num, sizeof num);
    CloseHandle(pi.hThread);
    say(s_status, "Counter-Strike is fetching your account (closes by itself)...");
    EnableWindow(s_main, FALSE);
    {
        DWORD start = GetTickCount();
        MSG msg;
        while (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 500, QS_ALLINPUT) != WAIT_OBJECT_0) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            if (GetTickCount() - start > 180000) { TerminateProcess(pi.hProcess, 26); break; }
        }
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    EnableWindow(s_main, TRUE);
    SetForegroundWindow(s_main);
    why = recover_result(code);
    if (why) { say(s_status, "The account was not recovered."); tell(why, MB_ICONERROR); return; }
    snprintf(line, sizeof line, "OK: %s recovered from Insignia", tag);
    say(s_acc_st, line);
    say(s_status, "Saved. Start Counter-Strike, choose Xbox Live, and sign in.");
    tell("Your account is on this PC now.\n\nStart Counter-Strike, choose Xbox Live, and pick your gamertag to sign in.",
         MB_ICONINFORMATION);
}

/* Load what was saved before, so the window shows the current setup. */
static void load_current(void)
{
    char path[MAX_PATH], ini[MAX_PATH], t[128];
    if (cs_settings_path("eeprom.bin", path, sizeof path, 0) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        SetWindowTextA(s_ee, path);
        check_eeprom(path);
    }
    if (cs_settings_path("settings.ini", ini, sizeof ini, 0)) {
        GetPrivateProfileStringA("insignia", "hdd_key", "", t, sizeof t, ini);
        if (t[0]) { SetWindowTextA(s_key, t); check_key(); }
        SecureZeroMemory(t, sizeof t);
    }
    if (cs_settings_path("lankey.bin", path, sizeof path, 0) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        say(s_lan_st, GetPrivateProfileIntA("network", "real_xbox", 0, ini) == 1
                      ? "On: System Link plays with xemu and players who turned it on."
                      : "Key saved, switch off: System Link plays with this game's default players.");
}

/* ---- window ----------------------------------------------------------------------------------- */
static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c;
    ui_init();
    if (!_stricmp(cls, "BUTTON") && (style & BS_TYPEMASK) == BS_GROUPBOX) {   /* a step: a heading on the page */
        if (s_nheads < (int)(sizeof s_heads / sizeof s_heads[0])) {
            SetRect(&s_heads[s_nheads].r, K(x), K(y) + TOP - 4, K(x + w), K(y) + TOP + 24);
            snprintf(s_heads[s_nheads].text, sizeof s_heads[0].text, "%s", text);
            s_nheads++;
        }
        return NULL;
    }
    c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | ui_style(cls, style, id) | (cls[0] == 'E' ? WS_BORDER : 0),
                        K(x), K(y) + TOP + 8, K(w), K(h), s_main, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)(cls[0] == 'S' ? s_label_font : s_button_font), TRUE);
    return c;
}

static void make_ui(void)
{
    int y = 12;
    mk("STATIC", "Play Counter-Strike on Xbox Live through Insignia with your own Xbox console's identity and account. "
       "Your console must already be registered with Insignia (its Setup Assistant); your account can come from Insignia.",
       SS_LEFT, 16, y, 560, 44, 0);
    y += 56;
    mk("BUTTON", "1. Your console's EEPROM (eeprom.bin)", BS_GROUPBOX, 10, y, 572, 86, 0);
    s_ee = mk("EDIT", "", ES_AUTOHSCROLL | ES_READONLY, 24, y + 24, 440, 23, ID_EE_EDIT);
    mk("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 474, y + 23, 96, 25, ID_EE_BROWSE);
    s_ee_st = mk("STATIC", "Needed: the EEPROM from your console (or your xemu eeprom.bin).", SS_LEFT, 24, y + 56, 546, 20, ID_EE_STATUS);
    y += 96;
    mk("BUTTON", "2. Your console's hard-drive key", BS_GROUPBOX, 10, y, 572, 86, 0);
    s_key = mk("EDIT", "", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 24, y + 24, 210, 23, ID_KEY_EDIT);
    mk("BUTTON", "Show", BS_AUTOCHECKBOX | WS_TABSTOP, 240, y + 25, 54, 22, ID_KEY_SHOW);
    mk("BUTTON", "From hddinfo.txt...", BS_PUSHBUTTON | WS_TABSTOP, 298, y + 23, 128, 25, ID_KEY_FILE);
    mk("BUTTON", "From BIOS + MCPX...", BS_PUSHBUTTON | WS_TABSTOP, 432, y + 23, 138, 25, ID_KEY_BIOS);
    s_key_st = mk("STATIC", "Needed: 32 hex digits (UnleashX, EvoX hddinfo.txt, XBlast or xemu show it).", SS_LEFT, 24, y + 56, 546, 20, ID_KEY_STATUS);
    y += 96;
    mk("BUTTON", "3. Your Xbox Live account", BS_GROUPBOX, 10, y, 572, 128, 0);
    mk("STATIC", "Recover it from Insignia with your gamertag and the 16-digit number on your insignia.live Web "
       "Dashboard, or load it from a dump of your console's drive, a raw drive image, or your xemu drive (.qcow2):",
       SS_LEFT, 24, y + 20, 546, 32, 0);
    s_acc = mk("EDIT", "", ES_AUTOHSCROLL | ES_READONLY, 24, y + 56, 440, 23, ID_ACC_EDIT);
    mk("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 474, y + 55, 96, 25, ID_ACC_BROWSE);
    s_acc_st = mk("STATIC", "Optional: an account from a drive, or recover one from Insignia.", SS_LEFT, 24, y + 88, 360, 20, ID_ACC_STATUS);
    mk("BUTTON", "Recover from Insignia...", BS_PUSHBUTTON | WS_TABSTOP, 394, y + 84, 176, 25, ID_CREATE);
    y += 140;
    mk("BUTTON", "4. System Link with xemu (optional)", BS_GROUPBOX, 10, y, 572, 58, 0);
    s_lan_st = mk("STATIC", "Off. Needs the System Link key from your BIOS + MCPX.",
                  SS_LEFT, 24, y + 25, 400, 28, ID_LAN_STATUS);
    mk("BUTTON", "System Link key...", BS_PUSHBUTTON | WS_TABSTOP, 434, y + 22, 136, 25, ID_LAN_KEY);
    y += 70;
    s_status = mk("STATIC", "", SS_LEFT, 16, y + 6, 360, 20, ID_STATUS);
    mk("BUTTON", "Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 386, y, 92, 28, ID_SAVE);
    mk("BUTTON", "Close", BS_PUSHBUTTON | WS_TABSTOP, 488, y, 92, 28, ID_CLOSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    char path[MAX_PATH];
    switch (m) {
    case WM_CREATE:
        s_main = h;
        make_ui();
        load_current();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ui_paint(h, "Insignia Setup");
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT:
        return ui_ctlcolor(m, (HDC)w, (HWND)l);
    case WM_DRAWITEM:
        ui_drawitem(h, (const DRAWITEMSTRUCT *)l);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_EE_BROWSE:
            if (browse("EEPROM (*.bin)\0*.bin\0All files\0*.*\0", path, sizeof path)) { SetWindowTextA(s_ee, path); check_eeprom(path); }
            return 0;
        case ID_KEY_FILE:
            if (browse("hddinfo.txt\0*.txt\0All files\0*.*\0", path, sizeof path)) key_from_hddinfo(path);
            return 0;
        case ID_KEY_BIOS: {
            char mcpx[MAX_PATH];
            MessageBoxA(s_main, "Choose your console's BIOS image first, then its MCPX boot ROM:\n\n"
                        "MCPX 1.0 for an Xbox 1.0 (kernels 3944, 4034, 4036, 4627)\n"
                        "MCPX 1.1 for an Xbox 1.1-1.6 (kernels 4817 to 5838)\n\n"
                        "They are read only to decrypt the hard-drive key and are not kept.",
                        "Insignia Setup", MB_ICONINFORMATION);
            if (browse("BIOS image (*.bin)\0*.bin\0All files\0*.*\0", path, sizeof path) &&
                browse("MCPX boot ROM (*.bin)\0*.bin\0All files\0*.*\0", mcpx, sizeof mcpx))
                key_from_bios(path, mcpx);
            return 0;
        }
        case ID_KEY_EDIT:
            if (HIWORD(w) == EN_CHANGE) check_key();
            return 0;
        case ID_KEY_SHOW:
            s_key_shown = !s_key_shown;
            SendMessageA(s_key, EM_SETPASSWORDCHAR, s_key_shown ? 0 : (WPARAM)'*', 0);
            InvalidateRect(s_key, NULL, TRUE);
            InvalidateRect((HWND)l, NULL, TRUE);
            return 0;
        case ID_ACC_BROWSE:
            if (browse("Drive dump or image (*.bin;*.img;*.qcow2;*.raw)\0*.bin;*.img;*.qcow2;*.raw\0All files\0*.*\0", path, sizeof path)) {
                SetWindowTextA(s_acc, path);
                check_accounts(path);
            }
            return 0;
        case ID_LAN_KEY: {
            char mcpx[MAX_PATH];
            if (!s_ee_ok) { say(s_lan_st, k_lan_why[1]); return 0; }
            MessageBoxA(s_main, "Choose your console's BIOS image first, then its MCPX boot ROM:\n\n"
                        "MCPX 1.0 for an Xbox 1.0 (kernels 3944, 4034, 4036, 4627)\n"
                        "MCPX 1.1 for an Xbox 1.1-1.6 (kernels 4817 to 5838)\n\n"
                        "Only the game's System Link key is made from them and saved. Your EEPROM, "
                        "hard-drive key and account stay as they are, and the BIOS is not kept.",
                        "Insignia Setup", MB_ICONINFORMATION);
            if (browse("BIOS image (*.bin)\0*.bin\0All files\0*.*\0", path, sizeof path) &&
                browse("MCPX boot ROM (*.bin)\0*.bin\0All files\0*.*\0", mcpx, sizeof mcpx))
                lan_key_ui(path, mcpx);
            return 0;
        }
        case ID_CREATE: recover(); return 0;
        case ID_SAVE: save(); return 0;
        case ID_CLOSE: DestroyWindow(h); return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* insignia_setup.exe --eeprom FILE (--hdd-key HEX | --hddinfo FILE) --drive FILE
 * does what the window does, without it (scripted installs, tests). The
 * window's controls are still made, hidden, so the checks and the save are
 * the same code. Exit 0 when saved; the reason goes to a message box.
 * insignia_setup.exe --bios FILE --mcpx FILE [--real-xbox 1|0] only saves the
 * System Link key (step 4) against the saved EEPROM; --real-xbox alone sets
 * the switch. Exit 6 with the reason on the console when that fails. */
static int run_args(void)
{
    const char *ee = NULL, *key = NULL, *info = NULL, *drive = NULL, *bios = NULL, *mcpx = NULL, *real_xbox = NULL;
    char t[128];
    int i;
    for (i = 1; i + 1 < __argc; i += 2) {
        if (!strcmp(__argv[i], "--eeprom")) ee = __argv[i + 1];
        else if (!strcmp(__argv[i], "--hdd-key")) key = __argv[i + 1];
        else if (!strcmp(__argv[i], "--hddinfo")) info = __argv[i + 1];
        else if (!strcmp(__argv[i], "--drive")) drive = __argv[i + 1];
        else if (!strcmp(__argv[i], "--bios")) bios = __argv[i + 1];
        else if (!strcmp(__argv[i], "--mcpx")) mcpx = __argv[i + 1];
        else if (!strcmp(__argv[i], "--real-xbox")) real_xbox = __argv[i + 1];
        else return 2;
    }
    s_cli = 1;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stderr);
    /* --bios FILE --mcpx FILE [--real-xbox 1|0] alone: only the System Link
     * key, checked against the saved eeprom.bin (an earlier setup's). */
    if (!ee && !key && !info && !drive && ((bios && mcpx) || real_xbox)) {
        char path[MAX_PATH];
        if (bios && mcpx) {
            if (cs_settings_path("eeprom.bin", path, sizeof path, 0)) check_eeprom(path);
            if ((i = lan_key_from_bios(bios, mcpx)) != 0) { fprintf(stderr, "%s\n", k_lan_why[i]); return 6; }
            fprintf(stderr, "System Link key saved (lankey.bin).\n");
        }
        if (real_xbox) {
            if (!set_real_xbox(real_xbox[0] == '1')) { fprintf(stderr, "settings.ini could not be written.\n"); return 6; }
            fprintf(stderr, "System Link with xemu %s ([network] real_xbox=%c).\n", real_xbox[0] == '1' ? "on" : "off",
                    real_xbox[0] == '1' ? '1' : '0');
        }
        return 0;
    }
    if (!ee || (!key && !info && !(bios && mcpx))) {
        fprintf(stderr, "usage: insignia_setup --eeprom FILE (--hdd-key HEX | --hddinfo FILE | --bios FILE --mcpx FILE) [--drive FILE]\n"
                        "       insignia_setup --bios FILE --mcpx FILE [--real-xbox 1|0]   (System Link key only)\n");
        return 2;
    }
    check_eeprom(ee);
    if (key) { SetWindowTextA(s_key, key); check_key(); }
    else if (info) key_from_hddinfo(info);
    else { key_from_bios(bios, mcpx); GetWindowTextA(s_key_st, t, sizeof t); fprintf(stderr, "%s\n", t); }
    if (drive) check_accounts(drive);
    GetWindowTextA(s_ee_st, t, sizeof t);
    if (!s_ee_ok) { tell(t, MB_ICONERROR); return 3; }
    GetWindowTextA(s_acc_st, t, sizeof t);
    if (drive && !s_acc_ok) { tell(t, MB_ICONERROR); return 4; }
    save();
    GetWindowTextA(s_status, t, sizeof t);
    return strncmp(t, "Saved", 5) == 0 ? 0 : 5;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, K(592), K(512) + TOP + 8 };
    (void)prev; (void)cmd;
    InitCommonControls();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CounterStrikeXBInsignia";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowExA(0, wc.lpszClassName, "Counter-Strike - Insignia Setup",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (__argc > 1 ? 0 : WS_VISIBLE),
                    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (__argc > 1)
        return run_args();
    ShowWindow(s_main, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(s_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
