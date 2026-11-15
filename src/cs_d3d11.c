/*
 * cs_d3d11.c -- Direct3D 11 render back end for Counter-Strike (phase 1).
 *
 * Registers an Nv2aBackend (tools/xboxrecomp/src/kernel/nv2a_backend.h, pinned
 * toolkit v0.17.0 + local patches) with the NV2A pushbuffer executor. The
 * executor still decodes the pushbuffer, runs vertex programs / fixed function
 * on the CPU and clips at the near plane; this file fills the resulting
 * triangles on the GPU instead of the toolkit's CPU rasteriser.
 *
 * Two draw paths. Screen-space batches (2D UI) use texture stage 0 times the
 * diffuse colour. Program / fixed-function batches (the 3D path, and D3D's
 * back-buffer resolve) carry the NV2A register combiners and four texture
 * stages (toolkit local patch 0004) and are shaded by an HLSL port of the
 * toolkit's nv2a_rc_eval (tools/xboxrecomp/src/kernel/nv2a_combiner.c).
 * Blend / depth / alpha-test / stencil / cull / colour-mask state applies to
 * both. Guest textures use level 0. Optional BC7 replacements have mipmaps.
 * Not modelled yet: cube maps, bump/dot
 * product stage programs (they read zero, as in the CPU rasteriser).
 *
 * Structure follows the other xboxrecomp D3D11 back ends on this PC as
 * references only (C:\Recomp\mvc2_pc\src\gpu_backend.c, Redcard 2003,
 * RoboTechBC Xbox): render targets and depth surfaces cached by guest
 * address, textures decoded by the toolkit and cached by content hash, state
 * objects cached by key. Game-specific parts of those (sprite filter, texture
 * packs, combiner shaders, shared present texture) are adapted to this title.
 *
 * Presentation: the toolkit's framebuffer window reads guest memory. The
 * executor calls present_sync() just before it snapshots the frame, so dirty
 * render targets are read back into guest memory there.
 *
 * Environment:
 *   RECOMP_CS_D3D11=0      keep the toolkit CPU rasteriser (A/B comparison)
 *   RECOMP_CS_D3D11_LOG=1  periodic one-line statistics
 *   RECOMP_CS_PIXOFS=<px>  viewport pixel-centre shift (default 0)
 *   RECOMP_CS_CULL=0       ignore the title's cull state
 *   RECOMP_CS_NO_ALPHA=1   ignore the title's alpha-test state (debug)
 *   RECOMP_CS_FORCE_A1=1   force combiner output alpha to 1 (debug)
 *   RECOMP_CS_ALPHA_LIFT=1 lift zero vertex alpha to 1 (old workaround; default off: it made
 *                          hidden glow sprites opaque -- the white/cyan blob)
 *   RECOMP_CS_WORLD_TRACE=1  one in-game frame: log every 3D batch (default off)
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_d3d11.h"
#include "cs_texture_pack.h"
#include "cs_settings.h"
#include "video/video_player.h"
#include "nv2a_backend.h"
#include "cs_vsh_hlsl.h"
#include "cs_rc_hlsl.h"
#include "d3d/d3d8_swizzle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern const float *nv2a_vsh_constant(uint32_t index);   /* kernel/nv2a_vsh_interp.c */

#define MAX_RT   8
#define MAX_DS   4
#define MAX_TEX  1024
#define VB_BYTES (8u * 1024u * 1024u)
#define VB_RING  3
#define CB_RING  1024

#define SAFE_REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* Guest addresses name the same memory through several views (0x00xxxxxx,
 * 0x80xxxxxx contiguous): compare them as physical offsets. */
#define PHYS(va) ((va) & 0x03FFFFFFu)

/* Targets are made at the player's render scale (F1 / config.exe): w and h
 * stay the title's sizes, everything on the GPU is s_scale times larger. With
 * MSAA the drawing goes to `ms` (rtv is its view) and is resolved into `tex`
 * (what srv samples and the presenter shows) before anything reads it. */
typedef struct {
    uint32_t va, rawva, w, h, bpp, pitch;   /* va: physical offset (match key); rawva: address as the title used it */
    uint32_t dirty, used;
    int pend;                      /* a copy is queued in stage, for the next present */
    int unresolved;                /* MSAA: drawn since the last resolve */
    ID3D11Texture2D *tex, *stage, *ms;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
} Rt;

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} Ds;

static int s_scale = 1, s_msaa = 1, s_aniso = 1, s_hires = 1;   /* what the targets and samplers are made with now */
static int s_presenting;                           /* our presenter draws the window (cs_window_present) */
static void settings_apply(int first);

typedef struct {
    uint32_t offset, w, h, color, pitch, used;
    uint32_t verified;            /* frame (+1) of the last full-content verify */
    uint64_t hash, sig;
    size_t replacement_bytes;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} TexEnt;

typedef struct { uint32_t key[12]; void *obj; } StateEnt;

typedef struct {
    float sw, sh, tw, th;
    uint32_t textured, afunc;
    float aref;
    uint32_t opaque;
    uint32_t dbg, pad[3];          /* RECOMP_CS_RC_DEBUG (combiner draws only) */
} Cb;

