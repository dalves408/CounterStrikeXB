/* cs_diag.c - what a player's problem report needs, in every build, at no cost while the game plays.
 * (Modelled on the Marvel vs. Capcom 2 recompilation's mvc2_diag.c.)
 *
 *   log file      the player build copies everything it prints to %LOCALAPPDATA%\CounterStrikeXB\logs\cs-<date>-<time>.log
 *                 and passes it on to the original stdout/stderr, which RecompLauncher captures for its crash report.
 *                 The last three runs are kept, a log stops taking routine lines at 20 MB, the long per-texture and
 *                 per-method tables are left out, and the Windows user name is replaced by %USERPROFILE%/%USERNAME%.
 *   [BUILD]       one line at start: version, symbol-file id, Windows, CPU, GPU, memory.
 *   [VIDEO]       every video setting in effect, the window size and the monitor's refresh rate: once when the game
 *                 is up, when a setting changes in the F1 menu (cs_overlay.c), and in every freeze or crash report.
 *   [FPS]         once a minute of play: average frame rate, 1% low, longest frame, frames over 50 and 100 ms.
 *   [FREEZE]      no frame for 10 s: every thread's call stack as module+offset (and symbol, when a .pdb is beside the
 *                 exe); again 20 s later; a line when frames move again. The ESC/F1 menus never stop the game, so a
 *                 freeze is a freeze whatever is on screen. scripts/symbolize_log.py turns the offsets into function
 *                 names with the release's kept .pdb (out\symbols\<version>), and every generated function is named
 *                 after its guest address (sub_XXXXXXXX), so a stack is the guest call chain.
 *   [CRASH]       a fatal exception nothing handled: code, where, call stack (main.c's VEH prints access violations).
 *
 * The thread stacks are taken by suspending one thread at a time, reading its registers and unwinding with ntdll's
 * RtlVirtualUnwind (no heap, no dbghelp while it is suspended), then resuming it before anything is printed. Output goes
 * through WriteFile, never the C runtime's stream lock, which a stuck thread may hold. */
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include "cs_diag.h"
#include "cs_settings.h"

extern unsigned cs_d3d11_frames(void);
extern int cs_d3d11_pace_hz(void);            /* cs_d3d11.c: the rate frames are paced at */          /* cs_d3d11.c: flips presented so far */
extern int xbox_FramebufferIsFullscreen(void);    /* toolkit fb_present.c */
extern void *xbox_FramebufferWindowHwnd(void);

#define LOG_KEEP 5                  /* runs kept, this one included: the game relaunches itself for every map, a new log each */
#define LOG_CAP  (20u << 20)        /* after this, only reports and errors */
#define MAX_FRAMES 24

/* ---- output that never takes the C runtime's lock ------------------------------------------------------------------ */
static void put(const char *s) { DWORD n; WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)strlen(s), &n, NULL); }

static void putf(const char *fmt, ...)
{
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    put(line);
}

/* ---- where an address is ------------------------------------------------------------------------------------------ */
static int s_have_pdb = -1;

static int have_pdb(void)
{
    if (s_have_pdb < 0) {
        IMAGEHLP_MODULE64 m;
        memset(&m, 0, sizeof m);
        m.SizeOfStruct = sizeof m;
        s_have_pdb = SymGetModuleInfo64(GetCurrentProcess(), (DWORD64)(uintptr_t)GetModuleHandleA(NULL), &m) &&
                     (m.SymType == SymPdb);
    }
    return s_have_pdb;
}

void cs_diag_where(const void *addr, char *out, size_t n)
{
    HMODULE mod = NULL;
    char path[MAX_PATH], *name;
    size_t len;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) || !GetModuleFileNameA(mod, path, sizeof path)) {
        snprintf(out, n, "0x%llX", (unsigned long long)(uintptr_t)addr);
        return;
    }
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    if (strchr(name, '.')) *strrchr(name, '.') = 0;
    snprintf(out, n, "%s+0x%llX", name, (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    len = strlen(out);
    if (mod == GetModuleHandleA(NULL) && have_pdb() && len + 8 < n) {
        char buf[sizeof(SYMBOL_INFO) + 128];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        DWORD64 d = 0;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 127;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &d, sym))
            snprintf(out + len, n - len, " (%s+0x%llX)", sym->Name, (unsigned long long)d);
    }
}

