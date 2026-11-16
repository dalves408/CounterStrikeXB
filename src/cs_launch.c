/*
 * cs_launch.c -- Counter-Strike: XLaunchNewImage back into itself.
 *
 * Hand-written CS code. Generated code untouched, no game state written.
 *
 * What the title does (runs/run_20261111_110459, David: Main Menu -> Training
 * -> Hostage Rescue): the menu front end ("SuperLauncher") does not load a map
 * in the same process. It fills the XAPI launch data page and calls
 * XLaunchNewImage("d:\\default.xbe") (it first probes the dev name
 * d:\\-xbox_cs-f.xbe, strings at 0x00329A88), i.e. it reboots into ITSELF with
 * launch data describing the game to start. XAPI then calls
 * HalReturnToFirmware(HalQuickRebootRoutine = 2). Log:
 *   [KERNEL] launch data page ...: type=0 titleid=0x4D530036
 *            path='\Device\CdRom0;default.xbe'
 *   [KERNEL] HalReturnToFirmware: routine=2 - title is exiting
 * The toolkit bridge treats every routine as "quit" and ExitProcess()es, so
 * the window just closed: that is the "crash after the menus".
 *
 * On hardware the kernel keeps the launch data page across the quick reboot,
 * loads the named XBE and points LaunchDataPage (ordinal 164) at that page,
 * where the new instance's XGetLaunchInfo finds it.
 *
 * Host model of that: when the title quick-reboots into this same XBE, save the
 * 4 KB page to a temp file, start a fresh copy of this executable (same command
 * line, same std handles, same environment plus CS_LAUNCH_PAGE=<file> and
 * CS_LAUNCH_PARENT=<pid>), then let the toolkit bridge finish its exit as
 * before. The child waits for the parent to be gone, then (cs_launch_restore,
 * called by main.c after xbox_kernel_bridge_init) puts the page back into guest
 * memory and points KDATA LaunchDataPage at it, exactly where the kernel export
 * reads it. A fresh process is the honest equivalent of a reboot: all guest
 * memory, threads and device state start clean. Any other target (dashboard,
 * other XBE) keeps the toolkit's exit.
 *
 * If CS_RUN_DIR is set (scripts/run.ps1), the child's PID is appended to
 * <CS_RUN_DIR>\pid.txt so bounded runs can stop the whole chain.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

/* Toolkit kernel-data layout (tools/xboxrecomp/src/kernel/xbox_memory_layout.h),
 * declared here to keep its headers apart from recomp_types.h. */
extern uint32_t g_xbox_region_base;
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
#define CS_KDATA_LAUNCH_DATA_PAGE (g_xbox_region_base + 0x00040000u + 0x030u)
#include "cs_launch.h"
#include "cs_diag.h"

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);   /* kernel_bridge.c */

#define CS_THUNK_HALRETURNTOFIRMWARE 0x003211FCu   /* title update (disc: 0x003211BC) */   /* ordinal 49 (analysis json) */
#define CS_LAUNCH_PAGE_SIZE 0x1000u
#define CS_SELF_PATH_TAIL ";default.xbe"

static uint32_t s_hal_va;
static int s_from_parent;
static int s_live;
#define CS_LAUNCH_MODE_LIVE 5u

static int cs_is_self_launch(uint32_t page)
{
    char path[521];
    size_t i, n, t = strlen(CS_SELF_PATH_TAIL);
    for (i = 0; i < 520; i++) {
        path[i] = (char)MEM8(page + 8 + (uint32_t)i);
        if (!path[i]) break;
    }
    path[i] = 0;
    n = strlen(path);
    if (MEM32(page + 4) != 0x4D530036u) return 0;          /* our title id */
    if (_strnicmp(path, "\\Device\\CdRom0", 14) != 0) return 0;
    return n >= t && _stricmp(path + n - t, CS_SELF_PATH_TAIL) == 0;
}

static void cs_log_launch_data(uint32_t page)
{
    uint32_t i, last = 0;
    for (i = 0; i < 3072; i++)
        if (MEM8(page + 0x400 + i)) last = i + 1;
    fprintf(stderr, "[CS2-LAUNCH] launch data: %u significant byte(s):", last);
    for (i = 0; i < last && i < 256; i++) {
        if (i % 32 == 0) fprintf(stderr, "\n[CS2-LAUNCH]   +%03X", i);
        fprintf(stderr, " %02X", MEM8(page + 0x400 + i));
    }
    fprintf(stderr, "\n[CS2-LAUNCH]   ascii: ");
    for (i = 0; i < last && i < 512; i++) {
        uint8_t c = MEM8(page + 0x400 + i);
        fputc(c >= 0x20 && c < 0x7F ? c : '.', stderr);
    }
    fprintf(stderr, "\n");
}

static int cs_relaunch_with(const uint8_t *buf);

static int cs_relaunch_self(uint32_t page)
{
    uint8_t buf[CS_LAUNCH_PAGE_SIZE];
    uint32_t i;
    for (i = 0; i < CS_LAUNCH_PAGE_SIZE; i++)
        buf[i] = MEM8(page + i);
    return cs_relaunch_with(buf);
}