static const char s_hlsl[] =
"cbuffer C : register(b0) { float2 surf; float2 tdim; uint textured; uint afunc; float aref; uint opaque; };\n"
"Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
"struct VI { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"VO vs(VI i) {\n"
"  VO o; float w = 1.0 / max(i.p.w, 1e-20);\n"
"  float nx = i.p.x / surf.x * 2.0 - 1.0; float ny = 1.0 - i.p.y / surf.y * 2.0;\n"
"  float nz = saturate(i.p.z / 16777215.0);\n"
"  o.pos = float4(nx * w, ny * w, nz * w, w); o.c = i.c; o.uv = i.uv; return o; }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (textured != 0) { float4 t = t0.Sample(s0, i.uv / tdim); if (opaque != 0) t.a = 1.0; c *= t; }\n"
"  float a = c.a * 255.0; float r = aref;\n"
"  bool ok = true;\n"
"  if (afunc == 0x200) ok = false; else if (afunc == 0x201) ok = a < r - 0.5;\n"
"  else if (afunc == 0x202) ok = abs(a - r) < 0.5; else if (afunc == 0x203) ok = a < r + 0.5;\n"
"  else if (afunc == 0x204) ok = a > r + 0.5; else if (afunc == 0x205) ok = abs(a - r) >= 0.5;\n"
"  else if (afunc == 0x206) ok = a > r - 0.5;\n"
"  if (!ok) discard;\n"
"  return c; }\n";


/* Register-combiner path. Port of nv2a_rc_eval + rc_stage_fetch from the
 * pinned toolkit (checked against nv2a_combiner.c); the same interpreter is
 * used by the other xboxrecomp D3D11 back ends on this PC. */
static const char s_hlsl_rc[] =
"cbuffer C : register(b0) { float2 surf; float2 tdim; uint textured; uint afunc; float aref; uint opaque; uint dbg; uint3 pad; };\n"
"cbuffer RC : register(b1) {\n"
"  uint4 hdr;      // control, stage_program, final0, final1\n"
"  uint4 hdr2;     // final_c0, final_c1, clip-plane mode, clip-plane stage present\n"
"  float4 fogc;    // fog colour rgb\n"
"  float4 uvs[4];  // xy: scale to normalised uv; z: stage texture valid; w: wrap in the shader (both axes WRAP)\n"
"  uint4 stg[16];  // [2s] colour icw, alpha icw, colour ocw, alpha ocw; [2s+1] factor0, factor1\n"
"};\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1); Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"SamplerState sm0 : register(s0); SamplerState sm1 : register(s1); SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
"struct VI { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; float4 d1 : TEXCOORD1;\n"
"            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float4 d1 : TEXCOORD1;\n"
"            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
"VO vs_rc(VI i) {\n"
"  VO o; float w = 1.0 / max(i.p.w, 1e-20);\n"
"  float nx = i.p.x / surf.x * 2.0 - 1.0; float ny = 1.0 - i.p.y / surf.y * 2.0;\n"
"  float nz = saturate(i.p.z / 16777215.0);\n"
"  o.pos = float4(nx * w, ny * w, nz * w, w);\n"
"  o.c = i.c; o.d1 = i.d1; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; o.fog = i.fog; return o; }\n"
"float4 unpackc(uint c) { return float4((float)((c >> 16) & 255u), (float)((c >> 8) & 255u), (float)(c & 255u), (float)(c >> 24)) / 255.0; }\n"
"float mapin(float x, uint b) {\n"
"  float p = max(x, 0.0); uint m = (b >> 5) & 7u;\n"
"  if (m == 0u) return p; if (m == 1u) return 1.0 - saturate(x); if (m == 2u) return 2.0 * p - 1.0;\n"
"  if (m == 3u) return -2.0 * p + 1.0; if (m == 4u) return p - 0.5; if (m == 5u) return -p + 0.5;\n"
"  if (m == 6u) return x; return -x; }\n"
"float3 mapout3(float3 x, uint m) {\n"
"  if (m == 0x08u) return x - 0.5; if (m == 0x10u) return x * 2.0; if (m == 0x18u) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20u) return x * 4.0; if (m == 0x30u) return x * 0.5; return x; }\n"
"float mapout1(float x, uint m) { return mapout3(float3(x, x, x), m).x; }\n"
"float3 read_rgb(float4 R[16], uint b) { float4 r = R[b & 15u]; float3 v = (b & 0x10u) ? r.aaa : r.rgb; return float3(mapin(v.x, b), mapin(v.y, b), mapin(v.z, b)); }\n"
"float read_alpha(float4 R[16], uint b) { float4 r = R[b & 15u]; return mapin((b & 0x10u) ? r.a : r.b, b); }\n"
"float4 stage_value(uint mode, float4 c, float4 s) {\n"
"  if (mode == 0u) return float4(0, 0, 0, 1);\n"
"  if (mode == 1u || mode == 2u) return s;\n"
"  if (mode == 3u) return float4(1, 1, 1, 1);       // cube map: not modelled, reads white\n"
"  if (mode == 4u) return saturate(c);\n"
"  return float4(0, 0, 0, 0); }\n"
"float4 ps_rc(VO i) : SV_Target {\n"
"  float4 R[16]; float4 tcs[4]; float4 smp[4];\n"
"  uint n = hdr.x & 255u, flags = hdr.x >> 8, sp = hdr.y, s, k;\n"
"  float4 out4;\n"
"  tcs[0] = i.t0; tcs[1] = i.t1; tcs[2] = i.t2; tcs[3] = i.t3;\n"
"  { float2 c = tcs[0].xy / ((tcs[0].w != 0.0) ? tcs[0].w : 1.0) * uvs[0].xy; float2 gx = ddx(c), gy = ddy(c); if (uvs[0].w != 0.0) c = frac(c); smp[0] = tx0.SampleGrad(sm0, c, gx, gy); }\n"
"  { float2 c = tcs[1].xy / ((tcs[1].w != 0.0) ? tcs[1].w : 1.0) * uvs[1].xy; float2 gx = ddx(c), gy = ddy(c); if (uvs[1].w != 0.0) c = frac(c); smp[1] = tx1.SampleGrad(sm1, c, gx, gy); }\n"
"  { float2 c = tcs[2].xy / ((tcs[2].w != 0.0) ? tcs[2].w : 1.0) * uvs[2].xy; float2 gx = ddx(c), gy = ddy(c); if (uvs[2].w != 0.0) c = frac(c); smp[2] = tx2.SampleGrad(sm2, c, gx, gy); }\n"
"  { float2 c = tcs[3].xy / ((tcs[3].w != 0.0) ? tcs[3].w : 1.0) * uvs[3].xy; float2 gx = ddx(c), gy = ddy(c); if (uvs[3].w != 0.0) c = frac(c); smp[3] = tx3.SampleGrad(sm3, c, gx, gy); }\n"
"  if (n > 8u) n = 8u;\n"
"  [unroll] for (k = 0u; k < 16u; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(fogc.rgb, i.fog); R[4] = i.c; R[5] = i.d1;\n"
"  [unroll] for (k = 0u; k < 4u; k++) {\n"
"    uint mode = (sp >> (k * 5u)) & 31u; float4 sv = smp[k];\n"
"    if (uvs[k].z == 0.0) sv = float4(1, 1, 1, 1);\n"
"    R[8u + k] = stage_value(mode, tcs[k], sv); }\n"
"  R[12].a = (sp & 31u) ? R[8].a : 1.0;\n"
"  if (hdr2.w != 0u) {\n"
"    [unroll] for (k = 0u; k < 4u; k++) if (((sp >> (k * 5u)) & 31u) == 5u) {\n"
"      [unroll] for (uint j = 0u; j < 4u; j++) { bool ge = ((hdr2.z >> (k * 4u + j)) & 1u) != 0u; float v = tcs[k][j]; if (ge ? (v >= 0.0) : (v < 0.0)) discard; } } }\n"
"  for (s = 0u; s < n; s++) {\n"
"    uint icw = stg[2u * s].x, aicw = stg[2u * s].y, ocw = stg[2u * s].z, aocw = stg[2u * s].w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12;\n"
"    float3 A, B, C, D, pab, pcd, ab, cd, ms, sum; float aA, aB, aC, aD, apab, apcd, aab, acd, ams, asum; bool mux_cd;\n"
"    R[1] = unpackc(stg[2u * ((flags & 0x010u) ? s : 0u) + 1u].x);\n"
"    R[2] = unpackc(stg[2u * ((flags & 0x100u) ? s : 0u) + 1u].y);\n"
"    mux_cd = (flags & 1u) ? (R[12].a >= 0.5) : (((int)(R[12].a * 255.0) & 1) != 0);\n"
"    A = read_rgb(R, icw >> 24); B = read_rgb(R, icw >> 16); C = read_rgb(R, icw >> 8); D = read_rgb(R, icw);\n"
"    pab = (fl & 2u) ? dot(A, B).xxx : A * B; pcd = (fl & 1u) ? dot(C, D).xxx : C * D;\n"
"    ab = clamp(mapout3(pab, fl & 0x38u), -1.0, 1.0); cd = clamp(mapout3(pcd, fl & 0x38u), -1.0, 1.0);\n"
"    sum = (fl & 4u) ? (mux_cd ? pcd : pab) : pab + pcd; ms = clamp(mapout3(sum, fl & 0x38u), -1.0, 1.0);\n"
"    aA = read_alpha(R, aicw >> 24); aB = read_alpha(R, aicw >> 16); aC = read_alpha(R, aicw >> 8); aD = read_alpha(R, aicw);\n"
"    apab = aA * aB; apcd = aC * aD;\n"
"    aab = clamp(mapout1(apab, afl & 0x38u), -1.0, 1.0); acd = clamp(mapout1(apcd, afl & 0x38u), -1.0, 1.0);\n"
"    asum = (afl & 4u) ? (mux_cd ? apcd : apab) : apab + apcd; ams = clamp(mapout1(asum, afl & 0x38u), -1.0, 1.0);\n"
"    if ((ocw >> 4) & 15u) { uint d = (ocw >> 4) & 15u; R[d].rgb = ab; if (fl & 0x80u) R[d].a = ab.z; }\n"
"    if (ocw & 15u) { uint d = ocw & 15u; R[d].rgb = cd; if (fl & 0x40u) R[d].a = cd.z; }\n"
"    if ((ocw >> 8) & 15u) R[(ocw >> 8) & 15u].rgb = ms;\n"
"    if ((aocw >> 4) & 15u) R[(aocw >> 4) & 15u].a = aab;\n"
"    if (aocw & 15u) R[aocw & 15u].a = acd;\n"
"    if ((aocw >> 8) & 15u) R[(aocw >> 8) & 15u].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0); }\n"
"  if (hdr.z != 0u || hdr.w != 0u) {\n"
"    uint f0 = hdr.z, f1 = hdr.w, ff = f1 & 255u; float3 A, B, C, D, E, F, a, b;\n"
"    R[1] = unpackc(hdr2.x); R[2] = unpackc(hdr2.y);\n"
"    a = (ff & 0x40u) ? 1.0 - R[5].rgb : R[5].rgb; b = (ff & 0x20u) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14].rgb = (ff & 0x80u) ? saturate(a + b) : a + b; R[14].a = 0.0;\n"
"    E = read_rgb(R, f1 >> 24); F = read_rgb(R, f1 >> 16); R[15].rgb = E * F; R[15].a = 0.0;\n"
"    A = read_rgb(R, f0 >> 24); B = read_rgb(R, f0 >> 16); C = read_rgb(R, f0 >> 8); D = read_rgb(R, f0);\n"
"    out4.rgb = D + C * (1.0 - A) + B * A; out4.a = read_alpha(R, f1 >> 8);\n"
"  } else { out4 = R[12]; }\n"
"  out4 = saturate(out4);\n"
"  if (opaque != 0u) out4.a = 1.0;\n"
"  if (dbg != 0u) {   // RECOMP_CS_RC_DEBUG: show one input instead of the result\n"
"    if (dbg <= 4u) out4 = float4(smp[dbg - 1u].rgb, 1.0); else if (dbg == 5u) out4 = float4(i.c.rgb, 1.0);\n"
"    else if (dbg == 6u) out4 = float4(i.c.aaa, 1.0); else if (dbg == 7u) out4 = float4(smp[0].aaa, 1.0);\n"
"    else if (dbg == 8u) out4 = float4(frac(tcs[0].xy / ((tcs[0].w != 0.0) ? tcs[0].w : 1.0) * uvs[0].xy), 0.0, 1.0);\n"
"    else if (dbg == 9u) out4 = float4(frac(tcs[1].xy / ((tcs[1].w != 0.0) ? tcs[1].w : 1.0) * uvs[1].xy), 0.0, 1.0);\n"
"    return out4; }\n"
"  { int a = (int)(out4.a * 255.0 + 0.5); int r = (int)(aref + 0.5); bool ok = true;\n"
"    if (afunc == 0x200u) ok = false; else if (afunc == 0x201u) ok = a < r; else if (afunc == 0x202u) ok = a == r;\n"
"    else if (afunc == 0x203u) ok = a <= r; else if (afunc == 0x204u) ok = a > r; else if (afunc == 0x205u) ok = a != r;\n"
"    else if (afunc == 0x206u) ok = a >= r;\n"
"    if (!ok) discard; }\n"
"  return out4; }\n";

/* b1 for the combiner pixel shader. */
typedef struct {
    uint32_t hdr[4], hdr2[4];
    float fogc[4], uvs[4][4];
    uint32_t stg[16][4];
} RcCb;

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs, *s_vs_rc;
static ID3D11PixelShader *s_ps, *s_ps_rc;
static ID3D11InputLayout *s_il;
static ID3D11RasterizerState *s_rs;
static ID3D11RasterizerState *rs_offset(ID3D11RasterizerState *base, int cull, int ccw, const Nv2aRenderState *r);
static ID3D11Texture2D *s_white;
static ID3D11ShaderResourceView *s_white_srv;
static ID3D11Buffer *s_vbr[VB_RING], *s_cbr[CB_RING], *s_rcr[CB_RING];
static unsigned s_vb_i, s_cb_i, s_rc_i;
static uint32_t s_vb_off;

static Rt s_rt[MAX_RT];
static Ds s_ds[MAX_DS];
static TexEnt s_tx[MAX_TEX];
static size_t s_pack_bytes;
#define CS_PACK_BUDGET (384u * 1024u * 1024u)
static StateEnt s_bs[96], s_dss[96], s_ss[200];
static int s_nbs, s_ndss, s_nss;

static uint32_t s_fr_draws, s_fr_tris;   /* this frame, for RECOMP_CS_FRAME_CSV */
static uint32_t s_fr_gv, s_fr_fog;        /* this frame: vertex-program draws, and those with fog on */
static int s_dump_now;                    /* the RECOMP_CS_DUMP flag was up at the last present */
static float s_pick_x, s_pick_y;          /* RECOMP_CS_PICK point */
static int s_pick_hit;                    /* this draw covers it: read the pixel back after drawing */
/* RECOMP_CS_FLASH=<dir>: every draw of the current and previous frame, kept so
 * that a frame which comes out (almost) all black after a normal one can be
 * explained: both draw lists go to stderr and the frame's targets to <dir>. */
typedef struct {
    uint32_t rt, rw, rh, n, kept, tex, fmt, tw, th, comb, bl, bsrc, bdst, at, afunc, aref, dt, cm, c0;
    float x0, y0, z0, rhw0, u0, v0;
    float vx[3][4], vt[3][4];      /* first triangle: position + rhw, texcoord 0 (combiner path) */
    uint32_t rc_ctl, rc_sp, rc_f0, rc_f1, rc_cicw, rc_aicw, rc_cocw, rc_aocw, rc_k0, st_valid, st0_fmt, st0_off, st0_w, st0_h;
    int srv_kind;                  /* 0 white, 1 render target, 2 decoded texture */
} DrawRec;
#define DL_MAX 256
static DrawRec s_dl[2][DL_MAX];
static unsigned s_dln[2], s_dlc;
static const char *s_flash_dir;
static int s_inited, s_failed;
static uint32_t s_frame, s_tick;
static int s_cull_on = 1, s_log, s_world_trace, s_no_alpha, s_force_a1;
static int s_alpha_lift = 0;
static float s_pixofs;
static unsigned st_draws, st_tris, st_culled, st_clears, st_flips, st_decodes, st_rtsamples,
                st_bad_tex, st_decode_fail, st_rb, st_vb_drop;
static unsigned st_map_fail;

/* Every write Map, checked: a failure is logged (the first 20, with the call site) and counted. */
static HRESULT map_w(ID3D11Resource *r, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE *m, int line)
{
    HRESULT hr = ID3D11DeviceContext_Map(s_ctx, r, 0, type, 0, m);
    if (FAILED(hr) && st_map_fail++ < 20)
        fprintf(stderr, "[CS-D3D11] Map failed at line %d (%s): 0x%08lX\n", line,
                type == D3D11_MAP_WRITE_DISCARD ? "discard" : "no-overwrite", (unsigned long)hr);
    return hr;
}
#define MAP_W(res, type, m) map_w((ID3D11Resource *)(res), (type), (m), __LINE__)
static void drawlog(uint32_t kind, const Nv2aSurface *s, const Nv2aBatch *b, uint32_t count, uint32_t kept,
                    uint32_t vkey);

/* ---- init ---------------------------------------------------------------- */

static int cs_d3d11_setup(void)
{
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    /* Nv2aVertex: x y z rhw | diffuse | u v | d1[4] | tex[4][4] | fog */
    D3D11_INPUT_ELEMENT_DESC el[9] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 60,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 76,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 92,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32_FLOAT,          0, 108, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    ID3DBlob *vsb2 = NULL, *psb2 = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    uint32_t white = 0xFFFFFFFFu;
    unsigned i;
    HRESULT hr;

    {   /* RECOMP_CS_WARP=1: Windows' software rasteriser instead of the GPU, to tell a renderer bug (shows on WARP
         * too) from a driver one (does not) */
        const char *w = getenv("RECOMP_CS_WARP");
        hr = D3D11CreateDevice(NULL, (w && *w == '1') ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[CS-D3D11] D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "cs", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[CS-D3D11] vs: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "cs", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[CS-D3D11] ps: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    SAFE_REL(vsb); SAFE_REL(psb);

    /* Combiner shaders; the input layout is built from the vs that reads every attribute. */
    hr = D3DCompile(s_hlsl_rc, sizeof s_hlsl_rc - 1, "cs_rc", NULL, NULL, "vs_rc", "vs_5_0", 0, 0, &vsb2, &err);
    if (FAILED(hr)) { fprintf(stderr, "[CS-D3D11] vs_rc: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = D3DCompile(s_hlsl_rc, sizeof s_hlsl_rc - 1, "cs_rc", NULL, NULL, "ps_rc", "ps_5_0", 0, 0, &psb2, &err);
    if (FAILED(hr)) { fprintf(stderr, "[CS-D3D11] ps_rc: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb2), ID3D10Blob_GetBufferSize(vsb2), NULL, &s_vs_rc);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb2), ID3D10Blob_GetBufferSize(psb2), NULL, &s_ps_rc);
    hr = ID3D11Device_CreateInputLayout(s_dev, el, 9, ID3D10Blob_GetBufferPointer(vsb2), ID3D10Blob_GetBufferSize(vsb2), &s_il);
    if (FAILED(hr)) fprintf(stderr, "[CS-D3D11] CreateInputLayout failed 0x%08lX\n", (unsigned long)hr);
    SAFE_REL(vsb2); SAFE_REL(psb2);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (i = 0; i < VB_RING; i++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vbr[i]);
    bd.ByteWidth = sizeof(Cb); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (i = 0; i < CB_RING; i++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cbr[i]);
    bd.ByteWidth = sizeof(RcCb);
    for (i = 0; i < CB_RING; i++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rcr[i]);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);

    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &white; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 4;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &s_white);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_white, NULL, &s_white_srv);

    if (!s_vs || !s_ps || !s_vs_rc || !s_ps_rc || !s_il || !s_rcr[0] || !s_vbr[0] || !s_cbr[0] || !s_rs || !s_white_srv) return 0;

    {
        IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
            fprintf(stderr, "[CS-D3D11] back end up on '%ls' (feature level 0x%X)\n", desc.Description, (unsigned)fl);
        SAFE_REL(ad); SAFE_REL(dd);
    }
    {   /* What the driver allows that differs between GPUs (the GPU vertex-program inputs are written with
         * NO_OVERWRITE into a buffer the shader reads) */
        D3D11_FEATURE_DATA_D3D11_OPTIONS o;
        memset(&o, 0, sizeof o);
        if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(s_dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)))
            fprintf(stderr, "[CS-D3D11] driver: no-overwrite on shader-read buffers %d, constant-buffer offsets %d\n",
                    (int)o.MapNoOverwriteOnDynamicBufferSRV, (int)o.ConstantBufferOffsetting);
    }
    return 1;
}

static int cs_ready(void)
{
    if (s_failed) return 0;
    if (!s_inited) {
        const char *e;
        s_inited = 1;
        s_log = getenv("RECOMP_CS_D3D11_LOG") != NULL;
        { const char *w = getenv("RECOMP_CS_WORLD_TRACE"); s_world_trace = w ? (atoi(w) > 1 ? atoi(w) : 600) : 0; }   /* value = frame to arm at */
        s_no_alpha = getenv("RECOMP_CS_NO_ALPHA") != NULL;
        s_force_a1 = getenv("RECOMP_CS_FORCE_A1") != NULL;
        /* Workaround for toolkit diffuse.a=0; remove when patch 0005 lands.
         * Default off since the toolkit material-alpha fixes; RECOMP_CS_ALPHA_LIFT=1 enables. */
        if ((e = getenv("RECOMP_CS_ALPHA_LIFT")) && *e) s_alpha_lift = e[0] != '0';
        if ((e = getenv("RECOMP_CS_CULL")) && e[0] == '0') s_cull_on = 0;
        s_pixofs = (e = getenv("RECOMP_CS_PIXOFS")) ? (float)atof(e) : 0.0f;
        if (!cs_d3d11_setup()) { s_failed = 1; fprintf(stderr, "[CS-D3D11] init failed; no GPU drawing\n"); return 0; }
        settings_apply(1);
    }
    return 1;
}

/* ---- render targets and depth surfaces ---------------------------------------- */

static void rt_release(Rt *r)
{
    SAFE_REL(r->rtv); SAFE_REL(r->srv); SAFE_REL(r->tex); SAFE_REL(r->stage); SAFE_REL(r->ms);
    memset(r, 0, sizeof *r);
}

/* MSAA: the samples drawn since the last resolve, into the texture that is sampled and shown. */
static void rt_resolve(Rt *r)
{
    if (r->ms && r->unresolved) {
        ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, (ID3D11Resource *)r->ms, 0,
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
        r->unresolved = 0;
    }
}

static Rt *get_rt(const Nv2aSurface *s)
{
    int i, free_i = -1, lru = -1;
    D3D11_TEXTURE2D_DESC td;
    Rt *r;

    for (i = 0; i < MAX_RT; i++) {
        if (s_rt[i].tex && s_rt[i].va == PHYS(s->color_va) && s_rt[i].w == s->width && s_rt[i].h == s->height) {
            s_rt[i].used = s_tick; s_rt[i].pitch = s->pitch; s_rt[i].bpp = s->bytes_per_pixel;
            return &s_rt[i];
        }
        if (!s_rt[i].tex) { if (free_i < 0) free_i = i; }
        else if (lru < 0 || s_rt[i].used < s_rt[lru].used) lru = i;
    }
    if (free_i < 0) { free_i = lru; rt_release(&s_rt[free_i]); }
    r = &s_rt[free_i];
    memset(&td, 0, sizeof td);
    td.Width = s->width * (UINT)s_scale; td.Height = s->height * (UINT)s_scale; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex))) { rt_release(r); return NULL; }
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
    if (s_msaa > 1) {
        td.SampleDesc.Count = (UINT)s_msaa; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->ms))) { rt_release(r); return NULL; }
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->ms, NULL, &r->rtv);
        td.SampleDesc.Count = 1;
    } else
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    if (s_scale == 1) {   /* the copy back to guest memory is at the title's size */
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->stage);
    }
    r->va = PHYS(s->color_va); r->rawva = s->color_va; r->w = s->width; r->h = s->height;
    r->pitch = s->pitch; r->bpp = s->bytes_per_pixel; r->used = s_tick; r->dirty = 0; r->unresolved = 0;
    if (!r->srv || !r->rtv || (s_scale == 1 && !r->stage)) { rt_release(r); return NULL; }
    if (s_log)
        fprintf(stderr, "[CS-D3D11] render target va=%08X %ux%u bpp=%u pitch=%u aa=%ux%u\n",
                s->color_va, s->width, s->height, s->bytes_per_pixel, s->pitch, s->aa_sx, s->aa_sy);
    return r;
}

static Ds *get_ds(const Nv2aSurface *s, uint32_t zva)
{
    int i, slot = -1;
    D3D11_TEXTURE2D_DESC td;
    if (!zva) return NULL;
    for (i = 0; i < MAX_DS; i++)
        if (s_ds[i].tex && s_ds[i].va == PHYS(zva) && s_ds[i].w == s->width && s_ds[i].h == s->height) return &s_ds[i];
    for (i = 0; i < MAX_DS; i++) if (!s_ds[i].tex) { slot = i; break; }
    if (slot < 0) { slot = (int)(s_tick % MAX_DS); SAFE_REL(s_ds[slot].dsv); SAFE_REL(s_ds[slot].tex); }
    memset(&td, 0, sizeof td);
    td.Width = s->width * (UINT)s_scale; td.Height = s->height * (UINT)s_scale; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.SampleDesc.Count = (UINT)s_msaa; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ds[slot].tex))) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_ds[slot].tex, NULL, &s_ds[slot].dsv);
    s_ds[slot].va = PHYS(zva); s_ds[slot].w = s->width; s_ds[slot].h = s->height;
    return s_ds[slot].dsv ? &s_ds[slot] : NULL;
}

