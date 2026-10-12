// Memory manager exports.
#include "kexports.h"
#include "../memory.h"
#include "../log.h"

#include <windows.h>
#include <mutex>
#include <unordered_map>

using namespace rt;

// The Xbox has no NX bit; titles happily execute from PAGE_READWRITE memory.
static DWORD to_host_protect(uint32_t p)
{
    p &= ~(0x200u | 0x400u); // PAGE_NOCACHE / PAGE_WRITECOMBINE
    DWORD guard = p & PAGE_GUARD;
    p &= 0xFF;
    switch (p) {
    case PAGE_READONLY: p = PAGE_EXECUTE_READ; break;
    case PAGE_READWRITE: p = PAGE_EXECUTE_READWRITE; break;
    case PAGE_WRITECOPY: p = PAGE_EXECUTE_WRITECOPY; break;
    case 0: p = PAGE_EXECUTE_READWRITE; break;
    default: break;
    }
    return p | guard;
}

static uint32_t to_xbox_protect(DWORD p)
{
    switch (p & 0xFF) {
    case PAGE_EXECUTE_READ: return PAGE_READONLY;
    case PAGE_EXECUTE_READWRITE: return PAGE_READWRITE;
    case PAGE_EXECUTE_WRITECOPY: return PAGE_WRITECOPY;
    default: return p;
    }
}

KEXPORT(void *) kx_MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress,
                                                uint32_t HighestAcceptableAddress, uint32_t Alignment,
                                                uint32_t ProtectionType)
{
    void *p = mem_alloc_contiguous(NumberOfBytes, LowestAcceptableAddress, HighestAcceptableAddress, Alignment);
    LOG_DEBUG("mm", "MmAllocateContiguousMemoryEx(0x%X, %X-%X, align %X) -> %p", NumberOfBytes,
              LowestAcceptableAddress, HighestAcceptableAddress, Alignment, p);
    return p;
}

KEXPORT(void *) kx_MmAllocateContiguousMemory(uint32_t NumberOfBytes)
{
    return kx_MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

KEXPORT(void) kx_MmFreeContiguousMemory(void *BaseAddress)
{
    mem_free_contiguous(BaseAddress);
}

KEXPORT(void) kx_MmPersistContiguousMemory(void *BaseAddress, uint32_t NumberOfBytes, uint8_t Persist) {}

KEXPORT(uint32_t) kx_MmGetPhysicalAddress(void *BaseAddress)
{
    return mem_physical_address(BaseAddress);
}

KEXPORT(uint32_t) kx_MmQueryAllocationSize(void *BaseAddress)
{
    if (mem_is_contiguous(BaseAddress))
        return mem_contiguous_size(BaseAddress);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(BaseAddress, &mbi, sizeof mbi))
        return 0;
    // Sum the committed regions of this allocation.
    uint8_t *p = static_cast<uint8_t *>(mbi.AllocationBase);
    uint32_t total = 0;
    while (VirtualQuery(p, &mbi, sizeof mbi) && mbi.AllocationBase == BaseAddress) {
        if (mbi.State == MEM_COMMIT)
            total += uint32_t(mbi.RegionSize);
        p += mbi.RegionSize;
    }
    return total;
}

KEXPORT(void) kx_MmSetAddressProtect(void *BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect)
{
    DWORD old;
    if (!VirtualProtect(BaseAddress, NumberOfBytes, to_host_protect(NewProtect), &old))
        LOG_WARN("mm", "MmSetAddressProtect(%p, %X, %X) failed (%lu)", BaseAddress, NumberOfBytes, NewProtect,
                 GetLastError());
}

