/*
 * cs_gpu.c -- Counter-Strike's D3D8LTCG <-> NV2A completion handshake.
 *
 * Hand-written CS glue; registers device-model behaviour with the pinned
 * toolkit. No generated code is touched and nothing here writes game state:
 * the toolkit acknowledges a GPU-owned semaphore, the way NV2A itself would.
 *
 * Why it is needed (measured, runs/boot4_wd60 and boot7_pbwd, watchdog):
 *   the main thread spins forever in D3D sub_0030A000 (BlockOnTime), reached
 *   from sub_00309490 during device creation, before AvSetDisplayMode. NV2A
 *   USER PUT == GET, so everything submitted is consumed, but the completion
 *   semaphore D3D compares against never advances because nothing plays the
 *   GPU's part of writing it.
 *
 * Verified layout (D3D section disassembly of this exact XBE, sha256
 * 8f0c12e3...cb105):
 *   0x00316FE8            global D3D device pointer (sub_0030A000 +0x01:
 *                         mov esi,[0x316fe8]; also sub_00309490 +0x09)
 *   device + 0x2C         last submitted fence "time" (sub_0030A000 +0x0C)
 *   device + 0x30         pointer to the GPU-written semaphore
 *                         (sub_0030A000 +0x07/+0x0A: mov eax,[esi+0x30];
 *                         mov ecx,[eax])
 *
 * The toolkit's xbox_Nv2aMirrorFence (kernel/xbox_memory_layout.h) exists for
 * exactly this D3D pattern ("D3D seeds it, submits work, then spins until it
 * reaches the submitted count"; first used for Wreckless). It follows the
 * chain fresh on every poll and bounds-checks each step, so registering it
 * before the device exists is harmless.
 *
 * Reference: the old C:\Recomp\CounterStrikeXB registered the same chain in
 * cs_runtime.c. Re-derived here from the disassembly rather than copied; the
 * old file's other CS hooks (pad mirroring, button pulses, guest pokes) are
 * deliberately not carried over.
 */
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cs_gpu.h"
#include "cs_d3d11.h"
/* nv2a/nv2a_mmio_hook.h: the PVIDEO register snapshot (returns bool) */
bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4], uint64_t *stop_generation);

/* kernel/xbox_memory_layout.h (pinned toolkit f0f8a46) */
int xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);

#define CS_D3D_DEVICE_PTR_VA   0x00316FE8u
#define CS_D3D_FENCE_PUT_OFF   0x2Cu   /* last submitted time            */
#define CS_D3D_FENCE_GET_PTR   0x30u   /* -> GPU-written completion word */

/*
 * 2. The NV2A video overlay (PVIDEO): CS's intro movies.
 *
 * CS plays its XMV intros (MGS_Logo, Valve_Logo, ESRB; boot flow 0x000ADB46)
 * through D3D's overlay, not as geometry: each decoded YUY2 frame goes to
 * D3DDevice_UpdateOverlay, which programs PVIDEO's bank registers and sets the
 * bank's bit in NV_PVIDEO_BUFFER. When the movie ends, the player's cleanup
 * (0x000C6BC0) calls D3DDevice_EnableOverlay(FALSE) at 0x0030A8D0:
 *
 *     mov  eax, [0x316fe8]       ; device
 *     mov  eax, [eax+0x934]      ; NV2A register base (0xFD000000)
 *     ...  mov [eax+0x8704], 1   ; NV_PVIDEO_STOP
 *   L: cmp  [eax+0x8700], 0      ; NV_PVIDEO_BUFFER
 *     jne  L
 *
 * Against the plain memory behind the aperture BUFFER never clears, so the
 * main thread spun there forever on a black screen (measured: runs/b6_peek,
 * end-of-movie flag [0x552308] = 1, then no further GPU work; watchdog stack
 * through 0x000C6BC7 in runs/v17_wd).
 *
 * The pinned toolkit already models PVIDEO (nv2a_core.c pvideo_write: STOP
 * clears the pending banks, as xemu does) and scans the overlay out over the
 * framebuffer (fb_present.c fb_overlay / pvideo_scanout.h, toolkit #220). Both
 * are reached only by a title that calls nv2a_hook_init -- which makes the
 * PVIDEO register page fault -- and routes those faults to
 * nv2a_hook_handle_mmio from its VEH. This file does exactly that and nothing
 * more: only the 4 KB PVIDEO page is trapped; every other NV2A register stays
 * the plain memory the rest of the toolkit expects. No game state is written.
 */
