/* cs_settings.c -- settings.ini for the game and config.exe (see cs_settings.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cs_settings.h"

const int CS_SET_RATE_HZ[CS_SET_RATES] = { 30, 60, 72 };
const int CS_SET_MSAA_N[CS_SET_MSAAS] = { 1, 2, 4, 8 };
const int CS_SET_ANISO_N[CS_SET_ANISOS] = { 1, 2, 4, 8, 16 };
const char *const CS_SET_SCALE_TEXT[CS_SET_SCALES] = {
    "Low  (1x, 640x480)", "Medium  (2x, 1280x960)", "High  (3x, 1920x1440)" };
const char *const CS_SET_RATE_TEXT[CS_SET_RATES] = {
    "30", "60", "72" };
const char *const CS_SET_MSAA_TEXT[CS_SET_MSAAS] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
const char *const CS_SET_ANISO_TEXT[CS_SET_ANISOS] = { "Off", "2x", "4x", "8x", "16x" };

CsSettings g_cs_settings;

void cs_settings_defaults(CsSettings *s)
{
    s->scale = 1;
    s->fullscreen = 1;      /* a fresh install starts fullscreen; settings.ini keeps the player's choice */
    s->stretch = 0;
    s->vsync = 0;
    s->frame_rate = 60;
    s->msaa = 1;
    s->aniso = 1;
    s->hires = 1;
}

int cs_settings_path(const char *name, char *out, unsigned n, int make_dir)
{
    char base[MAX_PATH];
    DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!got || got >= sizeof base - 48)
        return 0;
    snprintf(out, n, "%s\\CounterStrikeXB", base);
    if (make_dir)
        CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\CounterStrikeXB\\%s", base, name);
    return 1;
}