/* Unwind a captured context. Reads the other thread's stack only; no locks beyond ntdll's function-table lookup. */
static int unwind(CONTEXT *ctx, DWORD64 *pcs, int max)
{
    int d = 0;
    while (d < max && ctx->Rip) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f;
        pcs[d++] = ctx->Rip;
        f = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (!f) {                                   /* a leaf: the return address is on top of the stack */
            __try { ctx->Rip = *(const DWORD64 *)ctx->Rsp; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            ctx->Rsp += 8;
        } else {
            PVOID hd = NULL;
            DWORD64 ef = 0;
            __try { RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, f, ctx, &hd, &ef, NULL); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        }
    }
    return d;
}

static void put_stack(const char *tag, DWORD tid, const DWORD64 *pcs, int n)
{
    char line[4096], where[200];
    int len = snprintf(line, sizeof line, "%s thread %lu:", tag, (unsigned long)tid), i;
    for (i = 0; i < n && len < (int)sizeof line - 220; i++) {
        cs_diag_where((const void *)(uintptr_t)pcs[i], where, sizeof where);
        len += snprintf(line + len, sizeof line - len, "%s %s", i ? " <" : "", where);
    }
    snprintf(line + len, sizeof line - len, "\n");
    put(line);
}

/* ---- the log file -------------------------------------------------------------------------------------------------- */
static HANDLE s_file = INVALID_HANDLE_VALUE;    /* this run's log */
static HANDLE s_orig;                           /* where stderr went before (a launcher's pipe, or nothing) */
static HANDLE s_rd, s_wr;                       /* the pipe stdout and stderr now write into */
static DWORD  s_tee_tid, s_watch_tid;
static volatile LONG s_tee_busy;
static unsigned long long s_file_bytes;
static int s_capped;
static char s_profile[MAX_PATH], s_user[128];
static size_t s_profile_len, s_user_len;

static int starts(const char *s, size_t n, const char *p)
{
    size_t k = strlen(p);
    return n >= k && !memcmp(s, p, k);
}

static int important(const char *s, size_t n)
{
    static const char *const keys[] = { "[CRASH]", "[FREEZE]", "[BUILD]", "ERROR", "[FATAL]", "[LOG]" };
    size_t i, k;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
        for (k = 0; k + strlen(keys[i]) <= n; k++)
            if (!memcmp(s + k, keys[i], strlen(keys[i]))) return 1;
    return 0;
}

/* Copy a line, putting %USERPROFILE% for the profile folder and %USERNAME% for the name between path separators. */
static size_t scrub(const char *in, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n && o + 16 < cap) {
        if (s_profile_len && i + s_profile_len <= n && !_strnicmp(in + i, s_profile, s_profile_len)) {
            memcpy(out + o, "%USERPROFILE%", 13); o += 13; i += s_profile_len; continue;
        }
        if (s_user_len && (in[i] == '\\' || in[i] == '/') && i + 1 + s_user_len < n &&
            !_strnicmp(in + i + 1, s_user, s_user_len) && (in[i + 1 + s_user_len] == '\\' || in[i + 1 + s_user_len] == '/')) {
            out[o++] = in[i];
            memcpy(out + o, "%USERNAME%", 10); o += 10; i += 1 + s_user_len; continue;
        }
        out[o++] = in[i++];
    }
    return o;
}

static void emit(const char *line, size_t n)
{
    static char clean[8192 + 64];
    DWORD w;
    size_t m;

    /* The long per-texture and per-method tables say nothing a report needs and are most of the bytes. So is the
     * toolkit's draw counter (one line per 200 draws, three quarters of a match's log, which then reached the 20 MB
     * cap in about 12 minutes); a stalled picture is a [FREEZE] report, with every thread's stack. */
    if (starts(line, n, "  [TEXUSE]") || starts(line, n, "  [GPU]   0x") || starts(line, n, "  [GPU] draw #"))
        return;
    m = scrub(line, n, clean, sizeof clean);
    if (s_orig && !WriteFile(s_orig, clean, (DWORD)m, &w, NULL))
        s_orig = NULL;                              /* the reader went away; keep the file going */
    if (s_file == INVALID_HANDLE_VALUE)
        return;
    if (s_file_bytes > LOG_CAP && !important(clean, m)) {
        if (!s_capped) {
            static const char note[] = "[LOG] this log reached 20 MB: from here on only reports and errors are kept\n";
            s_capped = 1;
            WriteFile(s_file, note, sizeof note - 1, &w, NULL);
        }
        return;
    }
    if (WriteFile(s_file, clean, (DWORD)m, &w, NULL))
        s_file_bytes += w;
}