KEXPORT(uint32_t) kx_MmQueryAddressProtect(void *VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(VirtualAddress, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    return to_xbox_protect(mbi.Protect);
}

KEXPORT(void) kx_MmLockUnlockBufferPages(void *BaseAddress, uint32_t NumberOfBytes, uint8_t UnlockPages) {}
KEXPORT(void) kx_MmLockUnlockPhysicalPage(uint32_t PhysicalAddress, uint8_t UnlockPage) {}

KEXPORT(void *) kx_MmClaimGpuInstanceMemory(uint32_t NumberOfBytes, uint32_t *NumberOfPaddingBytes)
{
    // Instance memory ends 64 KiB below the top of RAM (the padding), as on a
    // retail kernel. PRAMIN aliases the whole top 1 MiB.
    constexpr uint32_t padding = 0x10000;
    if (NumberOfPaddingBytes)
        *NumberOfPaddingBytes = padding;
    if (NumberOfBytes == 0xFFFFFFFF)
        NumberOfBytes = XBOX_INSTANCE_SIZE - padding;
    void *p = phys_ptr(XBOX_RAM_SIZE - padding - NumberOfBytes);
    LOG_INFO("mm", "MmClaimGpuInstanceMemory(0x%X) -> %p", NumberOfBytes, p);
    return p;
}

KEXPORT(void *) kx_MmCreateKernelStack(uint32_t NumberOfBytes, uint8_t DebuggerThread)
{
    uint8_t *p = static_cast<uint8_t *>(VirtualAlloc(nullptr, NumberOfBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return p ? p + NumberOfBytes : nullptr;
}

KEXPORT(void) kx_MmDeleteKernelStack(void *StackBase, void *StackLimit)
{
    VirtualFree(StackLimit, 0, MEM_RELEASE);
}

KEXPORT(NTSTATUS) kx_MmQueryStatistics(XMM_STATISTICS *Statistics)
{
    if (!Statistics || Statistics->Length != sizeof(XMM_STATISTICS))
        return X_STATUS_INVALID_PARAMETER;
    Statistics->TotalPhysicalPages = XBOX_RAM_SIZE / 0x1000;
    Statistics->AvailablePages = mem_available_pages();
    Statistics->VirtualMemoryBytesCommitted = 0x01000000;
    Statistics->VirtualMemoryBytesReserved = 0x02000000;
    Statistics->CachePagesCommitted = 0x100;
    Statistics->PoolPagesCommitted = 0x100;
    Statistics->StackPagesCommitted = 0x40;
    Statistics->ImagePagesCommitted = 0xD64;
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtAllocateVirtualMemory(void **BaseAddress, uint32_t ZeroBits, uint32_t *AllocationSize,
                                             uint32_t AllocationType, uint32_t Protect)
{
    // MEM_NOZERO (0x800000) is Xbox-only; MEM_TOP_DOWN would hand out
    // addresses above 2 GiB, which Xbox code treats as physical memory.
    DWORD type = AllocationType & ~(0x800000u | MEM_TOP_DOWN);
    void *p = VirtualAlloc(*BaseAddress, *AllocationSize, type, to_host_protect(Protect));
    if (!p) {
        DWORD e = GetLastError();
        LOG_WARN("mm", "NtAllocateVirtualMemory(%p, 0x%X, type %X) failed (%lu)", *BaseAddress, *AllocationSize,
                 AllocationType, e);
        return e == ERROR_INVALID_ADDRESS ? X_STATUS_CONFLICTING_ADDRESSES : X_STATUS_NO_MEMORY;
    }
    if (uintptr_t(p) >= 0x80000000u)
        LOG_WARN("mm", "virtual allocation landed above 2 GiB (%p)", p);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(p, &mbi, sizeof mbi);
    *BaseAddress = (type & MEM_RESERVE) ? mbi.AllocationBase : mbi.BaseAddress;
    *AllocationSize = uint32_t((*AllocationSize + 0xFFF) & ~0xFFFu);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtFreeVirtualMemory(void **BaseAddress, uint32_t *FreeSize, uint32_t FreeType)
{
    SIZE_T size = (FreeType & MEM_RELEASE) ? 0 : *FreeSize;
    if (!VirtualFree(*BaseAddress, size, FreeType)) {
        LOG_WARN("mm", "NtFreeVirtualMemory(%p, 0x%X, %X) failed (%lu)", *BaseAddress, *FreeSize, FreeType,
                 GetLastError());
        return X_STATUS_MEMORY_NOT_ALLOCATED;
    }
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtQueryVirtualMemory(void *BaseAddress, MEMORY_BASIC_INFORMATION *MemoryInformation)
{
    if (!VirtualQuery(BaseAddress, MemoryInformation, sizeof *MemoryInformation))
        return X_STATUS_INVALID_PARAMETER;
    MemoryInformation->Protect = to_xbox_protect(MemoryInformation->Protect);
    MemoryInformation->AllocationProtect = to_xbox_protect(MemoryInformation->AllocationProtect);
    if (MemoryInformation->Type == MEM_IMAGE || MemoryInformation->Type == MEM_MAPPED)
        MemoryInformation->Type = MEM_PRIVATE;
    return X_STATUS_SUCCESS;
}

// ---- executive pool --------------------------------------------------------
static std::mutex g_pool_mutex;
static std::unordered_map<void *, uint32_t> g_pool_sizes;

KEXPORT(void *) kx_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag)
{
    void *p = _aligned_malloc(NumberOfBytes ? NumberOfBytes : 1, 16);
    if (p) {
        memset(p, 0, NumberOfBytes);
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        g_pool_sizes[p] = NumberOfBytes;
    }
    return p;
}

KEXPORT(void *) kx_ExAllocatePool(uint32_t NumberOfBytes)
{
    return kx_ExAllocatePoolWithTag(NumberOfBytes, 'enoN');
}

KEXPORT(void) kx_ExFreePool(void *P)
{
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        g_pool_sizes.erase(P);
    }
    _aligned_free(P);
}

KEXPORT(uint32_t) kx_ExQueryPoolBlockSize(void *PoolBlock)
{
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    auto it = g_pool_sizes.find(PoolBlock);
    return it == g_pool_sizes.end() ? 0 : it->second;
}