/* ---- state objects --------------------------------------------------------- */

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    }
    return D3D11_BLEND_OP_ADD;
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    }
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP sten_op(uint32_t o)
{
    switch (o) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static void *find_state(StateEnt *tab, int n, const uint32_t *key)
{
    int i;
    for (i = 0; i < n; i++)
        if (!memcmp(tab[i].key, key, sizeof tab[i].key)) return tab[i].obj;
    return NULL;
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *r)
{
    uint32_t key[12] = { r->blend_enable ? 1u : 0u, r->blend_src, r->blend_dst, r->blend_eq, r->color_mask };
    ID3D11BlendState *b = (ID3D11BlendState *)find_state(s_bs, s_nbs, key);
    D3D11_BLEND_DESC d;
    if (b) return b;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = r->blend_enable ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = blend_factor(r->blend_src, 0);
    d.RenderTarget[0].DestBlend = blend_factor(r->blend_dst, 0);
    d.RenderTarget[0].BlendOp = blend_op(r->blend_eq);
    d.RenderTarget[0].SrcBlendAlpha = blend_factor(r->blend_src, 1);
    d.RenderTarget[0].DestBlendAlpha = blend_factor(r->blend_dst, 1);
    d.RenderTarget[0].BlendOpAlpha = blend_op(r->blend_eq);
    d.RenderTarget[0].RenderTargetWriteMask =
        (uint8_t)(((r->color_mask & 0x00010000) ? 1 : 0) | ((r->color_mask & 0x100) ? 2 : 0) |
                  ((r->color_mask & 1) ? 4 : 0) | ((r->color_mask & 0x01000000) ? 8 : 0));
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &b))) return NULL;
    if (s_nbs < 96) { memcpy(s_bs[s_nbs].key, key, sizeof key); s_bs[s_nbs].obj = b; s_nbs++; }
    return b;
}

static ID3D11DepthStencilState *get_dss(const Nv2aRenderState *r, int have_ds)
{
    uint32_t key[12] = { (uint32_t)have_ds, r->depth_test_enable ? 1u : 0u, r->depth_func, r->depth_write ? 1u : 0u,
                         r->stencil_enable ? 1u : 0u, r->stencil_func, r->stencil_func_mask & 0xFF,
                         r->stencil_write_mask & 0xFF, r->stencil_fail, r->stencil_zfail, r->stencil_zpass, 0 };
    ID3D11DepthStencilState *d = (ID3D11DepthStencilState *)find_state(s_dss, s_ndss, key);
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_DEPTH_STENCILOP_DESC op;
    if (d) return d;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (have_ds && r->depth_test_enable) ? TRUE : FALSE;
    dd.DepthWriteMask = r->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = cmp_func(r->depth_func);
    dd.StencilEnable = (have_ds && r->stencil_enable) ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)(r->stencil_func_mask & 0xFF);
    dd.StencilWriteMask = (UINT8)(r->stencil_write_mask & 0xFF);
    op.StencilFailOp = sten_op(r->stencil_fail);
    op.StencilDepthFailOp = sten_op(r->stencil_zfail);
    op.StencilPassOp = sten_op(r->stencil_zpass);
    op.StencilFunc = cmp_func(r->stencil_func);
    dd.FrontFace = op; dd.BackFace = op;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &d))) return NULL;
    if (s_ndss < 96) { memcpy(s_dss[s_ndss].key, key, sizeof key); s_dss[s_ndss].obj = d; s_ndss++; }
    return d;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m)
{
    switch (m) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

static ID3D11SamplerState *get_sampler(const Nv2aTexture *t, int use_mips)
{
    /* SET_TEXTURE_FILTER: MIN in bits 16-23, MAG in 24-27; 1 = nearest. */
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    uint32_t key[12] = { minf == 1 ? 1u : 0u, magf == 1 ? 1u : 0u,
                         addr_mode(t->addr_u), addr_mode(t->addr_v), (uint32_t)s_aniso, (uint32_t)use_mips };
    ID3D11SamplerState *s = (ID3D11SamplerState *)find_state(s_ss, s_nss, key);
    D3D11_SAMPLER_DESC d;
    if (s) return s;
    memset(&d, 0, sizeof d);
    if (key[0] && key[1]) d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    else if (key[0]) d.Filter = D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    else if (key[1]) d.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    else d.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    d.MaxAnisotropy = 1;
    if (s_aniso > 1 && !key[0]) {   /* anisotropic filtering (F1): textures the title filters linearly */
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = (UINT)s_aniso;
    }
    if (use_mips && !key[0] && !key[1] && s_aniso == 1) d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = key[2]; d.AddressV = key[3]; d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = use_mips ? D3D11_FLOAT32_MAX : 0.0f; d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s))) return NULL;
    if (s_nss < (int)_countof(s_ss)) { memcpy(s_ss[s_nss].key, key, sizeof key); s_ss[s_nss].obj = s; s_nss++; }
    return s;
}

/* ---- textures ---------------------------------------------------------------- */

static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t px = t->width * t->height;
    unsigned block = d3d8_format_dxt_block_bytes(t->color);
    if (block) return ((t->width + 3) / 4) * ((t->height + 3) / 4) * block;
    switch (t->color) {
    case 0x00: case 0x01: case 0x0B: case 0x19: return px;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A: case 0x1B: case 0x28: return px * 2;
    }
    return px * 4;
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
    const uint64_t *w = (const uint64_t *)p;
    uint32_t i, q = n / 32;
    for (i = 0; i < q; i++, w += 4) {
        a = (a ^ w[0]) * K; a ^= a >> 29;
        b = (b ^ w[1]) * K; b ^= b >> 29;
        c = (c ^ w[2]) * K; c ^= c >> 29;
        d = (d ^ w[3]) * K; d ^= d >> 29;
    }
    for (i = q * 32; i < n; i++) { a = (a ^ p[i]) * K; a ^= a >> 29; }
    a ^= (b << 17) | (b >> 47); a *= K;
    a ^= (c << 31) | (c >> 33); a *= K;
    a ^= (d << 7) | (d >> 57); a *= K;
    return a ^ (a >> 32);
}

/* Cheap per-bind signature: full hash up to 32 KB, one word per KB above. */
static uint64_t sample_sig(const uint8_t *p, uint32_t n)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    const uint64_t *w = (const uint64_t *)p;
    uint32_t nw = n / 8, ns, step, i;
    uint64_t a = 0x452821E638D01377ull;
    if (n <= 32768 || nw < 2) return hash_bytes(p, n);
    ns = n / 1024; if (ns < 64) ns = 64; if (ns > 512) ns = 512;
    step = nw / ns;
    for (i = 0; i < ns; i++) { a = (a ^ w[(size_t)i * step]) * K; a ^= a >> 29; }
    a = (a ^ w[nw - 1]) * K; a ^= a >> 29;
    return a;
}

/* A texture read must land in mapped guest memory (64 MB RAM in its contiguous views). */
static int tex_range_ok(uint32_t va, uint32_t n)
{
    static const uint32_t base[3] = { 0x00000000u, 0x80000000u, 0xF0000000u };
    const uint32_t size = 0x04000000u;
    int i;
    for (i = 0; i < 3; i++)
        if (va - base[i] < size && n <= size - (va - base[i])) return 1;
    return 0;
}

static int tex_ok(const Nv2aTexture *t)
{
    if (t->width && t->width <= 4096 && t->height && t->height <= 4096 && tex_range_ok(t->offset, tex_bytes(t)))
        return 1;
    if (st_bad_tex++ < 20)
        fprintf(stderr, "[CS-D3D11] bad texture skipped: offset %08X %ux%u fmt %02X\n", t->offset, t->width, t->height, t->color);
    return 0;
}

static int tex_is_opaque_fmt(uint32_t color) { return color == 0x1E || color == 0x1F || color == 0x07; }

static int tex_matches(const TexEnt *e, const Nv2aTexture *t)
{
    return e->tex && e->offset == t->offset && e->w == t->width && e->h == t->height &&
           e->color == t->color && e->pitch == t->pitch;
}

static void release_texture(TexEnt *entry)
{
    s_pack_bytes -= entry->replacement_bytes;
    entry->replacement_bytes = 0;
    SAFE_REL(entry->srv);
    SAFE_REL(entry->tex);
}

static int pack_source_format(unsigned color)
{
    switch (color) {
    case 0x06: case 0x07: case 0x0c: case 0x0e: case 0x0f: case 0x3a: case 0x3c: return 1;
    default: return 0;
    }
}

/* The texture's original dimensions still determine its guest UV units. */
static ID3D11ShaderResourceView *get_tex(const Nv2aTexture *t, int *opaque, int *use_mips)
{
    int i, free_i = -1, lru = -1;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t n;
    uint64_t sig;
    int try_pack;
    TexEnt *e = NULL;

    *opaque = *use_mips = 0;
    if (!tex_ok(t)) return s_white_srv;
    *opaque = tex_is_opaque_fmt(t->color);

    /* A texture that is a render target we drew: sample it on the GPU, no readback needed. */
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].va == PHYS(t->offset) && s_rt[i].w == t->width && s_rt[i].h == t->height) {
            st_rtsamples++;
            rt_resolve(&s_rt[i]);
            return s_rt[i].srv;
        }

    n = tex_bytes(t);
    {   /* Direct-mapped index in front of the scan: the world binds a few
         * hundred textures a frame, and the linear search (with its LRU pick)
         * was ~10% of the executor thread. */
        static int16_t idx[4096];
        static int idx_init;
        uint32_t h = (t->offset * 2654435761u ^ t->width * 40503u ^ t->height * 9973u ^ t->color) >> 20;
        if (!idx_init) { memset(idx, 0xFF, sizeof idx); idx_init = 1; }
        if (idx[h] >= 0 && tex_matches(&s_tx[idx[h]], t)) {
            e = &s_tx[idx[h]];
        } else {
            for (i = 0; i < MAX_TEX; i++) {
                if (tex_matches(&s_tx[i], t)) { e = &s_tx[i]; break; }
                if (!s_tx[i].tex) { if (free_i < 0) free_i = i; }
                else if (lru < 0 || s_tx[i].used < s_tx[lru].used) lru = i;
            }
            if (e) idx[h] = (int16_t)(e - s_tx);
        }
    }
    /* Verified this frame already: the world binds the same lightmap and
     * detail textures hundreds of times a frame, and re-signing each bind (a
     * full hash under 32 KB) was ~9% of the executor thread. */
    if (e && e->verified == s_frame + 1) {
        e->used = s_tick;
        *use_mips = e->replacement_bytes != 0;
        return e->srv;
    }
    sig = sample_sig(mem + t->offset, n);
    if (e) {
        e->used = s_tick;
        if (e->sig == sig) {
            if (hash_bytes(mem + t->offset, n) == e->hash) {
                e->verified = s_frame + 1;
                *use_mips = e->replacement_bytes != 0;
                return e->srv;
            }
        }
        free_i = (int)(e - s_tx);                                /* content changed: decode into the same slot */
        release_texture(e);
    } else if (free_i < 0) {
        free_i = lru;
        release_texture(&s_tx[free_i]);
    }

    {
        uint32_t *argb = NULL;
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        TexEnt *ne = &s_tx[free_i];
        memset(ne, 0, sizeof *ne);
        try_pack = s_hires && pack_source_format(t->color) && cs_pack_enabled() && s_pack_bytes < CS_PACK_BUDGET;
        if (try_pack && cs_pack_load(s_dev, t->color, t->width, t->height, mem + t->offset, n,
                                     CS_PACK_BUDGET - s_pack_bytes,
                                     &ne->tex, &ne->srv, &ne->replacement_bytes))
            goto cache_texture;

        argb = malloc((size_t)t->width * t->height * 4);
        if (!argb) return s_white_srv;
        if (t->color == 0x28) {
            /* SZ_G8B8 (the toolkit's decoder has no case for it): two bytes a
             * texel, sampled as (R, G, B, A) = (lo, hi, lo, hi) -- xemu's
             * swizzle for this format. CS's buy menu wedges look their colours
             * up in one (dependent reads, cs_rc_hlsl.c). */
            const uint8_t *src = mem + t->offset;
            uint32_t mx, my, x, y;
            xbox_swizzle_masks(t->width, t->height, &mx, &my);
            for (y = 0; y < t->height; y++)
                for (x = 0; x < t->width; x++) {
                    const uint8_t *q = src + 2u * (swizzle_deposit(x, mx) | swizzle_deposit(y, my));
                    argb[(size_t)y * t->width + x] = ((uint32_t)q[1] << 24) | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[0];
                }
        } else if (!nv2a_backend_decode_texture(t, argb)) {
            if (st_decode_fail++ < 30)
                fprintf(stderr, "[CS-D3D11] texture decode failed fmt=%02X %ux%u offset=%08X\n", t->color, t->width, t->height, t->offset);
            free(argb);
            return s_white_srv;
        }
        {   /* RECOMP_CS_TEXDUMP=<dir>: every decoded texture (first 4000) as a BMP */
            static const char *tdir = (const char *)-1;
            static unsigned tn;
            if (tdir == (const char *)-1) tdir = getenv("RECOMP_CS_TEXDUMP");
            if (tdir && tn < 4000 && t->width <= 1024 && t->height <= 1024) {
                char path[MAX_PATH];
                FILE *f;
                uint32_t row = (t->width * 3 + 3) & ~3u, sz = 54 + row * t->height, yy, xx;
                uint8_t hdr[54] = { 'B', 'M' }, *line = (uint8_t *)calloc(1, row);
                int32_t nh = -(int32_t)t->height;
                size_t np = (size_t)t->width * t->height, kk, a0 = 0, aff = 0;
                tn++;
                for (kk = 0; kk < np; kk++) { if ((argb[kk] >> 24) == 0) a0++; else if ((argb[kk] >> 24) == 0xFF) aff++; }
                /* name carries the alpha mix: % of texels with alpha 0 and with alpha 255 */
                snprintf(path, sizeof path, "%s\\tex_%08X_fmt%02X_%ux%u_a0-%u_aff-%u.bmp", tdir, t->offset, t->color, t->width, t->height,
                         (unsigned)(a0 * 100 / np), (unsigned)(aff * 100 / np));
                f = fopen(path, "wb");
                if (f && line) {
                    memcpy(hdr + 2, &sz, 4); hdr[10] = 54; hdr[14] = 40;
                    memcpy(hdr + 18, &t->width, 4); memcpy(hdr + 22, &nh, 4); hdr[26] = 1; hdr[28] = 24;
                    fwrite(hdr, 1, 54, f);
                    for (yy = 0; yy < t->height; yy++) {
                        for (xx = 0; xx < t->width; xx++) {
                            uint32_t c = argb[(size_t)yy * t->width + xx];
                            line[xx * 3] = (uint8_t)c; line[xx * 3 + 1] = (uint8_t)(c >> 8); line[xx * 3 + 2] = (uint8_t)(c >> 16);
                        }
                        fwrite(line, 1, row, f);
                    }
                }
                if (f) fclose(f);
                free(line);
            }
        }
        if (tex_is_opaque_fmt(t->color)) {           /* no alpha channel: alpha is 1 */
            size_t k, np = (size_t)t->width * t->height;
            for (k = 0; k < np; k++) argb[k] |= 0xFF000000u;
        }
        if (try_pack && cs_pack_load(s_dev, CS_PACK_DECODED_BGRA, t->width, t->height, argb,
                                     (size_t)t->width * t->height * 4, CS_PACK_BUDGET - s_pack_bytes,
                                     &ne->tex, &ne->srv, &ne->replacement_bytes)) {
            free(argb);
            goto cache_texture;
        }
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = argb; sd.SysMemPitch = t->width * 4; sd.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &ne->tex))) {
            free(argb);
            return s_white_srv;
        }
        free(argb);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)ne->tex, NULL, &ne->srv);
        if (!ne->srv) { SAFE_REL(ne->tex); return s_white_srv; }
    cache_texture:
        s_pack_bytes += ne->replacement_bytes;
        *use_mips = ne->replacement_bytes != 0;
        ne->offset = t->offset; ne->w = t->width; ne->h = t->height; ne->color = t->color; ne->pitch = t->pitch;
        ne->hash = hash_bytes(mem + t->offset, n); ne->sig = sig;
        ne->verified = s_frame + 1; ne->used = s_tick;
        st_decodes++;
        return ne->srv;
    }
}