extern void nv2a_hook_init(ptrdiff_t xbox_mem_offset);
extern bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                  uint32_t fault_xbox_va, int is_write);
extern ptrdiff_t xbox_GetMemoryOffset(void);

#define CS_NV2A_PVIDEO_VA   0xFD008000u
#define CS_NV2A_PVIDEO_SIZE 0x1000u

static int s_pvideo_hooked;

void cs_gpu_init(void)
{
    if (xbox_Nv2aMirrorFence(CS_D3D_DEVICE_PTR_VA, CS_D3D_FENCE_PUT_OFF,
                             CS_D3D_FENCE_GET_PTR) != 0)
        fprintf(stderr, "[CS2] NV2A fence mirror: table full\n");

    /* The VEH (main.c) is installed before this runs, so the trapped page's
     * first fault already has somewhere to go. */
    nv2a_hook_init(xbox_GetMemoryOffset());
    s_pvideo_hooked = 1;

    /* GPU rasterisation back end for the pushbuffer executor (src/cs_d3d11.c);
     * RECOMP_CS_D3D11=0 keeps the toolkit CPU rasteriser. */
    cs_d3d11_init();
}

int cs_gpu_mmio_fault(void *exception_pointers)
{
    PEXCEPTION_POINTERS ep = (PEXCEPTION_POINTERS)exception_pointers;
    uintptr_t fault, base;
    uint64_t rel;

    if (!s_pvideo_hooked || !ep ||
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return 0;
    fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    base = (uintptr_t)xbox_GetMemoryOffset();
    rel = (uint64_t)fault - (uint64_t)base;
    if (fault < base || rel < CS_NV2A_PVIDEO_VA ||
        rel >= (uint64_t)CS_NV2A_PVIDEO_VA + CS_NV2A_PVIDEO_SIZE)
        return 0;
    {
        int is_write = ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0;
        int handled = nv2a_hook_handle_mmio(ep->ContextRecord, fault, (uint32_t)rel, is_write) ? 1 : 0;
        /* RECOMP_CS_PVIDEO_CSV=<file>: time of every write to NV_PVIDEO_BUFFER
         * (+0x700, a new movie frame handed to a bank) and NV_PVIDEO_STOP
         * (+0x704), on the same clock as RECOMP_CS_FRAME_CSV. Movie cadence. */
        if (handled && is_write && (rel == CS_NV2A_PVIDEO_VA + 0x700u || rel == CS_NV2A_PVIDEO_VA + 0x704u)) {
            static FILE *pv = (FILE *)-1;
            if (pv == (FILE *)-1) {
                const char *p = getenv("RECOMP_CS_PVIDEO_CSV");
                pv = p ? fopen(p, "w") : NULL;
                if (pv) fprintf(pv, "t_ms,reg,buffer,stop_gen\n");
            }
            if (pv) {
                static unsigned n;
                static uint32_t regs[0x1000 / 4];
                uint64_t gen = 0;
                /* After the write: BUFFER holds the pending-bank bits; a real
                 * STOP (bit 0) bumps the stop generation. */
                nv2a_hook_pvideo_snapshot(regs, &gen);
                fprintf(pv, "%.2f,%s,%08X,%llu\n", cs_now_ms(), rel == CS_NV2A_PVIDEO_VA + 0x700u ? "BUFFER" : "STOP",
                        regs[0x700 / 4], (unsigned long long)gen);
                fflush(pv);              (void)n; /* a few per frame: cheap, and probes read it live */
            }
        }
        return handled;
    }
}