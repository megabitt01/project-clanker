#pragma once
#include <cstddef>
#include <cstdint>

namespace rt {

// Xbox physical address space as seen by the title.
constexpr uint32_t XBOX_RAM_SIZE = 64u << 20;          // retail units ship with 64 MiB
constexpr uint32_t XBOX_PHYS_BASE = 0x80000000u;       // cached physical mapping
constexpr uint32_t XBOX_PHYS_WC_BASE = 0xF0000000u;    // second (write-combined) mapping
constexpr uint32_t XBOX_INSTANCE_SIZE = 1u << 20;      // NV2A RAMIN lives in the top 1 MiB
constexpr uint32_t NV2A_MMIO_BASE = 0xFD000000u;
constexpr uint32_t NV2A_MMIO_SIZE = 0x01000000u;
constexpr uint32_t NV2A_PRAMIN_BASE = NV2A_MMIO_BASE + 0x700000u;
constexpr uint32_t MCPX_MMIO_BASE = 0xFE000000u;        // APU, USB, NIC, ...
constexpr uint32_t MCPX_MMIO_SIZE = 0x01000000u;

void mem_init(void *image_space, uint32_t image_space_size);

inline uint8_t *phys_ptr(uint32_t phys) { return reinterpret_cast<uint8_t *>(XBOX_PHYS_BASE + (phys & (XBOX_RAM_SIZE - 1))); }

// Contiguous ("physical") memory allocator used by MmAllocateContiguousMemory.
void *mem_alloc_contiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment);
bool mem_free_contiguous(void *p);
uint32_t mem_contiguous_size(void *p);
bool mem_is_contiguous(const void *p);
uint32_t mem_physical_address(const void *va);
uint32_t mem_available_pages();

// Reserve a range of physical memory so the allocator never hands it out.
void mem_reserve_physical(uint32_t phys, uint32_t size);

} // namespace rt
