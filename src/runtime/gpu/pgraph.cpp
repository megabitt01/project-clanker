// PGRAPH: graphics engine method processing.
//
// The pusher hands every (subchannel, method, parameter) triple to
// pgraph_method. Objects are bound to subchannels with method 0 via the RAMHT
// hash table in instance memory. Kelvin (class 0x97) is the 3D engine; the
// remaining classes are 2D helpers D3D uses for copies and blits.
#include "nv2a_internal.h"
#include "kelvin.h"
#include "blit.h"
#include "renderer.h"
#include "../log.h"

#include <chrono>
#include <unordered_set>

namespace rt {

struct Subchannel {
    uint32_t object_class = 0;
    uint32_t instance = 0;
};
static Subchannel g_subch[8];
static std::unordered_set<uint64_t> g_logged;

static uint32_t ramht_hash(uint32_t handle)
{
    uint32_t ramht = reg(NV_PFIFO_RAMHT);
    uint32_t size = 1u << (((ramht >> 16) & 3) + 12);
    unsigned long bits_l;
    _BitScanForward(&bits_l, size);
    uint32_t bits = uint32_t(bits_l) - 1;
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & ((1u << bits) - 1);
        handle >>= bits;
    }
    uint32_t chid = reg(NV_PFIFO_CACHE1_PUSH1) & 0x1F;
    hash ^= chid << (bits - 4);
    return hash;
}

// Returns the instance offset (in RAMIN) bound to an object handle.
static bool ramht_lookup(uint32_t handle, uint32_t &instance, uint32_t &engine)
{
    uint32_t ramht = reg(NV_PFIFO_RAMHT);
    uint32_t base = ((ramht >> 4) & 0x1F) << 12;
    uint32_t size = 1u << (((ramht >> 16) & 3) + 12);
    uint32_t hash = ramht_hash(handle);
    // Linear probing over the table, starting at the hash slot.
    for (uint32_t i = 0; i < size / 8; i++) {
        uint32_t slot = (hash + i) % (size / 8);
        const uint32_t *e = reinterpret_cast<const uint32_t *>(ramin_ptr(base + slot * 8));
        if ((e[1] & 0x80000000u) && e[0] == handle) {
            instance = (e[1] & 0xFFFF) << 4;
            engine = (e[1] >> 16) & 3;
            return true;
        }
    }
    return false;
}

void pgraph_init() {}

static void log_once(uint32_t cls, uint32_t method, uint32_t param, const char *what)
{
    uint64_t key = (uint64_t(cls) << 32) | method;
    if (g_logged.insert(key).second)
        LOG_DEBUG("pgraph", "%s: class %02X method %04X = %08X", what, cls, method, param);
}

// Waits until the CPU acknowledges a PGRAPH interrupt (the NV2A stalls).
static void wait_interrupt_ack(std::unique_lock<std::mutex> &lk, bool &flag, const char *what)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (flag) {
        if (g_nv->intr_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            LOG_WARN("pgraph", "%s interrupt was not acknowledged; continuing", what);
            flag = false;
            g_nv->pgraph_pending = 0;
            break;
        }
    }
}

static void raise_pgraph(uint32_t bits)
{
    g_nv->pgraph_pending |= bits;
    nv2a_update_irq();
}

static void context_switch_if_needed(std::unique_lock<std::mutex> &lk)
{
    bool valid = (reg(NV_PGRAPH_CTX_CONTROL) & 0x10000) != 0;
    uint32_t chid = (reg(NV_PGRAPH_CTX_USER) >> 24) & 0x1F;
    uint32_t want = reg(NV_PFIFO_CACHE1_PUSH1) & 0x1F;
    if (valid && chid == want)
        return;
    set_mask(reg(NV_PGRAPH_TRAPPED_ADDR), 0x01F00000, want);
    g_nv->waiting_for_ctx_switch = true;
    LOG_INFO("pgraph", "context switch to channel %u requested", want);
    raise_pgraph(NV_PGRAPH_INTR_CONTEXT_SWITCH);
    wait_interrupt_ack(lk, g_nv->waiting_for_ctx_switch, "context switch");
    if (!(reg(NV_PGRAPH_CTX_CONTROL) & 0x10000) || ((reg(NV_PGRAPH_CTX_USER) >> 24) & 0x1F) != want) {
        // The handler did not load the channel itself; mark it resident.
        reg(NV_PGRAPH_CTX_CONTROL) |= 0x10000;
        set_mask(reg(NV_PGRAPH_CTX_USER), 0x1F000000, want);
    }
}

