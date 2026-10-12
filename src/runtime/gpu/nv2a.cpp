// NV2A register file, interrupts, PFIFO DMA pusher and display timing.
//
// Every BAR0 access by title code traps into nv2a_mmio_read/write (see
// mmio.cpp). Registers default to plain storage; the ones with side effects
// or live values (interrupt status, timers, FIFO pointers) are modelled here.
#include "nv2a_internal.h"
#include "../kernel/sync.h"
#include "../kernel/thread.h"
#include "../log.h"
#include "../mmio.h"
#include "renderer.h"
#include "kelvin.h"
#include "surface.h"

#include <windows.h>
#include <chrono>
#include <thread>

namespace rt {

// The lock for every thread but the pusher. std::mutex is not fair, so the
// pusher (which holds the lock while it works through the push buffer) steps
// aside whenever someone is waiting; see pusher_thread.
struct NvLock {
    NvLock()
    {
        g_nv->waiters++;
        g_nv->lock.lock();
        g_nv->waiters--;
    }
    ~NvLock() { g_nv->lock.unlock(); }
    NvLock(const NvLock &) = delete;
    NvLock &operator=(const NvLock &) = delete;
};


Nv2aState *g_nv;

static constexpr double CRYSTAL_HZ = 16666666.0;
static constexpr double VBLANK_HZ = 59.94;
static LARGE_INTEGER g_qpc_freq, g_qpc_start;

static double seconds_since_boot()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_qpc_start.QuadPart) / double(g_qpc_freq.QuadPart);
}

// ---- interrupts ---------------------------------------------------------------------
void nv2a_update_irq()
{
    uint32_t pmc = 0;
    if (reg(NV_PFIFO_INTR_0) & reg(NV_PFIFO_INTR_EN_0))
        pmc |= NV_PMC_INTR_0_PFIFO;
    if (g_nv->pgraph_pending & reg(NV_PGRAPH_INTR_EN))
        pmc |= NV_PMC_INTR_0_PGRAPH;
    if (reg(NV_PTIMER_INTR_0) & reg(NV_PTIMER_INTR_EN_0))
        pmc |= NV_PMC_INTR_0_PTIMER;
    if (reg(NV_PCRTC_INTR_0) & reg(NV_PCRTC_INTR_EN_0))
        pmc |= NV_PMC_INTR_0_PCRTC;
    reg(NV_PMC_INTR_0) = pmc;
    if (pmc && (reg(NV_PMC_INTR_EN_0) & 1))
        sync_raise_interrupt(NV2A_IRQ);
}

// ---- DMA objects -------------------------------------------------------------------
DmaObject nv_dma_load(uint32_t instance_offset)
{
    const uint32_t *o = reinterpret_cast<const uint32_t *>(ramin_ptr(instance_offset));
    uint32_t flags = o[0], limit = o[1], frame = o[2];
    DmaObject d;
    d.dma_class = flags & 0xFFF;
    d.address = (frame & 0xFFFFF000) | (flags >> 20);
    d.limit = limit;
    return d;
}

// ---- timer -----------------------------------------------------------------------
static uint64_t ptimer_clock()
{
    double t = seconds_since_boot();
    uint32_t num = reg(NV_PTIMER_NUMERATOR), den = reg(NV_PTIMER_DENOMINATOR);
    double core = 233333333.0;
    double ticks = t * core;
    if (num && den)
        ticks = ticks * den / num;
    return uint64_t(ticks);
}

static uint32_t raster_line()
{
    double t = seconds_since_boot() * VBLANK_HZ;
    double frac = t - double(uint64_t(t));
    return uint32_t(frac * 525.0);
}

