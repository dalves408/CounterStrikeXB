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
 *   %LOCALAPPDATA%\CounterStrikeXB\SaveData\Partition0.img  the drive's config sectors
 *
 * This program checks what the player gives it and writes those. The accounts
 * come from a dump of the console's drive (the first sectors are enough), a
 * raw drive image, or an xemu drive (.qcow2). Only sectors 9..15 (accounts) are
 * copied: the network settings stay this PC's (the game's router hands them
 * out). Creating a new account needs the dashboard's sign-up and is not here
 * yet. Nothing secret is shown: the hard-drive key field is masked and the
 * serial number shows only its last digits.
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

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

enum { ID_EE_EDIT = 100, ID_EE_BROWSE, ID_EE_STATUS, ID_KEY_EDIT, ID_KEY_SHOW, ID_KEY_FILE, ID_KEY_BIOS, ID_KEY_STATUS,
       ID_ACC_EDIT, ID_ACC_BROWSE, ID_ACC_STATUS, ID_CREATE, ID_SAVE, ID_CLOSE, ID_STATUS };

static HWND s_main, s_ee, s_ee_st, s_key, s_key_st, s_acc, s_acc_st, s_status;
static uint8_t s_eeprom[256];
static int s_ee_ok, s_acc_ok;
static uint8_t s_sectors[7][512];          /* the drive's sectors 9..15 */
static char s_gamertags[256];
static void say(HWND w, const char *t);
static void check_key(void);
static int s_cli;                           /* run from the command line: no message boxes */

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

static int search_2bl(const uint8_t *dec, unsigned n, uint8_t hd[16])
{
    unsigned p;
    for (p = 0; p + 16 <= n; p++)
        if (try_eeprom_key(dec + p, hd)) return 1;
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

/* 0: decrypted (hd filled; *rev names the MCPX revision that worked);
 * 1: files unreadable or the wrong size; 2: they do not unlock this EEPROM. */
static int hdkey_from_bios(const char *bios_path, const char *mcpx_path, uint8_t hd[16], const char **rev)
{
    size_t bn = 0, mn = 0;
    uint8_t *bios = read_file(bios_path, &bn), *mcpx = read_file(mcpx_path, &mn);
    const uint8_t *im;
    uint8_t *dec = NULL;
    int r = 1;
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
            if (search_2bl(dec, 0x3600, hd)) { *rev = "1.1"; r = 0; goto out; }
        }
        SecureZeroMemory(blk, sizeof blk); SecureZeroMemory(rk, sizeof rk);
    }
    {   /* MCPX 1.0 */
        memcpy(dec, im + 0x39E00, 0x6000);
        rc4(mcpx + 0x1A5, 16, dec, 0x6000);
        if (search_2bl(dec, 0x6000, hd)) { *rev = "1.0"; r = 0; goto out; }
    }
out:
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
    SetWindowTextA(s_key, hex);
    SecureZeroMemory(hd, sizeof hd); SecureZeroMemory(hex, sizeof hex);
    check_key();
    snprintf(msg, sizeof msg, "OK: hard-drive key decrypted from your BIOS (MCPX %s) and verified against the EEPROM.", rev);
    say(s_key_st, msg);
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
static void say(HWND w, const char *t) { SetWindowTextA(w, t); }

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