/* ---- Nv2aBackend ----------------------------------------------------------------- */

static void cs_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zst)
{
    Rt *rt;
    Ds *ds;
    (void)rs;
    if (!cs_ready()) return;
    drawlog(2, s, NULL, flags, argb, zst);
    rt = get_rt(s);
    if (!rt) return;
    if (flags & 0xF0) {
        float c[4];
        c[0] = ((argb >> 16) & 255) / 255.0f; c[1] = ((argb >> 8) & 255) / 255.0f;
        c[2] = (argb & 255) / 255.0f; c[3] = ((argb >> 24) & 255) / 255.0f;
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
        rt->dirty = 1; rt->unresolved = 1;
    }
    if ((flags & 3) && (ds = get_ds(s, rs->zeta_va)) != NULL) {
        UINT f = 0;
        if (flags & 1) f |= D3D11_CLEAR_DEPTH;
        if (flags & 2) f |= D3D11_CLEAR_STENCIL;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, ds->dsv, f, (float)(zst >> 8) / 16777215.0f, (UINT8)(zst & 0xFF));
    }
    st_clears++;
}

static int is_front(float area, uint32_t front_face)
{
    /* y grows downwards in surface pixels, so a positive area is clockwise. */
    return front_face == 0x901 ? area < 0.0f : area > 0.0f;
}

/* Specialised combiner pixel shaders (cs_rc_hlsl.c), cached by setup.
 * RECOMP_CS_RC_SPECIAL=0 keeps the generic interpreter; so does the
 * RECOMP_CS_RC_DEBUG view, which only the generic shader implements. */
#define RC_CACHE 512
typedef struct { uint64_t key; ID3D11PixelShader *ps; } RcEnt;
static RcEnt s_rcs[RC_CACHE];
static unsigned s_rcs_n, st_rc_compiles;

static ID3D11PixelShader *rc_shader(const Nv2aCombiner *rc)
{
    static int on = -1;
    static char src[64 * 1024];
    uint64_t key;
    unsigned i;
    size_t n;
    RcEnt *e;
    ID3DBlob *blob = NULL, *err = NULL;
    if (on < 0) { const char *v = getenv("RECOMP_CS_RC_SPECIAL"); on = !(v && v[0] == '0'); }
    if (!on) return s_ps_rc;
    key = cs_rc_key(rc);
    {   /* direct-mapped front, then the list */
        static int16_t front[1024];
        static int init;
        unsigned h = (unsigned)(key >> 54);
        if (!init) { memset(front, 0xFF, sizeof front); init = 1; }
        if (front[h] >= 0 && s_rcs[front[h]].key == key) return s_rcs[front[h]].ps ? s_rcs[front[h]].ps : s_ps_rc;
        for (i = 0; i < s_rcs_n; i++)
            if (s_rcs[i].key == key) { front[h] = (int16_t)i; return s_rcs[i].ps ? s_rcs[i].ps : s_ps_rc; }
        if (s_rcs_n == RC_CACHE) return s_ps_rc;
        e = &s_rcs[s_rcs_n];
        front[h] = (int16_t)s_rcs_n++;
    }
    e->key = key; e->ps = NULL;
    n = cs_rc_to_hlsl(rc, src, sizeof src);
    st_rc_compiles++;
    {   /* RECOMP_CS_RC_SRC=<dir>: keep each specialised shader's source, words first */
        const char *dir = getenv("RECOMP_CS_RC_SRC");
        if (dir && n) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s\\rc_%016llX.hlsl", dir, (unsigned long long)key);
            if ((f = fopen(path, "w")) != NULL) {
                uint32_t k;
                fprintf(f, "// control %08X program %08X final %08X %08X\n", rc->control, rc->stage_program, rc->final0, rc->final1);
                for (k = 0; k < 8; k++)
                    fprintf(f, "// stage %u icw %08X %08X ocw %08X %08X\n", k, rc->color_icw[k], rc->alpha_icw[k], rc->color_ocw[k], rc->alpha_ocw[k]);
                fwrite(src, 1, n, f);
                fclose(f);
            }
        }
    }
    if (n && SUCCEEDED(D3DCompile(src, n, "nv2a_rc", NULL, NULL, "main", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err)))
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &e->ps);
    if (!e->ps)
        fprintf(stderr, "[CS-D3D11] combiner %016llX: no specialised shader%s%s\n", (unsigned long long)key,
                err ? ": " : "", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
    SAFE_REL(blob); SAFE_REL(err);
    return e->ps ? e->ps : s_ps_rc;
}

/* Everything a draw binds apart from its vertex input and vertex shader:
 * constants, combiners, textures, samplers, blend/depth state, targets and
 * viewport. Shared by the CPU-transformed path (cs_draw) and the GPU vertex
 * program path (cs_draw_program). Returns whether the combiner shader is used;
 * srvs gets stage 0..3 (for the flash recorder). */
static int bind_state(const Nv2aSurface *s, const Nv2aBatch *b, Rt *rt, Ds *ds,
                      ID3D11ShaderResourceView *srvs[4])
{
    const Nv2aRenderState *r = b->state;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    Cb cb;
    ID3D11ShaderResourceView *srv = s_white_srv;
    ID3D11SamplerState *smp = NULL;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
    ID3D11Buffer *cbuf;
    float bf[4];
    int opaque = 0, use_rc = 0;
    RcCb rcb;
    ID3D11Buffer *rcbuf = NULL;
    ID3D11SamplerState *smps[4];

    srvs[0] = srvs[1] = srvs[2] = srvs[3] = s_white_srv;
    memset(&cb, 0, sizeof cb);
    cb.sw = (float)s->width; cb.sh = (float)s->height;
    cb.afunc = (!s_no_alpha && r->alpha_test_enable) ? r->alpha_func : 0x207;
    cb.aref = (float)(r->alpha_ref & 0xFF);
    if (b->texture) {
        int use_mips;
        srv = get_tex(b->texture, &opaque, &use_mips);
        smp = get_sampler(b->texture, use_mips);
        cb.textured = (srv != s_white_srv) ? 1u : 0u;
        cb.opaque = opaque ? 1u : 0u;
        cb.tw = (float)b->texture->width; cb.th = (float)b->texture->height;
    }
    if (s_force_a1) cb.opaque = 1u;
    { static int dbg = -1; if (dbg < 0) { const char *e = getenv("RECOMP_CS_RC_DEBUG"); dbg = e ? atoi(e) : 0; } cb.dbg = r->depth_test_enable ? (uint32_t)dbg : 0u; }
    if (!smp) { Nv2aTexture dflt; memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3; smp = get_sampler(&dflt, 0); }

    if (b->combiner && b->stages) {            /* 3D path: register combiners */
        const Nv2aCombiner *rc = b->combiner;
        int st;
        uint32_t k;
        use_rc = 1;
        memset(&rcb, 0, sizeof rcb);
        rcb.hdr[0] = rc->control; rcb.hdr[1] = rc->stage_program; rcb.hdr[2] = rc->final0; rcb.hdr[3] = rc->final1;
        rcb.hdr2[0] = rc->final_c0; rcb.hdr2[1] = rc->final_c1;       /* clip-plane stages: not modelled */
        memcpy(rcb.fogc, b->fog_color, sizeof rcb.fogc);
        for (k = 0; k < 8; k++) {
            rcb.stg[2 * k][0] = rc->color_icw[k]; rcb.stg[2 * k][1] = rc->alpha_icw[k];
            rcb.stg[2 * k][2] = rc->color_ocw[k]; rcb.stg[2 * k][3] = rc->alpha_ocw[k];
            rcb.stg[2 * k + 1][0] = rc->factor0[k]; rcb.stg[2 * k + 1][1] = rc->factor1[k];
        }
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *t = &b->stages[st];
            uint32_t mode = (rc->stage_program >> (st * 5)) & 0x1F;
            int op = 0;
            Nv2aTexture dflt;
            memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3;
            srvs[st] = s_white_srv;
            smps[st] = get_sampler(&dflt, 0);
            rcb.uvs[st][0] = rcb.uvs[st][1] = 1.0f;
            /* WRAP on both axes: wrap the coordinate in the shader (frac), as the
             * NV2A and the CPU rasteriser do exactly. CS's world detail stage
             * arrives with coordinates near +-1e6, where GPU texture units keep
             * no fractional precision and return a constant (dark) texel. */
            rcb.uvs[st][3] = (t->addr_u == 1 && t->addr_v == 1) ? 1.0f : 0.0f;
            if ((mode == 15 || mode == 16) && getenv("RECOMP_CS_DEPLOG")) {
                static unsigned nlog;
                if (nlog++ < 40)
                    fprintf(stderr, "[CS-DEP] stage %d mode %u valid %u tex %08X %ux%u fmt %02X pitch %u addr %u/%u filt %08X | t0 %08X %ux%u fmt %02X\n",
                            st, mode, (b->stage_valid >> st) & 1u, t->offset, t->width, t->height, t->color, t->pitch,
                            t->addr_u, t->addr_v, t->filter, b->stages[0].offset, b->stages[0].width, b->stages[0].height, b->stages[0].color);
            }
            if ((mode == 1 || mode == 2 || ((mode == 15 || mode == 16) && st > 0)) && (b->stage_valid & (1u << st))) {
                int use_mips;
                srvs[st] = get_tex(t, &op, &use_mips);
                smps[st] = get_sampler(t, use_mips && (mode == 1 || mode == 2));
                rcb.uvs[st][2] = (srvs[st] != s_white_srv) ? 1.0f : 0.0f;
                if (!(d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color)) && t->width && t->height) {
                    rcb.uvs[st][0] = 1.0f / (float)t->width;
                    rcb.uvs[st][1] = 1.0f / (float)t->height;
                }
            }
        }
        {   /* the same combiner setup as the last draw: keep its buffer */
            static RcCb last_rcb;
            static ID3D11Buffer *last_rcbuf;
            if (last_rcbuf && !memcmp(&last_rcb, &rcb, sizeof rcb)) {
                rcbuf = last_rcbuf;
            } else {
                rcbuf = s_rcr[s_rc_i++ % CB_RING];
                if (SUCCEEDED(MAP_W(rcbuf, D3D11_MAP_WRITE_DISCARD, &m))) {
                    memcpy(m.pData, &rcb, sizeof rcb);
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rcbuf, 0);
                }
                last_rcb = rcb; last_rcbuf = rcbuf;
            }
        }
    }

    {   /* likewise the per-draw constants */
        static Cb last_cb;
        static ID3D11Buffer *last_cbuf;
        if (last_cbuf && !memcmp(&last_cb, &cb, sizeof cb)) {
            cbuf = last_cbuf;
        } else {
            cbuf = s_cbr[s_cb_i++ % CB_RING];
            if (SUCCEEDED(MAP_W(cbuf, D3D11_MAP_WRITE_DISCARD, &m))) {
                memcpy(m.pData, &cb, sizeof cb);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cbuf, 0);
            }
            last_cb = cb; last_cbuf = cbuf;
        }
    }
    bs = get_blend(r);
    dss = get_dss(r, ds != NULL);
    bf[0] = ((r->blend_color >> 16) & 255) / 255.0f; bf[1] = ((r->blend_color >> 8) & 255) / 255.0f;
    bf[2] = (r->blend_color & 255) / 255.0f; bf[3] = ((r->blend_color >> 24) & 255) / 255.0f;

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, ds ? ds->dsv : NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, r->stencil_ref & 0xFF);
    vp.TopLeftX = vp.TopLeftY = s_pixofs * (float)s_scale;
    vp.Width = (float)(s->width * (uint32_t)s_scale); vp.Height = (float)(s->height * (uint32_t)s_scale);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(s_ctx, !use_rc ? s_ps : (cb.dbg ? s_ps_rc : rc_shader(b->combiner)), NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
    if (use_rc) {
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &rcbuf);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srvs);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smps);
    } else {
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &smp);
    }
    return use_rc;
}

/* RECOMP_CS_DRAWLOG=<file>: one 64-byte record per draw, clear and flip, every frame, so a frame that comes out
 * wrong can be set against its neighbours draw by draw (scripts/drawlog.py). kind: 0 CPU-path draw, 1 vertex-program
 * draw, 2 clear, 3 flip. */
typedef struct {
    uint32_t frame, kind, rt, rt_wh, zeta, tex, tex_fmt_wh, comb;   /* comb: combiner control ^ color_ocw[0] */
    uint32_t blend, alpha, depth, cull, cmask, count, kept, vkey;   /* packed state, see drawlog() */
} DrawLogRec;
static FILE *s_dlog;
static uint32_t s_dl_in_cpu, s_dl_in_gv, s_dl_no_rt, s_dl_none_kept;   /* this frame: draws that arrived, and why some were not drawn */

static void drawlog(uint32_t kind, const Nv2aSurface *s, const Nv2aBatch *b, uint32_t count, uint32_t kept,
                    uint32_t vkey)
{
    DrawLogRec d;
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_CS_DRAWLOG");
        init = 1;
        if (e && *e && (s_dlog = fopen(e, "wb")) != NULL) setvbuf(s_dlog, NULL, _IOFBF, 1 << 20);
    }
    if (!s_dlog) return;
    memset(&d, 0, sizeof d);
    d.frame = s_frame; d.kind = kind; d.count = count; d.kept = kept; d.vkey = vkey;
    if (s) { d.rt = s->color_va; d.rt_wh = (s->width << 16) | (s->height & 0xFFFF); }
    if (kind == 3) {   /* a flip: the executor's draw count, to tell commands never executed from draws dropped here */
        extern uint32_t xbox_Nv2aDrawCount(void);
        const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t dev = *(const volatile uint32_t *)(gm + 0x00317028u);   /* XDK D3D's device (read only, diagnostics) */
        d.tex = xbox_Nv2aDrawCount();
        if (dev >= 0x10000u && dev < 0x84000000u) {
            uint32_t gp = *(const volatile uint32_t *)(gm + dev + 0x30u);
            d.zeta = *(const volatile uint32_t *)(gm + dev + 0x2Cu);       /* CPU fence time */
            d.comb = (gp >= 0x10000u && gp < 0x84000000u) ? *(const volatile uint32_t *)(gm + gp) : 0xFFFFFFFFu;
            d.cmask = gp;                                                  /* where the GPU time is read */
        }
    }
    if (b) {
        const Nv2aRenderState *r = b->state;
        const Nv2aTexture *t = b->stages && (b->stage_valid & 1) ? &b->stages[0] : b->texture;
        d.zeta = r->zeta_va;
        if (t) { d.tex = t->offset; d.tex_fmt_wh = (t->color << 24) | ((t->width & 0xFFF) << 12) | (t->height & 0xFFF); }
        if (b->combiner) d.comb = b->combiner->control ^ b->combiner->color_ocw[0] ^ (b->combiner->final0 << 1);
        d.blend = (r->blend_enable ? 0x80000000u : 0) | ((r->blend_src & 0x7FFF) << 16) | (r->blend_dst & 0xFFFF);
        d.alpha = (r->alpha_test_enable ? 0x80000000u : 0) | ((r->alpha_func & 0xFFF) << 8) | (r->alpha_ref & 0xFF);
        d.depth = (r->depth_test_enable ? 0x80000000u : 0) | (r->depth_write ? 0x40000000u : 0) | ((r->depth_func & 0xFFF) << 16)
                | (r->stencil_enable ? 0x8000u : 0) | ((uint32_t)(r->depth_max * 255.0f) & 0xFF) | (((uint32_t)(r->depth_min * 255.0f) & 0x7F) << 8);
        d.cull = (r->cull_enable ? 0x80000000u : 0) | ((r->cull_face & 0xFFFF) << 8) | (r->front_face & 0xFF);
        d.cmask = r->color_mask;
    }
    fwrite(&d, sizeof d, 1, s_dlog);
    if (kind == 3 && (s_frame % 30) == 0) fflush(s_dlog);
}

