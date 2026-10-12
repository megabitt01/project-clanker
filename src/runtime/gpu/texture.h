#pragma once
#include "psh.h"

#include <cstdint>

namespace rt {

struct TextureBinding {
    unsigned tex = 0;      // GL texture name (0 = none)
    unsigned target = 0;   // GL_TEXTURE_2D / GL_TEXTURE_CUBE_MAP / GL_TEXTURE_3D
    PshTexType type = PSH_TEX_NONE;
    float scale_x = 1, scale_y = 1;  // texcoord scale (linear textures use texel coords)
};

// Resolves and binds the texture for a stage to GL texture unit `stage`.
TextureBinding texture_bind_stage(int stage);
void texture_new_frame();
// True (once) if binding a texture had to render on the GPU (for example to
// convert a depth surface), which disturbs other units and GL state.
bool texture_take_disturbed();

// Shared helper: Morton-order (swizzled) offsets.
void swizzle_tables(uint32_t w, uint32_t h, uint32_t *xoff, uint32_t *yoff);
void swizzle_tables_3d(uint32_t w, uint32_t h, uint32_t d, uint32_t *xoff, uint32_t *yoff, uint32_t *zoff);

} // namespace rt
