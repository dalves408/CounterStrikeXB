#ifndef CS_RC_HLSL_H
#define CS_RC_HLSL_H

#include <stddef.h>
#include <stdint.h>
#include "nv2a_combiner.h"

/* A ps_5_0 source (entry "main") specialised for this combiner setup; reads
 * the same b0/b1 constant buffers and t0-t3/s0-s3 as the generic combiner
 * shader. Returns the length, or 0 if buf is too small. See cs_rc_hlsl.c. */
size_t cs_rc_to_hlsl(const Nv2aCombiner *rc, char *buf, size_t n);

/* What the generated shader depends on, hashed: equal keys share a shader. */
uint64_t cs_rc_key(const Nv2aCombiner *rc);

#endif
