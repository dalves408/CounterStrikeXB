/*
 * cs_kernel.c -- Counter-Strike: kernel-call semantics the pinned toolkit
 * (xboxrecomp v0.17.0) gets wrong in a way this title depends on.
 *
 * Hand-written CS code. Nothing here writes game state: each wrapper calls the
 * toolkit's own bridge for the export and only restores what the real Xbox
 * kernel would have left untouched.
 *
 * 1. NtReadFile: a request that fails synchronously leaves the IO_STATUS_BLOCK
 *    alone.
 *
 *    On NT (and the Xbox kernel, which follows it) the I/O manager copies the
 *    final status into the caller's IO_STATUS_BLOCK only when the request
 *    succeeded or was pended. A request that fails with an error status before
 *    it ever pends (here: STATUS_END_OF_FILE, the file system rejecting an
 *    offset at or past end of file) returns the status to the caller and the
 *    block keeps whatever the caller put there. The toolkit's bridge writes the
 *    block unconditionally.
 *
 *    Why CS cares (measured, runs/v17_fresh, RECOMP_WATCH=0x005522E8):
 *      The first-boot cache builder (thread 0x000C6310) streams
 *      D:\CacheTree-<lang>.SXWad through a double-buffered reader that keeps
 *      one 128 KB overlapped read ahead (0x001FC2E0/0x001FC230). The last
 *      read-ahead starts past end of file. XAPI ReadFile (0x001FED71) sets
 *      Overlapped.Internal = STATUS_PENDING (0x103) before NtReadFile; the
 *      reader sees ERROR_HANDLE_EOF, marks the buffer empty and signals the
 *      event itself. When the cache is complete the reader is closed
 *      (0x001FC020 -> 0x001FC1B0), which calls GetOverlappedResult on that
 *      read. On hardware Internal is still 0x103, so XAPI's
 *      GetOverlappedResult (0x002019E9) returns TRUE. With the toolkit's
 *      unconditional write it is 0xC0000011, GetOverlappedResult fails, the
 *      reader's fatal path (0x001FC160) runs the game's error hook 0x000C5F40,
 *      which sets the "Cache" error (0x005522E8 = 0x0033B0D4), and the boot
 *      flow (0x000ADC05) draws "There's a problem with the disc you're using".
 *
 *    The event and APC completion the bridge performs on that path are left
 *    as they are: the reader signals the same event itself, and no CS caller
 *    passes an APC routine with a read that can fail synchronously.
 *
 * Hook point: kernel imports are called through the XBE's thunk table
 * (call dword ptr [slot]) and arrive here through RECOMP_ICALL, which asks
 * recomp_lookup_manual() first. The NtReadFile slot of this XBE is 0x003210D8
 * (XAPI ReadFile 0x001FEDC0: call [0x003210D8]; ordinal 219 per the bridge's
 * [READ] lines from 0x001FEDC6). The slot's value is read at run time, after
 * the bridge has resolved the table, so no synthetic address is hard-coded.
 */
#include <stdint.h>
#include <stddef.h>

#include <windows.h>
#include "recomp_types.h"
#include "cs_kernel.h"
#include "cs_d3d11.h"   /* cs_now_ms */
#include <stdio.h>
#include <stdlib.h>

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);   /* kernel_bridge.c */

#define CS_THUNK_NTREADFILE 0x003210D8u

static uint32_t s_ntreadfile_va;   /* synthetic VA the thunk slot holds */

/* NTSTATUS severity "error" (top two bits set). */
static int nt_error(uint32_t status) { return (status >> 30) == 3u; }

static void cs_NtReadFile(void)
{
    /* Entry: [esp] return address, then FileHandle, Event, ApcRoutine,
     * ApcContext, IoStatusBlock, Buffer, Length, ByteOffset. */
    uint32_t iosb = MEM32(g_esp + 4 + 4 * 4);
    uint32_t keep_status = 0, keep_info = 0;
    recomp_func_t bridge;

    if (iosb) {
        keep_status = MEM32(iosb);
        keep_info = MEM32(iosb + 4);
    }
    bridge = recomp_lookup_kernel(s_ntreadfile_va);
    if (!bridge)
        return;                    /* cannot happen once the table is bridged */
    {
        /* RECOMP_CS_IO_CSV=<file>: one line per read -- start on the frame
         * logs' clock, duration, handle, length, offset, status, thread,
         * the completion event and APC routine, and the bytes moved. The
         * arguments are read before the bridge pops them. */
        static FILE *io = (FILE *)-1;
        double t0 = 0;
        uint32_t handle = MEM32(g_esp + 4), length = MEM32(g_esp + 4 + 6 * 4);
        uint32_t offp = MEM32(g_esp + 4 + 7 * 4), off = offp ? MEM32(offp) : 0xFFFFFFFFu;
        uint32_t ev = MEM32(g_esp + 4 + 1 * 4), apc = MEM32(g_esp + 4 + 2 * 4);
        if (io == (FILE *)-1) {
            const char *p = getenv("RECOMP_CS_IO_CSV");
            io = p ? fopen(p, "w") : NULL;
            if (io) fprintf(io, "t_ms,dur_ms,handle,length,offset,status,tid,event,apc,info\n");
        }
        if (io) t0 = cs_now_ms();
        bridge();
        if (io) {
            fprintf(io, "%.2f,%.3f,%08X,%u,%u,%08X,%lu,%08X,%08X,%08X\n", t0, cs_now_ms() - t0, handle,
                    length, off, g_eax, (unsigned long)GetCurrentThreadId(), ev, apc,
                    iosb ? MEM32(iosb + 4) : 0);
            fflush(io);    /* a watchdog _exit must not lose the last reads */
        }
    }
    if (iosb && nt_error(g_eax)) {
        MEM32(iosb) = keep_status;
        MEM32(iosb + 4) = keep_info;
    }
}