static DWORD WINAPI tee_thread(LPVOID p)
{
    static char buf[65536], line[8192];
    size_t ln = 0;
    DWORD idle = 0;
    (void)p;
    for (;;) {
        DWORD avail = 0, got = 0, i;
        if (!PeekNamedPipe(s_rd, NULL, 0, NULL, &avail, NULL))
            return 0;
        if (!avail) {
            /* A line with no end yet goes out once nothing more has come for a moment, so the last words before a
             * crash are not held back. */
            if (ln && ++idle >= 10) { emit(line, ln); ln = 0; }
            if (!ln) InterlockedExchange(&s_tee_busy, 0);
            Sleep(20);
            continue;
        }
        idle = 0;
        InterlockedExchange(&s_tee_busy, 1);
        if (!ReadFile(s_rd, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL))
            return 0;
        for (i = 0; i < got; i++) {
            line[ln++] = buf[i];
            if (buf[i] == '\n' || ln == sizeof line) { emit(line, ln); ln = 0; }
        }
    }
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void prune_logs(const char *dir)
{
    static char names[64][MAX_PATH];
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;

    snprintf(pat, sizeof pat, "%s\\cs-*.log", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (n < 64) snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, MAX_PATH, by_name);             /* the names are dates, so this is oldest first */
    for (i = 0; i < n - (LOG_KEEP - 1); i++) {
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        DeleteFileA(path);
    }
}

void cs_diag_log_start(const char *data_dir)
{
    char dir[MAX_PATH], path[MAX_PATH];
    SYSTEMTIME st;
    int fd;

    if (!GetEnvironmentVariableA("USERPROFILE", s_profile, sizeof s_profile)) s_profile[0] = 0;
    if (!GetEnvironmentVariableA("USERNAME", s_user, sizeof s_user)) s_user[0] = 0;
    s_profile_len = strlen(s_profile);
    s_user_len = strlen(s_user);
    if (s_profile_len < 4) s_profile_len = 0;       /* never blank out something as short as "C:\" */
    if (s_user_len < 2) s_user_len = 0;

    snprintf(dir, sizeof dir, "%s\\logs", data_dir);
    CreateDirectoryA(dir, NULL);
    prune_logs(dir);
    GetLocalTime(&st);
    snprintf(path, sizeof path, "%s\\cs-%04d%02d%02d-%02d%02d%02d.log", dir, st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    s_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);

    /* stdout and stderr both into one pipe; the tee thread writes each line to the file and to the old stderr (or the
     * old stdout, when only that was connected). */
    s_orig = GetStdHandle(STD_ERROR_HANDLE);
    if (s_orig == INVALID_HANDLE_VALUE || !s_orig || GetFileType(s_orig) == FILE_TYPE_UNKNOWN)
        s_orig = GetStdHandle(STD_OUTPUT_HANDLE);
    if (s_orig == INVALID_HANDLE_VALUE || !s_orig || GetFileType(s_orig) == FILE_TYPE_UNKNOWN)
        s_orig = NULL;
    /* Our own copy: _dup2 below closes the handle behind fd 1 and 2, which is this one. */
    if (s_orig && !DuplicateHandle(GetCurrentProcess(), s_orig, GetCurrentProcess(), &s_orig, 0, FALSE,
                                   DUPLICATE_SAME_ACCESS))
        s_orig = NULL;
    if (!CreatePipe(&s_rd, &s_wr, NULL, 1 << 20))
        return;                                     /* no pipe: output goes where it always did, no file */
    if (_fileno(stdout) < 0) freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0) freopen("NUL", "w", stderr);
    fd = _open_osfhandle((intptr_t)s_wr, _O_WRONLY | _O_BINARY);
    if (fd < 0)
        return;
    _dup2(fd, _fileno(stdout));
    _dup2(fd, _fileno(stderr));
    SetStdHandle(STD_OUTPUT_HANDLE, s_wr);
    SetStdHandle(STD_ERROR_HANDLE, s_wr);
    CloseHandle(CreateThread(NULL, 0, tee_thread, NULL, 0, &s_tee_tid));
    atexit(cs_diag_flush);
}

