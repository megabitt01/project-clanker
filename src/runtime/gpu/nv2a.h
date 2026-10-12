#pragma once
#include <cstdint>

namespace rt {

void nv2a_init();
// Called by AvSetDisplayMode.
void nv2a_set_display_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t framebuffer);

// Current scanout surface (for the presenter).
struct ScanoutInfo {
    uint32_t address;   // physical address of the front buffer
    uint32_t pitch;
    uint32_t width, height;
    uint32_t format;    // AvSetDisplayMode format (bytes per pixel encoded)
};
ScanoutInfo nv2a_scanout();

} // namespace rt
