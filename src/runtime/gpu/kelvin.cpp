// Kelvin 3D engine: method processing.
#include "kelvin.h"
#include "capture.h"
#include "draw.h"
#include "kelvin_state.h"
#include "nv2a_internal.h"
#include "renderer.h"
#include "surface.h"
#include "texture.h"
#include "../log.h"

#include <chrono>
#include <cstring>

namespace rt {

uint32_t k[0x2000 / 4];
KelvinDynamic g_kd;

static uint64_t g_hist[0x2000 / 4];
static bool g_draw_initialized;

void kelvin_dump_histogram()
{
    char line[2048];
    int n = 0;
    line[0] = 0;
    for (uint32_t m = 0; m < 0x2000 / 4; m++) {
        if (!g_hist[m])
            continue;
        n += snprintf(line + n, sizeof line - n, "%04X:%llu ", m * 4, g_hist[m]);
        if (n > 1500) {
            LOG_INFO("kelvin", "%s", line);
            n = 0;
            line[0] = 0;
        }
    }
    if (n)
        LOG_INFO("kelvin", "%s", line);
}

static DmaObject ctx_dma(uint32_t method) { return nv_dma_load(kr(method)); }

static float as_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Immediate-mode attribute write; writing attribute 0 (position) inside a
// BEGIN/END emits a vertex with the current values of all attributes.
static void set_attr(uint32_t slot, const float *v, int count)
{
    if (v != g_kd.current_attr[slot])
        for (int i = 0; i < count; i++)
            g_kd.current_attr[slot][i] = v[i];
    if (count < 4 && slot == 0)
        g_kd.current_attr[0][3] = 1.0f;
    if (slot == 0 && g_kd.primitive != PRIM_END)
        g_kd.immediate.insert(g_kd.immediate.end(), &g_kd.current_attr[0][0], &g_kd.current_attr[0][0] + 64);
}

static void ensure_draw_init()
{
    if (!g_draw_initialized) {
        draw_init();
        g_draw_initialized = true;
    }
}

void kelvin_method(std::unique_lock<std::mutex> &lk, uint32_t subchannel, uint32_t method, uint32_t parameter)
{
    if (method >= 0x2000)
        return;
    g_hist[method >> 2]++;
    // Surface/context changes are worth seeing in a frame capture.
    if (capture_active() && method >= 0x180 && method < 0x220 && kr(method) != parameter) {
        char b[64];
        snprintf(b, sizeof b, "method %04X = %08X", method, parameter);
        capture_draw(b, nullptr);
    }
    kr(method) = parameter;

    // Array-style methods first.
    if (method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        uint32_t slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;
        if (g_kd.program_load < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
            g_kd.program[g_kd.program_load][slot % 4] = parameter;
            g_kd.program_generation++;
            if (slot % 4 == 3)
                g_kd.program_load++;
        }
        return;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        uint32_t slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
        if (g_kd.constant_load < NV2A_VERTEXSHADER_CONSTANTS) {
            g_kd.constants[g_kd.constant_load][slot % 4] = as_float(parameter);
            g_kd.constants_dirty = true;
            if (slot % 4 == 3)
                g_kd.constant_load++;
        }
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M && method < NV097_SET_VERTEX_DATA4F_M + 0x100) {
        uint32_t slot = (method - NV097_SET_VERTEX_DATA4F_M) / 16, c = (method / 4) % 4;
        g_kd.current_attr[slot][c] = as_float(parameter);
        if (c == 3 && slot == 0)
            set_attr(0, g_kd.current_attr[0], 4);
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M && method < NV097_SET_VERTEX_DATA2F_M + 0x80) {
        uint32_t slot = (method - NV097_SET_VERTEX_DATA2F_M) / 8, c = (method / 4) % 2;
        g_kd.current_attr[slot][c] = as_float(parameter);
        if (c == 1) {
            g_kd.current_attr[slot][2] = 0.0f;
            g_kd.current_attr[slot][3] = 1.0f;
            if (slot == 0)
                set_attr(0, g_kd.current_attr[0], 4);
        }
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB && method < NV097_SET_VERTEX_DATA4UB + 0x40) {
        uint32_t slot = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        float v[4] = {(parameter & 255) / 255.0f, ((parameter >> 8) & 255) / 255.0f, ((parameter >> 16) & 255) / 255.0f,
                      (parameter >> 24) / 255.0f};
        set_attr(slot, v, 4);
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA2S && method < NV097_SET_VERTEX_DATA2S + 0x40) {
        uint32_t slot = (method - NV097_SET_VERTEX_DATA2S) / 4;
        float v[4] = {float(int16_t(parameter & 0xFFFF)), float(int16_t(parameter >> 16)), 0.0f, 1.0f};
        set_attr(slot, v, 4);
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M && method < NV097_SET_VERTEX_DATA4S_M + 0x80) {
        uint32_t slot = (method - NV097_SET_VERTEX_DATA4S_M) / 8, c = (method / 4) % 2;
        g_kd.current_attr[slot][c * 2] = float(int16_t(parameter & 0xFFFF));
        g_kd.current_attr[slot][c * 2 + 1] = float(int16_t(parameter >> 16));
        if (c == 1 && slot == 0)
            set_attr(0, g_kd.current_attr[0], 4);
        return;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        g_kd.current_attr[0][(method - NV097_SET_VERTEX3F) / 4] = as_float(parameter);
        if (method == NV097_SET_VERTEX3F + 8)
            set_attr(0, g_kd.current_attr[0], 3);
        return;
    }
    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        g_kd.current_attr[0][(method - NV097_SET_VERTEX4F) / 4] = as_float(parameter);
        if (method == NV097_SET_VERTEX4F + 12)
            set_attr(0, g_kd.current_attr[0], 4);
        return;
    }
    if (method >= NV097_SET_DIFFUSE_COLOR4F && method < NV097_SET_DIFFUSE_COLOR4F + 16) {
        g_kd.current_attr[3][(method - NV097_SET_DIFFUSE_COLOR4F) / 4] = as_float(parameter);
        return;
    }
    if (method == NV097_SET_DIFFUSE_COLOR4UB || method == NV097_SET_SPECULAR_COLOR4UB) {
        // D3DCOLOR (ARGB)
        float *a = g_kd.current_attr[method == NV097_SET_DIFFUSE_COLOR4UB ? 3 : 4];
        a[0] = ((parameter >> 16) & 255) / 255.0f;
        a[1] = ((parameter >> 8) & 255) / 255.0f;
        a[2] = (parameter & 255) / 255.0f;
        a[3] = (parameter >> 24) / 255.0f;
        return;
    }
    if (method >= NV097_SET_TEXCOORD0_2F && method < NV097_SET_TEXCOORD0_2F + 8) {
        g_kd.current_attr[9][(method - NV097_SET_TEXCOORD0_2F) / 4] = as_float(parameter);
        return;
    }
    if (method >= NV097_SET_TEXCOORD0_4F && method < NV097_SET_TEXCOORD0_4F + 16) {
        g_kd.current_attr[9][(method - NV097_SET_TEXCOORD0_4F) / 4] = as_float(parameter);
        return;
    }

