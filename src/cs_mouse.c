/*
 * cs_mouse.c -- mouse aim, and keeping it off Xbox Live.
 *
 * Hand-written CS code. Generated code untouched, no game state written.
 *
 * Mouse aim is the toolkit's mouse layer (input/mkb_input.c) in its
 * mouse-only mode: raw mouse movement turns the view (cs_mouse_look below),
 * and the buttons and wheel are keys in the player's key map (keymap.ini:
 * mouse1..mouse5, mwheelup, mwheeldown; by default the alternates of the
 * triggers, Y, White and Black), counted only while mouse aim is on. It is a setting ([input] mouse_aim,
 * off by default; F1 and config.exe), with a sensitivity and an inversion.
 *
 * Xbox Live on Insignia is shared with players on Xboxes and xemu, so mouse
 * aim does not go there:
 *   - while it is on, the router refuses every packet for the internet
 *     (nvnet_nat.c's title gate), so Xbox Live cannot sign in, and the F1
 *     menu opens on Mouse aim saying why; turning it off lets the next
 *     sign-in through (checked per packet, no restart);
 *   - a copy of the game started into an Xbox Live match (cs_launch.c) never
 *     reads the mouse and never refuses its traffic, whatever the setting.
 * Single player, training and System Link (LAN or VPN) keep it.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "cs_mouse.h"
#include "cs_settings.h"
#include "cs_launch.h"
#include "cs_overlay.h"

/* toolkit: input/mkb_input.h, kernel/nvnet_nat.h (their headers bring the
 * Xbox's own types) */
extern void mkb_set_enabled(int on);
extern void mkb_set_mouse_only(int on);
extern void mkb_set_mouse_stick(int on);
extern int  mkb_take_mouse(long *dx, long *dy);
extern void mkb_set_aim(double sens, int invert_y);
extern void xbox_InputSetMouseKeys(int on);      /* input/keyboard.c: mouse1..5, mwheelup/down as keys */
extern int  xbox_FramebufferKeyDown(int vk);     /* input/keyboard.c: a key (or mouse button) held */
extern void nat_set_internet_gate(int (*blocked)(void));
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/* ---- mouse look: the view angles themselves ---------------------------------
 * The Xbox port dropped Half-Life's IN_MouseMove: its settings (sensitivity,
 * m_yaw, m_pitch, m_filter) are still registered in IN_Init (0x0019E6F0) but
 * nothing reads them. So the mouse turns the view the way IN_MouseMove did:
 * the client's look update (0x0019E1F0, the controller's IN_JoyMove) gets an
 * entry hook (recomp_manual.c) that adds the mouse's counts to the engine's
 * view angles, through the engine's own GetViewAngles / SetViewAngles (the
 * client's copy of cl_enginefunc_t, entries 34 and 35 at 0x00543190 and
 * 0x00543194), before the look update adds the stick and clamps the pitch.
 * The look update itself keeps the aim it turns: yaw at 0x00552C30, pitch at
 * 0x00552C34. When it runs in full it adds the stick to those, clamps the
 * pitch and SetViewAngles them, overwriting the engine's angles; in other
 * states (its early returns) it leaves the engine's angles alone. So the
 * mouse turns both, the same amount, and they stay in step whichever path
 * the frame takes. One count turns 0.022 degrees (m_yaw, m_pitch) times
 * Half-Life's default sensitivity 3, times the player's setting. */
#define ENG_GET_VIEWANGLES 0x00543190u
#define ENG_SET_VIEWANGLES 0x00543194u
#define DEG_PER_COUNT      (0.022f * 3.0f)
#define LOOK_YAW           0x00552C30u    /* the look update's own aim */
#define LOOK_PITCH         0x00552C34u
#define GUEST_RETURN       0xFFFF0000u    /* never returned to: the callee only pops it */

/* engine(float *angles) through the client's table, as the game calls it */
static void engine_angles(uint32_t slot, uint32_t angles)
{
    uint32_t va = MEM32(slot);
    recomp_func_t f;
    if (!va) return;
    f = recomp_lookup(va);
    if (!f) f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup_kernel(va);
    if (!f) return;
    g_esp -= 4; MEM32(g_esp) = angles;
    g_esp -= 4; MEM32(g_esp) = GUEST_RETURN;
    f();
}

static float getf(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }
static void putf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

/* sub_0019E1F0's entry hook (recomp_manual.c). The caller's registers are in
 * the globals here; every one is put back. */
/* In play: this copy of the game was started into a match (the menus run in a
 * copy of their own, over a live 3D scene, so the look update runs there too)
 * and the look update ran lately (every frame of play, the buy menu included). */
static volatile LONG s_play_tick;

/* Xbox Live stays closed while mouse aim or widescreen (cs_wide.c) is on: it is
 * shared with Xbox and xemu players, on a controller and 4:3. */
static int live_closed(void)
{
    return (g_cs_settings.mouse_aim || g_cs_settings.widescreen) && !cs_launch_xbox_live_match();
}

static const char *live_why(void)
{
    return g_cs_settings.mouse_aim ? (g_cs_settings.widescreen ? "mouse aim and widescreen are" : "mouse aim is")
                                   : "widescreen is";
}