static void check_key(void)
{
    char t[128];
    uint8_t k[16];
    GetWindowTextA(s_key, t, sizeof t);
    if (!t[0]) say(s_key_st, "Needed: your console's hard-drive key (32 hex digits).");
    else if (hex_key(t, k)) say(s_key_st, "OK: 16-byte hard-drive key.");
    else say(s_key_st, "Not a hard-drive key: it must be 32 hex digits (0-9, A-F).");
    SecureZeroMemory(k, sizeof k);
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
    for (i = 0; i < 7; i++)
        if (!read_sector(f, 9 + (uint64_t)i, s_sectors[i])) { CloseHandle(f); say(s_acc_st, "That file is too short, or a drive image this program cannot read."); return; }
    CloseHandle(f);
    for (i = 0; i < 7; i++) {
        const uint8_t *s = s_sectors[i];
        int k, empty = 1;
        for (k = 0; k < 512; k++) if (s[k]) { empty = 0; break; }
        if (empty) continue;
        if (!config_sector_ok(s)) { say(s_acc_st, "Those sectors are not an Xbox drive's account area (wrong file?)."); return; }
        valid++;
        if (s[24] && memcmp(s + 24, "SN.", 3) != 0) {         /* a player's gamertag (the machine account is SN.<serial>) */
            char tag[17];
            memcpy(tag, s + 24, 16); tag[16] = 0;
            for (k = 0; tag[k]; k++) if (tag[k] < 32 || tag[k] > 126) { tag[k] = 0; break; }
            if (tag[0]) {
                if (users++) strncat(s_gamertags, ", ", sizeof s_gamertags - strlen(s_gamertags) - 1);
                strncat(s_gamertags, tag, sizeof s_gamertags - strlen(s_gamertags) - 1);
            }
        }
    }
    if (!valid) { say(s_acc_st, "No Xbox Live accounts in that file (sectors 9-15 are empty)."); return; }
    s_acc_ok = 1;
    snprintf(msg, sizeof msg, users ? "OK: account(s) %s" : "Found the console's machine account but no player accounts.",
             s_gamertags);
    say(s_acc_st, msg);
}

/* ---- saving ----------------------------------------------------------------------------------- */
static int write_partition0(void)
{
    char path[MAX_PATH], bak[MAX_PATH];
    HANDLE f;
    uint8_t zero[512];
    LARGE_INTEGER sz;
    int i;
    if (!cs_settings_path("SaveData", path, sizeof path, 1)) return 0;
    CreateDirectoryA(path, NULL);
    strncat(path, "\\Partition0.img", sizeof path - strlen(path) - 1);
    snprintf(bak, sizeof bak, "%s.before-insignia", path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES)
        CopyFileA(path, bak, TRUE);                        /* the first time only */
    f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    memset(zero, 0, sizeof zero);
    if (GetFileSizeEx(f, &sz) && sz.QuadPart < 9 * 512) {  /* the sectors before ours exist, empty */
        LARGE_INTEGER li; DWORD w;
        li.QuadPart = sz.QuadPart;
        SetFilePointerEx(f, li, NULL, FILE_BEGIN);
        while (li.QuadPart < 9 * 512) { WriteFile(f, zero, 512, &w, NULL); li.QuadPart += 512; }
    }
    for (i = 0; i < 7; i++) {
        LARGE_INTEGER li; DWORD w = 0;
        li.QuadPart = (9 + i) * 512LL;
        if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN) || !WriteFile(f, s_sectors[i], 512, &w, NULL) || w != 512) {
            CloseHandle(f);
            return 0;
        }
    }
    CloseHandle(f);
    return 1;
}

static void save(void)
{
    char t[128], path[MAX_PATH], ini[MAX_PATH];
    uint8_t k[16];
    FILE *f;
    GetWindowTextA(s_key, t, sizeof t);
    if (!s_ee_ok) { tell("Choose your console's EEPROM first.", MB_ICONWARNING); return; }
    if (!hex_key(t, k)) { tell("Enter your console's hard-drive key (32 hex digits).", MB_ICONWARNING); return; }
    if (!s_acc_ok) { tell("Choose a dump of your console's drive with your Xbox Live account.", MB_ICONWARNING); return; }
    SecureZeroMemory(k, sizeof k);
    if (!cs_settings_path("eeprom.bin", path, sizeof path, 1) || !(f = fopen(path, "wb")) ||
        fwrite(s_eeprom, 1, 256, f) != 256 || fclose(f) != 0) {
        tell("The EEPROM could not be saved.", MB_ICONERROR);
        return;
    }
    if (!cs_settings_path("settings.ini", ini, sizeof ini, 1) || !WritePrivateProfileStringA("insignia", "hdd_key", t, ini)) {
        tell("The hard-drive key could not be saved.", MB_ICONERROR);
        return;
    }
    SecureZeroMemory(t, sizeof t);
    if (!write_partition0()) {
        tell("The account could not be written to the game's drive.", MB_ICONERROR);
        return;
    }
    say(s_status, "Saved. Start Counter-Strike, choose Xbox Live, and sign in.");
    tell("Your console identity and Xbox Live account are set up.\n\n"
         "Start Counter-Strike, choose Xbox Live, and pick your gamertag to sign in to Insignia.", MB_ICONINFORMATION);
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
}

