#ifndef CS_TEXTURE_PACK_H
#define CS_TEXTURE_PACK_H

#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define CS_PACK_DECODED_BGRA 0x100u

int cs_pack_enabled(void);
int cs_pack_load(ID3D11Device *device, unsigned format, unsigned width, unsigned height,
                 const void *pixels, size_t length, size_t byte_limit,
                 ID3D11Texture2D **texture, ID3D11ShaderResourceView **view, size_t *bytes);

/* Background loading. cs_pack_request: 0 when the pack has no replacement,
 * else a request id; the replacement is loaded on a worker thread.
 * cs_pack_take: 1 = here it is (now the caller's), 0 = still loading,
 * -1 = none (failed, too big for byte_limit, or already taken).
 * cs_pack_drop: free loaded replacements nobody took (hi-res turned off). */
int  cs_pack_request(ID3D11Device *device, unsigned format, unsigned width, unsigned height,
                     const void *pixels, size_t length);
int  cs_pack_take(int request, size_t byte_limit, ID3D11Texture2D **texture, ID3D11ShaderResourceView **view,
                  size_t *bytes);
void cs_pack_drop(void);

#endif