    switch (method) {
    case NV097_SET_FLIP_READ:
        set_mask(reg(NV_PGRAPH_SURFACE), NV_PGRAPH_SURFACE_READ_3D, parameter);
        break;
    case NV097_SET_FLIP_WRITE:
        set_mask(reg(NV_PGRAPH_SURFACE), NV_PGRAPH_SURFACE_WRITE_3D, parameter);
        break;
    case NV097_SET_FLIP_MODULO:
        set_mask(reg(NV_PGRAPH_SURFACE), NV_PGRAPH_SURFACE_MODULO_3D, parameter);
        break;
    case NV097_FLIP_INCREMENT_WRITE: {
        uint32_t &s = reg(NV_PGRAPH_SURFACE);
        uint32_t mod = get_mask(s, NV_PGRAPH_SURFACE_MODULO_3D);
        if (mod)
            set_mask(s, NV_PGRAPH_SURFACE_WRITE_3D, (get_mask(s, NV_PGRAPH_SURFACE_WRITE_3D) + 1) % mod);
        if (g_draw_initialized)
            draw_reports_resolve();
        surface_new_frame();
        texture_new_frame();
        capture_frame_boundary();
        break;
    }
    case NV097_FLIP_STALL: {
        // Block until the display has caught up with the frame being drawn.
        for (int guard = 0; guard < 600; guard++) {
            uint32_t s = reg(NV_PGRAPH_SURFACE);
            if (get_mask(s, NV_PGRAPH_SURFACE_READ_3D) != get_mask(s, NV_PGRAPH_SURFACE_WRITE_3D))
                break;
            g_nv->flip_cv.wait_for(lk, std::chrono::milliseconds(5));
        }
        break;
    }
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: {
        if (g_draw_initialized)
            draw_reports_resolve();
        DmaObject d = ctx_dma(NV097_SET_CONTEXT_DMA_SEMAPHORE);
        uint32_t off = kr(NV097_SET_SEMAPHORE_OFFSET);
        *reinterpret_cast<volatile uint32_t *>(dma_ptr(d, off)) = parameter;
        break;
    }
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        g_kd.program_load = parameter;
        break;
    case NV097_SET_TRANSFORM_PROGRAM_START:
        g_kd.program_start = parameter;
        break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        g_kd.constant_load = parameter;
        break;
    case NV097_SET_BEGIN_END:
        ensure_draw_init();
        if (parameter == PRIM_END)
            draw_flush();
        g_kd.primitive = parameter;
        g_kd.inline_array.clear();
        g_kd.inline_elements.clear();
        g_kd.draw_arrays.clear();
        g_kd.immediate.clear();
        break;
    case NV097_ARRAY_ELEMENT16:
        g_kd.inline_elements.push_back(parameter & 0xFFFF);
        g_kd.inline_elements.push_back(parameter >> 16);
        break;
    case NV097_ARRAY_ELEMENT32:
        g_kd.inline_elements.push_back(parameter);
        break;
    case NV097_DRAW_ARRAYS:
        g_kd.draw_arrays.push_back(parameter & 0xFFFFFF);
        g_kd.draw_arrays.push_back((parameter >> 24) + 1);
        break;
    case NV097_INLINE_ARRAY:
        g_kd.inline_array.push_back(parameter);
        break;
    case NV097_CLEAR_REPORT_VALUE:
        ensure_draw_init();
        draw_report_clear();
        break;
    case NV097_GET_REPORT:
        ensure_draw_init();
        draw_report_write(parameter);
        break;
    case NV097_CLEAR_SURFACE:
        ensure_draw_init();
        draw_clear(parameter);
        break;
    default:
        break;
    }
}

void kelvin_resolve_reports()
{
    if (g_draw_initialized)
        draw_reports_resolve();
}

void kelvin_present(std::unique_lock<std::mutex> &lk)
{
    uint32_t fb = reg(NV_PCRTC_START) & 0x03FFFFFF;
    uint32_t w = 0, h = 0;
    if (unsigned tex = draw_find_scanout(fb, w, h)) {
        renderer_present_texture(tex, w, h, false);
        return;
    }
    uint32_t pitch = g_nv->display_pitch ? g_nv->display_pitch : 640 * 4;
    renderer_present_memory(fb, g_nv->display_width, g_nv->display_height, pitch, g_nv->display_bpp);
}

} // namespace rt