void cs_diag_flush(void)
{
    int i;
    fflush(stdout);
    fflush(stderr);
    if (!s_rd)
        return;
    for (i = 0; i < 100; i++) {                     /* up to a second for the tee to catch up */
        DWORD avail = 0;
        if (!PeekNamedPipe(s_rd, NULL, 0, NULL, &avail, NULL) || (!avail && !s_tee_busy))
            break;
        Sleep(10);
    }
    if (s_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(s_file);
}

/* ---- [BUILD] ------------------------------------------------------------------------------------------------------- */
void cs_diag_build_line(void)
{
    char ver[32] = "dev", pdb[64] = "none", cpu[96] = "?", gpu[256] = "", os[64] = "?", disp[16] = "";
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    MEMORYSTATUSEX ms;
    DISPLAY_DEVICEA dev;
    HRSRC r;
    DWORD i, sz;

    /* version: the exe's own version resource (resources/cs.rc, set by build_installer.py) */
    if ((r = FindResourceA(NULL, MAKEINTRESOURCEA(1), RT_VERSION)) != NULL) {
        const BYTE *v = (const BYTE *)LockResource(LoadResource(NULL, r));
        DWORD n = SizeofResource(NULL, r), k;
        for (k = 0; v && k + 52 <= n; k += 4)       /* VS_FIXEDFILEINFO starts with 0xFEEF04BD */
            if (*(const DWORD *)(v + k) == 0xFEEF04BDu) {
                const VS_FIXEDFILEINFO *f = (const VS_FIXEDFILEINFO *)(v + k);
                snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
                         HIWORD(f->dwFileVersionLS));
                break;
            }
    }
    /* the .pdb this exe was linked with: what scripts/symbolize_log.py matches */
    for (i = 0; dd.VirtualAddress && i < dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++) {
        const IMAGE_DEBUG_DIRECTORY *d = (const IMAGE_DEBUG_DIRECTORY *)(base + dd.VirtualAddress) + i;
        if (d->Type == IMAGE_DEBUG_TYPE_CODEVIEW && d->AddressOfRawData) {
            const BYTE *cv = base + d->AddressOfRawData;
            if (*(const DWORD *)cv == 0x53445352u) {        /* "RSDS" */
                const GUID *g = (const GUID *)(cv + 4);
                snprintf(pdb, sizeof pdb, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", g->Data1, g->Data2,
                         g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                         g->Data4[6], g->Data4[7], *(const DWORD *)(cv + 20));
            }
        }
    }
    { HKEY k;
      if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k)) {
          sz = sizeof cpu; RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)cpu, &sz); RegCloseKey(k);
          for (sz = (DWORD)strlen(cpu); sz && cpu[sz - 1] == ' '; ) cpu[--sz] = 0; }
      if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k)) {
          char build[16] = "?", name[64] = "Windows"; DWORD ubr = 0;
          sz = sizeof build; RegQueryValueExA(k, "CurrentBuild", NULL, NULL, (BYTE *)build, &sz);
          sz = sizeof name; RegQueryValueExA(k, "ProductName", NULL, NULL, (BYTE *)name, &sz);
          sz = sizeof disp; RegQueryValueExA(k, "DisplayVersion", NULL, NULL, (BYTE *)disp, &sz);
          sz = sizeof ubr; RegQueryValueExA(k, "UBR", NULL, NULL, (BYTE *)&ubr, &sz);
          /* ProductName says "Windows 10" on Windows 11 too; build 22000 and later is 11 */
          if (atoi(build) >= 22000 && !strncmp(name, "Windows 10", 10)) name[9] = '1';
          snprintf(os, sizeof os, "%s %s (%s.%lu)", name, disp, build, (unsigned long)ubr);
          RegCloseKey(k); } }
    memset(&dev, 0, sizeof dev);
    dev.cb = sizeof dev;
    /* Every GPU, the one driving the main screen marked: on a laptop that is the
     * integrated one even when the game renders on the discrete one (the
     * [CS-D3D11] "back end up on" line names the GPU the game uses). */
    {
        char primary[128] = "";
        for (i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++)
            if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { snprintf(primary, sizeof primary, "%s", dev.DeviceString); break; }
        for (i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++) {
            size_t used = strlen(gpu);
            if (!dev.DeviceString[0] || strstr(gpu, dev.DeviceString)) continue;     /* one entry per GPU */
            snprintf(gpu + used, sizeof gpu - used, "%s%s%s", used ? ", " : "", dev.DeviceString,
                     !strcmp(dev.DeviceString, primary) ? " (screen)" : "");
        }
        if (!gpu[0]) strcpy(gpu, "?");
    }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    { SYSTEM_INFO si; GetSystemInfo(&si);
      putf("[BUILD] cs2_xbox_recomp %s, symbols %s | %s | %s, %lu logical CPUs | GPUs %s | %llu GB\n", ver, pdb, os, cpu,
           (unsigned long)si.dwNumberOfProcessors, gpu, (unsigned long long)((ms.ullTotalPhys + (1ull << 29)) >> 30)); }
}