// ---- register access -------------------------------------------------------------
static uint32_t nv2a_read(uint32_t off)
{
    switch (off) {
    case NV_PMC_BOOT_0:
        return 0x02A000A2;
    case NV_PBUS_PCI_NV_0:
        return 0x02A010DE;
    case NV_PFIFO_RUNOUT_STATUS:
        return 0x10; // empty
    case NV_PFIFO_CACHES:
        return reg(off) & ~0x10u;
    case NV_PFIFO_CACHE1_STATUS:
        return 0x10; // empty
    case NV_PFIFO_CACHE1_DMA_PUSH:
        return reg(off) & ~0x10u;
    case NV_PFIFO_CACHE1_DMA_PUT:
        return g_nv->dma_put;
    case NV_PFIFO_CACHE1_DMA_GET:
        return g_nv->dma_get;
    case NV_PFIFO_CACHE1_PULL0:
        return reg(off) & ~0x1000u;
    case NV_PTIMER_TIME_0:
        return uint32_t((ptimer_clock() & 0x7FFFFFF) << 5);
    case NV_PTIMER_TIME_1:
        return uint32_t((ptimer_clock() >> 27) & 0x1FFFFFFF);
    case NV_PFB_CSTATUS:
        return XBOX_RAM_SIZE;
    case NV_PFB_WBC:
        return reg(off) & ~(1u << 16);
    case NV_PGRAPH_INTR:
        return g_nv->pgraph_pending;
    case NV_PGRAPH_STATUS:
        return 0;
    case NV_PGRAPH_CHANNEL_CTX_TRIGGER:
        return 0;
    case NV_PCRTC_RASTER:
        return raster_line();
    default:
        break;
    }
    if (off >= NV_USER && off < NV_USER + 0x200000) {
        uint32_t r = off & 0xFFFF;
        if (r == NV_USER_DMA_PUT)
            return g_nv->dma_put;
        if (r == NV_USER_DMA_GET)
            return g_nv->dma_get;
        if (r == NV_USER_REF)
            return reg(NV_PFIFO_CACHE1_REF);
    }
    return reg(off);
}

static void nv2a_write(uint32_t off, uint32_t v)
{
    static uint8_t seen[NV2A_MMIO_SIZE / 4 / 8];
    if (off < NV_USER && !(seen[off >> 5] & (1 << ((off >> 2) & 7)))) {
        seen[off >> 5] |= uint8_t(1 << ((off >> 2) & 7));
        LOG_TRACE("nv2a", "first write to register %06X = %08X", off, v);
    }
    switch (off) {
    case NV_PMC_INTR_0:
        return; // read-only summary
    case NV_PMC_INTR_EN_0:
        reg(off) = v;
        nv2a_update_irq();
        return;
    case NV_PFIFO_INTR_0:
        reg(off) &= ~v;
        nv2a_update_irq();
        return;
    case NV_PFIFO_INTR_EN_0:
        reg(off) = v;
        nv2a_update_irq();
        return;
    case NV_PFIFO_CACHE1_DMA_PUT:
        g_nv->dma_put = v;
        g_nv->pusher_cv.notify_all();
        return;
    case NV_PFIFO_CACHE1_DMA_GET:
        g_nv->dma_get = v;
        g_nv->get_written++;
        g_nv->pusher_cv.notify_all();
        return;
    case NV_PTIMER_INTR_0:
        reg(off) &= ~v;
        nv2a_update_irq();
        return;
    case NV_PTIMER_INTR_EN_0:
        reg(off) = v;
        nv2a_update_irq();
        return;
    case NV_PGRAPH_INTR:
        g_nv->pgraph_pending &= ~v;
        if (!(g_nv->pgraph_pending & NV_PGRAPH_INTR_ERROR))
            g_nv->waiting_for_nop = false;
        if (!(g_nv->pgraph_pending & NV_PGRAPH_INTR_CONTEXT_SWITCH))
            g_nv->waiting_for_ctx_switch = false;
        g_nv->intr_cv.notify_all();
        nv2a_update_irq();
        return;
    case NV_PGRAPH_INTR_EN:
        reg(off) = v;
        nv2a_update_irq();
        return;
    case NV_PGRAPH_INCREMENT:
        if (v & NV_PGRAPH_INCREMENT_READ_3D) {
            uint32_t &s = reg(NV_PGRAPH_SURFACE);
            uint32_t mod = get_mask(s, NV_PGRAPH_SURFACE_MODULO_3D);
            if (mod)
                set_mask(s, NV_PGRAPH_SURFACE_READ_3D, (get_mask(s, NV_PGRAPH_SURFACE_READ_3D) + 1) % mod);
            g_nv->flip_cv.notify_all();
        }
        return;
    case NV_PGRAPH_FIFO:
        reg(off) = v;
        g_nv->pusher_cv.notify_all();
        return;
    case NV_PCRTC_INTR_0:
        reg(off) &= ~v;
        nv2a_update_irq();
        return;
    case NV_PCRTC_INTR_EN_0:
        reg(off) = v;
        nv2a_update_irq();
        return;
    case NV_PCRTC_START:
        reg(off) = v;
        LOG_TRACE("nv2a", "PCRTC_START = %08X", v);
        return;
    default:
        break;
    }
    if (off >= NV_USER && off < NV_USER + 0x200000) {
        uint32_t r = off & 0xFFFF;
        if (r == NV_USER_DMA_PUT) {
            g_nv->dma_put = v;
            g_nv->pusher_cv.notify_all();
            return;
        }
        if (r == NV_USER_DMA_GET) {
            g_nv->dma_get = v;
            g_nv->get_written++;
            return;
        }
    }
    reg(off) = v;
    if (off == NV_PFIFO_CACHE1_DMA_PUSH || off == NV_PFIFO_CACHE1_PUSH0)
        g_nv->pusher_cv.notify_all();
}

