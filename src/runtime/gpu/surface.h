// Render targets: NV2A colour/zeta surfaces backed by GL textures.
#pragma once
#include <cstdint>

namespace rt {

struct Surface {
    uint32_t addr = 0;      // physical address
    uint32_t pitch = 0;
    uint32_t width = 0, height = 0;   // in the game's pixels
    uint32_t pw = 0, ph = 0;          // GL texture size (scaled with --fullres)
    uint32_t format = 0;    // NV097 surface colour format (1-10) or zeta format (1 = Z16, 2 = Z24S8)
    bool depth = false;
    bool swizzled = false;
    unsigned tex = 0;
    uint64_t last_write_frame = 0;
    uint64_t mem_sig = 0;   // sparse hash of guest memory when the GPU last wrote it
    uint64_t write_gen = 0; // incremented on every GPU write
};

// Returns (creating if needed) the surface for the current Kelvin surface
// state. nullptr if colour/zeta is not bound.
Surface *surface_color_current();
Surface *surface_zeta_current();
// Binds an FBO with the given attachments; returns false if none.
bool surface_bind(Surface *color, Surface *zeta);
// Finds a surface that starts at a physical address (for render-to-texture
// and scan-out).
Surface *surface_find(uint32_t addr, bool want_depth);
void surface_mark_written(Surface *s);
uint64_t surface_frame();
void surface_new_frame();
// Finds or creates a colour surface at a physical address (used by 2D blits).
Surface *surface_get_color(uint32_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt);
// FBO with only this surface attached (for glBlitFramebuffer / readback).
unsigned surface_fbo(Surface *s);
// Finds the surface whose memory contains `addr` (returns it and the x/y of
// that address inside it), or nullptr.
Surface *surface_containing(uint32_t addr, uint32_t &x, uint32_t &y);
// True if the CPU has overwritten the surface's memory since the GPU last
// rendered to it (the GL copy is then stale and is discarded).
bool surface_is_stale(Surface *s);
void surface_discard(Surface *s);

// Render scaling (--fullres): surfaces are allocated at their size times the
// scale, and rendering is scaled to match. Coordinates passed to these
// helpers are in the game's pixels.
float surface_scale_x();
float surface_scale_y();
// glScissor for an inclusive rectangle.
void surface_scissor(int x0, int y0, int x1, int y1);
// Pixel transfer between a colour surface and memory (resampled when scaled).
// The surface's FBO is left bound for reading.
void surface_read(Surface *s, int x, int y, int w, int h, unsigned format, unsigned type, void *dst, int row_length);
void surface_write(Surface *s, int x, int y, int w, int h, unsigned format, unsigned type, const void *src,
                   int row_length);


} // namespace rt
