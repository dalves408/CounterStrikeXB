#ifndef CS_FRAME_H
#define CS_FRAME_H
/* cs_frame.c: the program the player starts opens the one game window and runs
 * the game (and each of its relaunches) inside it. Returns 1, with the game's exit
 * code in *exit_code, when this process was the frame; 0 when it is the game itself
 * (CS_FRAME_HWND set, CS_NO_FRAME=1, or no window could be made). */
int cs_frame_main(int *exit_code);
#endif