static void cs_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Rt *rt;
    Ds *ds;
    const Nv2aRenderState *r = b->state;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    uint32_t i, kept = 0, need;
    Nv2aVertex *dst;
    Cb cb;
    ID3D11ShaderResourceView *srv = s_white_srv;
    ID3D11SamplerState *smp = NULL;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
    ID3D11Buffer *cbuf, *vb;
    float bf[4];
    UINT stride = sizeof(Nv2aVertex), off = 0;
    int opaque = 0, use_rc = 0;
    RcCb rcb;
    ID3D11Buffer *rcbuf = NULL;
    ID3D11ShaderResourceView *srvs[4];
    ID3D11SamplerState *smps[4];

    if (!cs_ready() || !b->count) return;
    s_dl_in_cpu++;
    rt = get_rt(s);
    if (!rt) { s_dl_no_rt++; return; }
    ds = get_ds(s, r->zeta_va);
    {   /* RECOMP_CS_TRACE=N: describe the first N draws of every 100th frame */
        static int tn = -1;
        if (tn < 0) { const char *e = getenv("RECOMP_CS_TRACE"); tn = e ? atoi(e) : 0; }
        if (tn > 0 && (s_frame % 100) == 50) {
            static uint32_t lastf = ~0u; static int k;
            if (lastf != s_frame) { lastf = s_frame; k = 0; }
            if (k++ < tn) {
                const Nv2aVertex *v = &b->vertices[0];
                fprintf(stderr, "[CS-TRACE] f%u rt=%08X %ux%u n=%u tex=%s %ux%u fmt=%02X off=%08X | v0 (%.1f,%.1f,z%.0f,rhw%g) c=%08X uv=(%.1f,%.1f) | bl=%u %X/%X dt=%u/%X/%u at=%u/%X/%u cull=%u/%X/%X zeta=%08X cm=%08X\n",
                        s_frame, s->color_va, s->width, s->height, b->count,
                        b->texture ? "Y" : "n", b->texture ? b->texture->width : 0, b->texture ? b->texture->height : 0,
                        b->texture ? b->texture->color : 0, b->texture ? b->texture->offset : 0,
                        v->x, v->y, v->z, v->rhw, v->diffuse, v->u, v->v,
                        r->blend_enable, r->blend_src, r->blend_dst, r->depth_test_enable, r->depth_func, r->depth_write,
                        r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->cull_enable, r->cull_face, r->front_face, r->zeta_va, r->color_mask);
            }
        }
    }

    need = b->count * (uint32_t)sizeof(Nv2aVertex);
    if (need > VB_BYTES) {
        if (st_vb_drop++ < 40)
            fprintf(stderr, "[CS-D3D11] vb drop: n=%u need=%u max=%u combiner=%d\n",
                    b->count, need, VB_BYTES, b->combiner ? 1 : 0);
        return;
    }
    if (s_vb_off + need > VB_BYTES) { s_vb_off = 0; s_vb_i = (s_vb_i + 1) % VB_RING; }
    vb = s_vbr[s_vb_i];
    if (FAILED(MAP_W(vb, s_vb_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m)))
        return;
    dst = (Nv2aVertex *)((uint8_t *)m.pData + s_vb_off);
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *v = &b->vertices[i];
        float area;
        if (!(v[0].rhw > 0 && v[1].rhw > 0 && v[2].rhw > 0)) continue;
        area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (!(fabsf(area) > 0.0f) || !isfinite(area)) continue;
        if (s_cull_on && r->cull_enable) {
            int front = is_front(area, r->front_face);
            if ((r->cull_face == 0x405 && !front) || (r->cull_face == 0x404 && front) || r->cull_face == 0x408) {
                st_culled++;
                continue;
            }
        }
        dst[kept] = v[0]; dst[kept + 1] = v[1]; dst[kept + 2] = v[2];
        /* Workaround for toolkit diffuse.a=0, remove when patch 0005 lands.
         * World batches combine T0.a * V0.a; zero V0.a fails GREATER/127 /
         * SRC_ALPHA blend. Lift only pure-zero alpha (gated by
         * RECOMP_CS_ALPHA_LIFT, default on). */
        if (b->combiner && s_alpha_lift) {
            int k;
            for (k = 0; k < 3; k++)
                if ((dst[kept + k].diffuse & 0xFF000000u) == 0)
                    dst[kept + k].diffuse |= 0xFF000000u;
        }
        kept += 3;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)vb, 0);

    /* RECOMP_CS_PICK=x,y[,frame]: in one frame (default 700), name every
     * depth-tested draw with a triangle covering surface pixel (x,y) -- which
     * draw paints a spot, with its combiner, stage textures and coordinates. */
    {
        static int pk = -1;
        float px, py;
        static unsigned pframe = 700, plines;
        if (pk < 0) {
            const char *e = getenv("RECOMP_CS_PICK");
            pframe = 0;   /* default: the frames dumped by RECOMP_CS_DUMP (probe -DumpAt) */
            pk = (e && sscanf(e, "%f,%f,%u", &s_pick_x, &s_pick_y, &pframe) >= 2) ? 1 : 0;
        }
        px = s_pick_x; py = s_pick_y;
        s_pick_hit = 0;
        if (pk && (pframe ? s_frame == pframe : s_dump_now) && plines < 80) {
            uint32_t q;
            for (q = 0; q + 2 < b->count; q += 3) {
                const Nv2aVertex *a = &b->vertices[q], *bb = &b->vertices[q + 1], *c = &b->vertices[q + 2];
                float d1 = (bb->x - a->x) * (py - a->y) - (bb->y - a->y) * (px - a->x);
                float d2 = (c->x - bb->x) * (py - bb->y) - (c->y - bb->y) * (px - bb->x);
                float d3 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
                if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) {
                    int st;
                    plines++;
                    s_pick_hit = 1;
                    fprintf(stderr, "[PICK] f%u draw n=%u tri#%u rt=%08X z=%.0f c=%08X bl=%u %X/%X at=%u/%X/%u dt=%u/%X/%u rc=%s ctrl=%08X sp=%08X cicw0=%08X tex=%08X fmt=%02X %ux%u\n",
                            s_frame, b->count, q / 3, s->color_va, a->z, a->diffuse, r->blend_enable, r->blend_src, r->blend_dst,
                            r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->depth_test_enable, r->depth_func, r->depth_write,
                            b->combiner ? "yes" : "no", b->combiner ? b->combiner->control : 0, b->combiner ? b->combiner->stage_program : 0,
                            b->combiner ? b->combiner->color_icw[0] : 0, b->texture ? b->texture->offset : 0,
                            b->texture ? b->texture->color : 0, b->texture ? b->texture->width : 0, b->texture ? b->texture->height : 0);
                    for (st = 0; b->stages && st < 3; st++)
                        fprintf(stderr, "[PICK]    st%d fmt=%02X %ux%u off=%08X addr=%u/%u filt=%08X uv=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)\n", st,
                                b->stages[st].color, b->stages[st].width, b->stages[st].height, b->stages[st].offset,
                                b->stages[st].addr_u, b->stages[st].addr_v, b->stages[st].filter,
                                a->tex[st][0], a->tex[st][1], bb->tex[st][0], bb->tex[st][1], c->tex[st][0], c->tex[st][1]);
                    break;
                }
            }
        }
    }

    /* RECOMP_CS_WORLD_TRACE=1: dump every 3D batch for one in-game frame.
     * Arm on the first depth-tested, non-UI (z>1) combiner batch after the
     * loading screen (frame>=600). */
    if (s_world_trace && b->combiner && b->count >= 3) {
        static uint32_t wt_frame = 0, wt_armed;
        float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
        float zmin = 1e30f, zmax = -1e30f, rmin = 1e30f, rmax = -1e30f;
        uint32_t j, culled_here = (b->count / 3) - (kept / 3);
        if (!wt_armed && s_frame >= (uint32_t)s_world_trace && r->depth_test_enable && b->vertices[0].z > 1.0f && b->count >= 30u) {
            wt_armed = 1; wt_frame = s_frame;
            fprintf(stderr, "[CS-WORLD] armed at frame %u n=%u\n", s_frame, b->count);
        }
        if (wt_armed && s_frame == wt_frame) {
            for (j = 0; j < b->count; j++) {
                const Nv2aVertex *v = &b->vertices[j];
                if (v->x < xmin) xmin = v->x; if (v->x > xmax) xmax = v->x;
                if (v->y < ymin) ymin = v->y; if (v->y > ymax) ymax = v->y;
                if (v->z < zmin) zmin = v->z; if (v->z > zmax) zmax = v->z;
                if (v->rhw < rmin) rmin = v->rhw; if (v->rhw > rmax) rmax = v->rhw;
            }
                    fprintf(stderr,
                    "[CS-WORLD] f%u n=%u kept=%u culled=%u bb=(%.1f,%.1f)-(%.1f,%.1f) z=[%.0f,%.0f] rhw=[%g,%g] "
                    "cull=%u/%X/%X dt=%u/%X/%u zeta=%08X bl=%u %X/%X at=%u/%X/%u cm=%08X "
                    "sp=%08X f0=%08X f1=%08X valid=%X ctrl=%08X "
                    "cicw0=%08X cocw0=%08X aicw0=%08X aocw0=%08X\n",
                    s_frame, b->count, kept, culled_here,
                    xmin, ymin, xmax, ymax, zmin, zmax, rmin, rmax,
                    r->cull_enable, r->cull_face, r->front_face,
                    r->depth_test_enable, r->depth_func, r->depth_write, r->zeta_va,
                    r->blend_enable, r->blend_src, r->blend_dst,
                    r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->color_mask,
                    b->combiner->stage_program, b->combiner->final0, b->combiner->final1,
                    b->stage_valid, b->combiner->control,
                    b->combiner->color_icw[0], b->combiner->color_ocw[0],
                    b->combiner->alpha_icw[0], b->combiner->alpha_ocw[0]);
            {
                uint32_t k;
                for (k = 1; k < (b->combiner->control & 0xFFu) && k < 8; k++)
                    fprintf(stderr, "[CS-WORLD]   rc%u cicw=%08X cocw=%08X aicw=%08X aocw=%08X k0=%08X k1=%08X\n", k,
                            b->combiner->color_icw[k], b->combiner->color_ocw[k], b->combiner->alpha_icw[k],
                            b->combiner->alpha_ocw[k], b->combiner->factor0[k], b->combiner->factor1[k]);
                {   /* first vertices of a 3-stage (world) batch: screen position, base and lightmap coordinates */
                    uint32_t q;
                    if ((b->combiner->control & 0xFFu) >= 3u)
                        for (q = 0; q < b->count && q < 9; q++)
                            fprintf(stderr, "[CS-WORLD]     vtx%u xy=(%.1f,%.1f) t0=(%.4f,%.4f,%.3f,%.3f) t1=(%.4f,%.4f) c=%08X\n", q,
                                    b->vertices[q].x, b->vertices[q].y, b->vertices[q].tex[0][0], b->vertices[q].tex[0][1],
                                    b->vertices[q].tex[0][2], b->vertices[q].tex[0][3], b->vertices[q].tex[1][0], b->vertices[q].tex[1][1],
                                    b->vertices[q].diffuse);
                    if ((b->combiner->control & 0xFFu) >= 3u) {   /* the world program's constants c[96..106] */
                        uint32_t ci;
                        {   /* where in guest RAM does c[105]'s value (and c[98]'s) live? */
                            static int searched;
                            const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                            uint32_t which, a, hits;
                            if (!searched) {
                                searched = 1;
                                for (which = 98; which <= 105; which += 7) {
                                    const float *cv = nv2a_vsh_constant(which);
                                    hits = 0;
                                    for (a = 0x10000; a + 16 <= 0x04000000u && hits < 12; a += 4)
                                        if (!memcmp(gm + a, cv, 16)) {
                                            fprintf(stderr, "[CS-WORLD]     c[%u] value found in guest RAM at %08X\n", which, a);
                                            hits++;
                                        }
                                    if (!hits) fprintf(stderr, "[CS-WORLD]     c[%u] value not in guest RAM\n", which);
                                }
                            }
                        }
                        for (ci = 96; ci <= 106; ci++) {
                            const float *cv = nv2a_vsh_constant(ci);
                            fprintf(stderr, "[CS-WORLD]     c[%u] = %g %g %g %g  (t2 of vtx0 %g %g)\n", ci, cv[0], cv[1], cv[2], cv[3],
                                    b->vertices[0].tex[2][0], b->vertices[0].tex[2][1]);
                        }
                    }
                }
                fprintf(stderr, "[CS-WORLD]   rc0 k0=%08X k1=%08X fc0=%08X fc1=%08X v0=(%g,%g,%g,%g) c=%08X t0=(%g,%g,%g,%g) t1=(%g,%g,%g,%g)\n",
                        b->combiner->factor0[0], b->combiner->factor1[0], b->combiner->final_c0, b->combiner->final_c1,
                        b->vertices[0].x, b->vertices[0].y, b->vertices[0].z, b->vertices[0].rhw, b->vertices[0].diffuse,
                        b->vertices[0].tex[0][0], b->vertices[0].tex[0][1], b->vertices[0].tex[0][2], b->vertices[0].tex[0][3],
                        b->vertices[0].tex[1][0], b->vertices[0].tex[1][1], b->vertices[0].tex[1][2], b->vertices[0].tex[1][3]);
            }
            if (b->stages) {
                int st;
                for (st = 0; st < 4; st++) {
                    const Nv2aTexture *t = &b->stages[st];
                    uint32_t mode = (b->combiner->stage_program >> (st * 5)) & 0x1F;
                    fprintf(stderr, "[CS-WORLD]   st%d mode=%u valid=%u fmt=%02X %ux%u off=%08X pitch=%u\n",
                            st, mode, (b->stage_valid >> st) & 1u,
                            t->color, t->width, t->height, t->offset, t->pitch);
                }
            }
        }
    }

    if (!kept) { s_dl_none_kept++; return; }

    use_rc = bind_state(s, b, rt, ds, srvs);
    ID3D11DeviceContext_RSSetState(s_ctx, rs_offset(s_rs, 0, 0, b->state));
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(s_ctx, use_rc ? s_vs_rc : s_vs, NULL, 0);
    ID3D11DeviceContext_Draw(s_ctx, kept, s_vb_off / (UINT)sizeof(Nv2aVertex));
    if (s_pick_hit) {   /* RECOMP_CS_PICK: the pixel's value after this draw */
        static ID3D11Texture2D *one;
        D3D11_MAPPED_SUBRESOURCE pm;
        D3D11_BOX box;
        if (!one) {
            D3D11_TEXTURE2D_DESC td;
            memset(&td, 0, sizeof td);
            td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &one);
        }
        box.left = (UINT)s_pick_x; box.top = (UINT)s_pick_y; box.right = box.left + 1; box.bottom = box.top + 1; box.front = 0; box.back = 1;
        if (one && s_scale == 1 && s_msaa == 1 && box.right <= rt->w && box.bottom <= rt->h) {
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)one, 0, 0, 0, 0, (ID3D11Resource *)rt->tex, 0, &box);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)one, 0, D3D11_MAP_READ, 0, &pm))) {
                fprintf(stderr, "[PICK]    -> pixel now %08X\n", *(const uint32_t *)pm.pData);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)one, 0);
            }
        }
        s_pick_hit = 0;
    }
    s_vb_off += kept * (uint32_t)sizeof(Nv2aVertex);
    rt->dirty = 1; rt->unresolved = 1;
    st_draws++; st_tris += kept / 3;
    s_fr_draws++; s_fr_tris += kept / 3;
    drawlog(0, s, b, b->count, kept, 0);
    if (s_flash_dir && s_dln[s_dlc] < DL_MAX) {
        DrawRec *d = &s_dl[s_dlc][s_dln[s_dlc]++];
        const Nv2aVertex *v = &b->vertices[0];
        memset(d, 0, sizeof *d);
        d->rt = s->color_va; d->rw = s->width; d->rh = s->height; d->n = b->count; d->kept = kept;
        if (b->texture) { d->tex = b->texture->offset; d->fmt = b->texture->color; d->tw = b->texture->width; d->th = b->texture->height; }
        d->comb = b->combiner ? 1u : 0u;
        d->bl = r->blend_enable; d->bsrc = r->blend_src; d->bdst = r->blend_dst;
        d->at = r->alpha_test_enable; d->afunc = r->alpha_func; d->aref = r->alpha_ref;
        d->dt = r->depth_test_enable; d->cm = r->color_mask; d->c0 = v->diffuse;
        d->x0 = v->x; d->y0 = v->y; d->z0 = v->z; d->rhw0 = v->rhw; d->u0 = v->u; d->v0 = v->v;
        { int q; for (q = 0; q < 3 && q < (int)b->count; q++) {
            d->vx[q][0] = b->vertices[q].x; d->vx[q][1] = b->vertices[q].y; d->vx[q][2] = b->vertices[q].z; d->vx[q][3] = b->vertices[q].rhw;
            memcpy(d->vt[q], b->vertices[q].tex[0], sizeof d->vt[q]); } }
        if (b->combiner) {
            const Nv2aCombiner *c = b->combiner;
            d->rc_ctl = c->control; d->rc_sp = c->stage_program; d->rc_f0 = c->final0; d->rc_f1 = c->final1;
            d->rc_cicw = c->color_icw[0]; d->rc_aicw = c->alpha_icw[0]; d->rc_cocw = c->color_ocw[0]; d->rc_aocw = c->alpha_ocw[0];
            d->rc_k0 = c->factor0[0]; d->st_valid = b->stage_valid;
            if (b->stages) { d->st0_fmt = b->stages[0].color; d->st0_off = b->stages[0].offset; d->st0_w = b->stages[0].width; d->st0_h = b->stages[0].height; }
            d->srv_kind = use_rc ? (srvs[0] == s_white_srv ? 0 : 2) : -1;
            { int q; for (q = 0; q < MAX_RT; q++) if (use_rc && s_rt[q].srv && srvs[0] == s_rt[q].srv) d->srv_kind = 1; }
        }
    }
}

