#pragma once
#include <cstdint>

namespace rt {

// Writes an 8-bit RGBA image as an uncompressed PNG. rows go top to bottom.
bool write_png(const char *path, const uint8_t *rgba, int width, int height, int stride);

} // namespace rt
