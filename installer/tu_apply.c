/* tu_apply.c: see tu_apply.h. */
#include "tu_apply.h"

#include <bcrypt.h>
#include <compressapi.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "cabinet.lib")

#define HEAD 88u

static int read_all(const WCHAR *path, unsigned char **out, DWORD *n)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    DWORD got = 0;
    *out = NULL;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > 64 * 1024 * 1024 || !(*out = (unsigned char *)malloc((size_t)sz.QuadPart + 1))) {
        CloseHandle(f);
        return 0;
    }
    if (!ReadFile(f, *out, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        CloseHandle(f); free(*out); *out = NULL;
        return 0;
    }
    CloseHandle(f);
    *n = got;
    return 1;
}

static int sha256(const unsigned char *p, DWORD n, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg;
    int ok;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    ok = BCryptHash(alg, NULL, 0, (PUCHAR)p, n, out, 32) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static DWORD u32(const unsigned char *p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

int tu_apply_file(const WCHAR *xbe, const WCHAR *patch)
{
    unsigned char *pt = NULL, *src = NULL, *payload = NULL, *dst = NULL, h[32];
    DWORD pn = 0, sn = 0, block, ssize, dsize, plen, nblk, b, i;
    DECOMPRESSOR_HANDLE dec = NULL;
    SIZE_T got = 0;
    int r = TU_ERR_BAD_PATCH;
    WCHAR tmp[MAX_PATH];

    if (!read_all(xbe, &src, &sn)) return TU_ERR_IO;
    if (!sha256(src, sn, h)) { free(src); return TU_ERR_IO; }
    if (!read_all(patch, &pt, &pn)) { free(src); return TU_ERR_NO_PATCH; }
    if (pn < HEAD || memcmp(pt, "CSTU", 4) || u32(pt + 4) != 1) goto out;
    block = u32(pt + 8); ssize = u32(pt + 12); dsize = u32(pt + 48); plen = u32(pt + 84);
    if (!block || plen > pn - HEAD || dsize > 64 * 1024 * 1024) goto out;
    if (sn == dsize && !memcmp(h, pt + 52, 32)) { r = TU_ALREADY; goto out; }
    if (sn != ssize || memcmp(h, pt + 16, 32)) { r = TU_ERR_WRONG_XBE; goto out; }
    nblk = (dsize + block - 1) / block;
    payload = (unsigned char *)malloc((size_t)nblk * 4 + dsize);
    dst = (unsigned char *)malloc(dsize);
    if (!payload || !dst) goto out;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, NULL, &dec)) goto out;
    if (!Decompress(dec, pt + HEAD, plen, payload, (SIZE_T)nblk * 4 + dsize, &got) || got != (SIZE_T)nblk * 4 + dsize)
        goto out;
    for (b = 0; b < nblk; b++) {
        int off = (int)u32(payload + 4 * b);
        DWORD t = b * block, n = dsize - t < block ? dsize - t : block;
        const unsigned char *d = payload + (size_t)nblk * 4 + t;
        if (off >= 0 && ((DWORD)off > ssize || n > ssize - (DWORD)off)) goto out;
        for (i = 0; i < n; i++)
            dst[t + i] = (unsigned char)(d[i] + (off >= 0 ? src[off + i] : 0));
    }
    if (!sha256(dst, dsize, h) || memcmp(h, pt + 52, 32)) goto out;
    /* write beside it, then swap: a failure leaves the disc's file in place */
    r = TU_ERR_IO;
    if (wcslen(xbe) + 8 >= MAX_PATH) goto out;
    wcscpy_s(tmp, MAX_PATH, xbe);
    wcscat_s(tmp, MAX_PATH, L".new");
    {
        HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w = 0;
        BOOL ok;
        if (f == INVALID_HANDLE_VALUE) goto out;
        ok = WriteFile(f, dst, dsize, &w, NULL) && w == dsize && FlushFileBuffers(f);
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp, xbe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp);
            goto out;
        }
    }
    r = TU_PATCHED;
out:
    if (dec) CloseDecompressor(dec);
    free(pt); free(src); free(payload); free(dst);
    return r;
}

const char *tu_error_text(int code)
{
    switch (code) {
    case TU_ALREADY: return "default.xbe is already the title update";
    case TU_PATCHED: return "default.xbe was updated to the title update";
    case TU_ERR_NO_PATCH: return "the title update patch (cs_tu.xbepatch) is missing";
    case TU_ERR_BAD_PATCH: return "the title update patch is damaged";
    case TU_ERR_WRONG_XBE: return "default.xbe is not the supported version of Counter-Strike";
    default: return "default.xbe could not be read or replaced";
    }
}
