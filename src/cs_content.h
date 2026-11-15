/* cs_content.h -- unlock the downloadable maps (Office, Inferno); see cs_content.c. */
#ifndef CS_CONTENT_H
#define CS_CONTENT_H

/* Write the two maps' content packages under the save folder if missing.
 * Call after RECOMP_SAVE_DIR is set and before the game starts. */
void cs_content_install(void);

#endif
