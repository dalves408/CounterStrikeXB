#define COBJMACROS
#include "cs_texture_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PACK_PATH 2048
#define PACK_MAX_ENTRIES 32768
#define PACK_MAX_DIMENSION 8192
#define PACK_MAX_MIPS 14
#define DDS_HEADER_BYTES 148

typedef struct {
    uint64_t hash;
    unsigned format, width, height;
    char file[320];
} PackEntry;

static PackEntry *entries;
static size_t count;
static WCHAR root[PACK_PATH];
static int initialized;
static unsigned loaded, rejected;

static int compare_entry(const void *first, const void *second)
{
    const PackEntry *a = first, *b = second;
    if (a->format != b->format) return (a->format > b->format) ? 1 : -1;
    if (a->width != b->width) return (a->width > b->width) ? 1 : -1;
    if (a->height != b->height) return (a->height > b->height) ? 1 : -1;
    return (a->hash > b->hash) - (a->hash < b->hash);
}

static int safe_file(const char *path)
{
    size_t length = strlen(path);
    if (length < 5 || path[0] == '/' || strstr(path, "..") ||
        strcmp(path + length - 4, ".dds") != 0) return 0;
    return strspn(path, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
                        "._{}+-/") == length;
}

static void initialize(void)
{
    WCHAR index[PACK_PATH];
    char row[512];
    FILE *file;
    DWORD length;
    PackEntry *pack;
    size_t used = 0;

    initialized = 1;
    length = GetEnvironmentVariableW(L"RECOMP_CS_TEXTURE_PACK", root, _countof(root));
    if (length >= _countof(root) || (length && wcscmp(root, L"0") == 0)) return;
    if (!length) {
        WCHAR *slash;
        length = GetModuleFileNameW(NULL, root, _countof(root));
        if (!length || length >= _countof(root)) return;
        slash = wcsrchr(root, L'\\');
        if (!slash || wcscpy_s(slash + 1, _countof(root) - (slash + 1 - root), L"texture_pack")) return;
    }
    if (swprintf_s(index, _countof(index), L"%ls/index.tsv", root) < 0 ||
        _wfopen_s(&file, index, L"rb")) return;

    if (!fgets(row, sizeof row, file) ||
        (strcmp(row, "CSXB-DDS-1\n") && strcmp(row, "CSXB-DDS-1\r\n"))) {
        fclose(file);
        fprintf(stderr, "[CS DDS] Unsupported texture index format\n");
        return;
    }
    pack = malloc(PACK_MAX_ENTRIES * sizeof *pack);
    if (!pack) { fclose(file); return; }
    while (fgets(row, sizeof row, file)) {
        PackEntry *entry;
        unsigned long long hash;
        char extra;
        if (used == PACK_MAX_ENTRIES || !strchr(row, '\n')) break;
        entry = &pack[used];
        if (sscanf(row, "%x %u %u %16llx %319s %c", &entry->format,
                   &entry->width, &entry->height, &hash, entry->file, &extra) != 5 ||
            entry->format > CS_PACK_DECODED_BGRA || !entry->width || !entry->height ||
            entry->width > PACK_MAX_DIMENSION / 4 || entry->height > PACK_MAX_DIMENSION / 4 ||
            !safe_file(entry->file)) break;
        entry->hash = hash;
        ++used;
    }
    if (!feof(file) || ferror(file)) {
        fprintf(stderr, "[CS DDS] Invalid texture index\n");
        free(pack);
        fclose(file);
        return;
    }
    fclose(file);
    if (!used) { free(pack); return; }
    qsort(pack, used, sizeof *pack, compare_entry);
    for (size_t i = 1; i < used; ++i) {
        if (!compare_entry(&pack[i - 1], &pack[i])) {
            fprintf(stderr, "[CS DDS] Duplicate texture key in index\n");
            free(pack);
            return;
        }
    }
    entries = pack;
    count = used;
    fprintf(stderr, "[CS DDS] Indexed %zu replacement keys\n", count);
}

int cs_pack_enabled(void)
{
    if (!initialized) initialize();
    return count != 0;
}

/* Match the CRC-32/Adler-32 pair emitted by index_texture_pack.py. This runs
 * when a guest texture enters or changes in the renderer's existing cache. */
