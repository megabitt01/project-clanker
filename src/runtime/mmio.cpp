// Emulation of hardware register accesses.
//
// The Xbox libraries (D3D, DirectSound, the USB stack) poke device registers
// directly. Those register windows are reserved PAGE_NOACCESS, so each access
// raises an access violation that lands here. We decode the faulting
// instruction just far enough to know its length, operand size and register
// operands, perform the access against the device model, and resume.
#include "mmio.h"
#include "log.h"
#include "crash.h"

#include <windows.h>
#include <vector>

namespace rt {

struct MmioRegion {
    uint32_t base, size;
    MmioReadFn read;
    MmioWriteFn write;
};
static std::vector<MmioRegion> g_regions;
static PortReadFn g_port_read;
static PortWriteFn g_port_write;

void mmio_register(uint32_t base, uint32_t size, MmioReadFn read, MmioWriteFn write)
{
    g_regions.push_back({base, size, read, write});
}

void port_set_handlers(PortReadFn read, PortWriteFn write)
{
    g_port_read = read;
    g_port_write = write;
}

static const MmioRegion *find_region(uint32_t addr)
{
    for (const auto &r : g_regions)
        if (addr - r.base < r.size)
            return &r;
    return nullptr;
}

static uint32_t mem_read(uint32_t addr, int size)
{
    if (const MmioRegion *r = find_region(addr))
        return r->read(addr, size);
    LOG_WARN("mmio", "read%d from unmapped 0x%08X", size * 8, addr);
    return 0;
}

static void mem_write(uint32_t addr, uint32_t value, int size)
{
    if (const MmioRegion *r = find_region(addr)) {
        r->write(addr, value, size);
        return;
    }
    LOG_WARN("mmio", "write%d 0x%X to unmapped 0x%08X", size * 8, value, addr);
}

// ---------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------
static DWORD *gpr(CONTEXT *c, int r)
{
    switch (r & 7) {
    case 0: return &c->Eax;
    case 1: return &c->Ecx;
    case 2: return &c->Edx;
    case 3: return &c->Ebx;
    case 4: return &c->Esp;
    case 5: return &c->Ebp;
    case 6: return &c->Esi;
    default: return &c->Edi;
    }
}

static uint32_t get_reg(CONTEXT *c, int r, int size)
{
    if (size == 1)
        return r < 4 ? (*gpr(c, r) & 0xFF) : ((*gpr(c, r - 4) >> 8) & 0xFF);
    if (size == 2)
        return *gpr(c, r) & 0xFFFF;
    return *gpr(c, r);
}

static void set_reg(CONTEXT *c, int r, uint32_t v, int size)
{
    if (size == 1) {
        if (r < 4)
            *gpr(c, r) = (*gpr(c, r) & ~0xFFu) | (v & 0xFF);
        else
            *gpr(c, r - 4) = (*gpr(c, r - 4) & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if (size == 2) {
        *gpr(c, r) = (*gpr(c, r) & ~0xFFFFu) | (v & 0xFFFF);
    } else {
        *gpr(c, r) = v;
    }
}

static int modrm_length(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    if (mod != 3 && rm == 4) {
        if (mod == 0 && (p[1] & 7) == 5)
            n += 4;
        n++;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    if (mod == 1)
        n += 1;
    else if (mod == 2)
        n += 4;
    return n;
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
enum : uint32_t { F_CF = 1, F_PF = 4, F_AF = 0x10, F_ZF = 0x40, F_SF = 0x80, F_OF = 0x800 };
constexpr uint32_t F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF;

static uint32_t mask_of(int size) { return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; }
static uint32_t sign_of(int size) { return size == 4 ? 0x80000000u : size == 2 ? 0x8000u : 0x80u; }

static uint32_t szp_flags(uint32_t r, int size)
{
    uint32_t f = 0;
    r &= mask_of(size);
    if (r == 0) f |= F_ZF;
    if (r & sign_of(size)) f |= F_SF;
    uint8_t lo = uint8_t(r);
    lo ^= lo >> 4; lo ^= lo >> 2; lo ^= lo >> 1;
    if (!(lo & 1)) f |= F_PF;
    return f;
}

// op: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp. Returns result,
// updates eflags.
static uint32_t alu(int op, uint32_t a, uint32_t b, int size, DWORD &eflags)
{
    uint32_t m = mask_of(size), s = sign_of(size);
    a &= m;
    b &= m;
    uint32_t r = 0, f = 0;
    uint32_t cin = eflags & F_CF;
    switch (op) {
    case 0: case 2: {
        uint64_t wide = uint64_t(a) + b + (op == 2 ? cin : 0);
        r = uint32_t(wide) & m;
        if (wide > m) f |= F_CF;
        if ((a ^ r) & (b ^ r) & s) f |= F_OF;
        if ((a ^ b ^ r) & 0x10) f |= F_AF;
        break;
    }
    case 3: case 5: case 7: {
        uint64_t sub = uint64_t(b) + (op == 3 ? cin : 0);
        r = uint32_t(uint64_t(a) - sub) & m;
        if (uint64_t(a) < sub) f |= F_CF;
        if ((a ^ b) & (a ^ r) & s) f |= F_OF;
        if ((a ^ b ^ r) & 0x10) f |= F_AF;
        break;
    }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    case 6: r = a ^ b; break;
    }
    f |= szp_flags(r, size);
    eflags = (eflags & ~F_ARITH) | f;
    return r;
}

// ---------------------------------------------------------------------------
// Instruction emulation for a faulting MMIO access
// ---------------------------------------------------------------------------
static bool emulate_mmio(CONTEXT *c, uint32_t addr)
{
    const uint8_t *ip = reinterpret_cast<const uint8_t *>(uintptr_t(c->Eip));
    const uint8_t *p = ip;
    bool opsize16 = false, rep = false;
    for (;; p++) {
        uint8_t b = *p;
        if (b == 0x66) opsize16 = true;
        else if (b == 0xF3 || b == 0xF2) rep = true;
        else if (b == 0xF0 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {}
        else break;
    }
    int osz = opsize16 ? 2 : 4;
    uint8_t op = *p++;

    auto finish = [&](const uint8_t *end) {
        c->Eip = DWORD(uintptr_t(end));
        return true;
    };

    switch (op) {
    case 0x88: case 0x89: { // mov r/m, r
        int size = op == 0x88 ? 1 : osz;
        int reg = (p[0] >> 3) & 7;
        mem_write(addr, get_reg(c, reg, size), size);
        return finish(p + modrm_length(p));
    }
    case 0x8A: case 0x8B: { // mov r, r/m
        int size = op == 0x8A ? 1 : osz;
        int reg = (p[0] >> 3) & 7;
        set_reg(c, reg, mem_read(addr, size), size);
        return finish(p + modrm_length(p));
    }
    case 0xC6: case 0xC7: { // mov r/m, imm
        int size = op == 0xC6 ? 1 : osz;
        int n = modrm_length(p);
        uint32_t imm = size == 1 ? p[n] : size == 2 ? *reinterpret_cast<const uint16_t *>(p + n)
                                                     : *reinterpret_cast<const uint32_t *>(p + n);
        mem_write(addr, imm, size);
        return finish(p + n + size);
    }
    case 0xA0: case 0xA1: { // mov al/eax, moffs
        int size = op == 0xA0 ? 1 : osz;
        set_reg(c, 0, mem_read(addr, size), size);
        return finish(p + 4);
    }
    case 0xA2: case 0xA3: { // mov moffs, al/eax
        int size = op == 0xA2 ? 1 : osz;
        mem_write(addr, get_reg(c, 0, size), size);
        return finish(p + 4);
    }
    case 0x84: case 0x85: { // test r/m, r
        int size = op == 0x84 ? 1 : osz;
        alu(4, mem_read(addr, size), get_reg(c, (p[0] >> 3) & 7, size), size, c->EFlags);
        return finish(p + modrm_length(p));
    }
    case 0x86: case 0x87: { // xchg r/m, r
        int size = op == 0x86 ? 1 : osz;
        int reg = (p[0] >> 3) & 7;
        uint32_t old = mem_read(addr, size);
        mem_write(addr, get_reg(c, reg, size), size);
        set_reg(c, reg, old, size);
        return finish(p + modrm_length(p));
    }
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int aop = op >> 3;
        int size = (op & 1) ? osz : 1;
        bool to_reg = (op & 2) != 0;
        int reg = (p[0] >> 3) & 7;
        uint32_t m = mem_read(addr, size), r = get_reg(c, reg, size);
        if (to_reg) {
            uint32_t res = alu(aop, r, m, size, c->EFlags);
            if (aop != 7) set_reg(c, reg, res, size);
        } else {
            uint32_t res = alu(aop, m, r, size, c->EFlags);
            if (aop != 7) mem_write(addr, res, size);
        }
        return finish(p + modrm_length(p));
    }
    case 0x80: case 0x81: case 0x83: { // group 1 r/m, imm
        int size = op == 0x80 ? 1 : osz;
        int aop = (p[0] >> 3) & 7;
        int n = modrm_length(p);
        uint32_t imm;
        int ilen;
        if (op == 0x81) {
            imm = size == 2 ? *reinterpret_cast<const uint16_t *>(p + n) : *reinterpret_cast<const uint32_t *>(p + n);
            ilen = size;
        } else {
            imm = uint32_t(int32_t(int8_t(p[n])));
            ilen = 1;
        }
        uint32_t res = alu(aop, mem_read(addr, size), imm, size, c->EFlags);
        if (aop != 7) mem_write(addr, res, size);
        return finish(p + n + ilen);
    }
    case 0xF6: case 0xF7: { // test r/m, imm (only /0 is meaningful on MMIO)
        int size = op == 0xF6 ? 1 : osz;
        int sub = (p[0] >> 3) & 7;
        int n = modrm_length(p);
        if (sub != 0)
            break;
        uint32_t imm = size == 1 ? p[n] : size == 2 ? *reinterpret_cast<const uint16_t *>(p + n)
                                                     : *reinterpret_cast<const uint32_t *>(p + n);
        alu(4, mem_read(addr, size), imm, size, c->EFlags);
        return finish(p + n + size);
    }
    case 0xFE: case 0xFF: { // inc / dec / push r/m
        int size = op == 0xFE ? 1 : osz;
        int sub = (p[0] >> 3) & 7;
        int n = modrm_length(p);
        if (sub == 0 || sub == 1) {
            DWORD cf = c->EFlags & F_CF;
            uint32_t res = alu(sub == 0 ? 0 : 5, mem_read(addr, size), 1, size, c->EFlags);
            c->EFlags = (c->EFlags & ~F_CF) | cf;
            mem_write(addr, res, size);
            return finish(p + n);
        }
        if (sub == 6 && op == 0xFF) {
            uint32_t v = mem_read(addr, 4);
            c->Esp -= 4;
            *reinterpret_cast<uint32_t *>(uintptr_t(c->Esp)) = v;
            return finish(p + n);
        }
        break;
    }
    case 0x0F: {
        uint8_t op2 = *p++;
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            int size = (op2 & 1) ? 2 : 1;
            uint32_t v = mem_read(addr, size);
            if (op2 == 0xBE) v = uint32_t(int32_t(int8_t(v)));
            if (op2 == 0xBF) v = uint32_t(int32_t(int16_t(v)));
            set_reg(c, (p[0] >> 3) & 7, v, osz);
            return finish(p + modrm_length(p));
        }
        break;
    }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: { // movs / stos (possibly rep)
        int size = (op & 1) ? osz : 1;
        uint32_t count = rep ? c->Ecx : 1;
        int step = (c->EFlags & 0x400) ? -size : size;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v;
            if (op == 0xAA || op == 0xAB) {
                v = get_reg(c, 0, size);
            } else if (find_region(c->Esi)) {
                v = mem_read(c->Esi, size);
            } else {
                v = 0;
                memcpy(&v, reinterpret_cast<void *>(uintptr_t(c->Esi)), size);
            }
            if (find_region(c->Edi))
                mem_write(c->Edi, v, size);
            else
                memcpy(reinterpret_cast<void *>(uintptr_t(c->Edi)), &v, size);
            if (op == 0xA4 || op == 0xA5)
                c->Esi += step;
            c->Edi += step;
        }
        if (rep)
            c->Ecx = 0;
        return finish(p);
    }
    default:
        break;
    }
    LOG_ERROR("mmio", "cannot emulate instruction at 0x%08X (bytes %02X %02X %02X %02X %02X %02X) accessing 0x%08X",
              c->Eip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], addr);
    return false;
}

// ---------------------------------------------------------------------------
// Privileged instructions
// ---------------------------------------------------------------------------
static bool emulate_privileged(CONTEXT *c)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(uintptr_t(c->Eip));
    bool opsize16 = false;
    if (*p == 0x66) {
        opsize16 = true;
        p++;
    }
    int osz = opsize16 ? 2 : 4;
    auto port_in = [&](uint16_t port, int size) {
        uint32_t v = g_port_read ? g_port_read(port, size) : 0;
        set_reg(c, 0, v, size);
    };
    auto port_out = [&](uint16_t port, int size) {
        if (g_port_write) g_port_write(port, get_reg(c, 0, size), size);
    };
    switch (p[0]) {
    case 0xFA: case 0xFB: // cli / sti: interrupts are emulated, nothing to do
        c->Eip = DWORD(uintptr_t(p + 1));
        return true;
    case 0xF4: // hlt
        Sleep(0);
        c->Eip = DWORD(uintptr_t(p + 1));
        return true;
    case 0x0F:
        if (p[1] == 0x09 || p[1] == 0x08) { // wbinvd / invd
            c->Eip = DWORD(uintptr_t(p + 2));
            return true;
        }
        break;
    case 0xE4: port_in(p[1], 1); c->Eip = DWORD(uintptr_t(p + 2)); return true;
    case 0xE5: port_in(p[1], osz); c->Eip = DWORD(uintptr_t(p + 2)); return true;
    case 0xEC: port_in(uint16_t(c->Edx), 1); c->Eip = DWORD(uintptr_t(p + 1)); return true;
    case 0xED: port_in(uint16_t(c->Edx), osz); c->Eip = DWORD(uintptr_t(p + 1)); return true;
    case 0xE6: port_out(p[1], 1); c->Eip = DWORD(uintptr_t(p + 2)); return true;
    case 0xE7: port_out(p[1], osz); c->Eip = DWORD(uintptr_t(p + 2)); return true;
    case 0xEE: port_out(uint16_t(c->Edx), 1); c->Eip = DWORD(uintptr_t(p + 1)); return true;
    case 0xEF: port_out(uint16_t(c->Edx), osz); c->Eip = DWORD(uintptr_t(p + 1)); return true;
    }
    return false;
}

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: {
        uint32_t addr = uint32_t(er->ExceptionInformation[1]);
        if (find_region(addr) && emulate_mmio(c, addr))
            return EXCEPTION_CONTINUE_EXECUTION;
        break;
    }
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        if (emulate_privileged(c))
            return EXCEPTION_CONTINUE_EXECUTION;
        break;
    default:
        break;
    }
    crash_note_first_chance(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void exceptions_init()
{
    AddVectoredExceptionHandler(1, vectored_handler);
    crash_init();
}

} // namespace rt
