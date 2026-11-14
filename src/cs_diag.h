/* cs_diag.h - what a player's problem report needs, in every build (cs_diag.c). */
#ifndef CS_DIAG_H
#define CS_DIAG_H

#include <stddef.h>

/* Player build: everything printed from here on also goes to <data_dir>\logs\cs-<date>-<time>.log (the last three
 * runs are kept), trimmed and with the Windows user name blanked out, and on to wherever it went before
 * (RecompLauncher captures it). Call before anything is printed. */
void cs_diag_log_start(const char *data_dir);

/* One line naming this build (version and symbol-file id) and the PC, for matching a report to its symbols. */
void cs_diag_build_line(void);

/* The freeze report: when no frame has been presented for freeze_secs, every thread's call stack goes to the log,
 * once at freeze_secs and once 20 s later. Costs one wake-up a second. verbose (RECOMP_STALL_DUMP) repeats it. */
void cs_diag_start(unsigned freeze_secs, int verbose);

/* "cs2_xbox_recomp+0x1A2B3C", or another module's name and offset, plus the symbol when a .pdb is beside the exe. */
void cs_diag_where(const void *addr, char *out, size_t n);

/* A fatal exception that nothing handled: the code, where, and the call stack, then the log is written out. */
void cs_diag_install_crash_filter(void);

/* A [VIDEO] line: every video setting in effect, the window and the monitor. `why`: "start", "settings changed"... */
void cs_diag_log_video(const char *why);

/* Push everything printed so far into the log before the process ends. */
void cs_diag_flush(void);

#endif
