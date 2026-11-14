/* cs_play: starts the game and stays until the game has really ended, for RecompLauncher.
 *
 * Counter-Strike reboots into itself to go from the menus to a match and on every map change
 * (src/cs_launch.c: XLaunchNewImage -> a fresh cs2_xbox_recomp.exe, the old one exits). A launcher
 * that watches the process it started would see the game "end" at the first map load. This starts
 * the game in a job object, waits until no process is left in it, and exits with the exit code of
 * the last one to end, so the launcher sees one session and a crash is still a crash. Everything it
 * was given (arguments, environment, stdout/stderr) is passed on unchanged.
 *
 *   cs_play.exe [arguments for the game]
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WCHAR exe[MAX_PATH], *slash, *line, *args;
    WCHAR *cmdline;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE job, port;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    DWORD code = 1, msg;
    ULONG_PTR key;
    LPOVERLAPPED ov;
    size_t n;
    int in_quote = 0;
    (void)inst; (void)prev; (void)cmd; (void)show;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH) || !(slash = wcsrchr(exe, L'\\')))
        return 1;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - exe), L"cs2_xbox_recomp.exe");
    /* the game's own command line: its path, then whatever followed ours */
    line = GetCommandLineW();
    for (args = line; *args && (in_quote || (*args != L' ' && *args != L'\t')); args++)
        if (*args == L'"') in_quote = !in_quote;
    n = wcslen(exe) + wcslen(args) + 4;
    cmdline = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
    if (!cmdline) return 1;
    swprintf_s(cmdline, n, L"\"%s\"%s", exe, args);

    job = CreateJobObjectW(NULL, NULL);
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!job || !port) return 1;
    acp.CompletionKey = job;
    acp.CompletionPort = port;
    SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &acp, sizeof acp);
    /* if this helper is ended (the launcher's Stop), the game goes with it */
    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    *slash = 0;                                       /* the game runs in its own folder */
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, exe, &si, &pi))
        return 1;
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* A handle on each process as it joins (an ended process's exit code can only be read through
     * one), its code when it ends, and done when none is left: the last code is the session's. */
    {
        enum { MAXP = 64 };
        DWORD pid[MAXP];
        HANDLE ph[MAXP];
        int np = 0, i;
        while (GetQueuedCompletionStatus(port, &msg, &key, &ov, INFINITE)) {
            DWORD id = (DWORD)(ULONG_PTR)ov;
            if (key != (ULONG_PTR)job) continue;
            if (msg == JOB_OBJECT_MSG_NEW_PROCESS) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id);
                if (h) {
                    if (np == MAXP) { CloseHandle(ph[0]); memmove(ph, ph + 1, (MAXP - 1) * sizeof *ph);
                                      memmove(pid, pid + 1, (MAXP - 1) * sizeof *pid); np--; }
                    pid[np] = id; ph[np] = h; np++;
                }
            } else if (msg == JOB_OBJECT_MSG_EXIT_PROCESS || msg == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) {
                for (i = 0; i < np; i++)
                    if (pid[i] == id) {
                        DWORD c;
                        WaitForSingleObject(ph[i], 5000);
                        if (GetExitCodeProcess(ph[i], &c)) code = c;
                        CloseHandle(ph[i]);
                        ph[i] = ph[np - 1]; pid[i] = pid[np - 1]; np--;
                        break;
                    }
            } else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
                break;
            }
        }
    }
    return (int)code;
}
