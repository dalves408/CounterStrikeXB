#ifndef CS_LAUNCH_H
#define CS_LAUNCH_H
#include <stdint.h>
/* XLaunchNewImage back into default.xbe (see cs_launch.c). */
typedef void (*cs_launch_func_t)(void);
cs_launch_func_t cs_launch_override(uint32_t xbox_va);
void cs_launch_restore(void);   /* main.c, after xbox_kernel_bridge_init() */
/* This copy of the game was started into an Xbox Live match (from the game's launch data). */
int cs_launch_xbox_live_match(void);
/* ...into any match (training 2, single player 3, System Link 4, Xbox Live 5); 0 for the menus. */
int cs_launch_match(void);
#endif
