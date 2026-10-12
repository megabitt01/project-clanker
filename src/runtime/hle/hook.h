// Function replacement helpers.
//
// A hook overwrites the first bytes of an original XBE function with a jump
// to a native replacement. Every address used with these helpers refers to
// the specific default.xbe the build verifies (see cmake/CheckRom.cmake).
#pragma once
#include <cstdint>

namespace rt {

// Redirects the function at `addr` to `target` (jmp rel32). `expect` (if
// non-null) is checked against the original bytes first so a wrong address
// is caught early.
void hook_function(uint32_t addr, const void *target, const char *name, const uint8_t *expect = nullptr,
                   int expect_len = 0);

// Detour: redirects `addr` to `target` but first copies the original's
// leading `len` bytes (whole instructions, no relative branches) into a
// trampoline, which is returned so the replacement can call the original.
void *hook_detour(uint32_t addr, int len, const void *target, const char *name);

// Writes raw bytes into the XBE image.
void patch_bytes(uint32_t addr, const void *bytes, int len);

// Registration of all HLE modules.
void hle_install_all();
void hle_install_xapi();
void hle_install_xinput();
void hle_install_dsound();
void hle_install_xnet();
void hle_install_xmv_trace();
void game_install_hooks();   // decompiled game code (src/game)

} // namespace rt

#define HOOK(addr, fn) ::rt::hook_function(addr, reinterpret_cast<const void *>(&fn), #fn)
