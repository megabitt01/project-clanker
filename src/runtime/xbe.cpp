#include "xbe.h"
#include "log.h"

#include <windows.h>
#include <cstdio>

namespace rt {

static constexpr uint32_t XBE_BASE = 0x00010000;
static constexpr uint32_t ENTRY_KEY_RETAIL = 0xA8FC57AB;
static constexpr uint32_t THUNK_KEY_RETAIL = 0x5B6D40B6;
static constexpr uint32_t SECTION_PRELOAD = 0x2;

static std::vector<uint8_t> g_file;

// The XBE image header normally lives at the image base (0x00010000). That
// address holds swbf2.exe's own PE header, which Windows keeps consulting
// (e.g. for thread stack sizes), so the XBE header is placed at
// XBE_HEADER_COPY instead -- free space inside the reserved image range -- and
// every instruction that addresses a header field directly is retargeted.
static constexpr uint32_t XBE_HEADER_COPY = 0x00D80000;

struct HeaderRef {
    uint32_t insn;      // instruction address
    uint8_t disp_off;   // offset of the 32-bit absolute address in it
    uint32_t field;     // original header field address
};
// Found with tools/ghidra/HdrRefs.java (absolute memory operands into
// 0x10000-0x10BE0); all are in XAPI, XONLINE and XNET.
static const HeaderRef k_header_refs[] = {
    {0x0031efbe, 2, 0x10118},
    {0x0031f17a, 1, 0x10118},
    {0x0031f580, 1, 0x10118},
    {0x0031f9b8, 2, 0x10118},
    {0x00321343, 1, 0x10130},
    {0x0032165c, 1, 0x10118},
    {0x003218db, 2, 0x10124},
    {0x00321f29, 2, 0x10118},
    {0x00321f2f, 1, 0x10108},
    {0x0032248e, 1, 0x10118},
    {0x0032282a, 2, 0x10118},
    {0x003229af, 1, 0x10118},
    {0x00322a8a, 1, 0x10118},
    {0x00323126, 1, 0x10118},
    {0x003231cd, 2, 0x10138},
    {0x003231d3, 2, 0x10134},
    {0x003231ea, 2, 0x10124},
    {0x00323240, 2, 0x10124},
    {0x00323249, 1, 0x10118},
    {0x00323274, 1, 0x10124},
    {0x00323297, 1, 0x10118},
    {0x003238dd, 2, 0x1011c},
    {0x003238e3, 2, 0x10120},
    {0x003bdce5, 1, 0x10118},
    {0x003c2f17, 1, 0x10118},
    {0x003c2f95, 1, 0x10118},
    {0x003c300a, 1, 0x10118},
    {0x003c302d, 1, 0x10118},
    {0x003c3045, 1, 0x10118},
    {0x003c31ad, 2, 0x10124},
    {0x003c4802, 1, 0x10118},
    {0x003e326e, 2, 0x10118},
    {0x003e785b, 2, 0x10118},
    {0x003f237c, 1, 0x10118}
};

static uint32_t rd32(uint32_t off) { return *reinterpret_cast<const uint32_t *>(&g_file[off]); }

void xbe_load_section(XbeSectionHeader *s)
{
    uint8_t *dst = reinterpret_cast<uint8_t *>(uintptr_t(s->VirtualAddress));
    memset(dst, 0, s->VirtualSize);
    uint32_t n = s->RawSize < s->VirtualSize ? s->RawSize : s->VirtualSize;
    if (s->RawAddress + n <= g_file.size())
        memcpy(dst, &g_file[s->RawAddress], n);
}

static uint32_t &hdr32(uint32_t off) { return *reinterpret_cast<uint32_t *>(uintptr_t(XBE_HEADER_COPY + off)); }

static XbeSectionHeader *sections(uint32_t &count)
{
    count = hdr32(0x11C);
    return reinterpret_cast<XbeSectionHeader *>(uintptr_t(hdr32(0x120)));
}

uint32_t xbe_header_address() { return XBE_HEADER_COPY; }

static void relocate_header(uint32_t header_size)
{
    auto reloc = [&](uint32_t &v) {
        if (v >= XBE_BASE && v < XBE_BASE + header_size)
            v = v - XBE_BASE + XBE_HEADER_COPY;
    };
    // Pointer fields of the image header (0x104, the base address, stays).
    static const uint32_t ptr_fields[] = {0x118, 0x120, 0x14C, 0x150, 0x154, 0x164, 0x168, 0x16C, 0x170, 0x178};
    for (uint32_t f : ptr_fields)
        reloc(hdr32(f));
    uint32_t n;
    XbeSectionHeader *s = sections(n);
    for (uint32_t i = 0; i < n; i++) {
        reloc(*reinterpret_cast<uint32_t *>(&s[i].SectionName));
        reloc(*reinterpret_cast<uint32_t *>(&s[i].HeadSharedPageReferenceCount));
        reloc(*reinterpret_cast<uint32_t *>(&s[i].TailSharedPageReferenceCount));
    }
}

static void patch_header_refs()
{
    for (const HeaderRef &r : k_header_refs) {
        uint32_t &disp = *reinterpret_cast<uint32_t *>(uintptr_t(r.insn + r.disp_off));
        if (disp != r.field)
            fatal("XBE header reference at 0x%08X does not match: is default.xbe the expected build?", r.insn);
        disp = r.field - XBE_BASE + XBE_HEADER_COPY;
    }
    LOG_INFO("xbe", "image header at 0x%08X, %zu references retargeted", XBE_HEADER_COPY,
             sizeof k_header_refs / sizeof k_header_refs[0]);
}

XbeSectionHeader *xbe_find_section(const char *name)
{
    uint32_t n;
    XbeSectionHeader *s = sections(n);
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(s[i].SectionName, name))
            return &s[i];
    return nullptr;
}