/* ---- [VIDEO] ------------------------------------------------------------------------------------------------------- */
static void video_line(const char *why)
{
    const CsSettings *c = &g_cs_settings;
    HWND h = (HWND)xbox_FramebufferWindowHwnd();
    RECT r = { 0, 0, 0, 0 };
    DEVMODEA dm;
    SYSTEMTIME st;
    if (h) GetClientRect(h, &r);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) dm.dmDisplayFrequency = 0;
    GetLocalTime(&st);
    putf("[VIDEO] %02d:%02d:%02d %s: render scale %dx, msaa %d, anisotropy %d, vsync %d, frame cap %d, stretch %d, "
         "fullscreen %d, hi-res textures %d | window %ldx%ld, monitor %lu Hz\n", st.wHour, st.wMinute, st.wSecond, why,
         c->scale, c->msaa, c->aniso, c->vsync, c->frame_rate, c->stretch, xbox_FramebufferIsFullscreen(), c->hires,
         (long)r.right, (long)r.bottom, (unsigned long)dm.dmDisplayFrequency);
}

void cs_diag_log_video(const char *why) { video_line(why); }

/* ---- [FPS] --------------------------------------------------------------------------------------------------------- */
/* Frame times in 0.25 ms bins up to 250 ms, the rest in the last bin; the longest kept exactly. A minute's summary is
 * printed from the render thread when the minute is up, so nothing is shared with another thread. A gap of over 5 s
 * (a freeze, which has its own report, or the window held) starts a new minute instead of counting. */
#define FPS_BINS 1001
#define FPS_WINDOW_S 60

void cs_diag_frame(void)
{
    static LARGE_INTEGER f, last, start;
    static unsigned hist[FPS_BINS], frames, over50, over100;
    static double longest, total;
    LARGE_INTEGER now;
    double ms;

    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    if (!last.QuadPart || now.QuadPart - last.QuadPart > f.QuadPart * 5) {
        memset(hist, 0, sizeof hist);
        frames = over50 = over100 = 0;
        longest = total = 0;
        last = start = now;
        return;
    }
    ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)f.QuadPart;
    last = now;
    hist[ms * 4 < FPS_BINS - 1 ? (unsigned)(ms * 4) : FPS_BINS - 1]++;
    frames++;
    total += ms;
    if (ms > longest) longest = ms;
    if (ms > 50) over50++;
    if (ms > 100) over100++;

    if (now.QuadPart - start.QuadPart >= f.QuadPart * FPS_WINDOW_S && frames) {
        /* 1% low: the frame time that the slowest 1% of frames reach, as a rate */
        unsigned want = frames / 100 ? frames / 100 : 1, seen = 0;
        int b;
        double low_ms = longest;
        for (b = FPS_BINS - 1; b >= 0; b--) {
            seen += hist[b];
            if (seen >= want) { low_ms = b == FPS_BINS - 1 ? longest : (b + 1) / 4.0; break; }
        }
        if (low_ms > longest) low_ms = longest;
        putf("[FPS] last %.0f s: %.1f fps average, 1%% low %.1f fps, longest frame %.1f ms, %u frame(s) over 50 ms, "
             "%u over 100 ms (cap %d, vsync %d)\n", total / 1000.0, frames * 1000.0 / total,
             low_ms > 0 ? 1000.0 / low_ms : 0.0, longest, over50, over100, cs_d3d11_pace_hz(),
             g_cs_settings.vsync);
        memset(hist, 0, sizeof hist);
        frames = over50 = over100 = 0;
        longest = total = 0;
        start = now;
    }
}

