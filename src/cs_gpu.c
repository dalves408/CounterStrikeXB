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
#include <stdint.h>
#include <stdio.h>

#include "cs_gpu.h"

/* kernel/xbox_memory_layout.h (pinned toolkit f0f8a46) */
int xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);

#define CS_D3D_DEVICE_PTR_VA   0x00316FE8u
#define CS_D3D_FENCE_PUT_OFF   0x2Cu   /* last submitted time            */
#define CS_D3D_FENCE_GET_PTR   0x30u   /* -> GPU-written completion word */

void cs_gpu_init(void)
{
    if (xbox_Nv2aMirrorFence(CS_D3D_DEVICE_PTR_VA, CS_D3D_FENCE_PUT_OFF,
                             CS_D3D_FENCE_GET_PTR) != 0)
        fprintf(stderr, "[CS2] NV2A fence mirror: table full\n");
}
