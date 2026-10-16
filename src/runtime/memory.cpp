#include "memory.h"
#include "log.h"

#include <windows.h>
#include <map>
#include <mutex>
#include <vector>

namespace rt {

static HANDLE g_ram_section;
static std::mutex g_phys_mutex;
static std::vector<uint8_t> g_page_used;        // one byte per 4 KiB physical page
static std::map<uint32_t, uint32_t> g_allocs;   // phys base -> size in bytes

static constexpr uint32_t PAGE = 0x1000;
static constexpr uint32_t NUM_PAGES = XBOX_RAM_SIZE / PAGE;

// The Xbox code needs fixed addresses high in the 32-bit address space. With
// Proton's classic 32-bit Wine, native Linux libraries can occupy them.
static const char *wine_hint()
{
    if (!GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version"))
        return "";
    return "\n\nThe address is in use by another library. Under Proton, try the launch option "
           "PROTON_USE_WOW64=1 %command% (Proton 9 or newer), which keeps native libraries out of the 32-bit "
           "address space.";
}

static void *map_view(uint32_t va, uint32_t offset, uint32_t size)
{
    void *p = MapViewOfFileEx(g_ram_section, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, offset, size,
                              reinterpret_cast<void *>(uintptr_t(va)));
    if (p != reinterpret_cast<void *>(uintptr_t(va)))
        fatal("Could not map Xbox physical memory at 0x%08X (error %lu).%s", va, GetLastError(), wine_hint());
    return p;
}

static void reserve_noaccess(uint32_t va, uint32_t size)
{
    void *p = VirtualAlloc(reinterpret_cast<void *>(uintptr_t(va)), size, MEM_RESERVE, PAGE_NOACCESS);
    if (p != reinterpret_cast<void *>(uintptr_t(va)))
        fatal("Could not reserve MMIO window 0x%08X+0x%X (error %lu).%s", va, size, GetLastError(), wine_hint());
}

void mem_init(void *image_space, uint32_t image_space_size)
{
    // Everything after the loader's PE header page is ours: make it writable
    // and executable so the XBE sections can be copied in and run.
    DWORD old;
    uint8_t *base = reinterpret_cast<uint8_t *>(0x11000);
    uint32_t end = uint32_t(uintptr_t(image_space)) + image_space_size;
    if (!VirtualProtect(base, end - 0x11000, PAGE_EXECUTE_READWRITE, &old))
        fatal("VirtualProtect of the XBE image range failed (error %lu)", GetLastError());

    // swbf2.exe reserved these ranges at startup so nothing else could take
    // them; release the reservations now that they are mapped for real.
    for (uint32_t va : {XBOX_PHYS_BASE, XBOX_PHYS_WC_BASE, NV2A_MMIO_BASE, MCPX_MMIO_BASE})
        VirtualFree(reinterpret_cast<void *>(uintptr_t(va)), 0, MEM_RELEASE);

    g_ram_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0,
                                       XBOX_RAM_SIZE, nullptr);
    if (!g_ram_section)
        fatal("CreateFileMapping for Xbox RAM failed (error %lu)", GetLastError());
    map_view(XBOX_PHYS_BASE, 0, XBOX_RAM_SIZE);
    map_view(XBOX_PHYS_WC_BASE, 0, XBOX_RAM_SIZE);

    // NV2A: BAR0 registers trap (no access) so every access is emulated,
    // except PRAMIN which is a window onto the top 1 MiB of RAM.
    reserve_noaccess(NV2A_MMIO_BASE, 0x700000);
    map_view(NV2A_PRAMIN_BASE, XBOX_RAM_SIZE - XBOX_INSTANCE_SIZE, XBOX_INSTANCE_SIZE);
    reserve_noaccess(NV2A_MMIO_BASE + 0x800000, 0x800000);
    reserve_noaccess(MCPX_MMIO_BASE, MCPX_MMIO_SIZE);

    g_page_used.assign(NUM_PAGES, 0);
    // Low 1 MiB: on hardware this holds the kernel. D3D also scribbles on
    // physical address 0 during device bring-up.
    mem_reserve_physical(0, 0x100000);
    // Top 1 MiB: GPU instance memory (claimed via MmClaimGpuInstanceMemory).
    mem_reserve_physical(XBOX_RAM_SIZE - XBOX_INSTANCE_SIZE, XBOX_INSTANCE_SIZE);
    LOG_INFO("mem", "Xbox RAM mapped at 0x%08X and 0x%08X, MMIO traps installed", XBOX_PHYS_BASE,
             XBOX_PHYS_WC_BASE);
}

void mem_reserve_physical(uint32_t phys, uint32_t size)
{
    std::lock_guard<std::mutex> lock(g_phys_mutex);
    for (uint32_t p = phys / PAGE; p < (phys + size + PAGE - 1) / PAGE && p < NUM_PAGES; p++)
        g_page_used[p] = 1;
}

void *mem_alloc_contiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment)
{
    if (size == 0)
        return nullptr;
    uint32_t pages = (size + PAGE - 1) / PAGE;
    uint32_t align_pages = alignment > PAGE ? (alignment + PAGE - 1) / PAGE : 1;
    if (highest >= XBOX_RAM_SIZE)
        highest = XBOX_RAM_SIZE - 1;
    uint32_t lo = lowest / PAGE, hi = highest / PAGE;

    std::lock_guard<std::mutex> lock(g_phys_mutex);
    // Allocate top-down like the Xbox kernel does, leaving low memory for
    // allocations that demand it.
    int64_t first = int64_t(hi) + 1 - pages;
    if (first < 0)
        return nullptr;
    first -= first % align_pages;
    for (int64_t start = first; start >= int64_t(lo); start -= align_pages) {
        bool ok = true;
        for (uint32_t i = 0; i < pages; i++) {
            if (g_page_used[uint32_t(start) + i]) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        for (uint32_t i = 0; i < pages; i++)
            g_page_used[uint32_t(start) + i] = 1;
        uint32_t phys = uint32_t(start) * PAGE;
        g_allocs[phys] = pages * PAGE;
        void *va = phys_ptr(phys);
        memset(va, 0, pages * PAGE);
        return va;
    }
    LOG_ERROR("mem", "contiguous allocation of 0x%X bytes failed", size);
    return nullptr;
}

bool mem_is_contiguous(const void *p)
{
    uint32_t a = uint32_t(uintptr_t(p));
    return (a >= XBOX_PHYS_BASE && a < XBOX_PHYS_BASE + XBOX_RAM_SIZE) ||
           (a >= XBOX_PHYS_WC_BASE && a < XBOX_PHYS_WC_BASE + XBOX_RAM_SIZE);
}

uint32_t mem_physical_address(const void *va)
{
    uint32_t a = uint32_t(uintptr_t(va));
    if (a >= XBOX_PHYS_BASE && a < XBOX_PHYS_BASE + XBOX_RAM_SIZE)
        return a - XBOX_PHYS_BASE;
    if (a >= XBOX_PHYS_WC_BASE && a < XBOX_PHYS_WC_BASE + XBOX_RAM_SIZE)
        return a - XBOX_PHYS_WC_BASE;
    if (a >= NV2A_PRAMIN_BASE && a < NV2A_PRAMIN_BASE + XBOX_INSTANCE_SIZE)
        return XBOX_RAM_SIZE - XBOX_INSTANCE_SIZE + (a - NV2A_PRAMIN_BASE);
    LOG_WARN("mem", "MmGetPhysicalAddress on non-contiguous address 0x%08X", a);
    return a;
}

bool mem_free_contiguous(void *p)
{
    if (!mem_is_contiguous(p))
        return false;
    uint32_t phys = mem_physical_address(p);
    std::lock_guard<std::mutex> lock(g_phys_mutex);
    auto it = g_allocs.find(phys);
    if (it == g_allocs.end()) {
        LOG_WARN("mem", "MmFreeContiguousMemory on unknown block 0x%08X", uint32_t(uintptr_t(p)));
        return false;
    }
    for (uint32_t i = 0; i < it->second / PAGE; i++)
        g_page_used[phys / PAGE + i] = 0;
    g_allocs.erase(it);
    return true;
}

uint32_t mem_contiguous_size(void *p)
{
    uint32_t phys = mem_physical_address(p);
    std::lock_guard<std::mutex> lock(g_phys_mutex);
    auto it = g_allocs.upper_bound(phys);
    if (it == g_allocs.begin())
        return 0;
    --it;
    if (phys >= it->first && phys < it->first + it->second)
        return it->second - (phys - it->first);
    return 0;
}

uint32_t mem_available_pages()
{
    std::lock_guard<std::mutex> lock(g_phys_mutex);
    uint32_t n = 0;
    for (uint8_t u : g_page_used)
        n += !u;
    return n;
}

} // namespace rt