bool xbe_load(const char *path, XbeInfo &info)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("xbe", "cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file.resize(size);
    fread(g_file.data(), 1, size, f);
    fclose(f);

    if (size < 0x1000 || memcmp(g_file.data(), "XBEH", 4) != 0) {
        LOG_ERROR("xbe", "%s is not an XBE", path);
        return false;
    }
    if (rd32(0x104) != XBE_BASE) {
        LOG_ERROR("xbe", "unexpected base address 0x%08X", rd32(0x104));
        return false;
    }
    uint32_t header_size = rd32(0x108);
    uint32_t image_size = rd32(0x10C);

    memcpy(reinterpret_cast<void *>(uintptr_t(XBE_HEADER_COPY)), g_file.data(), header_size);
    relocate_header(header_size);

    uint32_t n;
    XbeSectionHeader *s = sections(n);
    for (uint32_t i = 0; i < n; i++) {
        xbe_load_section(&s[i]);
        if (s[i].Flags & SECTION_PRELOAD)
            s[i].SectionReferenceCount = 1;
        LOG_DEBUG("xbe", "section %-10s %08X-%08X flags %X", s[i].SectionName, s[i].VirtualAddress,
                  s[i].VirtualAddress + s[i].VirtualSize, s[i].Flags);
    }

    patch_header_refs();

    info.entry_point = rd32(0x128) ^ ENTRY_KEY_RETAIL;
    info.kernel_thunk = rd32(0x158) ^ THUNK_KEY_RETAIL;
    info.stack_size = rd32(0x130);
    info.tls_addr = rd32(0x12C);
    info.image_end = XBE_BASE + image_size;
    uint32_t cert = rd32(0x118) - XBE_BASE;
    info.title_id = rd32(cert + 8);
    const wchar_t *wname = reinterpret_cast<const wchar_t *>(&g_file[cert + 12]);
    for (int i = 0; i < 40 && wname[i]; i++)
        info.title_name += char(wname[i]);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    LOG_INFO("xbe", "loaded '%s' (title %08X), %u sections, entry %08X, image ends %08X", info.title_name.c_str(),
             info.title_id, n, info.entry_point, info.image_end);
    return true;
}

} // namespace rt