static uint64_t content_key(const uint8_t *data, size_t length)
{
    static uint32_t table[256];
    static int ready;
    uint32_t crc = 0xffffffffu, a = 1, b = 0;
    if (!ready) {
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (unsigned n = 0; n < 8; ++n)
                value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
            table[i] = value;
        }
        ready = 1;
    }
    while (length) {
        /* 5552 is the largest chunk that keeps the Adler sums in uint32_t. */
        size_t chunk = length < 5552 ? length : 5552;
        for (size_t i = 0; i < chunk; ++i) {
            uint8_t value = *data++;
            crc = (crc >> 8) ^ table[(crc ^ value) & 255];
            a += value;
            b += a;
        }
        a %= 65521;
        b %= 65521;
        length -= chunk;
    }
    return ((uint64_t)(crc ^ 0xffffffffu) << 32) | ((uint64_t)b << 16) | a;
}

static uint32_t read32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int load_bc7(ID3D11Device *device, const PackEntry *entry, size_t byte_limit,
                    ID3D11Texture2D **texture, ID3D11ShaderResourceView **view, size_t *bytes)
{
    WCHAR name[320], path[PACK_PATH];
    FILE *file;
    uint8_t header[DDS_HEADER_BYTES], *data;
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA levels[PACK_MAX_MIPS] = {0};
    unsigned w, h, expected_mips = 1;
    size_t size = 0;
    HRESULT hr;

    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, entry->file, -1, name, _countof(name)) ||
        swprintf_s(path, _countof(path), L"%ls/%ls", root, name) < 0 ||
        _wfopen_s(&file, path, L"rb")) return 0;
    if (fread(header, 1, sizeof header, file) != sizeof header ||
        memcmp(header, "DDS ", 4) || read32(header + 4) != 124 ||
        read32(header + 76) != 32 || memcmp(header + 84, "DX10", 4) ||
        read32(header + 128) != DXGI_FORMAT_BC7_UNORM ||
        read32(header + 132) != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
        read32(header + 136) != 0 || read32(header + 140) != 1) {
        fclose(file);
        return 0;
    }
    w = desc.Width = read32(header + 16);
    h = desc.Height = read32(header + 12);
    desc.MipLevels = read32(header + 28);
    if (w != entry->width * 4 || h != entry->height * 4) { fclose(file); return 0; }
    for (unsigned longest = w > h ? w : h; longest > 1; longest >>= 1) ++expected_mips;
    if (desc.MipLevels != expected_mips || desc.MipLevels > PACK_MAX_MIPS) { fclose(file); return 0; }
    for (unsigned i = 0; i < desc.MipLevels; ++i) {
        levels[i].SysMemPitch = ((w + 3) / 4) * 16;
        levels[i].SysMemSlicePitch = levels[i].SysMemPitch * ((h + 3) / 4);
        size += levels[i].SysMemSlicePitch;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (size > byte_limit) { fclose(file); return 0; }
    data = malloc(size);
    if (!data) { fclose(file); return 0; }
    if (fread(data, 1, size, file) != size || fgetc(file) != EOF || ferror(file)) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    for (unsigned i = 0, offset = 0; i < desc.MipLevels; ++i) {
        levels[i].pSysMem = data + offset;
        offset += levels[i].SysMemSlicePitch;
    }
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_BC7_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(device, &desc, levels, texture);
    free(data);
    if (FAILED(hr)) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)*texture, NULL, view))) {
        ID3D11Texture2D_Release(*texture);
        *texture = NULL;
        return 0;
    }
    *bytes = size;
    return 1;
}

/* ---- background loading ------------------------------------------------------
 * A replacement is several MB read from disk; loading it on the renderer's
 * thread the first time a texture shows up was a hitch each time, mid-match.
 * Now the renderer asks for it (cs_pack_request), keeps drawing the original,
 * and swaps the replacement in once this thread has it (cs_pack_take). The
 * D3D11 device is free-threaded, so the texture is created here too. */
typedef struct {
    volatile LONG state;            /* JOB_IDLE / QUEUED / READY / FAILED */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    size_t bytes;
} PackJob;
enum { JOB_IDLE, JOB_QUEUED, JOB_READY, JOB_FAILED };
#define JOB_QUEUE 4096
static PackJob *jobs;
static int job_queue[JOB_QUEUE];
static unsigned job_head, job_tail;
static CRITICAL_SECTION job_lock;
static HANDLE job_wake;
static ID3D11Device *job_device;

