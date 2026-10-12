// NV2A vertex program (transform program microcode) -> GLSL.
#pragma once
#include <cstdint>
#include <string>

namespace rt {

// Translates the program starting at `start` (until the FINAL flag) into the
// body of a GLSL vertex shader main(). The generated code reads v0..v15,
// c[192], and writes oPos/oD0/oD1/oB0/oB1/oFog/oPts/oT0..oT3.
std::string vsh_translate(const uint32_t (*program)[4], uint32_t start, uint32_t length, uint64_t *hash_out);

} // namespace rt
