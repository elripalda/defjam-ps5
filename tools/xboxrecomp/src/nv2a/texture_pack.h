#ifndef RECOMP_TEXTURE_PACK_H
#define RECOMP_TEXTURE_PACK_H
#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* Source dimensions/stride remain guest properties. Replacements never alter them. */
typedef struct RecompTextureSource {
    uint32_t address, format, width, height, row_texels;
    const void *data;
    uint32_t bytes, stride, row_bytes, rows;
    const void *palette;
    uint32_t palette_entries;
} RecompTextureSource;
typedef int (*RecompTextureSample)(void *user, uint32_t x, uint32_t y, uint32_t *argb);
int texture_pack_active(void);
/* The [scale] factor of the texture at this guest address, when it was the last one bound at
 * stage 0 (1 otherwise, or when it has none). */
float texture_pack_scale_for(uint32_t address);
/* veto(source, sample, user) != 0: the pack's picture is not used for that texture. */
void texture_pack_set_veto(int (*veto)(const RecompTextureSource *, RecompTextureSample, void *));
int texture_pack_hash(const RecompTextureSource *source, char out[65]);
int texture_pack_bind(IDirect3DDevice8 *device, unsigned stage,
                      const RecompTextureSource *source, RecompTextureSample sample, void *user);
void texture_pack_shutdown(IDirect3DDevice8 *device);
/* Read the packs again (RECOMP_TEXTURE_PACKS and the other variables as they are now) at the
 * next bind: what the settings menu calls when a pack is switched on or off. Any thread. */
void texture_pack_reload(void);
#endif