static DWORD WINAPI job_thread(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        int id = -1;
        EnterCriticalSection(&job_lock);
        if (job_head != job_tail) id = job_queue[job_tail++ % JOB_QUEUE];
        LeaveCriticalSection(&job_lock);
        if (id < 0) { WaitForSingleObject(job_wake, INFINITE); continue; }
        {
            PackJob *j = &jobs[id];
            ID3D11Texture2D *tex = NULL;
            ID3D11ShaderResourceView *srv = NULL;
            size_t bytes = 0;
            if (load_bc7(job_device, &entries[id], (size_t)-1, &tex, &srv, &bytes)) {
                j->tex = tex; j->srv = srv; j->bytes = bytes;
                InterlockedExchange(&j->state, JOB_READY);
                if (loaded++ < 16 || getenv("RECOMP_CS_TEXTURE_PACK_LOG"))
                    fprintf(stderr, "[CS DDS] Loaded %s (%ux%u -> %ux%u)\n", entries[id].file,
                            entries[id].width, entries[id].height, entries[id].width * 4, entries[id].height * 4);
            } else {
                InterlockedExchange(&j->state, JOB_FAILED);
                if (rejected++ < 8) fprintf(stderr, "[CS DDS] Using original for %s\n", entries[id].file);
            }
        }
    }
}

int cs_pack_request(ID3D11Device *device, unsigned format, unsigned width, unsigned height,
                    const void *pixels, size_t length)
{
    PackEntry key = {0};
    const PackEntry *entry;
    int id;
    if (!cs_pack_enabled()) return 0;
    key.format = format;
    key.width = width;
    key.height = height;
    key.hash = content_key(pixels, length);
    entry = bsearch(&key, entries, count, sizeof *entries, compare_entry);
    if (!entry) return 0;
    if (!jobs) {
        HANDLE t;
        jobs = (PackJob *)calloc(count, sizeof *jobs);
        if (!jobs) return 0;
        InitializeCriticalSection(&job_lock);
        job_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        job_device = device;
        ID3D11Device_AddRef(device);
        t = CreateThread(NULL, 0, job_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    id = (int)(entry - entries);
    if (InterlockedCompareExchange(&jobs[id].state, JOB_QUEUED, JOB_IDLE) == JOB_IDLE) {
        EnterCriticalSection(&job_lock);
        if (job_head - job_tail < JOB_QUEUE) job_queue[job_head++ % JOB_QUEUE] = id;
        else InterlockedExchange(&jobs[id].state, JOB_IDLE);     /* full: asked again at a later bind */
        LeaveCriticalSection(&job_lock);
        SetEvent(job_wake);
    }
    return id + 1;
}

int cs_pack_take(int request, size_t byte_limit, ID3D11Texture2D **texture, ID3D11ShaderResourceView **view,
                 size_t *bytes)
{
    PackJob *j;
    if (request <= 0 || !jobs || request > (int)count) return -1;
    j = &jobs[request - 1];
    switch (j->state) {
    case JOB_QUEUED: return 0;
    case JOB_READY:
        if (j->bytes > byte_limit) return -1;          /* stays loaded until the budget allows it */
        *texture = j->tex; *view = j->srv; *bytes = j->bytes;
        j->tex = NULL; j->srv = NULL; j->bytes = 0;
        InterlockedExchange(&j->state, JOB_IDLE);
        return 1;
    case JOB_IDLE: return -1;                           /* taken by another texture with the same content */
    default: return -1;
    }
}

void cs_pack_drop(void)
{
    unsigned i;
    if (!jobs) return;
    for (i = 0; i < count; i++)
        if (jobs[i].state == JOB_READY) {
            if (jobs[i].srv) ID3D11ShaderResourceView_Release(jobs[i].srv);
            if (jobs[i].tex) ID3D11Texture2D_Release(jobs[i].tex);
            jobs[i].srv = NULL; jobs[i].tex = NULL; jobs[i].bytes = 0;
            InterlockedExchange(&jobs[i].state, JOB_IDLE);
        }
}

int cs_pack_load(ID3D11Device *device, unsigned format, unsigned width, unsigned height,
                 const void *pixels, size_t length, size_t byte_limit,
                 ID3D11Texture2D **texture, ID3D11ShaderResourceView **view, size_t *bytes)
{
    PackEntry key = {0};
    const PackEntry *entry;
    if (!cs_pack_enabled() || !byte_limit) return 0;
    key.format = format;
    key.width = width;
    key.height = height;
    key.hash = content_key(pixels, length);
    entry = bsearch(&key, entries, count, sizeof *entries, compare_entry);
    if (!entry) return 0;
    if (!load_bc7(device, entry, byte_limit, texture, view, bytes)) {
        if (rejected++ < 8) fprintf(stderr, "[CS DDS] Using original for %s\n", entry->file);
        return 0;
    }
    if (loaded++ < 16 || getenv("RECOMP_CS_TEXTURE_PACK_LOG"))
        fprintf(stderr, "[CS DDS] Loaded %s (%ux%u -> %ux%u)\n",
                entry->file, width, height, width * 4, height * 4);
    return 1;
}