/*
 * 2. Diagnostic only (CS_IRQL_DIAG=1): who raises to DISPATCH_LEVEL and never
 *    lowers. The toolkit's [IRQLHOLD] report names the bridge
 *    (bridge_KeRaiseIrqlToDpcLevel) and one stack word, which is not enough to
 *    find the guest caller. This logs the guest stack for the first raises at
 *    each distinct esp. Behaviour is unchanged: the bridge does the raise.
 */
#include <stdio.h>
#include <stdlib.h>
#define CS_THUNK_KERAISEIRQLTODPC 0x003211A8u   /* ordinal 129 */
static uint32_t s_keraise_va;

int xbox_IrqlRaisedCount(void);                  /* kernel_hal.c */
#define CS_THUNK_KFLOWERIRQL 0x003211A4u        /* ordinal 161, fastcall cl */
static uint32_t s_kflower_va;
static volatile long s_irql_logged;

/* One line per raise/lower made from guest esp in [lo, hi) (CS_IRQL_DIAG_LO /
 * _HI, hex; default the main thread's 0x0108ED00..0x0108EE00), with the
 * toolkit's raised-thread count before and after and the guest stack. */
static void cs_irql_log(const char *what, recomp_func_t bridge)
{
    static uint32_t lo = 1, hi;
    uint32_t esp = g_esp, ecx = g_ecx;
    int before, i;
    if (lo == 1) {
        const char *l = getenv("CS_IRQL_DIAG_LO"), *h = getenv("CS_IRQL_DIAG_HI");
        lo = l ? strtoul(l, NULL, 16) : 0x0108ED00u;
        hi = h ? strtoul(h, NULL, 16) : 0x0108EE00u;
    }
    before = xbox_IrqlRaisedCount();
    if (bridge)
        bridge();
    if (esp >= lo && esp < hi && s_irql_logged < 3000) {
        s_irql_logged++;
        fprintf(stderr, "[CS2-IRQL] %s tid %lu esp %08X ecx %08X eax %08X depth %d->%d |",
                what, (unsigned long)GetCurrentThreadId(), esp, ecx, g_eax,
                before, xbox_IrqlRaisedCount());
        for (i = 0; i < 12; i++)
            fprintf(stderr, " %08X", MEM32(esp + 4 * i));
        fprintf(stderr, "\n");
    }
}

static void cs_KeRaiseIrqlToDpcLevel(void)
{
    cs_irql_log("raise", recomp_lookup_kernel(s_keraise_va));
}

static void cs_KfLowerIrql(void)
{
    cs_irql_log("lower", recomp_lookup_kernel(s_kflower_va));
}

#ifdef CS_ABI_CHECK
/* RECOMP_ABI_CHECK's reporter (CMake option CS_ABI_CHECK, a separate build):
 * the first report for each callee, with the time and thread. The toolkit's
 * own reporter is only built into an RECOMP_ABI_CHECK toolkit. This is what
 * named the intro movie decoder coming back with esi changed. */
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0, uint32_t edi0, uint32_t esp0)
{
    static uint32_t seen[256];
    static unsigned n;
    unsigned i;
    for (i = 0; i < n; i++)
        if (seen[i] == va) return;
    if (n < 256) seen[n++] = va;
    fprintf(stderr, "[ABI] t=%.1f tid=%lu sub_%08X:%s%s%s%s ebx %08X->%08X esi %08X->%08X edi %08X->%08X esp %08X->%08X\n",
            cs_now_ms(), (unsigned long)GetCurrentThreadId(), va,
            g_ebx != ebx0 ? " ebx" : "", g_esi != esi0 ? " esi" : "", g_edi != edi0 ? " edi" : "",
            g_esp < esp0 + 4 ? " esp" : "", ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
}
#endif

recomp_func_t cs_kernel_override(uint32_t xbox_va)
{
    if (!s_ntreadfile_va)
        s_ntreadfile_va = MEM32(CS_THUNK_NTREADFILE);
    if (!s_keraise_va)
        s_keraise_va = MEM32(CS_THUNK_KERAISEIRQLTODPC);
    if (xbox_va == s_ntreadfile_va)
        return cs_NtReadFile;
    if (!s_kflower_va)
        s_kflower_va = MEM32(CS_THUNK_KFLOWERIRQL);
    {   /* read once: these two are called thousands of times a frame, and a
         * getenv per call (an environment scan) was ~15% of the game thread */
        static int diag = -1;
        if (diag < 0) diag = getenv("CS_IRQL_DIAG") != NULL;
        if (diag && xbox_va == s_keraise_va)
            return cs_KeRaiseIrqlToDpcLevel;
        if (diag && xbox_va == s_kflower_va)
            return cs_KfLowerIrql;
    }
    return (recomp_func_t)0;
}
