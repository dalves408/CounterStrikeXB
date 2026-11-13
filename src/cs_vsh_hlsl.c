/*
 * NV2A vertex program -> HLSL vertex shader, for the D3D11 back end's GPU
 * vertex path (cs_d3d11.c). The translation mirrors the toolkit's CPU
 * interpreter (tools/xboxrecomp/src/kernel/nv2a_vsh_interp.c) instruction for
 * instruction: the same field layout, the same "both units read before either
 * writes" rule, the ILU writing R1 when paired with a MAC op, R13..R15 reading
 * R0, the same RCC clamp and LIT/EXP/LOG results, the same output clamp of the
 * colours and the same defaults for unwritten outputs. Where the CPU path
 * then turns oPos into surface pixels (x, y times the anti-aliasing scale; a
 * +0 w read as 1) and oFog.x into a fog factor, the epilogue does the same.
 *
 * Inputs come from a Buffer<float4>: vertex `vid` at in_base + vid * in_n, one
 * row per attribute the program reads, in v0..v15 order.
 */
#include "cs_vsh_hlsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, len; int overflow; } Out;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (o->overflow) return;
    va_start(ap, fmt);
    k = vsnprintf(o->p + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= o->n - o->len) { o->overflow = 1; return; }
    o->len += (size_t)k;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* Source k of an instruction as an HLSL float4 expression. */