/* The title asked for the Dashboard (LDT_LAUNCH_DASHBOARD): Xbox Live's "New
 * Account" and account management live there, not in the game, and there is no
 * Dashboard here -- the request used to end in the toolkit's exit, and the game
 * closed. Answered as a Dashboard does when the player backs out: it boots the
 * title again with LDT_FROM_DASHBOARD and the title's own context (LD_FROM_DASHBOARD
 * .dwContext <- LD_LAUNCH_DASHBOARD.dwContext). Through the same relaunch as a map
 * change, so it stays in the frame window. */
#define CS_LDT_LAUNCH_DASHBOARD 1u
#define CS_LDT_FROM_DASHBOARD   2u

static int cs_back_from_dashboard(uint32_t page)
{
    static const char self_path[] = "\\Device\\CdRom0;default.xbe";
    uint8_t buf[CS_LAUNCH_PAGE_SIZE];
    uint32_t reason = MEM32(page + 0x400), context = MEM32(page + 0x404);

    memset(buf, 0, sizeof buf);
    *(uint32_t *)(buf + 0) = CS_LDT_FROM_DASHBOARD;
    *(uint32_t *)(buf + 4) = MEM32(page + 4);              /* the title */
    memcpy(buf + 8, self_path, sizeof self_path);
    *(uint32_t *)(buf + 0x400) = context;                  /* LD_FROM_DASHBOARD.dwContext */
    fprintf(stderr, "[CS2-LAUNCH] Dashboard requested (reason %u, context 0x%08X): there is none here;"
                    " back to the game as from the Dashboard\n", reason, context);
    return cs_relaunch_with(buf);
}