int cs_mouse_ui_command(uint32_t text_va)
{
    /* the main menu's Xbox Live item runs "!enterLive"; the Live pages open as
     * "open SuperLauncher_Live..." */
    static const char enter[] = "!enterLive", live[] = "open SuperLauncher_Live";
    char t[96];
    unsigned i;
    if (!text_va) return 0;
    for (i = 0; i < sizeof t - 1 && MEM8(text_va + i); i++) t[i] = (char)MEM8(text_va + i);
    t[i] = 0;
    if (!live_closed())
        return 0;
    if (strncmp(t, enter, sizeof enter - 1) != 0 && strncmp(t, live, sizeof live - 1) != 0)
        return 0;
    fprintf(stderr, "[CS-MOUSE] %s on: the Xbox Live menu stays closed\n", live_why());
    cs_overlay_live_refused();
    return 1;
}

int cs_mouse_ui_refuse(void)
{
    if (!cs_mouse_ui_command(g_edx))
        return 0;
    g_esp += 4;                              /* ret: as if the command had run */
    return 1;
}

int cs_mouse_menu_clicks(void)
{
    int r = 0;
    if (!g_cs_settings.mouse_aim || cs_launch_xbox_live_match())
        return 0;
    if (cs_launch_match() && GetTickCount() - (DWORD)s_play_tick < 300)
        return 0;
    if (xbox_FramebufferKeyDown(0x01)) r |= 1;     /* VK_LBUTTON */
    if (xbox_FramebufferKeyDown(0x02)) r |= 2;     /* VK_RBUTTON */
    return r;
}

void cs_mouse_look(void)
{
    long dx, dy;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi;
    uint32_t ebp = g_ebp, esp = g_esp, efl = g_eflags, buf;
    float scale, pitch, yaw, dp, dyaw, lp;
    InterlockedExchange(&s_play_tick, (LONG)GetTickCount());
    if (!mkb_take_mouse(&dx, &dy) || cs_overlay_active())
        return;
    scale = DEG_PER_COUNT * (float)g_cs_settings.mouse_sens / 100.0f;
    buf = (esp - 0x80u) & ~15u;                /* below anything live on the guest stack */
    g_esp = buf;
    engine_angles(ENG_GET_VIEWANGLES, buf);
    dp = (float)dy * scale * (g_cs_settings.mouse_invert ? -1.0f : 1.0f);
    dyaw = -(float)dx * scale;
    pitch = getf(buf) + dp;
    yaw = getf(buf + 4) + dyaw;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    yaw = fmodf(yaw, 360.0f);
    putf(buf, pitch);
    putf(buf + 4, yaw);
    g_esp = buf;
    engine_angles(ENG_SET_VIEWANGLES, buf);
    lp = getf(LOOK_PITCH) + dp;
    if (lp > 89.0f) lp = 89.0f;
    if (lp < -89.0f) lp = -89.0f;
    putf(LOOK_PITCH, lp);
    putf(LOOK_YAW, fmodf(getf(LOOK_YAW) + dyaw, 360.0f));
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
    g_ebp = ebp; g_esp = esp; g_eflags = efl;
}

static int live_gate(void)
{
    static volatile LONG last;
    DWORD now;
    if (!live_closed())
        return 0;
    now = GetTickCount();
    if (now - (DWORD)last > 8000) {          /* logged once per attempt, not per packet */
        InterlockedExchange(&last, (LONG)now);
        fprintf(stderr, "[CS-MOUSE] %s on: Xbox Live traffic refused\n", live_why());
    }
    return 1;              /* quietly: the notice is for choosing Xbox Live (!enterLive), not
                            * for the game's own silent sign-in at start-up */
}

void cs_mouse_apply(void)
{
    int on = g_cs_settings.mouse_aim && !cs_launch_xbox_live_match();
    mkb_set_aim(g_cs_settings.mouse_sens / 100.0, g_cs_settings.mouse_invert);
    mkb_set_enabled(on);
    xbox_InputSetMouseKeys(on);   /* the buttons and wheel, bound in keymap.ini */
}

static DWORD WINAPI test_notice(LPVOID unused)
{
    (void)unused;
    Sleep(8000);
    cs_overlay_live_refused();
    return 0;
}

void cs_mouse_init(void)
{
    mkb_set_mouse_only(1);
    mkb_set_mouse_stick(0);       /* the view angles, not the stick (cs_mouse_look) */
    nat_set_internet_gate(live_gate);
    cs_mouse_apply();
    {   /* CS_TEST_LIVE_NOTICE=1: show the Xbox Live notice 8 s after start (to look at it) */
        const char *t = getenv("CS_TEST_LIVE_NOTICE");
        if (t && *t == '1') CloseHandle(CreateThread(NULL, 0, test_notice, NULL, 0, NULL));
    }
    if (g_cs_settings.mouse_aim)
        fprintf(stderr, "[CS-MOUSE] mouse aim %s (sensitivity %d%%%s)\n",
                cs_launch_xbox_live_match() ? "off in this Xbox Live match" : "on",
                g_cs_settings.mouse_sens, g_cs_settings.mouse_invert ? ", inverted" : "");
}
