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

#endif
