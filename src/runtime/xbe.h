#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt {

struct XbeSectionHeader {
    uint32_t Flags;
    uint32_t VirtualAddress;
    uint32_t VirtualSize;
    uint32_t RawAddress;
    uint32_t RawSize;
    const char *SectionName;
    uint32_t SectionReferenceCount;
    uint16_t *HeadSharedPageReferenceCount;
    uint16_t *TailSharedPageReferenceCount;
    uint8_t ShaDigest[20];
};
static_assert(sizeof(XbeSectionHeader) == 56);

struct XbeTlsDirectory {
    uint32_t RawDataStart, RawDataEnd, IndexAddress, CallbackAddress, SizeOfZeroFill, Characteristics;
};

struct XbeInfo {
    uint32_t entry_point;
    uint32_t kernel_thunk;
    uint32_t stack_size;
    uint32_t tls_addr;
    uint32_t image_end;
    uint32_t title_id;
    std::string title_name;
};

// Maps default.xbe into the reserved image range at 0x00010000.
bool xbe_load(const char *path, XbeInfo &info);
void xbe_load_section(XbeSectionHeader *section);
// Returns the section header with the given name (e.g. "D3D"), or nullptr.
XbeSectionHeader *xbe_find_section(const char *name);
// Where the XBE image header lives (see xbe.cpp).
uint32_t xbe_header_address();

} // namespace rt