int cs_settings_index(const int *list, int n, int v)
{
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < n; i++) {
        int d = abs(list[i] - v);
        if (list[i] == 0 && v != 0) d = 1 << 29;     /* 0 (uncapped) only matches itself */
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int get(const char *path, const char *key, int def)
{
    return (int)GetPrivateProfileIntA("display", key, def, path);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cs_settings_load(CsSettings *s)
{
    char path[MAX_PATH];
    cs_settings_defaults(s);
    if (!cs_settings_path("settings.ini", path, sizeof path, 0))
        return;
    s->scale = clampi(get(path, "resolution_scale", s->scale), 1, CS_SET_SCALES);
    s->fullscreen = get(path, "fullscreen", s->fullscreen) != 0;
    s->stretch = get(path, "stretch", s->stretch) != 0;
    s->vsync = get(path, "vsync", s->vsync) != 0;
    {   /* 72 is the game's own limit: older choices above it, and uncapped (0), are 72 */
        int r = get(path, "frame_rate", s->frame_rate);
        if (r <= 0 || r > 72) r = 72;
        s->frame_rate = CS_SET_RATE_HZ[cs_settings_index(CS_SET_RATE_HZ, CS_SET_RATES, r)];
    }
    s->msaa = CS_SET_MSAA_N[cs_settings_index(CS_SET_MSAA_N, CS_SET_MSAAS, get(path, "msaa", s->msaa))];
    s->aniso = CS_SET_ANISO_N[cs_settings_index(CS_SET_ANISO_N, CS_SET_ANISOS, get(path, "anisotropy", s->aniso))];
    s->hires = get(path, "hires_textures", s->hires) != 0;
}

static int put(const char *path, const char *key, int v)
{
    char val[16];
    snprintf(val, sizeof val, "%d", v);
    return WritePrivateProfileStringA("display", key, val, path) != 0;
}

/* settings.ini's last write time as this program last read or wrote it, so a
 * change made elsewhere (config.exe, RecompLauncher) can be noticed. */
static FILETIME s_seen;

static int ini_time(FILETIME *t)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!cs_settings_path("settings.ini", path, sizeof path, 0) ||
        !GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    *t = a.ftLastWriteTime;
    return 1;
}

void cs_settings_mark_seen(void)
{
    FILETIME t;
    if (ini_time(&t)) s_seen = t;
}

int cs_settings_changed_on_disk(CsSettings *out)
{
    FILETIME t;
    if (!ini_time(&t) || CompareFileTime(&t, &s_seen) == 0) return 0;
    s_seen = t;
    cs_settings_load(out);
    return 1;
}

int cs_settings_save(const CsSettings *s)
{
    char path[MAX_PATH];
    int ok;
    if (!cs_settings_path("settings.ini", path, sizeof path, 1))
        return 0;
    ok = put(path, "resolution_scale", s->scale);
    ok &= put(path, "fullscreen", s->fullscreen);
    ok &= put(path, "stretch", s->stretch);
    ok &= put(path, "vsync", s->vsync);
    ok &= put(path, "frame_rate", s->frame_rate);
    ok &= put(path, "msaa", s->msaa);
    ok &= put(path, "anisotropy", s->aniso);
    ok &= put(path, "hires_textures", s->hires);
    cs_settings_mark_seen();
    return ok;
}

void cs_settings_startup(void)
{
    char path[MAX_PATH];
    cs_settings_load(&g_cs_settings);
    cs_settings_mark_seen();
    /* _putenv_s: the toolkit reads these with getenv, the CRT's own copy. */
    /* RecompLauncher's Big Picture mode (RECOMPLAUNCHER_MODE=bigpicture) always
     * starts fullscreen, whatever the setting says; the setting itself is left alone. */
    {
        const char *mode = getenv("RECOMPLAUNCHER_MODE");
        if (getenv("CS_ACCT")) {
            _putenv_s("RECOMP_FULLSCREEN", "0");     /* an account run for Insignia Setup: stay out of its way */
        } else if (mode && _stricmp(mode, "bigpicture") == 0) {
            _putenv_s("RECOMP_FULLSCREEN", "1");
            fprintf(stderr, "[CS-SET] RecompLauncher Big Picture mode: fullscreen\n");
        } else if (!getenv("RECOMP_FULLSCREEN"))
            _putenv_s("RECOMP_FULLSCREEN", g_cs_settings.fullscreen ? "1" : "0");
    }
    if (!getenv("RECOMP_KEYMAP") && cs_settings_path("keymap.ini", path, sizeof path, 0)
        && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        _putenv_s("RECOMP_KEYMAP", path);
    /* Xbox Live (Insignia) identity: the player's own console's EEPROM, copied
     * to eeprom.bin in the data folder (the configurator does it), and the
     * console's hard-drive key, [insignia] hdd_key= in settings.ini. The
     * toolkit's console_id.c checks and uses them; neither is ever logged. */
    if (!getenv("RECOMP_XBOX_EEPROM") && cs_settings_path("eeprom.bin", path, sizeof path, 0)
        && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        _putenv_s("RECOMP_XBOX_EEPROM", path);
    if (!getenv("RECOMP_XBOX_HDKEY")) {
        char ini[MAX_PATH], key[80];
        if (cs_settings_path("settings.ini", ini, sizeof ini, 0)) {
            GetPrivateProfileStringA("insignia", "hdd_key", "", key, sizeof key, ini);
            if (key[0]) _putenv_s("RECOMP_XBOX_HDKEY", key);
            SecureZeroMemory(key, sizeof key);
        }
    }
    /* [input] other_controllers=0: Xbox controllers (XInput) only. On by
     * default: PlayStation, Switch Pro and other controllers through SDL3
     * (toolkit sdl3_pads.c). Off is for a pad Steam Input or DS4Windows already
     * presents as an Xbox one, which would otherwise count twice. */
    if (!getenv("RECOMP_SDL_PADS") && cs_settings_path("settings.ini", path, sizeof path, 0)
        && GetPrivateProfileIntA("input", "other_controllers", 1, path) == 0)
        _putenv_s("RECOMP_SDL_PADS", "0");
    /* [network] peers=host[,host...]: system link partners the LAN broadcast
     * does not reach (another subnet, a VPN such as Tailscale). The toolkit's
     * NIC link sends each frame to them too (nvnet_link.c). */
    if (!getenv("RECOMP_NET_PEERS") && cs_settings_path("settings.ini", path, sizeof path, 0)) {
        char peers[512];
        GetPrivateProfileStringA("network", "peers", "", peers, sizeof peers, path);
        if (peers[0]) {
            _putenv_s("RECOMP_NET_PEERS", peers);
            fprintf(stderr, "[CS-SET] system link peers: %s\n", peers);
        }
    }
    /* [network] real_xbox=1: system link with real Xboxes and xemu. They key
     * system-link traffic with the game's real XboxLANKey; this build's
     * default is the all-zero key, which every other copy of it shares, so
     * the switch is off unless the player wants the real ones. The key is
     * lankey.bin in the data folder, which Insignia Setup derives from the
     * player's own BIOS (never built in). Not logged. */
    if (!getenv("RECOMP_XBOX_LAN_KEY") && cs_settings_path("settings.ini", path, sizeof path, 0)
        && GetPrivateProfileIntA("network", "real_xbox", 0, path) == 1) {
        char key[MAX_PATH];
        if (cs_settings_path("lankey.bin", key, sizeof key, 0) && GetFileAttributesA(key) != INVALID_FILE_ATTRIBUTES) {
            _putenv_s("RECOMP_XBOX_LAN_KEY", key);
            fprintf(stderr, "[CS-SET] system link with xemu (the real Xbox LAN key)\n");
        } else {
            fprintf(stderr, "[CS-SET] real_xbox=1, but no lankey.bin: run Insignia Setup with your BIOS + MCPX; "
                            "system link stays with this build's other players\n");
        }
    }
    /* [network] xemu=host:port: system link with xemu, set to its "udp"
     * network backend with that bind address and this PC as its remote
     * address (port 30740). The address is not logged. */
    if (!getenv("RECOMP_NET_XEMU") && cs_settings_path("settings.ini", path, sizeof path, 0)) {
        char xemu[256];
        GetPrivateProfileStringA("network", "xemu", "", xemu, sizeof xemu, path);
        if (xemu[0]) {
            _putenv_s("RECOMP_NET_XEMU", xemu);
            fprintf(stderr, "[CS-SET] system link with xemu\n");
        }
    }
    fprintf(stderr, "[CS-SET] scale %dx, fullscreen %d, stretch %d, vsync %d, frame rate %d, msaa %d, aniso %d, hi-res textures %d\n",
            g_cs_settings.scale, g_cs_settings.fullscreen, g_cs_settings.stretch, g_cs_settings.vsync,
            g_cs_settings.frame_rate, g_cs_settings.msaa, g_cs_settings.aniso, g_cs_settings.hires);
}

/* ---- the frame-rate top for a monitor --------------------------------------------------------- */
/* Adaptive sync in a monitor's EDID: the AMD FreeSync vendor block or an HDMI
 * Forum block with a VRR range (CTA-861 extensions), or a DisplayID 2.0
 * Adaptive-Sync block. G-Sync Compatible monitors carry one of these; a native
 * G-Sync module may not, and then 60 is the top until the refresh rate is set
 * to 144 Hz. */
static int edid_vrr(const BYTE *e, DWORD n)
{
    DWORD b;
    for (b = 128; b + 128 <= n; b += 128) {
        const BYTE *x = e + b;
        if (x[0] == 0x02) {                             /* CTA-861: data blocks from 4 to x[2] */
            DWORD p = 4, end = x[2] >= 4 && x[2] <= 127 ? x[2] : 4;
            while (p < end) {
                DWORD tag = x[p] >> 5, len = x[p] & 31;
                if (p + 1 + len > end) break;
                if (tag == 3 && len >= 3) {
                    DWORD oui = x[p + 1] | (x[p + 2] << 8) | ((DWORD)x[p + 3] << 16);
                    if (oui == 0x00001A) return 1;      /* AMD FreeSync */
                    if (oui == 0xC45DD8 && len >= 9 && (x[p + 9] & 0x3F)) return 1;   /* HDMI 2.1 VRRmin */
                }
                p += 1 + len;
            }
        } else if (x[0] == 0x70) {                      /* DisplayID: blocks of tag, revision, length */
            DWORD p = 5, end = 5 + x[2];
            if (end > 127) end = 127;
            while (p + 3 <= end) {
                if (x[p] == 0x2B) return 1;             /* Adaptive-Sync */
                if (!x[p] && !x[p + 2]) break;
                p += 3 + x[p + 2];
            }
        }
    }
    return 0;
}

/* The monitor's EDID from the registry: its device interface name,
 * \?\DISPLAY#<id>#<instance>#{guid}, names Enum\DISPLAY\<id>\<instance>. */
static int monitor_vrr(const char *display)
{
    DISPLAY_DEVICEA m;
    DWORD i;
    memset(&m, 0, sizeof m);
    m.cb = sizeof m;
    for (i = 0; EnumDisplayDevicesA(display, i, &m, EDD_GET_DEVICE_INTERFACE_NAME); i++) {
        char id[128], key[400], *a, *b, *c;
        BYTE edid[1024];
        DWORD sz = sizeof edid;
        HKEY k;
        if (!(m.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        snprintf(id, sizeof id, "%s", m.DeviceID);
        a = strchr(id, '#'); b = a ? strchr(a + 1, '#') : NULL; c = b ? strchr(b + 1, '#') : NULL;
        if (!c) continue;
        *b = 0; *c = 0;
        snprintf(key, sizeof key, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s\\%s\\Device Parameters", a + 1, b + 1);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k)) continue;
        if (!RegQueryValueExA(k, "EDID", NULL, NULL, edid, &sz) && sz >= 128 && edid_vrr(edid, sz)) {
            RegCloseKey(k);
            return 1;
        }
        RegCloseKey(k);
    }
    return 0;
}

int cs_settings_rate_max(void *monitor, int *hz, int *vrr)
{
    MONITORINFOEXA mi;
    DEVMODEA dm;
    HMONITOR mon = monitor ? (HMONITOR)monitor : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    int r = 0, v;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoA(mon, (MONITORINFO *)&mi) && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        r = (int)dm.dmDisplayFrequency;
    v = mi.szDevice[0] ? monitor_vrr(mi.szDevice) : 0;
    if (hz) *hz = r;
    if (vrr) *vrr = v;
    /* Adaptive sync (any refresh rate that reaches 72, 120 Hz included): the monitor follows the game. Without
     * it, 72 shows each frame for the same number of refreshes only on 144 Hz (and 72, 216, 288); Windows
     * reports 143.9 Hz as 143. */
    if (v && r >= 71) return 72;
    return r >= 71 && (r % 72 <= 1 || r % 72 >= 71) ? 72 : 60;
}

int cs_settings_rate_choices(int rate_max) { return rate_max >= 72 ? CS_SET_RATES : CS_SET_RATES - 1; }

int cs_settings_rate_effective(int frame_rate, int rate_max)
{
    return frame_rate > rate_max ? rate_max : frame_rate;
}
