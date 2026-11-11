#ifndef CS_KERNEL_H
#define CS_KERNEL_H
#include <stdint.h>
/* Kernel-thunk overrides for CS (see cs_kernel.c). Called from
 * recomp_lookup_manual() for synthetic kernel VAs (>= 0xFE000000) only. */
typedef void (*cs_func_t)(void);
cs_func_t cs_kernel_override(uint32_t xbox_va);
#endif
