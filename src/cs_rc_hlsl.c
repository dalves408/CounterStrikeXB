/*
 * NV2A register-combiner setup -> a pixel shader specialised for it.
 *
 * The D3D11 back end's generic combiner shader (s_hlsl_rc in cs_d3d11.c)
 * interprets the combiner words per pixel: a loop over up to eight stages
 * indexing a float4 R[16] by values from a constant buffer. GPUs keep such an
 * array in local memory, and in Counter-Strike's world that shader made the
 * GPU take ~23 ms a frame. Here every register index, input mapping, output
 * scale and mux choice is a compile-time constant and the registers are plain
 * variables; the arithmetic is the generic shader's, step for step.
 *
 * What is specialised: control (stage count, flags), stage program, final
 * combiner words, and each used stage's colour/alpha input and output words.
 * What stays a run-time value in the same constant buffer: factor colours,
 * final-stage constants, fog colour, texture scale/valid/wrap, alpha test,
 * the opaque flag. So shaders are shared across draws that differ only in
 * those.
 */
#include "cs_rc_hlsl.h"

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

/* mapin applied to an expression, for mapping m = (b >> 5) & 7. */
static void mapin(char *d, size_t n, const char *x, uint32_t b)
{
    switch ((b >> 5) & 7u) {
    case 0: snprintf(d, n, "max(%s, 0.0)", x); break;
    case 1: snprintf(d, n, "(1.0 - saturate(%s))", x); break;
    case 2: snprintf(d, n, "(2.0 * max(%s, 0.0) - 1.0)", x); break;
    case 3: snprintf(d, n, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;
    case 4: snprintf(d, n, "(max(%s, 0.0) - 0.5)", x); break;
    case 5: snprintf(d, n, "(-max(%s, 0.0) + 0.5)", x); break;
    case 6: snprintf(d, n, "(%s)", x); break;
    default: snprintf(d, n, "(-(%s))", x); break;
    }
}

static void read_rgb(char *d, size_t n, uint32_t b)
{
    char x[32];
    snprintf(x, sizeof x, "R%u.%s", b & 15u, (b & 0x10u) ? "aaa" : "rgb");
    mapin(d, n, x, b);
}

static void read_alpha(char *d, size_t n, uint32_t b)
{
    char x[32];
    snprintf(x, sizeof x, "R%u.%s", b & 15u, (b & 0x10u) ? "a" : "b");
    mapin(d, n, x, b);
}

static void mapout(char *d, size_t n, const char *x, uint32_t m)
{
    switch (m & 0x38u) {
    case 0x08: snprintf(d, n, "((%s) - 0.5)", x); break;
    case 0x10: snprintf(d, n, "((%s) * 2.0)", x); break;
    case 0x18: snprintf(d, n, "(((%s) - 0.5) * 2.0)", x); break;
    case 0x20: snprintf(d, n, "((%s) * 4.0)", x); break;
    case 0x30: snprintf(d, n, "((%s) * 0.5)", x); break;
    default: snprintf(d, n, "(%s)", x); break;
    }
}

static const char k_head[] =
"cbuffer C : register(b0) { float2 surf; float2 tdim; uint textured; uint afunc; float aref; uint opaque; uint dbg; uint3 pad; };\n"
"cbuffer RC : register(b1) { uint4 hdr; uint4 hdr2; float4 fogc; float4 uvs[4]; uint4 stg[16]; };\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1); Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"SamplerState sm0 : register(s0); SamplerState sm1 : register(s1); SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float4 d1 : TEXCOORD1;\n"
"            float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5; float fog : TEXCOORD6; };\n"
"float4 unpackc(uint c) { return float4((float)((c >> 16) & 255u), (float)((c >> 8) & 255u), (float)(c & 255u), (float)(c >> 24)) / 255.0; }\n"
"float4 tsample(Texture2D t, SamplerState s, float4 tc, float4 u) {\n"
"  float2 c = tc.xy / ((tc.w != 0.0) ? tc.w : 1.0) * u.xy; if (u.w != 0.0) c = frac(c);\n"
"  return (u.z == 0.0) ? float4(1, 1, 1, 1) : t.Sample(s, c); }\n"
"float4 dsample(Texture2D t, SamplerState s, float2 c, float4 u) {\n"
"  if (u.w != 0.0) c = frac(c); return (u.z == 0.0) ? float4(1, 1, 1, 1) : t.Sample(s, c); }\n"
"float4 main(VO i) : SV_Target {\n"
"  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0, R13 = 0, R14 = 0, R15 = 0;\n"
"  float4 out4; bool mux_cd;\n"
"  R3 = float4(fogc.rgb, i.fog); R4 = i.c; R5 = i.d1;\n";

/* An output word's destination for one half: register d (0: none). */
static void stage(Out *o, const Nv2aCombiner *rc, uint32_t s)
{
    uint32_t flags = rc->control >> 8;
    uint32_t icw = rc->color_icw[s], aicw = rc->alpha_icw[s];
    uint32_t ocw = rc->color_ocw[s], aocw = rc->alpha_ocw[s];
    uint32_t fl = ocw >> 12, afl = aocw >> 12, d;
    char A[96], B[96], C[96], D[96], t1[160], t2[160];

    put(o, "  { // stage %u\n", s);
    put(o, "    R1 = unpackc(stg[%uu].x); R2 = unpackc(stg[%uu].y);\n",
        2u * ((flags & 0x010u) ? s : 0u) + 1u, 2u * ((flags & 0x100u) ? s : 0u) + 1u);
    if ((fl & 4u) || (afl & 4u))
        put(o, "    mux_cd = %s;\n", (flags & 1u) ? "(R12.a >= 0.5)" : "((((int)(R12.a * 255.0)) & 1) != 0)");
    read_rgb(A, sizeof A, icw >> 24); read_rgb(B, sizeof B, icw >> 16);
    read_rgb(C, sizeof C, icw >> 8);  read_rgb(D, sizeof D, icw);
    put(o, "    float3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
    put(o, "    float3 pab = %s, pcd = %s;\n", (fl & 2u) ? "dot(A, B).xxx" : "A * B", (fl & 1u) ? "dot(C, D).xxx" : "C * D");
    mapout(t1, sizeof t1, "pab", fl); mapout(t2, sizeof t2, "pcd", fl);
    put(o, "    float3 ab = clamp(%s, -1.0, 1.0), cd = clamp(%s, -1.0, 1.0);\n", t1, t2);
    put(o, "    float3 sum = %s;\n", (fl & 4u) ? "(mux_cd ? pcd : pab)" : "pab + pcd");
    mapout(t1, sizeof t1, "sum", fl);
    put(o, "    float3 ms = clamp(%s, -1.0, 1.0);\n", t1);
    read_alpha(A, sizeof A, aicw >> 24); read_alpha(B, sizeof B, aicw >> 16);
    read_alpha(C, sizeof C, aicw >> 8);  read_alpha(D, sizeof D, aicw);
    put(o, "    float aA = %s, aB = %s, aC = %s, aD = %s;\n", A, B, C, D);
    put(o, "    float apab = aA * aB, apcd = aC * aD;\n");
    mapout(t1, sizeof t1, "apab", afl); mapout(t2, sizeof t2, "apcd", afl);
    put(o, "    float aab = clamp(%s, -1.0, 1.0), acd = clamp(%s, -1.0, 1.0);\n", t1, t2);
    put(o, "    float asum = %s;\n", (afl & 4u) ? "(mux_cd ? apcd : apab)" : "apab + apcd");
    mapout(t1, sizeof t1, "asum", afl);
    put(o, "    float ams = clamp(%s, -1.0, 1.0);\n", t1);
    if ((d = (ocw >> 4) & 15u) != 0) {
        put(o, "    R%u.rgb = ab;\n", d);
        if (fl & 0x80u) put(o, "    R%u.a = ab.z;\n", d);
    }
    if ((d = ocw & 15u) != 0) {
        put(o, "    R%u.rgb = cd;\n", d);
        if (fl & 0x40u) put(o, "    R%u.a = cd.z;\n", d);
    }
    if ((d = (ocw >> 8) & 15u) != 0) put(o, "    R%u.rgb = ms;\n", d);
    if ((d = (aocw >> 4) & 15u) != 0) put(o, "    R%u.a = aab;\n", d);
    if ((d = aocw & 15u) != 0) put(o, "    R%u.a = acd;\n", d);
    if ((d = (aocw >> 8) & 15u) != 0) put(o, "    R%u.a = ams;\n", d);
    put(o, "    R0 = float4(0, 0, 0, 0);\n  }\n");
}

size_t cs_rc_to_hlsl(const Nv2aCombiner *rc, char *buf, size_t n)
{
    static const char *const tx[4] = { "tx0, sm0, i.t0", "tx1, sm1, i.t1", "tx2, sm2, i.t2", "tx3, sm3, i.t3" };
    static const char *const tc[4] = { "i.t0", "i.t1", "i.t2", "i.t3" };
    Out o;
    uint32_t ns = rc->control & 255u, sp = rc->stage_program, k;

    o.p = buf; o.n = n; o.len = 0; o.overflow = 0;
    if (!n) return 0;
    if (ns > 8u) ns = 8u;
    put(&o, "%s", k_head);
    for (k = 0; k < 4; k++) {        /* stage_value, per the stage's mode */
        uint32_t mode = (sp >> (k * 5u)) & 31u;
        if (mode == 0u) put(&o, "  R%u = float4(0, 0, 0, 1);\n", 8u + k);
        else if (mode == 1u || mode == 2u) put(&o, "  R%u = tsample(%s, uvs[%u]);\n", 8u + k, tx[k], k);
        else if (mode == 3u) put(&o, "  R%u = float4(1, 1, 1, 1);\n", 8u + k);
        else if (mode == 4u) put(&o, "  R%u = saturate(%s);\n", 8u + k, tc[k]);
        /* DEPENDENT_AR / _GB (D3D texreg2ar / texreg2gb): this stage's texture
         * at coordinates taken from stage 0's colour, (alpha, red) or (green,
         * blue). The input-stage register is not tracked; stage 0 is the one
         * D3D's shaders use. CS's buy menu wedges look their colours up so. */
        else if ((mode == 15u || mode == 16u) && k > 0u)
            put(&o, "  R%u = dsample(tx%u, sm%u, R8.%s, uvs[%u]);\n", 8u + k, k, k, mode == 15u ? "ar" : "gb", k);
        else put(&o, "  R%u = float4(0, 0, 0, 0);\n", 8u + k);
    }
    put(&o, "  R12.a = %s;\n", (sp & 31u) ? "R8.a" : "1.0");
    for (k = 0; k < ns; k++)
        stage(&o, rc, k);
    if (rc->final0 || rc->final1) {
        uint32_t f0 = rc->final0, f1 = rc->final1, ff = f1 & 255u;
        char A[96], B[96], C[96], D[96], E[96], F[96], G[96];
        put(&o, "  {\n    R1 = unpackc(hdr2.x); R2 = unpackc(hdr2.y);\n");
        put(&o, "    float3 fa = %s, fb = %s;\n", (ff & 0x40u) ? "1.0 - R5.rgb" : "R5.rgb", (ff & 0x20u) ? "1.0 - R12.rgb" : "R12.rgb");
        put(&o, "    R14.rgb = %s; R14.a = 0.0;\n", (ff & 0x80u) ? "saturate(fa + fb)" : "fa + fb");
        read_rgb(E, sizeof E, f1 >> 24); read_rgb(F, sizeof F, f1 >> 16);
        put(&o, "    R15.rgb = %s * %s; R15.a = 0.0;\n", E, F);
        read_rgb(A, sizeof A, f0 >> 24); read_rgb(B, sizeof B, f0 >> 16);
        read_rgb(C, sizeof C, f0 >> 8);  read_rgb(D, sizeof D, f0);
        read_alpha(G, sizeof G, f1 >> 8);
        put(&o, "    float3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
        put(&o, "    out4.rgb = D + C * (1.0 - A) + B * A; out4.a = %s;\n  }\n", G);
    } else {
        put(&o, "  out4 = R12;\n");
    }
    put(&o,
        "  out4 = saturate(out4);\n"
        "  if (opaque != 0u) out4.a = 1.0;\n"
        "  { int a = (int)(out4.a * 255.0 + 0.5); int r = (int)(aref + 0.5); bool ok = true;\n"
        "    if (afunc == 0x200u) ok = false; else if (afunc == 0x201u) ok = a < r; else if (afunc == 0x202u) ok = a == r;\n"
        "    else if (afunc == 0x203u) ok = a <= r; else if (afunc == 0x204u) ok = a > r; else if (afunc == 0x205u) ok = a != r;\n"
        "    else if (afunc == 0x206u) ok = a >= r;\n"
        "    if (!ok) discard; }\n"
        "  return out4; }\n");
    return o.overflow ? 0 : o.len;
}

uint64_t cs_rc_key(const Nv2aCombiner *rc)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t ns = rc->control & 255u, k;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 1099511628211ull)
    if (ns > 8u) ns = 8u;
    MIX(rc->control); MIX(rc->stage_program); MIX(rc->final0); MIX(rc->final1);
    for (k = 0; k < ns; k++) {
        MIX(rc->color_icw[k]); MIX(rc->alpha_icw[k]); MIX(rc->color_ocw[k]); MIX(rc->alpha_ocw[k]);
    }
#undef MIX
    return h;
}
