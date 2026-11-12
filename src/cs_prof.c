/*
 * cs_prof.c -- in-process diagnostics: dispatch-gate watchdog and a sampling
 * profiler. Hand-written CS code; reads only host thread state, never guest
 * memory, and does nothing unless its environment variable is set.
 *
 *   RECOMP_CS_GATE_WATCH=1        When one thread holds the toolkit's one-CPU
 *                                 dispatch gate for more than 100 ms
 *                                 (RECOMP_CS_GATE_WATCH_MS overrides), print that
 *                                 thread's call stack, symbolized from the PDB,
 *                                 once per hold. Interrupts posted to a holder run
 *                                 only at its safe points, so a long hold is a
 *                                 long interrupt delay (stalled movies, flips).
 *   RECOMP_CS_PROF=<from>-<to>    Sample every thread about every millisecond
 *                                 between those seconds after start and print
 *                                 the hottest functions and call chains per
 *                                 thread at <to> (stderr, [PROF] lines).
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_prof.h"

extern unsigned long xbox_DispatchGateOwner(void);   /* kernel_hal.c */

#define DEPTH 6

static CRITICAL_SECTION s_sym_lock;
static int s_sym_ready;

static void sym_init(void)
{
    if (s_sym_ready) return;
    InitializeCriticalSection(&s_sym_lock);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    s_sym_ready = 1;
}

static void sym_name(DWORD64 pc, char *out, size_t n)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    EnterCriticalSection(&s_sym_lock);
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, si))
        snprintf(out, n, "%s+0x%llX", si->Name, (unsigned long long)disp);
    else
        snprintf(out, n, "%016llX", (unsigned long long)pc);
    LeaveCriticalSection(&s_sym_lock);
}

/* Function name only (no offset), for aggregation. */
static void sym_func(DWORD64 pc, char *out, size_t n)
{
    char *plus;
    sym_name(pc, out, n);
    plus = strstr(out, "+0x");
    if (plus) *plus = 0;
}

/* Walk a suspended thread's stack with the x64 unwind tables. */
static int capture(HANDLE th, DWORD64 *pcs, int max)
{
    CONTEXT c;
    int n = 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) return 0;
    while (n < max && c.Rip) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION rf;
        pcs[n++] = c.Rip;
        rf = RtlLookupFunctionEntry(c.Rip, &image, NULL);
        if (!rf) {                         /* leaf function: return address on top */
            if (!c.Rsp) break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        } else {
            void *handler = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, rf, &c, &handler, &frame, NULL);
        }
    }
    return n;
}

/* ---- dispatch-gate watchdog ------------------------------------------------ */

static DWORD WINAPI gate_watch(LPVOID p)
{
    DWORD limit = (DWORD)(uintptr_t)p;
    unsigned long owner = 0, reported = 0;
    DWORD since = 0;
    unsigned episodes = 0;
    for (;;) {
        unsigned long now_owner = xbox_DispatchGateOwner();
        DWORD now = GetTickCount();
        if (now_owner != owner) { owner = now_owner; since = now; reported = 0; }
        else if (owner && !reported && now - since >= limit && episodes < 200) {
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, owner);
            reported = 1;
            if (th) {
                DWORD64 pcs[16];
                int n, i;
                if (SuspendThread(th) != (DWORD)-1) {
                    n = capture(th, pcs, 16);
                    ResumeThread(th);
                    episodes++;
                    fprintf(stderr, "[GATE] tid %lu has held the dispatch gate %lu ms; stack:\n", owner, now - since);
                    for (i = 0; i < n; i++) {
                        char nm[300];
                        sym_name(pcs[i], nm, sizeof nm);
                        fprintf(stderr, "[GATE]   #%d %s\n", i, nm);
                    }
                    fflush(stderr);
                }
                CloseHandle(th);
            }
        }
        Sleep(5);
    }
    return 0;
}

/* ---- sampling profiler ---------------------------------------------------------- */

typedef struct { DWORD tid; DWORD64 pc[DEPTH]; uint8_t n; } Sample;

typedef struct { char key[600]; DWORD tid; unsigned count; } Agg;

static int agg_cmp(const void *a, const void *b)
{
    const Agg *x = (const Agg *)a, *y = (const Agg *)b;
    if (x->tid != y->tid) return x->tid < y->tid ? -1 : 1;
    return (int)y->count - (int)x->count;
}

