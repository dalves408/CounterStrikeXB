#ifndef CS_LAUNCH_H
#define CS_LAUNCH_H
#include <stdint.h>
/* XLaunchNewImage back into default.xbe (see cs_launch.c). */
typedef void (*cs_launch_func_t)(void);
cs_launch_func_t cs_launch_override(uint32_t xbox_va);
void cs_launch_restore(void);   /* main.c, after xbox_kernel_bridge_init() */
#endif
