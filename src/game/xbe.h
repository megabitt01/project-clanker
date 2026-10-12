// Access to code and data that still lives in default.xbe.
//
// Decompiled functions call not-yet-decompiled ones (and XDK library code)
// through these typed addresses, and read/write the title's globals in place.
// Every address refers to the specific build verified by CMake.
#pragma once
#include <cstdint>

namespace game {

template <class T> inline T &xbe_global(uint32_t address) { return *reinterpret_cast<T *>(uintptr_t(address)); }
template <class Fn> inline Fn xbe_function(uint32_t address) { return reinterpret_cast<Fn>(uintptr_t(address)); }

// Floating-point constants in .rdata, referenced by address so decompiled
// code uses exactly the values the original does.
inline float xbe_float(uint32_t address) { return xbe_global<const float>(address); }

} // namespace game
