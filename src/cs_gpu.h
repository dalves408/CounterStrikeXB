#ifndef CS_GPU_H
#define CS_GPU_H
/* Register CS's D3D8LTCG completion fence and the PVIDEO overlay trap with the
 * toolkit (see cs_gpu.c). Call after xbox_MemoryLayoutInit, VEH installed. */
void cs_gpu_init(void);
/* From the VEH on an access violation: 1 = a PVIDEO register access was
 * emulated and stepped over (return EXCEPTION_CONTINUE_EXECUTION). */
int cs_gpu_mmio_fault(void *exception_pointers);
#endif