static uint32_t mmio_read(uint32_t addr, int size)
{
    uint32_t off = addr - NV2A_MMIO_BASE;
    NvLock lk;
    uint32_t v = nv2a_read(off & ~3u);
    if (size == 4)
        return v;
    v >>= (off & 3) * 8;
    return size == 1 ? (v & 0xFF) : (v & 0xFFFF);
}

static void mmio_write(uint32_t addr, uint32_t value, int size)
{
    uint32_t off = addr - NV2A_MMIO_BASE;
    NvLock lk;
    if (size != 4) {
        // Sub-word writes (VGA CRTC index/data etc.) are merged into storage.
        uint32_t shift = (off & 3) * 8, mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        uint32_t &r = reg(off & ~3u);
        r = (r & ~mask) | ((value << shift) & mask);
        return;
    }
    nv2a_write(off, value);
}

// ---- DMA pusher ---------------------------------------------------------------------
static uint32_t pushbuffer_base()
{
    uint32_t inst = reg(NV_PFIFO_CACHE1_DMA_INSTANCE) & 0xFFFF;
    if (!inst)
        return 0;
    return nv_dma_load(inst << 4).address;
}

// The pusher only fetches while D3D has it enabled: it suspends it
// (CACHE1_DMA_PUSH access, CACHE1_PUSH0) while resetting GET/PUT, and in
// between the two writes GET would otherwise point into a dead push buffer.
static bool pusher_enabled()
{
    return (reg(NV_PFIFO_CACHE1_DMA_PUSH) & 1) && (reg(NV_PFIFO_CACHE1_PUSH0) & 1);
}

