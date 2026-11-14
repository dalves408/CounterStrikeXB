/* ---- Report a problem (included by config.c) --------------------------------------------------------------------
 * Opens the logs folder with the newest log selected, and a new GitHub issue filled in with the game version and
 * the PC (as in the Marvel vs. Capcom 2 recompilation's settings program). The player describes the problem and
 * drags the log onto the page (GitHub needs a free account); nothing is sent by this program. RecompLauncher's
 * own report button sends the same log without either. */
#define REPORT_REPO "dalves408/CounterStrikeXB"

static void url_append(char *out, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlen(out);
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void reg_str(HKEY root, const char *key, const char *name, char *out, DWORD n)
{
    HKEY k;
    if (!RegOpenKeyExA(root, key, 0, KEY_READ, &k)) {
        if (RegQueryValueExA(k, name, NULL, NULL, (BYTE *)out, &n)) out[0] = 0;
        RegCloseKey(k);
    }
}

static void do_report(HWND h)
{
    char logs[MAX_PATH], newest[MAX_PATH] = "", pat[MAX_PATH], exe[MAX_PATH], *slash;
    char ver[32] = "?", cpu[96] = "?", gpu[128] = "?", osname[64] = "Windows", disp[16] = "", build[16] = "";
    char body[2048], url[8192], arg[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    DISPLAY_DEVICEA dev;
    MEMORYSTATUSEX ms;
    DWORD i, sz, hnd;

    if (!cs_settings_path("logs", logs, sizeof logs, 1)) strcpy(logs, ".");
    CreateDirectoryA(logs, NULL);
    snprintf(pat, sizeof pat, "%s\\cs-*.log", logs);
    if ((f = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do { if (strcmp(fd.cFileName, newest) > 0) snprintf(newest, sizeof newest, "%s", fd.cFileName); }
        while (FindNextFileA(f, &fd));
        FindClose(f);
    }

    /* the game's version, from cs2_xbox_recomp.exe beside this program */
    if (GetModuleFileNameA(NULL, exe, sizeof exe) && (slash = strrchr(exe, '\\')) != NULL) {
        strcpy(slash + 1, "cs2_xbox_recomp.exe");
        if ((sz = GetFileVersionInfoSizeA(exe, &hnd)) != 0) {
            void *vi = malloc(sz);
            VS_FIXEDFILEINFO *ff;
            UINT len;
            if (vi && GetFileVersionInfoA(exe, 0, sz, vi) && VerQueryValueA(vi, "\\", (void **)&ff, &len) && ff)
                snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(ff->dwFileVersionMS), LOWORD(ff->dwFileVersionMS),
                         HIWORD(ff->dwFileVersionLS));
            free(vi);
        }
    }
    reg_str(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", cpu, sizeof cpu);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", osname, sizeof osname);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", disp, sizeof disp);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", build, sizeof build);
    if (atoi(build) >= 22000 && !strncmp(osname, "Windows 10", 10)) osname[9] = '1';   /* 11 still says "10" there */
    memset(&dev, 0, sizeof dev);
    dev.cb = sizeof dev;
    for (i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++)
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { snprintf(gpu, sizeof gpu, "%s", dev.DeviceString); break; }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);

    snprintf(body, sizeof body,
             "**What happened?**\n\n\n"
             "**What were you doing just before?** (menus, a match, which map, a map change, changing settings...)\n\n\n"
             "**Log:** drag the selected file from the logs folder that just opened (%s) into this box.\n\n"
             "---\nGame %s | %s %s (build %s) | %s | %s | %llu GB\n",
             newest[0] ? newest : "cs-....log", ver, osname, disp, build, cpu, gpu,
             (unsigned long long)((ms.ullTotalPhys + (1ull << 29)) >> 30));
    strcpy(url, "https://github.com/" REPORT_REPO "/issues/new?title=");
    url_append(url, sizeof url, "Problem: ");
    strcat(url, "&body=");
    url_append(url, sizeof url, body);

    if (newest[0]) {
        snprintf(arg, sizeof arg, "/select,\"%s\\%s\"", logs, newest);
        ShellExecuteA(h, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteA(h, "open", logs, NULL, NULL, SW_SHOWNORMAL);
    }
    ShellExecuteA(h, "open", url, NULL, NULL, SW_SHOWNORMAL);
    MessageBoxA(h, newest[0]
                ? "Two windows opened:\n\n"
                  "  - your logs folder, with the newest log selected\n"
                  "  - a new problem report on GitHub (it needs a free GitHub account)\n\n"
                  "Say what happened, then drag the selected log file onto the report and submit it.\n\n"
                  "The game makes a new log each time it starts and on every map change; the newest is the last one. "
                  "Report right after the problem. Logs hold no personal information: your Windows user name is blanked out.\n\n"
                  "Using RecompLauncher? Its own report button sends the log for you."
                : "Your logs folder and a new problem report on GitHub opened. There is no log yet: play until the problem "
                  "happens, then click Report a problem again.",
                "Counter-Strike", MB_ICONINFORMATION);
    status("The logs folder and the problem report page opened.");
}