/* ---- window ----------------------------------------------------------------------------------- */
static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(cls[0] == 'E' ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, s_main, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void make_ui(void)
{
    int y = 12;
    mk("STATIC", "Play Counter-Strike on Xbox Live through Insignia with your own Xbox console's identity and account. "
       "Your console must already be registered with Insignia (its Setup Assistant), with an Xbox Live account on it.",
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
    mk("STATIC", "Load it from a dump of your console's drive (the first 1 MB is enough), a raw drive image, or your xemu drive (.qcow2):",
       SS_LEFT, 24, y + 22, 546, 32, 0);
    s_acc = mk("EDIT", "", ES_AUTOHSCROLL | ES_READONLY, 24, y + 56, 440, 23, ID_ACC_EDIT);
    mk("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 474, y + 55, 96, 25, ID_ACC_BROWSE);
    s_acc_st = mk("STATIC", "Needed: the drive with the account you signed in with on your console.", SS_LEFT, 24, y + 88, 360, 20, ID_ACC_STATUS);
    {
        HWND c = mk("BUTTON", "Create a new account...", BS_PUSHBUTTON | WS_TABSTOP, 394, y + 84, 176, 25, ID_CREATE);
        EnableWindow(c, FALSE);       /* needs the dashboard's sign-up: not available yet */
    }
    y += 140;
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
            SendMessageA(s_key, EM_SETPASSWORDCHAR,
                         SendMessageA((HWND)l, BM_GETCHECK, 0, 0) == BST_CHECKED ? 0 : (WPARAM)'*', 0);
            InvalidateRect(s_key, NULL, TRUE);
            return 0;
        case ID_ACC_BROWSE:
            if (browse("Drive dump or image (*.bin;*.img;*.qcow2;*.raw)\0*.bin;*.img;*.qcow2;*.raw\0All files\0*.*\0", path, sizeof path)) {
                SetWindowTextA(s_acc, path);
                check_accounts(path);
            }
            return 0;
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
 * the same code. Exit 0 when saved; the reason goes to a message box. */
static int run_args(void)
{
    const char *ee = NULL, *key = NULL, *info = NULL, *drive = NULL, *bios = NULL, *mcpx = NULL;
    char t[128];
    int i;
    for (i = 1; i + 1 < __argc; i += 2) {
        if (!strcmp(__argv[i], "--eeprom")) ee = __argv[i + 1];
        else if (!strcmp(__argv[i], "--hdd-key")) key = __argv[i + 1];
        else if (!strcmp(__argv[i], "--hddinfo")) info = __argv[i + 1];
        else if (!strcmp(__argv[i], "--drive")) drive = __argv[i + 1];
        else if (!strcmp(__argv[i], "--bios")) bios = __argv[i + 1];
        else if (!strcmp(__argv[i], "--mcpx")) mcpx = __argv[i + 1];
        else return 2;
    }
    s_cli = 1;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stderr);
    if (!ee || !drive || (!key && !info && !(bios && mcpx))) {
        fprintf(stderr, "usage: insignia_setup --eeprom FILE (--hdd-key HEX | --hddinfo FILE) --drive FILE\n");
        return 2;
    }
    check_eeprom(ee);
    if (key) { SetWindowTextA(s_key, key); check_key(); }
    else if (info) key_from_hddinfo(info);
    else { key_from_bios(bios, mcpx); GetWindowTextA(s_key_st, t, sizeof t); fprintf(stderr, "%s\n", t); }
    check_accounts(drive);
    GetWindowTextA(s_ee_st, t, sizeof t);
    if (!s_ee_ok) { tell(t, MB_ICONERROR); return 3; }
    GetWindowTextA(s_acc_st, t, sizeof t);
    if (!s_acc_ok) { tell(t, MB_ICONERROR); return 4; }
    save();
    GetWindowTextA(s_status, t, sizeof t);
    return strncmp(t, "Saved", 5) == 0 ? 0 : 5;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, 592, 470 };
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