static void pusher_thread(void *)
{
    renderer_thread_init();
    std::unique_lock<std::mutex> lk(g_nv->lock);
    LOG_INFO("nv2a", "DMA pusher running");
    uint32_t method = 0, subchannel = 0, count = 0;
    bool non_increasing = false;
    for (;;) {
        g_nv->pusher_cv.wait(lk, [] {
            return (g_nv->dma_get != g_nv->dma_put && pusher_enabled()) || g_nv->present_pending;
        });
        if (g_nv->present_pending) {
            g_nv->present_pending = false;
            pgraph_present(lk);
        }
        uint32_t base = pushbuffer_base();
        uint64_t get_gen = g_nv->get_written;
        while (g_nv->dma_get != g_nv->dma_put && !g_nv->present_pending && pusher_enabled()) {
            // The CPU must be able to reach the registers while the pusher
            // runs (on hardware they are independent). D3D resets GET/PUT
            // when it re-creates its device; a pusher that kept the lock
            // while wandering through the stale push buffer deadlocked the
            // game on quitting to the title screen.
            if (g_nv->waiters.load(std::memory_order_relaxed)) {
                lk.unlock();
                while (g_nv->waiters.load(std::memory_order_relaxed))
                    std::this_thread::yield();
                lk.lock();
                base = pushbuffer_base();
            }
            if (g_nv->get_written != get_gen) {
                // GET was moved by the CPU: drop any partially parsed command.
                get_gen = g_nv->get_written;
                count = 0;
                g_nv->subroutine_active = false;
                continue;
            }
            uint32_t word = *reinterpret_cast<volatile uint32_t *>(phys_ptr(base + g_nv->dma_get));
            g_nv->dma_get += 4;
            if (count) {
                pgraph_method(lk, subchannel, method, word);
                g_nv->methods_processed++;
                if (!non_increasing)
                    method += 4;
                count--;
                continue;
            }
            if ((word & 0xE0000003) == 0x20000000) {        // old jump
                g_nv->dma_get = word & 0x1FFFFFFC;
            } else if ((word & 3) == 1) {                   // jump
                g_nv->dma_get = word & 0xFFFFFFFC;
            } else if ((word & 3) == 2) {                   // call
                if (g_nv->subroutine_active)
                    LOG_ERROR("nv2a", "pushbuffer: nested call at %08X", g_nv->dma_get - 4);
                g_nv->subroutine_return = g_nv->dma_get;
                g_nv->subroutine_active = true;
                g_nv->dma_get = word & 0xFFFFFFFC;
            } else if (word == 0x00020000) {                // return
                if (!g_nv->subroutine_active)
                    LOG_ERROR("nv2a", "pushbuffer: return without call");
                g_nv->dma_get = g_nv->subroutine_return;
                g_nv->subroutine_active = false;
            } else if ((word & 0xE0030003) == 0 || (word & 0xE0030003) == 0x40000000) {
                method = word & 0x1FFC;
                subchannel = (word >> 13) & 7;
                count = (word >> 18) & 0x7FF;
                non_increasing = (word & 0x40000000) != 0;
            } else {
                LOG_ERROR("nv2a", "pushbuffer: bad command %08X at %08X", word, g_nv->dma_get - 4);
            }
        }
    }
}

// ---- vblank ------------------------------------------------------------------------
static void vblank_thread(void *)
{
    using Clock = std::chrono::steady_clock;
    auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / VBLANK_HZ));
    auto next = Clock::now() + period;
    for (;;) {
        std::this_thread::sleep_until(next);
        next += period;
        NvLock lk;
        g_nv->vblank_count++;
        if (g_nv->vblank_count == 600)
            kelvin_dump_histogram();
        if (g_nv->vblank_count % 300 == 0) {
            static uint64_t last_frames;
            uint64_t frames = surface_frame();
            LOG_INFO("nv2a", "vblank %llu: %.1f fps, %llu methods, %llu draws, put %08X get %08X, front %08X",
                     g_nv->vblank_count, (frames - last_frames) / 5.0, g_nv->methods_processed, g_nv->draw_calls,
                     g_nv->dma_put, g_nv->dma_get, reg(NV_PCRTC_START));
            last_frames = frames;
        }
        g_nv->present_pending = true;
        g_nv->pusher_cv.notify_all();
        reg(NV_PCRTC_INTR_0) |= 1;
        // PTIMER alarm
        if ((reg(NV_PTIMER_INTR_EN_0) & 1)) {
            uint64_t now = ptimer_clock();
            uint32_t t0 = uint32_t((now & 0x7FFFFFF) << 5);
            if (int32_t(t0 - reg(NV_PTIMER_ALARM_0)) >= 0)
                reg(NV_PTIMER_INTR_0) |= 1;
        }
        nv2a_update_irq();
    }
}