/* ---- frame timing log (RECOMP_CS_FRAME_CSV=<file>) ------------------------------
 * One line per guest flip: time since process start, interval to the previous
 * flip, draws and triangles of the frame, readback time, and a signature of the
 * display-sized (640x480) target: mean luma, share of near-black pixels, and a
 * hash of a sub-sampled grid. A flash shows up as a frame whose luma or black
 * share jumps against both neighbours; a stall as a long interval. */
static FILE *s_csv;
static double s_sig_luma, s_sig_black;
static uint64_t s_sig_hash;
static int s_sig_valid;

double cs_now_ms(void)
{
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        /* Absolute QPC ms of t_ms 0, to line up logs on the raw QPC clock
         * (the APU traces) with frames.csv and samples.csv. */
        fprintf(stderr, "[CLOCK] t0_qpc_ms=%.1f\n", (double)(t0.QuadPart / f.QuadPart) * 1000.0
                + (double)(t0.QuadPart % f.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static void frame_signature(const D3D11_MAPPED_SUBRESOURCE *m, uint32_t w, uint32_t h)
{
    uint32_t x, y, n = 0, nblack = 0;
    uint64_t sum = 0, hsh = 1469598103934665603ULL;
    for (y = 2; y < h; y += 4) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)m->pData + (size_t)y * m->RowPitch);
        for (x = 2; x < w; x += 4) {
            uint32_t c = row[x] & 0xFFFFFFu;
            uint32_t l = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8;
            sum += l; n++;
            if (l < 16) nblack++;
            hsh = (hsh ^ c) * 1099511628211ULL;
        }
    }
    s_sig_luma = n ? (double)sum / n : 0;
    s_sig_black = n ? 100.0 * nblack / n : 0;
    s_sig_hash = hsh;
    s_sig_valid = 1;
}

/* Map a staging copy and write it into guest memory. */
static void rb_write(const Rt *rt)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t y, x, w = rt->w, h = rt->h, pitch = rt->pitch, bpp = rt->bpp;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rt->stage, 0, D3D11_MAP_READ, 0, &m))) return;
    if ((s_csv || s_flash_dir) && w == 640 && h == 480 && bpp == 4) frame_signature(&m, w, h);
    if (bpp == 4 && pitch >= w * 4) {
        for (y = 0; y < h; y++)
            memcpy(mem + rt->rawva + (size_t)y * pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    } else if (bpp == 2 && pitch >= w * 2) {
        for (y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint16_t *d = (uint16_t *)(mem + rt->rawva + (size_t)y * pitch);
            for (x = 0; x < w; x++) {
                uint32_t c = src[x];
                d[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rt->stage, 0);
}

/* RECOMP_CS_DUMP=<dir>: while the file <dir>\dump.flag exists, write each
 * dirty render target as <dir>\rt_<frame>_<va>_<w>x<h>.bmp at every present. */
static void dump_rt_bmp(const Rt *rt, const char *dir)
{
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH];
    FILE *f;
    uint32_t y, x, row = (rt->w * 3 + 3) & ~3u;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t sz = 54 + row * rt->h;
    uint8_t *line;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rt->stage, 0, D3D11_MAP_READ, 0, &m))) return;
    snprintf(path, sizeof path, "%s\\rt_%05u_%08X_%ux%u.bmp", dir, s_frame, rt->va, rt->w, rt->h);
    f = fopen(path, "wb");
    if (f) {
        memcpy(hdr + 2, &sz, 4); hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &rt->w, 4); { int32_t nh = -(int32_t)rt->h; memcpy(hdr + 22, &nh, 4); }
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        line = (uint8_t *)calloc(1, row);
        for (y = 0; line && y < rt->h; y++) {
            const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < rt->w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
            fwrite(line, 1, row, f);
        }
        free(line);
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rt->stage, 0);
}

/* Called by the executor at FLIP_STALL, just before it snapshots guest memory
 * for the window: finish the GPU work and write every drawn target back. */
static void cs_present_sync(void)
{
    int i, nrb = 0, readi[MAX_RT];
    static const char *dump_dir = (const char *)-1;
    static double prev_black = -1;
    static unsigned flashes;
    static double last_flip;
    double t0 = cs_now_ms(), t1;
    int dumping = 0;
    if (!s_inited || s_failed) return;
    s_sig_valid = 0;
    if (dump_dir == (const char *)-1) dump_dir = getenv("RECOMP_CS_DUMP");
    if (dump_dir) {
        char flag[MAX_PATH];
        snprintf(flag, sizeof flag, "%s\\dump.flag", dump_dir);
        dumping = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
    }
    s_dump_now = dumping;
    if (dumping) {   /* first dump frame: the whole 64 MB of guest RAM as <dir>\ram.bin, laid out
                      * like xemu's physical.ram, for scripts/ramtex.py and scripts/pbfind.py */
        static int ram_done;
        if (!ram_done) {
            char path[MAX_PATH];
            FILE *f;
            ram_done = 1;
            snprintf(path, sizeof path, "%s\\ram.bin", dump_dir);
            if ((f = fopen(path, "wb")) != NULL) {   /* the 0x80000000 (physical) view: what the GPU reads */
                fwrite((const uint8_t *)xbox_GetMemoryOffset() + 0x80000000u, 1, 0x04000000u, f);
                fclose(f);
                fprintf(stderr, "[CS-D3D11] guest RAM written to %s at frame %u\n", path, s_frame);
            }
            /* and the title's low virtual range, like xemu's va_game_00010000.bin */
            snprintf(path, sizeof path, "%s\\va_game_00010000.bin", dump_dir);
            if ((f = fopen(path, "wb")) != NULL) {
                fwrite((const uint8_t *)xbox_GetMemoryOffset() + 0x10000u, 1, 0x7D0000u, f);
                fclose(f);
            }
        }
    }
    {
        /* Normal play: only the display-sized target (what the window shows)
         * goes back to guest memory, and one flip late: map the copy queued at
         * the previous flip -- long finished on the GPU -- then queue this
         * one's. Copying and mapping in one go stalled this thread on the GPU
         * every flip (10-19 ms in game). Other targets (the 640x960 back
         * buffer, glow buffers) are sampled on the GPU and never read here.
         * Dumps, flash capture and RECOMP_CS_RB_ALL=1 keep the synchronous
         * all-targets path below. */
        static int rb_all = -1, fb_dump = -1;
        if (rb_all < 0) { const char *e = getenv("RECOMP_CS_RB_ALL"); rb_all = e && *e == '1'; }
        if (fb_dump < 0) fb_dump = getenv("RECOMP_FB_DUMP") != NULL;
        if (s_presenting && !dumping && !s_flash_dir && !rb_all && !fb_dump) {
            /* Our presenter shows the frame straight from the GPU: nothing
             * needs it in guest memory. */
            for (i = 0; i < MAX_RT; i++) s_rt[i].dirty = 0;
            ID3D11DeviceContext_Flush(s_ctx);
            goto done;
        }
        if (!dumping && !s_flash_dir && !rb_all) {
            for (i = 0; i < MAX_RT; i++) {
                Rt *rt = &s_rt[i];
                if (!rt->tex) continue;
                if (rt->pend) { rb_write(rt); rt->pend = 0; readi[nrb] = i; st_rb++; nrb++; }
                if (rt->dirty && rt->stage && rt->w == 640 && rt->h == 480) {
                    rt_resolve(rt);
                    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->stage, (ID3D11Resource *)rt->tex);
                    rt->pend = 1;
                }
                rt->dirty = 0;
            }
            /* Submit the frame now. Left in the driver's command buffer, the
             * GPU started this frame only when the next flip's Map forced it,
             * and that Map then waited for the whole frame (~9 ms in game). */
            ID3D11DeviceContext_Flush(s_ctx);
            goto done;
        }
    }
    for (i = 0; i < MAX_RT; i++) {
        Rt *rt = &s_rt[i];
        if (!rt->tex || !rt->dirty || !rt->stage) continue;   /* no stage: scaled, no copy back */
        rt_resolve(rt);
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->stage, (ID3D11Resource *)rt->tex);
        if (dumping) dump_rt_bmp(rt, dump_dir);
        rb_write(rt);
        rt->dirty = 0;
        readi[nrb] = i;
        st_rb++; nrb++;
    }
    if (s_flash_dir && s_sig_valid) {
        if (prev_black >= 0 && prev_black < 20.0 && s_sig_black >= 95.0 && s_fr_draws > 2 && flashes < 10) {
            unsigned k, f;
            flashes++;
            fprintf(stderr, "[FLASH] frame %u at %.1f ms: black %.0f%% after %.0f%%; %u draws. Previous frame, then this one:\n",
                    s_frame, t0, s_sig_black, prev_black, s_fr_draws);
            for (f = 0; f < 2; f++) {
                unsigned which = f == 0 ? (s_dlc ^ 1u) : s_dlc;
                fprintf(stderr, "[FLASH]  %s frame (%u draws)\n", f == 0 ? "previous" : "black", s_dln[which]);
                for (k = 0; k < s_dln[which]; k++) {
                    const DrawRec *d = &s_dl[which][k];
                    fprintf(stderr, "[FLASH]   #%u rt=%08X %ux%u n=%u kept=%u tex=%08X fmt=%02X %ux%u rc=%u bl=%u %X/%X at=%u/%X/%u dt=%u cm=%08X v0=(%.1f,%.1f,z%.0f,rhw%g) c=%08X uv=(%.1f,%.1f)\n",
                            k, d->rt, d->rw, d->rh, d->n, d->kept, d->tex, d->fmt, d->tw, d->th, d->comb, d->bl, d->bsrc, d->bdst,
                            d->at, d->afunc, d->aref, d->dt, d->cm, d->x0, d->y0, d->z0, d->rhw0, d->c0, d->u0, d->v0);
                    if (d->rw == 640 && d->rh == 480)
                        fprintf(stderr, "[FLASH]       rc ctl=%08X sp=%08X f0=%08X f1=%08X s0 cicw=%08X aicw=%08X cocw=%08X aocw=%08X k0=%08X valid=%X st0 fmt=%02X off=%08X %ux%u srv=%s\n",
                                d->rc_ctl, d->rc_sp, d->rc_f0, d->rc_f1, d->rc_cicw, d->rc_aicw, d->rc_cocw, d->rc_aocw, d->rc_k0,
                                d->st_valid, d->st0_fmt, d->st0_off, d->st0_w, d->st0_h,
                                d->srv_kind == 1 ? "rendertarget" : d->srv_kind == 2 ? "decoded" : d->srv_kind == 0 ? "WHITE" : "n/a");
                    if (d->rw == 640 && d->rh == 480)
                        fprintf(stderr, "[FLASH]       tri (%.1f,%.1f,%g,%g)/(%.1f,%.1f,%g,%g)/(%.1f,%.1f,%g,%g) tex0 (%g,%g,%g,%g)/(%g,%g,%g,%g)/(%g,%g,%g,%g)\n",
                                d->vx[0][0], d->vx[0][1], d->vx[0][2], d->vx[0][3], d->vx[1][0], d->vx[1][1], d->vx[1][2], d->vx[1][3],
                                d->vx[2][0], d->vx[2][1], d->vx[2][2], d->vx[2][3], d->vt[0][0], d->vt[0][1], d->vt[0][2], d->vt[0][3],
                                d->vt[1][0], d->vt[1][1], d->vt[1][2], d->vt[1][3], d->vt[2][0], d->vt[2][1], d->vt[2][2], d->vt[2][3]);
                }
            }
            fflush(stderr);
            for (k = 0; k < (unsigned)nrb; k++) dump_rt_bmp(&s_rt[readi[k]], s_flash_dir);
        }
        prev_black = s_sig_black;
    }
    if (s_flash_dir) { s_dlc ^= 1u; s_dln[s_dlc] = 0; }
done:
    if (s_csv) {
        t1 = cs_now_ms();
        if (s_sig_valid)
            fprintf(s_csv, "%.2f,%u,%.2f,%u,%u,%d,%.3f,%.1f,%.2f,%016llX,%u,%u\n", t0, s_frame, last_flip ? t0 - last_flip : 0.0,
                    s_fr_draws, s_fr_tris, nrb, t1 - t0, s_sig_luma, s_sig_black, (unsigned long long)s_sig_hash,
                    s_fr_gv, s_fr_fog);
        else
            fprintf(s_csv, "%.2f,%u,%.2f,%u,%u,%d,%.3f,,,,%u,%u\n", t0, s_frame, last_flip ? t0 - last_flip : 0.0,
                    s_fr_draws, s_fr_tris, nrb, t1 - t0, s_fr_gv, s_fr_fog);
        last_flip = t0;
    }
    s_fr_draws = s_fr_tris = s_fr_gv = s_fr_fog = 0;
}

/* GPU time per frame (RECOMP_CS_D3D11_LOG): timestamp queries around each
 * flip-to-flip interval, read four frames later so nothing waits. */
