/* cs_content.c -- the two downloadable maps, Office and Inferno, unlocked at start-up.
 *
 * Both maps are on the disc (cs_office.SXWad, de_inferno.SXWad). On a console
 * Xbox Live delivered only an unlock: a content package in
 * E:\TDATA\4d530036\$c\<offer id>\ whose ContentMeta.xbx names the offer. When
 * one exists the game lists the map. It reads nothing else from the package: XAPI's
 * content enumeration (sub_00202380 in the title update) opens ContentMeta.xbx,
 * checks the "XCSF" header and takes the offer ID, flags and display name.
 *
 * So the game writes those two files itself if they are missing, under the save
 * folder's TDATA (the toolkit maps \Device\Harddisk0\Partition1\TDATA there).
 * Packages already present, such as a player's own copied from a console, are
 * left alone.
 *
 * The header is signed the way XAPI's content check (sub_00207288 ->
 * sub_002071A3) verifies one: key = HMAC-SHA1(XboxHDKey, title ID), digest =
 * HMAC-SHA1(key, header bytes 0x14..0x88), stored in the first 20 bytes, under
 * the XboxHDKey the runtime reports (the player's for Insignia, else zeros);
 * files written under another key are re-signed.
 */
#include "cs_content.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")

#define CS_TITLE_ID 0x4D530036u
#define HDR_SIZE    0x88u

static int hmac_sha1(const uint8_t *key, ULONG klen, const uint8_t *data, ULONG n, uint8_t out[20])
{
    BCRYPT_ALG_HANDLE alg;
    NTSTATUS st;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return 0;
    st = BCryptHash(alg, (PUCHAR)key, klen, (PUCHAR)data, n, out, 20);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0;
}

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ContentMeta.xbx: the 0x88-byte header, then the display names (UTF-16 with a
 * BOM, an INI the way Xbox Live wrote them). */
/* The XboxHDKey the toolkit will report: the player's (RECOMP_XBOX_HDKEY, set
 * from settings.ini by cs_settings_startup) or all zeros. */
static void current_hd_key(uint8_t k[16])
{
    const char *s = getenv("RECOMP_XBOX_HDKEY");
    int n = 0;
    memset(k, 0, 16);
    for (; s && *s && n < 32; s++) {
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else continue;
        if (n & 1) k[n / 2] |= (uint8_t)v; else k[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    if (n != 32) memset(k, 0, 16);
}

/* The header digest under the current key; 1 when buf's first 20 bytes match. */
static int digest(uint8_t *buf, int check)
{
    uint8_t hd_key[16], key[20], tid[4], d[20];
    current_hd_key(hd_key);
    put32(tid, CS_TITLE_ID);
    if (!hmac_sha1(hd_key, 16, tid, 4, key) || !hmac_sha1(key, 20, buf + 0x14, HDR_SIZE - 0x14, d))
        return 0;
    SecureZeroMemory(hd_key, sizeof hd_key);
    if (check) return memcmp(d, buf, 20) == 0;
    memcpy(buf, d, 20);
    return 1;
}

/* A package this file wrote earlier whose signature is not the current key's
 * (written before the player set their hard-drive key, say): rewritten, or the
 * game's own check counts it as not installed and offers it again (the "new
 * content" arrow on Xbox Live > Download Content). Packages it did not write
 * -- a player's own, copied from a console, with a file table -- are kept. */
static int needs_rewrite(const char *path)
{
    uint8_t buf[1024];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < HDR_SIZE || memcmp(buf + 0x14, "XCSF", 4)) return 0;     /* not a package: leave it */
    if (rd32le(buf + 0x50) != 0 || n > 0x200) return 0;              /* has a file table: not ours */
    return !digest(buf, 1);
}

static int write_meta(const char *path, uint32_t offer, const wchar_t *name)
{
    uint8_t buf[1024];
    wchar_t text[256];
    int tl;
    uint32_t nbytes;
    FILE *f;

    tl = swprintf(text, 256, L"\xFEFF[default]\r\nName=\"%ls\"\r\n[EN]\r\nName=\"%ls\"\r\n", name, name);
    if (tl <= 0) return 0;
    nbytes = (uint32_t)tl * 2u;
    if (HDR_SIZE + nbytes > sizeof buf) return 0;
    memset(buf, 0, sizeof buf);
    memcpy(buf + 0x14, "XCSF", 4);
    put32(buf + 0x18, HDR_SIZE);        /* header size */
    put32(buf + 0x1C, 1);               /* version */
    put32(buf + 0x20, 1);               /* flags: bit 0, content present */
    put32(buf + 0x24, CS_TITLE_ID);
    put32(buf + 0x28, offer);           /* offer ID, low half */
    put32(buf + 0x2C, CS_TITLE_ID);     /* offer ID, high half: the title */
    put32(buf + 0x30, 0xFFFFFFFFu);
    put32(buf + 0x34, HDR_SIZE);        /* display names: offset, size */
    put32(buf + 0x38, nbytes);
    memcpy(buf + HDR_SIZE, text, nbytes);
    if (!digest(buf, 0))
        return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(buf, 1, HDR_SIZE + nbytes, f) != HDR_SIZE + nbytes) { fclose(f); return 0; }
    return fclose(f) == 0;
}

void cs_content_install(void)
{
    static const struct { uint32_t offer; const wchar_t *name; const char *map; } maps[] = {
        { 1, L"Office Map", "cs_office" },
        { 2, L"Inferno Map", "de_inferno" },
    };
    const char *root = getenv("RECOMP_SAVE_DIR");
    char dir[MAX_PATH], path[MAX_PATH];
    int i;

    if (!root || !*root) root = "SaveData";
    for (i = 0; i < 2; i++) {
        snprintf(dir, sizeof dir, "%s\\TDATA\\%08x\\$c\\%08x%08x", root, CS_TITLE_ID, CS_TITLE_ID, maps[i].offer);
        snprintf(path, sizeof path, "%s\\ContentMeta.xbx", dir);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && !needs_rewrite(path))
            continue;
        {   /* the folders above it */
            char p[MAX_PATH];
            char *s;
            snprintf(p, sizeof p, "%s", dir);
            for (s = p; *s; s++)
                if (*s == '\\' && s > p && s[-1] != ':') { *s = 0; CreateDirectoryA(p, NULL); *s = '\\'; }
            CreateDirectoryA(p, NULL);
        }
        fprintf(stderr, "[CS2] %s: %s\n", maps[i].map,
                write_meta(path, maps[i].offer, maps[i].name) ? "unlocked (content package written)"
                                                              : "could not write its content package");
    }
}