void pgraph_method(std::unique_lock<std::mutex> &lk, uint32_t subchannel, uint32_t method, uint32_t parameter)
{
    // The CPU may revoke PGRAPH FIFO access while it services interrupts.
    while (!(reg(NV_PGRAPH_FIFO) & 1))
        g_nv->pusher_cv.wait_for(lk, std::chrono::milliseconds(10));
    context_switch_if_needed(lk);

    Subchannel &sc = g_subch[subchannel];
    if (method == 0) {
        uint32_t instance, engine;
        if (!ramht_lookup(parameter, instance, engine)) {
            LOG_ERROR("pgraph", "SET_OBJECT: handle %08X not in RAMHT", parameter);
            return;
        }
        const uint32_t *obj = reinterpret_cast<const uint32_t *>(ramin_ptr(instance));
        sc.object_class = obj[0] & 0xFF;
        sc.instance = instance;
        LOG_DEBUG("pgraph", "subchannel %u <- handle %08X class %02X (instance %05X)", subchannel, parameter,
                  sc.object_class, instance);
        return;
    }
    // Methods 0x180-0x1FF take object handles (context DMAs).
    if (method >= 0x180 && method < 0x200) {
        uint32_t instance, engine;
        if (ramht_lookup(parameter, instance, engine))
            parameter = instance;
        else if (parameter)
            LOG_WARN("pgraph", "context handle %08X (method %04X) not in RAMHT", parameter, method);
    }

    // Software methods: NO_OPERATION with a non-zero argument raises a
    // notification interrupt and stalls until the CPU handles it.
    if (method == 0x100) {
        if (parameter == 0)
            return;
        uint32_t &ta = reg(NV_PGRAPH_TRAPPED_ADDR);
        set_mask(ta, 0x01F00000, reg(NV_PFIFO_CACHE1_PUSH1) & 0x1F);
        set_mask(ta, 0x00070000, subchannel);
        set_mask(ta, 0x00001FFC, method >> 2);
        reg(NV_PGRAPH_TRAPPED_DATA_LOW) = parameter;
        reg(NV_PGRAPH_NSOURCE) = NV_PGRAPH_NSOURCE_NOTIFICATION;
        g_nv->waiting_for_nop = true;
        raise_pgraph(NV_PGRAPH_INTR_ERROR);
        wait_interrupt_ack(lk, g_nv->waiting_for_nop, "software method");
        return;
    }

    switch (sc.object_class) {
    case 0x97:
        kelvin_method(lk, subchannel, method, parameter);
        break;
    case 0x44:
        // Pattern object: its colour methods load PGRAPH_PATT_COLOR0/1
        // (0x400B10/14). D3D uses PATT_COLOR0 as a GPU progress marker that
        // the CPU polls (see D3D_BlockOnTime), so these must take effect.
        if (method == 0x310 || method == 0x314)
            reg(0x400B10 + (method - 0x310)) = parameter;
        else
            log_once(sc.object_class, method, parameter, "unhandled pattern method");
        break;
    case 0x62:
    case 0x9F:
    case 0x39:
        blit_method(sc.object_class, method, parameter);
        break;
    default:
        log_once(sc.object_class, method, parameter, "unhandled 2D method");
        break;
    }
}

void pgraph_present(std::unique_lock<std::mutex> &lk)
{
    kelvin_present(lk);
}

} // namespace rt
