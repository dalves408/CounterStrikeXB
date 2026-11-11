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

#include "recomp_types.h"
#include "cs_kernel.h"

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
    bridge();
    if (iosb && nt_error(g_eax)) {
        MEM32(iosb) = keep_status;
        MEM32(iosb + 4) = keep_info;
    }
}

recomp_func_t cs_kernel_override(uint32_t xbox_va)
{
    if (!s_ntreadfile_va)
        s_ntreadfile_va = MEM32(CS_THUNK_NTREADFILE);
    if (xbox_va == s_ntreadfile_va)
        return cs_NtReadFile;
    return (recomp_func_t)0;
}
