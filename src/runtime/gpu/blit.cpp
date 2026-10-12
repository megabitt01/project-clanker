// NV2A 2D engine: surfaces 2D (class 0x62), image blit (0x9F) and
// memory-to-memory format (0x39).
#include "blit.h"
#include "capture.h"
#include "gl.h"
#include "nv2a_internal.h"
#include "surface.h"
#include "../log.h"

#include <cstdio>
#include <cstring>

namespace rt {

static uint32_t g_s2d[0x800 / 4];   // class 0x62 state
static uint32_t g_blit[0x800 / 4];  // class 0x9F state
static uint32_t g_m2mf[0x800 / 4];  // class 0x39 state

static uint32_t bpp_for_s2d(uint32_t fmt)
{
    switch (fmt) {
    case 0x1: return 1;                                // Y8
    case 0x2: case 0x3: case 0x4: case 0x5: return 2;  // X1R5G5B5 / R5G6B5 / Y16
    default: return 4;                                 // X8R8G8B8 / A8R8G8B8 / Y32
    }
}

// Copies a rectangle between two pieces of guest memory, honouring pitches.
static void mem_blit(uint32_t src, uint32_t spitch, uint32_t dst, uint32_t dpitch, uint32_t sx, uint32_t sy,
                     uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, uint32_t bpp)
{
    for (uint32_t row = 0; row < h; row++) {
        uint32_t s = src + (sy + row) * spitch + sx * bpp, d = dst + (dy + row) * dpitch + dx * bpp;
        if (s + w * bpp > XBOX_RAM_SIZE || d + w * bpp > XBOX_RAM_SIZE)
            return;
        memmove(phys_ptr(d), phys_ptr(s), w * bpp);
    }
}

static void image_blit()
{
    uint32_t point_in = g_blit[0x300 >> 2], point_out = g_blit[0x304 >> 2], size = g_blit[0x308 >> 2];
    uint32_t sx = point_in & 0xFFFF, sy = point_in >> 16, dx = point_out & 0xFFFF, dy = point_out >> 16;
    uint32_t w = size & 0xFFFF, h = size >> 16;
    uint32_t fmt = g_s2d[0x300 >> 2], pitch = g_s2d[0x304 >> 2];
    uint32_t spitch = pitch & 0xFFFF, dpitch = pitch >> 16;
    DmaObject sd = nv_dma_load(g_s2d[0x184 >> 2]), dd = nv_dma_load(g_s2d[0x188 >> 2]);
    uint32_t src = (sd.address + g_s2d[0x308 >> 2]) & 0x07FFFFFF;
    uint32_t dst = (dd.address + g_s2d[0x30C >> 2]) & 0x07FFFFFF;
    uint32_t bpp = bpp_for_s2d(fmt);
    if (!w || !h)
        return;
    if (capture_active()) {
        char b[160];
        snprintf(b, sizeof b, "blit %08X(%u,%u) -> %08X(%u,%u) %ux%u fmt=%X pitch=%08X", src, sx, sy, dst, dx, dy, w, h,
                 fmt, pitch);
        capture_draw(b, nullptr);
    }

    uint32_t ssx = 0, ssy = 0;
    Surface *ss = surface_containing(src, ssx, ssy);
    if (ss && bpp == 4) {
        // GPU source: copy on the GPU into a destination surface, then write
        // the pixels back to guest memory as well for any CPU readers.
        uint32_t dw = dpitch / bpp, dh = dy + h;
        if (dw < dx + w)
            dw = dx + w;
        Surface *ds = surface_get_color(dst, dpitch, dw, dh, 8);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, surface_fbo(ss));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, surface_fbo(ds));
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(ssx + sx, ssy + sy, ssx + sx + w, ssy + sy + h, dx, dy, dx + w, dy + h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, surface_fbo(ds));
        glPixelStorei(GL_PACK_ROW_LENGTH, dpitch / 4);
        if (dst + (dy + h - 1) * dpitch + (dx + w) * 4 <= XBOX_RAM_SIZE)
            glReadPixels(dx, dy, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, phys_ptr(dst + dy * dpitch + dx * 4));
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        surface_mark_written(ds); // after the write-back, so the memory signature matches
        return;
    }
    mem_blit(src, spitch, dst, dpitch, sx, sy, dx, dy, w, h, bpp);
    // If the destination is a GL surface, refresh the copied region.
    uint32_t tx = 0, ty = 0;
    if (Surface *ds = surface_containing(dst, tx, ty)) {
        if (bpp == 4) {
            glBindTexture(GL_TEXTURE_2D, ds->tex);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, dpitch / 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, tx + dx, ty + dy, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                            phys_ptr(dst + dy * dpitch + dx * 4));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            surface_mark_written(ds);
        }
    }
}

static void m2mf_copy()
{
    DmaObject in = nv_dma_load(g_m2mf[0x184 >> 2]), out = nv_dma_load(g_m2mf[0x188 >> 2]);
    uint32_t src = in.address + g_m2mf[0x30C >> 2], dst = out.address + g_m2mf[0x310 >> 2];
    int32_t pin = int32_t(g_m2mf[0x314 >> 2]), pout = int32_t(g_m2mf[0x318 >> 2]);
    uint32_t len = g_m2mf[0x31C >> 2], lines = g_m2mf[0x320 >> 2];
    if (capture_active()) {
        char b[160];
        snprintf(b, sizeof b, "m2mf %08X -> %08X %ux%u", src, dst, len, lines);
        capture_draw(b, nullptr);
    }
    for (uint32_t l = 0; l < lines; l++) {
        uint32_t s = src + l * pin, d = dst + l * pout;
        if (s + len > XBOX_RAM_SIZE || d + len > XBOX_RAM_SIZE)
            return;
        memmove(phys_ptr(d), phys_ptr(s), len);
    }
}

void blit_method(uint32_t object_class, uint32_t method, uint32_t parameter)
{
    if (method >= 0x800)
        return;
    switch (object_class) {
    case 0x62:
        g_s2d[method >> 2] = parameter;
        break;
    case 0x9F:
        g_blit[method >> 2] = parameter;
        if (method == 0x308)
            image_blit();
        break;
    case 0x39:
        g_m2mf[method >> 2] = parameter;
        if (method == 0x328)
            m2mf_copy();
        break;
    default:
        break;
    }
}

} // namespace rt
