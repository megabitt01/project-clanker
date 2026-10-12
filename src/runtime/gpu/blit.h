#pragma once
#include <cstdint>

namespace rt {

// Methods for the 2D classes (0x62 surfaces, 0x9F image blit, 0x39 M2MF).
// Context-DMA methods (0x180-0x1FF) arrive already translated to instances.
void blit_method(uint32_t object_class, uint32_t method, uint32_t parameter);

} // namespace rt