void nv2a_set_display_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t framebuffer)
{
    NvLock lk;
    g_nv->display_format = format;
    g_nv->display_pitch = pitch;
    g_nv->display_fb = framebuffer;
    reg(NV_PCRTC_START) = framebuffer;
    // Format is a D3DFORMAT: LIN_A8R8G8B8 (0x12) / LIN_X8R8G8B8 (0x1E) are
    // 32 bpp, LIN_R5G6B5 (0x11) / LIN_X1R5G5B5 (0x1C) are 16 bpp.
    uint32_t bpp = (format == 0x12 || format == 0x1E) ? 4 : 2;
    g_nv->display_bpp = bpp;
    if (pitch)
        g_nv->display_width = pitch / bpp > 1920 ? 640 : pitch / bpp;
    // Mode bits 24-27 encode the vertical size class used by the AV encoder
    // tables; every SD/480p mode we report is 480 lines.
    g_nv->display_height = g_nv->display_width >= 1280 ? 720 : 480;
}

ScanoutInfo nv2a_scanout()
{
    NvLock lk;
    ScanoutInfo s;
    s.address = reg(NV_PCRTC_START) & 0x0FFFFFFF;
    s.pitch = g_nv->display_pitch;
    s.width = g_nv->display_width;
    s.height = g_nv->display_height;
    s.format = g_nv->display_format;
    return s;
}

static uint32_t mcpx_read(uint32_t addr, int size)
{
    LOG_DEBUG("mcpx", "read%d %08X", size * 8, addr);
    return 0;
}

static void mcpx_write(uint32_t addr, uint32_t value, int size)
{
    LOG_DEBUG("mcpx", "write%d %08X = %08X", size * 8, addr, value);
}

static uint32_t port_read(uint16_t port, int size)
{
    LOG_TRACE("io", "in%d port %04X", size * 8, port);
    // MCPX GPIO: bit 5 reports the current video field. D3D's vblank ISR
    // samples it, and XMV movie playback waits for a particular field, so
    // it must alternate every vblank like an interlaced display's.
    if (port == 0x80C0)
        return (g_nv->vblank_count & 1) ? 0x20 : 0x00;
    return 0;
}

static void port_write(uint16_t port, uint32_t value, int size)
{
    LOG_TRACE("io", "out%d port %04X = %X", size * 8, port, value);
}

void nv2a_init()
{
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_start);
    g_nv = new Nv2aState();
    g_nv->regs = static_cast<uint32_t *>(VirtualAlloc(nullptr, NV2A_MMIO_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    reg(NV_PRAMDAC_NVPLL_COEFF) = 0x00011C01;
    reg(NV_PRAMDAC_MPLL_COEFF) = 0x00007702;
    reg(NV_PRAMDAC_VPLL_COEFF) = 0x0003C20D;
    reg(NV_PFB_CFG0) = 0x03070103;
    reg(NV_PGRAPH_FIFO) = 1;
    pgraph_init();
    mmio_register(NV2A_MMIO_BASE, 0x700000, mmio_read, mmio_write);
    mmio_register(NV2A_MMIO_BASE + 0x800000, 0x800000, mmio_read, mmio_write);
    mmio_register(MCPX_MMIO_BASE, MCPX_MMIO_SIZE, mcpx_read, mcpx_write);
    port_set_handlers(port_read, port_write);
    xt_create_host(pusher_thread, nullptr, "nv2a-pusher", THREAD_PRIORITY_ABOVE_NORMAL);
    xt_create_host(vblank_thread, nullptr, "nv2a-vblank", THREAD_PRIORITY_HIGHEST);
}

} // namespace rt
