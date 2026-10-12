// NV2A pixel pipeline (texture shader stages + register combiners) -> GLSL.
#pragma once
#include <cstdint>
#include <string>

namespace rt {

enum PshTexType : uint8_t {
    PSH_TEX_NONE = 0,
    PSH_TEX_2D = 1,
    PSH_TEX_RECT = 2,     // linear texture: texcoords in texels (scaled in shader)
    PSH_TEX_CUBE = 3,
    PSH_TEX_3D = 4,
};

struct PshState {
    uint32_t color_icw[8], color_ocw[8], alpha_icw[8], alpha_ocw[8];
    uint32_t final0, final1, control;
    uint32_t stage_program, other_stage_input, dot_mapping;
    uint8_t tex_type[4];
    uint8_t alpha_func;   // 0 = alpha test disabled, else (GL func - 0x200 + 1)
    uint8_t two_sided;
    uint8_t flat_shade;
    uint8_t fog_enable;
    uint8_t z_cull;       // cull fragments outside the depth range (NV097 CULL_NEAR_FAR without ZCLAMP)
    uint8_t pad[2];
};

// Builds a complete fragment shader for the state.
std::string psh_generate(const PshState &s);

} // namespace rt