static double s_gpu_ms_sum;
static unsigned s_gpu_ms_n;
static unsigned st_gv_draws, st_gv_fallback;   /* GPU vertex program draws (below) */

static void gpu_timer_flip(void)
{
    enum { N = 4 };
    static ID3D11Query *dj[N], *ts[N];
    static int k, started[N], ok = -1;
    D3D11_QUERY_DESC qd;
    int i;
    if (ok < 0) {
        ok = 1;
        for (i = 0; i < N && ok; i++) {
            qd.MiscFlags = 0;
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &dj[i]))) ok = 0;
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (ok && FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &ts[i]))) ok = 0;
        }
    }
    if (!ok) return;
    /* close the interval opened at the previous flip */
    {
        int p = (k + N - 1) % N;
        if (started[p] == 1) {
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)dj[p]);
            started[p] = 2;
        }
    }
    /* the slot we are about to reuse: read its result (four frames old) */
    if (started[k] == 2) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
        UINT64 t0 = 0, t1 = 0;
        int nxt = (k + 1) % N;
        if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)dj[k], &d, sizeof d, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)ts[k], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)ts[nxt], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && !d.Disjoint && d.Frequency && t1 > t0) {
            s_gpu_ms_sum += (double)(t1 - t0) * 1000.0 / (double)d.Frequency;
            s_gpu_ms_n++;
        }
        started[k] = 0;
    }
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)dj[k]);
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)ts[k]);
    started[k] = 1;
    k = (k + 1) % N;
}

/* Flips presented so far: the freeze watch (cs_diag.c) sees the game is alive while this moves. */
unsigned cs_d3d11_frames(void) { return s_frame; }

static void cs_flip(void)
{
    static DWORD last;
    DWORD now;
    if (!s_inited || s_failed) return;
    if (s_log) gpu_timer_flip();
    drawlog(3, NULL, NULL, s_dl_in_cpu, s_dl_in_gv, (s_dl_no_rt << 16) | (s_dl_none_kept & 0xFFFF));
    s_dl_in_cpu = s_dl_in_gv = s_dl_no_rt = s_dl_none_kept = 0;
    s_frame++; s_tick++; st_flips++;
    if (s_csv && (s_frame % 30) == 0) fflush(s_csv);   /* runs are often ended by killing the process */
    now = GetTickCount();
    if (s_log && now - last >= 2000) {
        fprintf(stderr, "[CS-D3D11] frame=%u flips=%u draws=%u (gpu-vsh %u, cpu-fallback %u) tris=%u culled=%u clears=%u texdecodes=%u rtsamples=%u readbacks=%u vbdrop=%u gpu_ms/frame=%.2f\n",
                s_frame, st_flips, st_draws, st_gv_draws, st_gv_fallback, st_tris, st_culled, st_clears, st_decodes, st_rtsamples, st_rb, st_vb_drop,
                s_gpu_ms_n ? s_gpu_ms_sum / s_gpu_ms_n : 0.0);
        st_flips = st_draws = st_tris = st_culled = st_clears = st_decodes = st_rtsamples = st_rb = 0;
        st_gv_draws = st_gv_fallback = 0; s_gpu_ms_sum = 0; s_gpu_ms_n = 0;
        last = now;
    }
}

/* ---- GPU vertex programs (RECOMP_CS_GPU_VSH, default on) -----------------
 *
 * The executor hands program batches over untransformed (toolkit patch 0016):
 * the program, c[0..191], each distinct vertex's inputs and a triangle list.
 * The program is translated to an HLSL vertex shader once (cs_vsh_hlsl.c,
 * cached by content) and runs on the GPU; the pixel side is the same combiner
 * shader and state as the CPU path. A program that fails to translate or
 * compile, or a batch that does not fit, goes back to the CPU path. */
#define GV_IN_ROWS  (2u * 1024u * 1024u)        /* float4 input ring: 32 MB */
#define GV_IB_IDX   (2u * 1024u * 1024u)        /* uint32 index ring: 8 MB */
#define GV_CACHE    128

typedef struct { uint64_t key; ID3D11VertexShader *vs; int failed; } GvEnt;
typedef struct { uint32_t in_base, in_n; float aa[2]; uint32_t fog_en, fog_mode; float fog_p[2]; } GvCb;

static GvEnt s_gv[GV_CACHE];
static unsigned s_gv_n;
static ID3D11Buffer *s_gv_in, *s_gv_ib, *s_gv_cb, *s_gv_pcb;
static ID3D11ShaderResourceView *s_gv_srv;
static ID3D11RasterizerState *s_gv_rs[3][2];    /* none/front/back x front-CCW */

/* The rasterizer state for a draw: base (culling, winding) plus the title's
 * polygon offset when it enables one. CS draws its wall decals (the A/B site
 * marks, sprays, bullet holes) in the wall's plane with D3D's ZBIAS; without
 * the offset they z-fight with the wall and flicker as the view moves. NV2A's
 * bias is in units of depth resolution and the depth buffer is 24-bit unorm,
 * so it maps onto DepthBias one to one; the scale factor onto the slope. */
static ID3D11RasterizerState *rs_offset(ID3D11RasterizerState *base, int cull, int ccw, const Nv2aRenderState *r)
{
    typedef struct { ID3D11RasterizerState *rs; int cull, ccw; float factor, units; } RsEnt;
    static RsEnt cache[32];
    static unsigned n, nlog;
    D3D11_RASTERIZER_DESC rd;
    unsigned i;
    static int off = -1;
    if (off < 0) { const char *v = getenv("RECOMP_CS_ZBIAS"); off = v && v[0] == '0'; }   /* =0: ignore it */
    if (off || !r->poly_offset_fill || (r->poly_offset_factor == 0.0f && r->poly_offset_units == 0.0f))
        return base;
    for (i = 0; i < n; i++)
        if (cache[i].cull == cull && cache[i].ccw == ccw && cache[i].factor == r->poly_offset_factor &&
            cache[i].units == r->poly_offset_units)
            return cache[i].rs;
    if (n == 32) return base;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.DepthClipEnable = TRUE;
    rd.CullMode = cull == 0 ? D3D11_CULL_NONE : (cull == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK);
    rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
    rd.DepthBias = (INT)floorf(r->poly_offset_units + 0.5f);
    rd.SlopeScaledDepthBias = r->poly_offset_factor;
    if (nlog++ < 8)
        fprintf(stderr, "[CS-D3D11] polygon offset factor %g units %g\n", r->poly_offset_factor, r->poly_offset_units);
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &cache[n].rs))) return base;
    cache[n].cull = cull; cache[n].ccw = ccw; cache[n].factor = r->poly_offset_factor; cache[n].units = r->poly_offset_units;
    return cache[n++].rs;
}
static uint32_t s_gv_in_off, s_gv_ib_off;       /* ring positions (rows, indices) */
static int s_gv_ready = -1;

static int gv_init(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    D3D11_RASTERIZER_DESC rd;
    int c, f;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = GV_IN_ROWS * 16u; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_gv_in))) return 0;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0; sv.Buffer.NumElements = GV_IN_ROWS;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_gv_in, &sv, &s_gv_srv))) return 0;
    bd.ByteWidth = GV_IB_IDX * 4u; bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_gv_ib))) return 0;
    bd.ByteWidth = 192u * 16u; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_gv_cb))) return 0;
    bd.ByteWidth = (UINT)((sizeof(GvCb) + 15u) & ~15u);
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_gv_pcb))) return 0;
    for (c = 0; c < 3; c++)
        for (f = 0; f < 2; f++) {
            memset(&rd, 0, sizeof rd);
            rd.FillMode = D3D11_FILL_SOLID; rd.DepthClipEnable = TRUE;
            rd.CullMode = c == 0 ? D3D11_CULL_NONE : (c == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK);
            rd.FrontCounterClockwise = f ? TRUE : FALSE;
            if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_gv_rs[c][f]))) return 0;
        }
    return 1;
}

static uint64_t gv_key(const Nv2aProgramDraw *p)
{
    uint64_t h = 1469598103934665603ull ^ p->input_mask;
    uint32_t i, k;
    for (i = 0; i < p->program_count; i++)
        for (k = 0; k < 4; k++) h = (h ^ p->program[i][k]) * 1099511628211ull;
    return h;
}

static ID3D11VertexShader *gv_shader(const Nv2aProgramDraw *p)
{
    static char src[256 * 1024];
    uint64_t key = gv_key(p);
    ID3DBlob *blob = NULL, *err = NULL;
    GvEnt *e = NULL;
    unsigned i;
    size_t n;

    for (i = 0; i < s_gv_n; i++)
        if (s_gv[i].key == key) return s_gv[i].failed ? NULL : s_gv[i].vs;
    if (s_gv_n == GV_CACHE) {           /* full: forget the lot (titles use a handful) */
        for (i = 0; i < s_gv_n; i++) SAFE_REL(s_gv[i].vs);
        s_gv_n = 0;
    }
    e = &s_gv[s_gv_n++];
    memset(e, 0, sizeof *e);
    e->key = key;
    n = cs_vsh_to_hlsl(p->program, p->program_count, p->input_mask, src, sizeof src);
    if (!n || FAILED(D3DCompile(src, n, "nv2a_vsh", NULL, NULL, "main", "vs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))
        || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                                  ID3D10Blob_GetBufferSize(blob), NULL, &e->vs))) {
        e->failed = 1;
        fprintf(stderr, "[CS-D3D11] vertex program %016llX (%u instructions): no GPU shader%s%s\n",
                (unsigned long long)key, p->program_count, err ? ": " : "",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        { const char *d = getenv("RECOMP_CS_GPU_VSH_DUMP"); if (d) { FILE *f = fopen(d, "w"); if (f) { fwrite(src, 1, n, f); fclose(f); } } }
    } else if (s_log) {
        fprintf(stderr, "[CS-D3D11] vertex program %016llX: %u instructions on the GPU (%u cached)\n",
                (unsigned long long)key, p->program_count, s_gv_n);
    }
    SAFE_REL(blob); SAFE_REL(err);
    return e->failed ? NULL : e->vs;
}

static int cs_draw_program(const Nv2aSurface *s, const Nv2aBatch *b, const Nv2aProgramDraw *p)
{
    const Nv2aRenderState *r = b->state;
    s_dl_in_gv++;
    ID3D11VertexShader *vs;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srvs[4];
    ID3D11Buffer *cbs[2];
    uint32_t nin = 0, rows, k;
    GvCb g;
    Rt *rt;
    Ds *ds;
    int use_rc, cull;

    if (s_gv_ready < 0) {
        const char *e = getenv("RECOMP_CS_GPU_VSH");
        s_gv_ready = (e && e[0] == '0') ? 0 : -2;   /* -2: wanted, not created yet */
    }
    if (!s_gv_ready || !cs_ready() || !b->combiner || !b->stages) { st_gv_fallback++; return 0; }
    if (s_gv_ready == -2) {
        s_gv_ready = gv_init();
        fprintf(stderr, "[CS-D3D11] GPU vertex programs %s\n", s_gv_ready ? "on" : "unavailable (CPU path)");
        if (!s_gv_ready) return 0;
    }
    for (k = 0; k < 16; k++) nin += (p->input_mask >> k) & 1u;
    rows = p->vertex_count * nin;
    if (!nin || rows > GV_IN_ROWS || p->index_count > GV_IB_IDX || !p->index_count) { st_gv_fallback++; return 0; }
    if (r->cull_enable && r->cull_face == 0x408) return 1;           /* FRONT_AND_BACK: nothing drawn */
    if (!(vs = gv_shader(p))) { st_gv_fallback++; return 0; }
    rt = get_rt(s);
    if (!rt) return 1;
    ds = get_ds(s, r->zeta_va);

    /* Inputs and indices into their rings; constants and per-draw values. */
    s_gv_in_off = (s_gv_in_off + nin - 1) / nin * nin;   /* vertex-aligned: indices carry the base */
    if (s_gv_in_off + rows > GV_IN_ROWS) s_gv_in_off = 0;
    if (FAILED(MAP_W(s_gv_in, s_gv_in_off ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) return 0;
    memcpy((uint8_t *)m.pData + (size_t)s_gv_in_off * 16u, p->inputs, (size_t)rows * 16u);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_gv_in, 0);
    if (s_gv_ib_off + p->index_count > GV_IB_IDX) s_gv_ib_off = 0;
    if (FAILED(MAP_W(s_gv_ib, s_gv_ib_off ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) return 0;
    {
        uint32_t *dst = (uint32_t *)m.pData + s_gv_ib_off, base = s_gv_in_off / nin, q;
        for (q = 0; q < p->index_count; q++) dst[q] = p->indices[q] + base;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_gv_ib, 0);
    {   /* c[0..191] only when they changed since the last program draw */
        static float last_c[192][4];
        static int have_c;
        if (!have_c || memcmp(last_c, p->constants, sizeof last_c)) {
            if (FAILED(MAP_W(s_gv_cb, D3D11_MAP_WRITE_DISCARD, &m))) return 0;
            memcpy(m.pData, p->constants, sizeof last_c);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_gv_cb, 0);
            memcpy(last_c, p->constants, sizeof last_c);
            have_c = 1;
        }
    }
    memset(&g, 0, sizeof g);
    g.in_base = 0; g.in_n = nin;
    g.aa[0] = (float)(s->aa_sx ? s->aa_sx : 1); g.aa[1] = (float)(s->aa_sy ? s->aa_sy : 1);
    s_fr_gv++; if (p->fog_enable) s_fr_fog++;
    g.fog_en = p->fog_enable; g.fog_mode = p->fog_mode; g.fog_p[0] = p->fog_param[0]; g.fog_p[1] = p->fog_param[1];
    {
        static GvCb last_g;
        static int have_g;
        if (!have_g || memcmp(&last_g, &g, sizeof g)) {
            if (FAILED(MAP_W(s_gv_pcb, D3D11_MAP_WRITE_DISCARD, &m))) return 0;
            memcpy(m.pData, &g, sizeof g);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_gv_pcb, 0);
            last_g = g; have_g = 1;
        }
    }

    use_rc = bind_state(s, b, rt, ds, srvs);
    (void)use_rc;
    /* Culling on the GPU, by the winding the strips were assembled with. Surface
     * y grows downwards, as in the D3D11 viewport, so NV2A's CW is D3D's CW. */
    cull = !r->cull_enable ? 0 : (r->cull_face == 0x404 ? 1 : (r->cull_face == 0x405 ? 2 : 0));
    if (!s_cull_on) cull = 0;
    ID3D11DeviceContext_RSSetState(s_ctx, rs_offset(s_gv_rs[cull][r->front_face == 0x901 ? 1 : 0], cull, r->front_face == 0x901, r));
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_gv_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    cbs[0] = s_gv_cb; cbs[1] = s_gv_pcb;
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 2, 2, cbs);
    ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &s_gv_srv);
    ID3D11DeviceContext_DrawIndexed(s_ctx, p->index_count, s_gv_ib_off, 0);
    s_gv_in_off += rows;
    s_gv_ib_off += p->index_count;
    rt->dirty = 1; rt->unresolved = 1;
    st_draws++; st_gv_draws++; st_tris += p->index_count / 3;
    s_fr_draws++; s_fr_tris += p->index_count / 3;
    drawlog(1, s, b, p->index_count, p->vertex_count, (uint32_t)gv_key(p));
    return 1;
}