static void src_expr(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[64];

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(base, sizeof base, "R[%u]", reg <= 12 ? reg : 0);
    else if (mux == 2)
        snprintf(base, sizeof base, "v%u", field(ins, 1, 9, 4));
    else if (field(ins, 3, 1, 1))
        snprintf(base, sizeof base, "cr(%u + a0)", field(ins, 1, 13, 8));
    else
        snprintf(base, sizeof base, "c[%u]", field(ins, 1, 13, 8) < 192 ? field(ins, 1, 13, 8) : 0);
    snprintf(b, n, "%s%s.%c%c%c%c", neg ? "-" : "", base, sw[(swz >> 6) & 3],
             sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static const char *mask_sw(uint32_t m, char *b)
{
    int k = 0;
    b[k++] = '.';
    if (m & 8) b[k++] = 'x';
    if (m & 4) b[k++] = 'y';
    if (m & 2) b[k++] = 'z';
    if (m & 1) b[k++] = 'w';
    b[k] = 0;
    return b;
}

static uint32_t reads_of(uint32_t mac, uint32_t ilu)
{
    uint32_t r = 0;
    if (mac) r |= 1;
    if (mac == 2 || (mac >= 4 && mac <= 12)) r |= 2;
    if (mac == 3 || mac == 4 || ilu) r |= 4;
    return r;
}

static const char k_head[] =
"cbuffer C : register(b0) { float2 surf; float2 tdim; uint textured; uint afunc; float aref; uint opaque; uint dbg; uint3 pad; };\n"
"cbuffer V : register(b2) { float4 c[192]; };\n"
"cbuffer P : register(b3) { uint in_base; uint in_n; float2 aa; uint fog_en; uint fog_mode; float2 fog_p; };\n"
"Buffer<float4> vin : register(t0);\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float4 d1 : TEXCOORD1;\n"
"            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
"float4 cr(int i) { return c[(i >= 0 && i < 192) ? i : 0]; }\n"
"float4 rcc(float x) { float f = 1.0 / x; float a = abs(f);\n"
"  if (a < 5.42101e-20) f = (f < 0) ? -5.42101e-20 : 5.42101e-20;\n"
"  if (a > 1.884467e19) f = (f < 0) ? -1.884467e19 : 1.884467e19; return f.xxxx; }\n"
"float4 ilu_exp(float x) { float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
"float4 ilu_log(float x) { float ax = abs(x); if (ax == 0.0) return float4(-asfloat(0x7f800000u), 1.0, -asfloat(0x7f800000u), 1.0);\n"
"  float e = floor(log2(ax)); return float4(e, ax / exp2(e), log2(ax), 1.0); }\n"
"float4 ilu_lit(float4 s) { float nl = max(s.x, 0.0), nh = max(s.y, 0.0), p = clamp(s.w, -127.9961, 127.9961);\n"
"  return float4(1.0, nl, (s.x > 0.0) ? pow(nh, p) : 0.0, 1.0); }\n"
"float fogf(float d) {\n"
"  if (fog_en == 0u) return 1.0;\n"
"  float f;\n"
"  if (fog_mode == 0x800u || fog_mode == 0x802u) f = fog_p.x + exp2(d * fog_p.y * 16.0) - 1.5;\n"
"  else if (fog_mode == 0x801u || fog_mode == 0x803u) f = fog_p.x + exp2(-d * d * fog_p.y * fog_p.y * 32.0) - 1.5;\n"
"  else f = fog_p.x + d * fog_p.y - 1.0;\n"
"  if (fog_mode == 0x802u || fog_mode == 0x803u || fog_mode == 0x804u) f = abs(f);\n"
"  if (f != f) f = 1.0;\n"
"  return saturate(f); }\n"
"VO main(uint vid : SV_VertexID) {\n"
"  float4 R[13], O[13]; int a0 = 0; uint b = in_base + vid * in_n;\n"
"  [unroll] for (int k = 0; k < 13; k++) { R[k] = float4(0, 0, 0, 0); O[k] = float4(0, 0, 0, 0); }\n"
"  O[3].w = 1.0; O[4].w = 1.0; O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;\n";

static const char k_tail[] =
"  VO o; float4 P = R[12]; float w = P.w; if (asuint(w) == 0u) w = 1.0;\n"
"  float nx = P.x * aa.x / surf.x * 2.0 - 1.0, ny = 1.0 - P.y * aa.y / surf.y * 2.0;\n"
"  float nz = min(P.z / 16777215.0, 1.0);\n"
"  o.pos = float4(nx * w, ny * w, nz * w, w);\n"
"  o.c = saturate(O[3]); o.d1 = saturate(O[4]); o.t0 = O[9]; o.t1 = O[10]; o.t2 = O[11]; o.t3 = O[12];\n"
"  o.fog = fogf(O[5].x); return o; }\n";

size_t cs_vsh_to_hlsl(const uint32_t (*prog)[4], uint32_t count, uint32_t mask, char *buf, size_t n)
{
    Out o;
    uint32_t i, k, slot = 0;
    char ms[8];

    o.p = buf; o.n = n; o.len = 0; o.overflow = 0;
    if (!n) return 0;
    buf[0] = 0;
    put(&o, "%s", k_head);
    for (k = 0; k < 16; k++)
        if (mask >> k & 1)
            put(&o, "  float4 v%u = vin[b + %uu];\n", k, slot++);

    for (i = 0; i < count; i++) {
        const uint32_t *ins = prog[i];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4), it = mac ? 1u : tdst;
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        uint32_t to_o = field(ins, 3, 11, 1), from_ilu = field(ins, 3, 2, 1);
        uint32_t rd = reads_of(mac, ilu);
        char a[96], bb[96], cc[96];

        if (!mac || mac == 13) mac_mask = 0;
        if (!ilu) ilu_mask = 0;
        put(&o, "  { // %u: mac %u ilu %u\n", i, mac, ilu);
        if (rd & 1) { src_expr(a, sizeof a, ins, 0); put(&o, "    float4 A = %s;\n", a); }
        if (rd & 2) { src_expr(bb, sizeof bb, ins, 1); put(&o, "    float4 B = %s;\n", bb); }
        if (rd & 4) { src_expr(cc, sizeof cc, ins, 2); put(&o, "    float4 C = %s;\n", cc); }
        put(&o, "    float4 m = float4(0, 0, 0, 0), u = float4(0, 0, 0, 0);\n");
        switch (mac) {
        case 0: break;
        case 1: put(&o, "    m = A;\n"); break;
        case 2: put(&o, "    m = A * B;\n"); break;
        case 3: put(&o, "    m = A + C;\n"); break;
        case 4: put(&o, "    m = A * B + C;\n"); break;
        case 5: put(&o, "    m = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6: put(&o, "    m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7: put(&o, "    m = dot(A, B).xxxx;\n"); break;
        case 8: put(&o, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9: put(&o, "    m = (A < B) ? A : B;\n"); break;
        case 10: put(&o, "    m = (A >= B) ? A : B;\n"); break;
        case 11: put(&o, "    m = float4(A < B);\n"); break;
        case 12: put(&o, "    m = float4(A >= B);\n"); break;
        case 13: put(&o, "    a0 = (int)floor(A.x + 0.001); m = A;\n"); break;
        default: put(&o, "    m = A;\n"); break;
        }
        switch (ilu) {
        case 0: break;
        case 1: put(&o, "    u = C;\n"); break;
        case 2: put(&o, "    u = (C.x == 0.0) ? asfloat(0x7f800000u).xxxx : (1.0 / C.x).xxxx;\n"); break;
        case 3: put(&o, "    u = rcc(C.x);\n"); break;
        case 4: put(&o, "    u = (1.0 / sqrt(abs(C.x))).xxxx;\n"); break;
        case 5: put(&o, "    u = ilu_exp(C.x);\n"); break;
        case 6: put(&o, "    u = ilu_log(C.x);\n"); break;
        case 7: put(&o, "    u = ilu_lit(C);\n"); break;
        }
        if (mac_mask && tdst <= 12)
            put(&o, "    R[%u]%s = m%s;\n", tdst, mask_sw(mac_mask, ms), ms);
        if (ilu_mask && it <= 12)
            put(&o, "    R[%u]%s = u%s;\n", it, mask_sw(ilu_mask, ms), ms);
        if (omask && to_o) {
            const char *s = from_ilu ? "u" : "m";
            if (oaddr == 0)
                put(&o, "    R[12]%s = %s%s;\n", mask_sw(omask, ms), s, ms);
            else if (oaddr < 13)
                put(&o, "    O[%u]%s = %s%s;\n", oaddr, mask_sw(omask, ms), s, ms);
        }
        put(&o, "  }\n");
        if (field(ins, 3, 0, 1))
            break;
    }
    put(&o, "%s", k_tail);
    return o.overflow ? 0 : o.len;
}
