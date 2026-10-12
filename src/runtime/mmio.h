#pragma once
#include <cstdint>

namespace rt {

// Device register callbacks. size is 1, 2 or 4 bytes.
typedef uint32_t (*MmioReadFn)(uint32_t addr, int size);
typedef void (*MmioWriteFn)(uint32_t addr, uint32_t value, int size);

void mmio_register(uint32_t base, uint32_t size, MmioReadFn read, MmioWriteFn write);

typedef uint32_t (*PortReadFn)(uint16_t port, int size);
typedef void (*PortWriteFn)(uint16_t port, uint32_t value, int size);
void port_set_handlers(PortReadFn read, PortWriteFn write);

// Installs the vectored exception handler that emulates MMIO accesses and
// privileged instructions executed by Xbox code.
void exceptions_init();

} // namespace rt