/* ---- [FREEZE] ------------------------------------------------------------------------------------------------------ */
static ULONGLONG s_t0;

static void dump_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    BOOL ok;

    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    for (ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        DWORD64 pcs[MAX_FRAMES];
        CONTEXT ctx;
        HANDLE th;
        int n = 0;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || te.th32ThreadID == s_tee_tid) continue;
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx))
                n = unwind(&ctx, pcs, MAX_FRAMES);
            ResumeThread(th);
        }
        CloseHandle(th);
        if (n) put_stack("[FREEZE]", te.th32ThreadID, pcs, n);
    }
    CloseHandle(snap);
}

static void freeze_report(unsigned secs, unsigned frames, int verbose)
{
    putf("[FREEZE] no new frame for %u s (frames so far %u, %lu s since start)\n", secs, frames,
         (unsigned long)((GetTickCount64() - s_t0) / 1000));
    cs_diag_build_line();
    video_line("at the freeze");
    dump_threads();
    (void)verbose;
    put("[FREEZE] end of report\n");
    cs_diag_flush();
}

static unsigned s_freeze_secs;
static int s_verbose;

static DWORD WINAPI watch_thread(LPVOID p)
{
    unsigned last = cs_d3d11_frames();
    unsigned still = 0, reports = 0, total = 0;
    int video_logged = 0;
    (void)p;
    for (;;) {
        unsigned now;
        Sleep(1000);
        now = cs_d3d11_frames();
        if (now && !video_logged) {             /* once: the settings in use when the game is up */
            video_line("start");
            video_logged = 1;
        }
        if (now != last || now == 0) {
            if (reports && now != last)
                putf("[FREEZE] frames are moving again after %u s\n", still);
            last = now; still = 0; reports = 0;
            continue;
        }
        still++;
        if (s_verbose) {                            /* developer builds: RECOMP_STALL_DUMP, every N seconds */
            if (still % s_freeze_secs == 0) freeze_report(still, now, 1);
        } else if ((still == s_freeze_secs || still == s_freeze_secs + 20) && total < 6) {
            freeze_report(still, now, 0);
            reports++; total++;
        }
    }
}

void cs_diag_start(unsigned freeze_secs, int verbose)
{
    s_t0 = GetTickCount64();
    if (!freeze_secs) return;
    s_freeze_secs = freeze_secs;
    s_verbose = verbose;
    CloseHandle(CreateThread(NULL, 0, watch_thread, NULL, 0, &s_watch_tid));
}

/* ---- [CRASH] ------------------------------------------------------------------------------------------------------- */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG once;
    DWORD64 pcs[MAX_FRAMES];
    CONTEXT ctx;
    char where[200];
    int n;

    if (InterlockedExchange(&once, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    cs_diag_where(ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    putf("[CRASH] unhandled exception 0x%08lX at %s\n", (unsigned long)ep->ExceptionRecord->ExceptionCode, where);
    ctx = *ep->ContextRecord;
    n = unwind(&ctx, pcs, MAX_FRAMES);
    put_stack("[CRASH]", GetCurrentThreadId(), pcs, n);
    cs_diag_build_line();
    video_line("at the crash");
    cs_diag_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void cs_diag_install_crash_filter(void)
{
    SetUnhandledExceptionFilter(crash_filter);
}
