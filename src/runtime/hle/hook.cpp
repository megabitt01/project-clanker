#include "hook.h"
#include "../log.h"

#include <windows.h>
#include <cstring>

namespace rt {

void patch_bytes(uint32_t addr, const void *bytes, int len)
{
    memcpy(reinterpret_cast<void *>(uintptr_t(addr)), bytes, len);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(uintptr_t(addr)), len);
}

void hook_function(uint32_t addr, const void *target, const char *name, const uint8_t *expect, int expect_len)
{
    uint8_t *p = reinterpret_cast<uint8_t *>(uintptr_t(addr));
    if (expect && memcmp(p, expect, expect_len) != 0)
        fatal("hook %s: bytes at 0x%08X do not match the expected function", name, addr);
    uint8_t jmp[5] = {0xE9};
    int32_t rel = int32_t(uintptr_t(target) - (uintptr_t(addr) + 5));
    memcpy(jmp + 1, &rel, 4);
    patch_bytes(addr, jmp, 5);
    LOG_DEBUG("hle", "hooked %08X -> %s", addr, name);
}

void *hook_detour(uint32_t addr, int len, const void *target, const char *name)
{
    uint8_t *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, reinterpret_cast<void *>(uintptr_t(addr)), len);
    tramp[len] = 0xE9;
    int32_t rel = int32_t((addr + len) - (uintptr_t(tramp) + len + 5));
    memcpy(tramp + len + 1, &rel, 4);
    hook_function(addr, target, name);
    return tramp;
}

void hle_install_all()
{
    hle_install_xapi();
    hle_install_xinput();
    hle_install_dsound();
    hle_install_xnet();
    hle_install_xmv_trace();
    game_install_hooks();
}

} // namespace rt
