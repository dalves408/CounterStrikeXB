#ifndef CS_VSH_HLSL_H
#define CS_VSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

/* Translate an NV2A vertex program (count instructions, the last FINAL) into
 * an HLSL vs_5_0 source with entry point "main". mask: the inputs v0..v15 it
 * reads, packed in that order per vertex. Returns the length, or 0 if buf is
 * too small. See cs_vsh_hlsl.c. */
size_t cs_vsh_to_hlsl(const uint32_t (*prog)[4], uint32_t count, uint32_t mask, char *buf, size_t n);

#endif
