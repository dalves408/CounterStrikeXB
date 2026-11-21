/*
 * cs_wide.c -- true widescreen: more of the world at the sides, not a
 * stretched 4:3 picture.
 *
 * The game draws at 640x480 and its renderer is Half-Life's (GoldSrc). Two
 * engine functions decide how much of the world is seen:
 *   - R_SetupGL (0x0004E330) builds the projection with gluPerspective
 *     (0x0004E280): fov_y from CalcFov(fov_x, width, height), and the aspect
 *     width / height. (The Xbox build kept GoldSrc's r_anamorphic cvar, which
 *     would force a 16:9 aspect there, but nothing ever sets it.)
 *   - R_SetFrustum (0x0004DDB0) builds the culling planes from fov_x, the
 *     horizontal field of view (the float at 0x00377688), and the same CalcFov.
 * The shape is the screen's: the resolution of the monitor the game's window
 * is on (cs_settings_wide_aspect), 16:9, 16:10 or up to 21:9.
 * Widescreen keeps the vertical view (fov_y) and widens the horizontal one,
 * as PC games do ("Hor+"): gluPerspective gets the aspect times the factor,
 * and R_SetFrustum culls with fov_x widened to match, so nothing at the new
 * edges is culled. The game still renders 640x480 pixels, squeezed; the D3D11
 * back end (cs_d3d11.c) draws them into a wider target, so the 3D view comes
 * out at the right shape and full resolution, and keeps the 2D HUD at its own
 * shape, anchored to the left, middle or right of the wider screen.
 *
 * Xbox Live stays 4:3: it is shared with Xbox and xemu players, and a wider
 * view sees more. As with mouse aim (cs_mouse.c), the Xbox Live menu stays
 * closed while widescreen is on, with a notice that can turn it off, and a
 * copy of the game started into an Xbox Live match is always 4:3.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "cs_wide.h"
#include "cs_settings.h"
#include "cs_launch.h"

#define FOV_X 0x00377688u   /* the horizontal field of view R_SetFrustum reads */

static float s_factor = 1.0f;

/* The frame window the game draws in (cs_frame.c), whose monitor sets the shape. */
static void *frame_window(void)
{
    char v[32];
    if (!GetEnvironmentVariableA("CS_FRAME_HWND", v, sizeof v)) return NULL;
    return (void *)(ULONG_PTR)_strtoui64(v, NULL, 10);
}

void cs_wide_apply(void)
{
    float a = cs_settings_wide_aspect(&g_cs_settings, frame_window());
    const char *t = getenv("RECOMP_WIDE");   /* testing: an aspect such as 1.7778 */
    if (t && atof(t) > 1.0) a = (float)atof(t);
    s_factor = (a > 0.0f && !cs_launch_xbox_live_match()) ? a / (4.0f / 3.0f) : 1.0f;
    if (s_factor < 1.0f) s_factor = 1.0f;
}

float cs_wide_factor(void) { return s_factor; }
float cs_wide_aspect(void) { return s_factor * (4.0f / 3.0f); }

void cs_wide_init(void)
{
    cs_wide_apply();
    if (g_cs_settings.widescreen || s_factor != 1.0f)
        fprintf(stderr, "[CS-WIDE] widescreen %s (%s, %.4f times 4:3)%s\n",
                s_factor != 1.0f ? "on" : cs_launch_xbox_live_match() ? "off in this Xbox Live match" : "on, but the screen is 4:3",
                cs_settings_aspect_name(s_factor != 1.0f ? cs_wide_aspect() : 0.0f), s_factor,
                getenv("RECOMP_WIDE_TRACE") ? ", tracing" : "");
}

/* gluPerspective(double fovy, double aspect, double zNear, double zFar):
 * [esp] is the return address, aspect at esp + 0x0C. */
void cs_wide_perspective(void)
{
    if (s_factor != 1.0f)
        MEMD(g_esp + 0x0Cu) = MEMD(g_esp + 0x0Cu) * (double)s_factor;
}

static float getf(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }
static void putf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

float cs_wide_frustum_begin(void)
{
    float fov = getf(FOV_X);
    if (s_factor != 1.0f && fov > 1.0f && fov < 179.0f) {
        double half = tan(fov * (3.14159265358979 / 360.0)) * s_factor;
        float wide = (float)(atan(half) * (360.0 / 3.14159265358979));
        if (wide > 178.0f) wide = 178.0f;
        putf(FOV_X, wide);
    }
    return fov;
}

void cs_wide_frustum_end(float saved)
{
    if (s_factor != 1.0f)
        putf(FOV_X, saved);
}
