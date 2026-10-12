// NV2A (Xbox GPU) emulation: shared state between the register file, the
// PFIFO DMA pusher and the PGRAPH method processor.
#pragma once
#include "nv2a.h"
#include "../memory.h"

#include <intrin.h>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rt {

// ---- register block offsets (relative to BAR0 = 0xFD000000) -----------------
enum : uint32_t {
    NV_PMC_BOOT_0 = 0x000000,
    NV_PMC_INTR_0 = 0x000100,
    NV_PMC_INTR_EN_0 = 0x000140,
    NV_PMC_ENABLE = 0x000200,

    NV_PBUS_PCI_NV_0 = 0x001800,

    NV_PFIFO_INTR_0 = 0x002100,
    NV_PFIFO_INTR_EN_0 = 0x002140,
    NV_PFIFO_RAMHT = 0x002210,
    NV_PFIFO_RAMFC = 0x002214,
    NV_PFIFO_RUNOUT_STATUS = 0x002400,
    NV_PFIFO_CACHES = 0x002500,
    NV_PFIFO_CACHE1_PUSH0 = 0x003200,
    NV_PFIFO_CACHE1_PUSH1 = 0x003204,
    NV_PFIFO_CACHE1_STATUS = 0x003214,
    NV_PFIFO_CACHE1_DMA_PUSH = 0x003220,
    NV_PFIFO_CACHE1_DMA_STATE = 0x003228,
    NV_PFIFO_CACHE1_DMA_INSTANCE = 0x00322C,
    NV_PFIFO_CACHE1_DMA_PUT = 0x003240,
    NV_PFIFO_CACHE1_DMA_GET = 0x003244,
    NV_PFIFO_CACHE1_REF = 0x003248,
    NV_PFIFO_CACHE1_DMA_SUBROUTINE = 0x00324C,
    NV_PFIFO_CACHE1_PULL0 = 0x003250,

    NV_PTIMER_INTR_0 = 0x009100,
    NV_PTIMER_INTR_EN_0 = 0x009140,
    NV_PTIMER_NUMERATOR = 0x009200,
    NV_PTIMER_DENOMINATOR = 0x009210,
    NV_PTIMER_TIME_0 = 0x009400,
    NV_PTIMER_TIME_1 = 0x009410,
    NV_PTIMER_ALARM_0 = 0x009420,

    NV_PFB_CFG0 = 0x100200,
    NV_PFB_CSTATUS = 0x10020C,
    NV_PFB_WBC = 0x100410,

    NV_PGRAPH_INTR = 0x400100,
    NV_PGRAPH_NSOURCE = 0x400108,
    NV_PGRAPH_INTR_EN = 0x400140,
    NV_PGRAPH_CTX_CONTROL = 0x400144,
    NV_PGRAPH_CTX_USER = 0x400148,
    NV_PGRAPH_STATUS = 0x400700,
    NV_PGRAPH_TRAPPED_ADDR = 0x400704,
    NV_PGRAPH_TRAPPED_DATA_LOW = 0x400708,
    NV_PGRAPH_SURFACE = 0x400710,
    NV_PGRAPH_INCREMENT = 0x40071C,
    NV_PGRAPH_FIFO = 0x400720,
    NV_PGRAPH_CHANNEL_CTX_TRIGGER = 0x400788,

    NV_PCRTC_INTR_0 = 0x600100,
    NV_PCRTC_INTR_EN_0 = 0x600140,
    NV_PCRTC_START = 0x600800,
    NV_PCRTC_CONFIG = 0x600804,
    NV_PCRTC_RASTER = 0x600808,

    NV_PRAMDAC_NVPLL_COEFF = 0x680500,
    NV_PRAMDAC_MPLL_COEFF = 0x680504,
    NV_PRAMDAC_VPLL_COEFF = 0x680508,

    NV_USER = 0x800000,
    NV_USER_DMA_PUT = 0x40,
    NV_USER_DMA_GET = 0x44,
    NV_USER_REF = 0x48,
};

enum : uint32_t {
    NV_PMC_INTR_0_PFIFO = 1u << 8,
    NV_PMC_INTR_0_PGRAPH = 1u << 12,
    NV_PMC_INTR_0_PTIMER = 1u << 20,
    NV_PMC_INTR_0_PCRTC = 1u << 24,

    NV_PGRAPH_INTR_NOTIFY = 1u << 0,
    NV_PGRAPH_INTR_CONTEXT_SWITCH = 1u << 12,
    NV_PGRAPH_INTR_ERROR = 1u << 20,
    NV_PGRAPH_NSOURCE_NOTIFICATION = 1u << 0,

    NV_PGRAPH_SURFACE_WRITE_3D = 0x00700000,
    NV_PGRAPH_SURFACE_READ_3D = 0x07000000,
    NV_PGRAPH_SURFACE_MODULO_3D = 0x70000000,
    NV_PGRAPH_INCREMENT_READ_3D = 1u << 1,
};

constexpr uint32_t NV2A_IRQ = 3;

inline uint32_t get_mask(uint32_t v, uint32_t mask)
{
    unsigned long shift;
    _BitScanForward(&shift, mask);
    return (v & mask) >> shift;
}
inline void set_mask(uint32_t &v, uint32_t mask, uint32_t val)
{
    unsigned long shift;
    _BitScanForward(&shift, mask);
    v = (v & ~mask) | ((val << shift) & mask);
}

struct DmaObject {
    uint32_t dma_class;
    uint32_t address;   // physical
    uint32_t limit;
};

struct Nv2aState {
    std::mutex lock;
    std::condition_variable pusher_cv;  // new work for the pusher
    std::condition_variable intr_cv;    // PGRAPH interrupt acknowledged
    std::condition_variable flip_cv;    // PGRAPH_SURFACE READ_3D advanced

    uint32_t *regs;                     // backing store for every register
    uint32_t pgraph_pending = 0;        // NV_PGRAPH_INTR state
    bool waiting_for_nop = false;
    bool waiting_for_ctx_switch = false;

    // DMA pusher
    uint32_t dma_put = 0, dma_get = 0;
    uint32_t subroutine_return = 0;
    bool subroutine_active = false;
    uint64_t methods_processed = 0;
    uint64_t draw_calls = 0;

    // Display
    uint32_t display_fb = 0, display_pitch = 640 * 4, display_format = 0;
    uint32_t display_width = 640, display_height = 480, display_bpp = 4;
    uint64_t vblank_count = 0;
    bool present_pending = false;
};

extern Nv2aState *g_nv;

inline uint32_t &reg(uint32_t off) { return g_nv->regs[off >> 2]; }

// Interrupt line evaluation. Call with g_nv->lock held.
void nv2a_update_irq();

// Instance memory / DMA objects
inline uint8_t *ramin_ptr(uint32_t off) { return phys_ptr(XBOX_RAM_SIZE - XBOX_INSTANCE_SIZE + off); }
DmaObject nv_dma_load(uint32_t instance_offset);
inline void *dma_ptr(const DmaObject &d, uint32_t offset) { return phys_ptr(d.address + offset); }

// PGRAPH
void pgraph_init();
// Executes one method; called by the pusher with g_nv->lock held (may wait
// on condition variables, releasing it).
void pgraph_method(std::unique_lock<std::mutex> &lk, uint32_t subchannel, uint32_t method, uint32_t parameter);
// Called on the GPU thread to put the current front buffer on screen.
void pgraph_present(std::unique_lock<std::mutex> &lk);

} // namespace rt