static void report(Sample *s, size_t ns, DWORD self)
{
    /* Two views per thread: leaf function, and leaf <- caller <- caller. */
    Agg *leaf = (Agg *)calloc(ns ? ns : 1, sizeof(Agg));
    Agg *chain = (Agg *)calloc(ns ? ns : 1, sizeof(Agg));
    size_t nl = 0, nc = 0, i, j;
    DWORD tids[256];
    unsigned tcount[256];
    int nt = 0, t;
    if (!leaf || !chain) return;
    for (i = 0; i < ns; i++) {
        char k1[300], k2[600], f[200];
        int d;
        if (s[i].tid == self) continue;
        sym_func(s[i].pc[0], k1, sizeof k1);
        k2[0] = 0;
        for (d = 0; d < s[i].n && d < 3; d++) {
            sym_func(s[i].pc[d], f, sizeof f);
            if (d) strncat(k2, " <- ", sizeof k2 - strlen(k2) - 1);
            strncat(k2, f, sizeof k2 - strlen(k2) - 1);
        }
        for (j = 0; j < nl; j++) if (leaf[j].tid == s[i].tid && !strcmp(leaf[j].key, k1)) break;
        if (j == nl) { snprintf(leaf[nl].key, sizeof leaf[nl].key, "%s", k1); leaf[nl].tid = s[i].tid; nl++; }
        leaf[j].count++;
        for (j = 0; j < nc; j++) if (chain[j].tid == s[i].tid && !strcmp(chain[j].key, k2)) break;
        if (j == nc) { snprintf(chain[nc].key, sizeof chain[nc].key, "%s", k2); chain[nc].tid = s[i].tid; nc++; }
        chain[j].count++;
        for (t = 0; t < nt && tids[t] != s[i].tid; t++) ;
        if (t == nt && nt < 256) { tids[nt] = s[i].tid; tcount[nt] = 0; nt++; }
        if (t < 256) tcount[t]++;
    }
    qsort(leaf, nl, sizeof(Agg), agg_cmp);
    qsort(chain, nc, sizeof(Agg), agg_cmp);
    fprintf(stderr, "[PROF] %zu samples over %d threads\n", ns, nt);
    for (t = 0; t < nt; t++) {
        unsigned shown = 0;
        if (tcount[t] < ns / 200) continue;           /* threads with under 0.5% of samples */
        fprintf(stderr, "[PROF] thread %lu: %u samples\n", tids[t], tcount[t]);
        for (j = 0; j < nl && shown < 12; j++)
            if (leaf[j].tid == tids[t]) {
                fprintf(stderr, "[PROF]   %5.1f%%  %s\n", 100.0 * leaf[j].count / tcount[t], leaf[j].key);
                shown++;
            }
        shown = 0;
        for (j = 0; j < nc && shown < 8; j++)
            if (chain[j].tid == tids[t]) {
                fprintf(stderr, "[PROF]     chain %5.1f%%  %s\n", 100.0 * chain[j].count / tcount[t], chain[j].key);
                shown++;
            }
    }
    fflush(stderr);
    free(leaf); free(chain);
}

static DWORD WINAPI prof_thread(LPVOID p)
{
    const char *spec = (const char *)p;
    double from = 0, to = 0;
    DWORD t0 = GetTickCount(), self = GetCurrentThreadId();
    size_t cap = 4u << 20, ns = 0;
    Sample *s;
    HANDLE ths[256];
    DWORD tid_of[256], list_tick = 0;
    int nth = 0;
    if (sscanf(spec, "%lf-%lf", &from, &to) != 2 || to <= from) {
        fprintf(stderr, "[PROF] RECOMP_CS_PROF wants <from>-<to> seconds\n");
        return 0;
    }
    s = (Sample *)malloc(cap * sizeof(Sample));
    if (!s) return 0;
    timeBeginPeriod(1);
    while (GetTickCount() - t0 < (DWORD)(from * 1000)) Sleep(10);
    fprintf(stderr, "[PROF] sampling %.0f-%.0f s\n", from, to);
    while (GetTickCount() - t0 < (DWORD)(to * 1000) && ns < cap) {
        /* The thread list is refreshed twice a second; between refreshes the
         * open handles are reused, so a round costs only suspend/context/resume. */
        int k;
        if (GetTickCount() - list_tick >= 500) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            for (k = 0; k < nth; k++) CloseHandle(ths[k]);
            nth = 0;
            te.dwSize = sizeof te;
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
                do {
                    HANDLE th;
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
                    th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (th && nth < 256) { ths[nth] = th; tid_of[nth] = te.th32ThreadID; nth++; }
                    else if (th) CloseHandle(th);
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
            list_tick = GetTickCount();
        }
        for (k = 0; k < nth && ns < cap; k++) {
            if (SuspendThread(ths[k]) != (DWORD)-1) {
                Sample *x = &s[ns];
                x->tid = tid_of[k];
                x->n = (uint8_t)capture(ths[k], x->pc, DEPTH);
                ResumeThread(ths[k]);
                if (x->n) ns++;
            }
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    report(s, ns, self);
    free(s);
    return 0;
}

void cs_prof_init(void)
{
    const char *e;
    if ((e = getenv("RECOMP_CS_GATE_WATCH")) && *e && *e != '0') {
        const char *ms = getenv("RECOMP_CS_GATE_WATCH_MS");
        DWORD limit = ms ? (DWORD)atoi(ms) : 100;
        sym_init();
        CloseHandle(CreateThread(NULL, 0, gate_watch, (LPVOID)(uintptr_t)limit, 0, NULL));
    }
    if ((e = getenv("RECOMP_CS_PROF")) && *e) {
        sym_init();
        CloseHandle(CreateThread(NULL, 0, prof_thread, (LPVOID)e, 0, NULL));
    }
}
