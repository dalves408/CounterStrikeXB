#ifndef CS_D3D11_H
#define CS_D3D11_H
/* Registers the D3D11 NV2A back end (src/cs_d3d11.c). Call before the title submits GPU work. */
void cs_d3d11_init(void);
/* Milliseconds since cs_d3d11_init (QueryPerformanceCounter); shared clock for the timing logs. */
double cs_now_ms(void);
/* The top frame-rate cap for the monitor the game window is on (60 or 72, cs_settings_rate_max), checked every
 * two seconds by the renderer; the F1 menu offers 72 only when this is 72. */
int cs_d3d11_rate_max(void);
/* The F1 menu changed a setting (g_cs_settings): apply it between frames. */
void cs_d3d11_settings_changed(void);
#endif
