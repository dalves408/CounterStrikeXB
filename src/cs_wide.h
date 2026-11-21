/* cs_wide.h -- true widescreen in the screen's own shape, kept off Xbox Live (cs_wide.c). */
#ifndef CS_WIDE_H
#define CS_WIDE_H

/* How much wider than 4:3 the picture is now: 1 (off, a 4:3 screen, and
 * always in an Xbox Live match), 4/3 on a 16:9 screen, 6/5 on 16:10. */
float cs_wide_factor(void);
/* The picture's aspect now (4/3 when off). */
float cs_wide_aspect(void);
void  cs_wide_init(void);         /* at start, after the settings are loaded */
void  cs_wide_apply(void);        /* after the setting changes */

/* Entry work of the engine's wrappers (recomp_manual.c). */
void  cs_wide_perspective(void);  /* gluPerspective: widen its aspect */
float cs_wide_frustum_begin(void);/* R_SetFrustum: cull with the wider view; returns what to restore */
void  cs_wide_frustum_end(float saved);

#endif