/* ---- presenting to the window, and the player's settings ------------------
 *
 * The toolkit's window used to show the frame from guest memory (a 640x480
 * copy read back from the GPU every flip). Now this device presents the
 * title's front buffer straight into a swap chain on that window at each flip
 * (xbox_FramebufferSetPresenter), so the picture keeps the render scale and
 * anti-aliasing it was drawn with, and nothing is read back. On top goes the
 * video overlay (the intro movies), composited by the toolkit into a 640x480
 * frame and drawn with its alpha.
 *
 * Settings (F1 menu, config.exe; cs_settings.h): render scale and MSAA remake
 * every target between frames; anisotropy remakes the samplers; hi-res textures empties the texture cache; stretch, vsync
 * and the frame-rate cap are read here at each present. The ESC/F1 menus are
 * a separate window over this one (cs_overlay.c): the game keeps drawing. */
static IDXGISwapChain1 *s_sc;
static ID3D11RenderTargetView *s_bb_rtv;
static UINT s_bb_w, s_bb_h;
static int s_tearing;
static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps, *s_blit_ps_a;
static ID3D11SamplerState *s_blit_smp;
static ID3D11BlendState *s_blit_alpha;
static ID3D11Texture2D *s_ov_tex;
static ID3D11ShaderResourceView *s_ov_srv;
static uint32_t *s_ov_buf;
static int s_ov_valid;                  /* the overlay texture holds a frame to draw */
static volatile LONG s_settings_dirty;

static const char s_blit_hlsl[] =
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vs(uint i : SV_VertexID) { VO o; float2 uv = float2((i << 1) & 2, i & 2); o.uv = uv;\n"
"  o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 ps(VO i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n"
"float4 ps_a(VO i) : SV_Target { return t.Sample(s, i.uv); }\n";

void cs_d3d11_settings_changed(void) { InterlockedExchange(&s_settings_dirty, 1); }

/* The most samples, up to `want`, the card has for both the colour and the depth format. */
static int msaa_supported(int want)
{
    int n;
    for (n = want > 8 ? 8 : want; n > 1; n >>= 1) {
        UINT qc = 0, qd = 0;
        ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_B8G8R8A8_UNORM, (UINT)n, &qc);
        ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_D24_UNORM_S8_UINT, (UINT)n, &qd);
        if (qc > 0 && qd > 0) return n;
    }
    return 1;
}

/* Between frames, on the executor thread: make the targets and samplers match the settings. */
static void settings_apply(int first)
{
    int scale = g_cs_settings.scale < 1 ? 1 : g_cs_settings.scale > CS_SET_SCALES ? CS_SET_SCALES : g_cs_settings.scale;
    int msaa = msaa_supported(g_cs_settings.msaa), aniso = g_cs_settings.aniso, i;
    if (aniso < 1) aniso = 1;
    if (aniso > 16) aniso = 16;
    if (scale != s_scale || msaa != s_msaa) {
        for (i = 0; i < MAX_RT; i++) if (s_rt[i].tex || s_rt[i].ms) rt_release(&s_rt[i]);
        for (i = 0; i < MAX_DS; i++) { SAFE_REL(s_ds[i].dsv); SAFE_REL(s_ds[i].tex); memset(&s_ds[i], 0, sizeof s_ds[i]); }
        s_scale = scale; s_msaa = msaa;
    }
    if (aniso != s_aniso) {
        for (i = 0; i < s_nss; i++) { ID3D11SamplerState *o = (ID3D11SamplerState *)s_ss[i].obj; SAFE_REL(o); }
        s_nss = 0;
        s_aniso = aniso;
    }
    if ((g_cs_settings.hires != 0) != s_hires) {
        /* Hi-res textures on/off: empty the texture cache; each texture is
         * decoded (or its replacement loaded) again at its next bind. */
        for (i = 0; i < MAX_TEX; i++) { release_texture(&s_tx[i]); memset(&s_tx[i], 0, sizeof s_tx[i]); }
        s_hires = g_cs_settings.hires != 0;
    }
    if (first || s_log)
        fprintf(stderr, "[CS-D3D11] render scale %dx (%ux%u), msaa %d%s, anisotropy %d, hi-res textures %d\n", s_scale,
                640u * (unsigned)s_scale, 480u * (unsigned)s_scale, s_msaa,
                msaa != g_cs_settings.msaa ? " (the most this card has)" : "", s_aniso, s_hires);
}

static int blit_init(void)
{
    static int done, ok;
    ID3DBlob *b = NULL, *err = NULL;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bd;
    D3D11_TEXTURE2D_DESC td;
    if (done) return ok;
    done = 1;
    if (SUCCEEDED(D3DCompile(s_blit_hlsl, sizeof s_blit_hlsl - 1, "blit", NULL, NULL, "vs", "vs_4_0", 0, 0, &b, &err))) {
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
        SAFE_REL(b);
    }
    if (SUCCEEDED(D3DCompile(s_blit_hlsl, sizeof s_blit_hlsl - 1, "blit", NULL, NULL, "ps", "ps_4_0", 0, 0, &b, &err))) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
        SAFE_REL(b);
    }
    if (SUCCEEDED(D3DCompile(s_blit_hlsl, sizeof s_blit_hlsl - 1, "blit", NULL, NULL, "ps_a", "ps_4_0", 0, 0, &b, &err))) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps_a);
        SAFE_REL(b);
    }
    SAFE_REL(err);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX; sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_blit_smp);
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_blit_alpha);
    memset(&td, 0, sizeof td);
    td.Width = 640; td.Height = 480; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ov_tex)))
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ov_tex, NULL, &s_ov_srv);
    s_ov_buf = (uint32_t *)malloc(640u * 480u * 4u);
    ok = s_blit_vs && s_blit_ps && s_blit_ps_a && s_blit_smp && s_blit_alpha && s_ov_srv && s_ov_buf;
    if (!ok) fprintf(stderr, "[CS-D3D11] presenter shaders failed; the window keeps the old path\n");
    return ok;
}

/* A swap chain on the window, its back buffer the window's client size. */
static int swapchain_ensure(HWND h)
{
    RECT cr;
    UINT w, hh;
    if (!s_sc) {
        IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory2 *f = NULL;
        DXGI_SWAP_CHAIN_DESC1 d;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&f))) {
            IDXGIFactory5 *f5 = NULL;
            BOOL tear = FALSE;
            if (SUCCEEDED(IDXGIFactory2_QueryInterface(f, &IID_IDXGIFactory5, (void **)&f5))) {
                if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear)))
                    tear = FALSE;
                SAFE_REL(f5);
            }
            s_tearing = tear ? 1 : 0;
            memset(&d, 0, sizeof d);
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.Scaling = DXGI_SCALING_STRETCH;
            d.Flags = s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown *)s_dev, h, &d, NULL, NULL, &s_sc);
            if (SUCCEEDED(hr))
                IDXGIFactory2_MakeWindowAssociation(f, h, DXGI_MWA_NO_ALT_ENTER);   /* F11 / Alt+Enter are the window's */
        }
        SAFE_REL(f); SAFE_REL(ad); SAFE_REL(dd);
        if (FAILED(hr)) {
            fprintf(stderr, "[CS-D3D11] swap chain on the window failed 0x%08lX\n", (unsigned long)hr);
            return 0;
        }
        fprintf(stderr, "[CS-D3D11] presenting to the window (tearing %s)\n", s_tearing ? "available" : "not available");
    }
    GetClientRect(h, &cr);
    w = (UINT)(cr.right > 0 ? cr.right : 1); hh = (UINT)(cr.bottom > 0 ? cr.bottom : 1);
    if (!s_bb_rtv || w != s_bb_w || hh != s_bb_h) {
        ID3D11Texture2D *bb = NULL;
        SAFE_REL(s_bb_rtv);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_sc, 0, w, hh, DXGI_FORMAT_UNKNOWN,
                                                 s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
            return 0;
        if (SUCCEEDED(IDXGISwapChain1_GetBuffer(s_sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_bb_rtv);
            SAFE_REL(bb);
        }
        s_bb_w = w; s_bb_h = hh;
    }
    return s_bb_rtv != NULL;
}

/* The title's front buffer (and the video overlay) on the window. `fresh`:
 * compose the video overlay again (else the last one is drawn). */
static void present_frame(HWND h, uint32_t fb_va, int fresh)
{
    Rt *rt = NULL;
    D3D11_VIEWPORT vp;
    float black[4] = { 0, 0, 0, 1 };
    int i;
    UINT sync, flags;
    ID3D11ShaderResourceView *none = NULL;

    if (!swapchain_ensure(h)) return;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].va == PHYS(fb_va) && (!rt || s_rt[i].w == 640)) rt = &s_rt[i];
    {   /* Which surfaces get presented: each new one is logged once, and how often each was shown */
        static uint32_t seen_va[16], seen_n[16];
        static int nseen;
        int k;
        for (k = 0; k < nseen && seen_va[k] != PHYS(fb_va); k++) {}
        if (k == nseen && nseen < 16) {
            seen_va[nseen] = PHYS(fb_va); seen_n[nseen] = 0; nseen++;
            fprintf(stderr, "[CS-D3D11] present: new surface %08X (%ux%u%s) at frame %u\n", PHYS(fb_va),
                    rt ? rt->w : 0, rt ? rt->h : 0, rt ? "" : ", no target", s_frame);
        }
        if (k < nseen && ++seen_n[k] % 600 == 0 && s_log)
            for (k = 0; k < nseen; k++)
                fprintf(stderr, "[CS-D3D11] present: %08X shown %u times\n", seen_va[k], seen_n[k]);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_bb_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_bb_rtv, black);
    {   /* the picture: 4:3 in the middle, or the whole window with Stretch */
        float aw = (float)s_bb_w, ah = (float)s_bb_h;
        float fw = rt ? (float)rt->w : 640.0f, fh = rt ? (float)rt->h : 480.0f;
        vp.MinDepth = 0; vp.MaxDepth = 1;
        if (g_cs_settings.stretch) { vp.TopLeftX = vp.TopLeftY = 0; vp.Width = aw; vp.Height = ah; }
        else if (aw * fh > ah * fw) { vp.Height = ah; vp.Width = ah * fw / fh; vp.TopLeftX = (aw - vp.Width) * 0.5f; vp.TopLeftY = 0; }
        else { vp.Width = aw; vp.Height = aw * fh / fw; vp.TopLeftX = 0; vp.TopLeftY = (ah - vp.Height) * 0.5f; }
    }
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
    if (rt) {
        rt_resolve(rt);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &rt->srv);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    }
    if (fresh) {   /* the movie overlay, composed by the toolkit: drawn pixels opaque, the rest clear */
        const uint32_t key = 0x00010203u;
        uint32_t n;
        for (n = 0; n < 640u * 480u; n++) s_ov_buf[n] = key;
        s_ov_valid = xbox_FramebufferComposeOverlay(s_ov_buf);
        if (s_ov_valid) {
            for (n = 0; n < 640u * 480u; n++)
                s_ov_buf[n] = s_ov_buf[n] == key ? 0u : (s_ov_buf[n] | 0xFF000000u);
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_ov_tex, 0, NULL, s_ov_buf, 640u * 4u, 0);
        }
    }
    if (s_ov_valid) {
        ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blit_alpha, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps_a, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_ov_srv);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    }
    if (s_dlog) {   /* RECOMP_CS_DRAWLOG: the frame number as 16 squares top left (white = 1), to match screen grabs */
        static ID3D11ShaderResourceView *black;
        int bit;
        if (!black) {
            D3D11_TEXTURE2D_DESC td; D3D11_SUBRESOURCE_DATA sd; ID3D11Texture2D *t = NULL; uint32_t px = 0xFF000000u;
            memset(&td, 0, sizeof td); td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE; sd.pSysMem = &px; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))) {
                ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &black);
                t->lpVtbl->Release(t);
            }
        }
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        for (bit = 0; bit < 16 && black; bit++) {
            D3D11_VIEWPORT q;
            q.TopLeftX = vp.TopLeftX + 4.0f + bit * 12.0f; q.TopLeftY = vp.TopLeftY + 4.0f; q.Width = q.Height = 10.0f;
            q.MinDepth = 0; q.MaxDepth = 1;
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &q);
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, ((s_frame >> bit) & 1) ? &s_white_srv : &black);
            ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        }
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    sync = g_cs_settings.vsync ? 1u : 0u;
    flags = (!sync && s_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    IDXGISwapChain1_Present(s_sc, sync, flags);
}

/* The frame-rate cap (settings: 30..240, 0 = none): wait out the rest of the frame. */
static void frame_cap(void)
{
    static LARGE_INTEGER f, next;
    static int timer_res;
    LARGE_INTEGER now;
    LONGLONG period;
    int hz = g_cs_settings.frame_rate;
    if (hz <= 0) { next.QuadPart = 0; return; }
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    if (!timer_res) { timer_res = 1; timeBeginPeriod(1); }
    period = f.QuadPart / hz;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > period * 2) next.QuadPart = now.QuadPart;
    next.QuadPart += period;
    for (;;) {
        LONGLONG left;
        QueryPerformanceCounter(&now);
        left = next.QuadPart - now.QuadPart;
        if (left <= 0) break;
        if (left * 1000 / f.QuadPart >= 2) Sleep((DWORD)(left * 1000 / f.QuadPart - 1));
        else YieldProcessor();
    }
}

/* The flip (xbox_FramebufferSetPresenter), on the executor thread. */
static void cs_window_present(uint32_t fb_va, uint32_t pitch)
{
    static HWND s_icon_for;
    HWND h = (HWND)xbox_FramebufferWindowHwnd();
    (void)pitch;
    if (h && h != s_icon_for && !(GetWindowLongPtrA(h, GWL_STYLE) & WS_CHILD)) {
        /* the toolkit's window has no icon: give it the game's (resources/cs.rc); inside the frame
         * (src/cs_frame.c) it is a child and the frame has the icon */
        HINSTANCE self = GetModuleHandleA(NULL);
        HANDLE big = LoadImageA(self, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON), 0);
        HANDLE little = LoadImageA(self, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
        if (big) PostMessageA(h, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (little) PostMessageA(h, WM_SETICON, ICON_SMALL, (LPARAM)little);
        s_icon_for = h;
    }
    if (!s_inited || s_failed || !h || !blit_init()) return;
    s_presenting = 1;
    present_frame(h, fb_va, 1);
    if (InterlockedExchange(&s_settings_dirty, 0)) settings_apply(0);
    frame_cap();
}

static const Nv2aBackend s_backend = { cs_clear, cs_draw, cs_flip };

void cs_d3d11_init(void)
{
    const char *e = getenv("RECOMP_CS_D3D11");
    if (e && e[0] == '0') {
        fprintf(stderr, "[CS-D3D11] disabled (RECOMP_CS_D3D11=0): toolkit CPU rasteriser\n");
        return;
    }
    {
        const char *csv = getenv("RECOMP_CS_FRAME_CSV");
        s_flash_dir = getenv("RECOMP_CS_FLASH");
        /* Append: the title relaunches itself (menus -> game, and on every map
         * change), and a "w" here erased everything the previous process
         * logged. The header goes in once, into an empty file. */
        if (csv && (s_csv = fopen(csv, "a")) != NULL) {
            setvbuf(s_csv, NULL, _IOFBF, 1 << 16);
            fseek(s_csv, 0, SEEK_END);
            if (ftell(s_csv) == 0)
                fprintf(s_csv, "t_ms,frame,dt_ms,draws,tris,readbacks,rb_ms,luma,black_pct,hash,gv,fog\n");
        }
    }
    cs_now_ms();
    {   /* RECOMP_CS_PRESENT=0: leave the window to the toolkit (guest-memory copy, 1x only) */
        const char *p = getenv("RECOMP_CS_PRESENT");
        if (getenv("RECOMP_FB_WINDOW") && !(p && *p == '0'))
            xbox_FramebufferSetPresenter(cs_window_present);
    }
    nv2a_backend_register_with_present_sync(&s_backend, cs_present_sync);
    nv2a_backend_register_program(cs_draw_program);
}
