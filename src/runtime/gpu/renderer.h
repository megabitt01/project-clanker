#pragma once
#include <cstdint>

namespace rt {

// Runs on the GPU (pusher) thread: makes the GL context current and builds
// the presentation resources.
void renderer_thread_init();

// Presents an Xbox-memory framebuffer (linear, 16 or 32 bpp) to the window.
void renderer_present_memory(uint32_t phys, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp);
// Presents a GL texture (an emulated render target) to the window.
void renderer_present_texture(unsigned tex, uint32_t width, uint32_t height, bool flip_y);

void renderer_enable_screenshots(const char *dir, double interval_seconds);
unsigned renderer_compile_program(const char *vs, const char *fs, const char *name);

} // namespace rt