static int cs_relaunch_with(const uint8_t *buf)
{
    char tmp[MAX_PATH], file[MAX_PATH], exe[MAX_PATH], pid[16];
    FILE *f;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    if (!GetTempPathA(sizeof(tmp), tmp)) return 0;
    _snprintf(file, sizeof(file), "%scs2_launch_%lu.bin", tmp,
              (unsigned long)GetCurrentProcessId());
    file[sizeof(file) - 1] = 0;
    f = fopen(file, "wb");
    if (!f) return 0;
    ok = fwrite(buf, 1, CS_LAUNCH_PAGE_SIZE, f) == CS_LAUNCH_PAGE_SIZE;
    fclose(f);
    if (!ok) return 0;
    {   /* Diagnostics: CS_LAUNCH_SAVE=<file> keeps a copy, so a later run can
         * start straight into the game with -ExtraEnv CS_LAUNCH_PAGE=<copy>. */
        const char *keep = getenv("CS_LAUNCH_SAVE");
        if (keep && *keep)
            CopyFileA(file, keep, FALSE);
    }

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return 0;
    _snprintf(pid, sizeof(pid), "%lu", (unsigned long)GetCurrentProcessId());
    pid[sizeof(pid) - 1] = 0;
    SetEnvironmentVariableA("CS_LAUNCH_PAGE", file);
    SetEnvironmentVariableA("CS_LAUNCH_PARENT", pid);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fflush(stdout);
    fflush(stderr);
    ok = CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL,
                        NULL /* same cwd */, &si, &pi);
    SetEnvironmentVariableA("CS_LAUNCH_PAGE", NULL);
    SetEnvironmentVariableA("CS_LAUNCH_PARENT", NULL);
    if (!ok) {
        fprintf(stderr, "[CS2-LAUNCH] CreateProcess failed (%lu)\n", GetLastError());
        DeleteFileA(file);
        return 0;
    }
    fprintf(stderr, "[CS2-LAUNCH] quick reboot into default.xbe: started PID %lu"
                    " with launch page %s\n", (unsigned long)pi.dwProcessId, file);
    {
        const char *rd = getenv("CS_RUN_DIR");
        if (rd && *rd) {
            char pf[MAX_PATH];
            _snprintf(pf, sizeof(pf), "%s\\pid.txt", rd);
            pf[sizeof(pf) - 1] = 0;
            f = fopen(pf, "a");
            if (f) { fprintf(f, "%lu\n", (unsigned long)pi.dwProcessId); fclose(f); }
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static void cs_HalReturnToFirmware(void)
{
    uint32_t routine = MEM32(g_esp + 4);
    uint32_t page = MEM32(CS_KDATA_LAUNCH_DATA_PAGE);
    recomp_func_t bridge = recomp_lookup_kernel(s_hal_va);

    if (routine == 2 && page && cs_is_self_launch(page)) {
        cs_log_launch_data(page);
        if (!cs_relaunch_self(page))
            fprintf(stderr, "[CS2-LAUNCH] relaunch failed; exiting as before\n");
    } else if (routine == 2 && page && MEM32(page) == CS_LDT_LAUNCH_DASHBOARD && MEM32(page + 4) == 0x4D530036u) {
        if (!cs_back_from_dashboard(page))
            fprintf(stderr, "[CS2-LAUNCH] relaunch failed; exiting as before\n");
    } else if (page) {
        char path[521];
        uint32_t i;
        for (i = 0; i < 520; i++) {
            path[i] = (char)MEM8(page + 8 + i);
            if (!path[i]) break;
        }
        path[i] = 0;
        fprintf(stderr, "[CS2-LAUNCH] firmware return %u to another image: type %u title 0x%08X path '%s'\n",
                routine, MEM32(page), MEM32(page + 4), path);
        cs_log_launch_data(page);
        /* The disc's other programs, downloader.xbe (Xbox Live > Downloadable
         * Content) and update.xbe (Xbox Live's title updater), are not
         * translated: the game closed when it handed off to one. Come back to
         * the game instead, as from the Dashboard. The downloadable maps are
         * unlocked already (cs_content.c) and the game runs as the update. */
        if (routine == 2 && MEM32(page + 4) == 0x4D530036u &&
            (strstr(path, "downloader.xbe") || strstr(path, "update.xbe"))) {
            uint8_t buf[CS_LAUNCH_PAGE_SIZE];
            static const char self_path[] = "\\Device\\CdRom0;default.xbe";
            memset(buf, 0, sizeof buf);
            *(uint32_t *)(buf + 0) = CS_LDT_FROM_DASHBOARD;
            *(uint32_t *)(buf + 4) = 0x4D530036u;
            memcpy(buf + 8, self_path, sizeof self_path);
            fprintf(stderr, "[CS2-LAUNCH] %s is not part of this program: back to the game\n",
                    strstr(path, "downloader.xbe") ? "downloader.xbe" : "update.xbe");
            if (!cs_relaunch_with(buf))
                fprintf(stderr, "[CS2-LAUNCH] relaunch failed; exiting as before\n");
        }
    }
    cs_diag_flush();       /* the bridge's ExitProcess would cut the log's last lines */
    if (bridge)
        bridge();          /* toolkit: logs, dumps, ExitProcess */
}

recomp_func_t cs_launch_override(uint32_t xbox_va)
{
    if (!s_hal_va)
        s_hal_va = MEM32(CS_THUNK_HALRETURNTOFIRMWARE);
    if (xbox_va == s_hal_va)
        return cs_HalReturnToFirmware;
    return (recomp_func_t)0;
}

void cs_launch_restore(void)
{
    char file[MAX_PATH];
    const char *pp;
    uint8_t buf[CS_LAUNCH_PAGE_SIZE];
    uint32_t page, i;
    FILE *f;
    size_t got;

    if (!GetEnvironmentVariableA("CS_LAUNCH_PAGE", file, sizeof(file)))
        return;
    pp = getenv("CS_LAUNCH_PARENT");
    s_from_parent = pp && *pp;   /* only a hand-over file is consumed */
    if (pp && *pp) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, strtoul(pp, NULL, 10));
        if (h) {
            if (WaitForSingleObject(h, 15000) != WAIT_OBJECT_0)
                fprintf(stderr, "[CS2-LAUNCH] parent %s still running after 15 s\n", pp);
            CloseHandle(h);
        }
    }
    /* Not inherited by a later relaunch from this instance. */
    SetEnvironmentVariableA("CS_LAUNCH_PAGE", NULL);
    SetEnvironmentVariableA("CS_LAUNCH_PARENT", NULL);
    _putenv("CS_LAUNCH_PAGE=");
    _putenv("CS_LAUNCH_PARENT=");

    f = fopen(file, "rb");
    if (!f) {
        fprintf(stderr, "[CS2-LAUNCH] cannot open launch page %s\n", file);
        return;
    }
    got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (s_from_parent)
        DeleteFileA(file);
    if (got != sizeof(buf)) {
        fprintf(stderr, "[CS2-LAUNCH] short launch page %s (%zu)\n", file, got);
        return;
    }
    page = xbox_HeapAlloc(CS_LAUNCH_PAGE_SIZE, 4096);
    if (!page) {
        fprintf(stderr, "[CS2-LAUNCH] no memory for the launch page\n");
        return;
    }
    for (i = 0; i < CS_LAUNCH_PAGE_SIZE; i++)
        MEM8(page + i) = buf[i];
    MEM32(CS_KDATA_LAUNCH_DATA_PAGE) = page;
    fprintf(stderr, "[CS2-LAUNCH] relaunched with launch page at 0x%08X"
                    " (type %u, title 0x%08X)\n", page, MEM32(page), MEM32(page + 4));
    cs_log_launch_data(page);
    /* The game's own launch data names the match it reboots into: its first word is 5 for an Xbox Live match (4
     * System Link, 1 back to the menus), followed by the Live session. Read, never written. */
    s_live = MEM32(page + 4) == 0x4D530036u && MEM32(page + 0x400) == CS_LAUNCH_MODE_LIVE;
    if (s_live)
        fprintf(stderr, "[CS2-LAUNCH] an Xbox Live match\n");
}

int cs_launch_xbox_live_match(void) { return s_live; }
