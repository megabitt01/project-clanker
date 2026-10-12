// Kernel export implementations (xboxkrnl.exe replacement).
#pragma once
#include "xboxkrnl.h"

#define KEXPORT(ret) extern "C" ret __stdcall
#define KEXPORT_FASTCALL(ret) extern "C" ret __fastcall

namespace rt {

struct KernelExport {
    uint32_t ordinal;
    const char *name;
    void *address;
};

// Fills the XBE's kernel thunk table.
void kernel_resolve_imports(uint32_t thunk_table_va);
// Paths / directories used for the virtual Xbox file system.
void io_init(const char *rom_dir, const char *userdata_dir);
void hal_init();

} // namespace rt
