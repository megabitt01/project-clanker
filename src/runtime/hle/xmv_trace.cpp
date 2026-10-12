// Debug tracing of the XMV movie decoder (enabled with SWBF2_TRACE_XMV=1).
#include "hook.h"
#include "../log.h"

#include <cstdlib>

namespace {
typedef long(__stdcall *CreateFn)(uint32_t, void *, void *, void *, void *, void **);
typedef long(__stdcall *NextFrameFn)(void *, void *, uint32_t *, void *);
CreateFn g_create;
NextFrameFn g_next;
int g_frames;

long __stdcall trace_create(uint32_t a, void *b, void *c, void *d, void *e, void **out)
{
    long r = g_create(a, b, c, d, e, out);
    LOG_INFO("xmv", "XMVDecoder_CreateDecoderForPackets(%u) -> %08lX decoder %p", a, r, out ? *out : nullptr);
    return r;
}

long __stdcall trace_next(void *dec, void *surface, uint32_t *status, void *t)
{
    long r = g_next(dec, surface, status, t);
    if (g_frames++ % 60 == 0) {
        const uint32_t *d = static_cast<const uint32_t *>(dec);
        auto f = [&](int off) { return d[off / 4]; };
        LOG_INFO("xmv", "GetNextFrame -> %08lX status %u | streams %u 0x3c=%u 0x50=%X 0x60=%X 0x64=%X 0x68=%X 0x6c=%X 0x74=%u "
                        "b0=%X ac=%u f8=%X e4=%X e8=%u bc=%u",
                 r, status ? *status : 0, f(0x48), f(0x3c), f(0x50), f(0x60), f(0x64), f(0x68), f(0x6c), f(0x74),
                 f(0xb0), f(0xac), f(0xf8), f(0xe4), f(0xe8), f(0xbc));
        if (f(0x48) && f(0x148)) {
            const uint32_t *st = reinterpret_cast<const uint32_t *>(uintptr_t(f(0x148)));
            LOG_INFO("xmv", "  audio packet status[0]=%08X enabled=%u", st[0],
                     f(0x138) ? *reinterpret_cast<const uint32_t *>(uintptr_t(f(0x138))) : 0);
        }
    }
    return r;
}
} // namespace

namespace rt {
void hle_install_xmv_trace()
{
    if (!getenv("SWBF2_TRACE_XMV"))
        return;
    g_create = reinterpret_cast<CreateFn>(hook_detour(0x003F3300, 6, reinterpret_cast<void *>(&trace_create), "xmv-create"));
    g_next = reinterpret_cast<NextFrameFn>(hook_detour(0x003F39F1, 6, reinterpret_cast<void *>(&trace_next), "xmv-next"));
}
} // namespace rt
