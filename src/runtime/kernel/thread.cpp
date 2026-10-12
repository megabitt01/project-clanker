// Xbox threads on top of Windows threads.
//
// Xbox code expects:
//  * fs:[0x20/0x24/0x28/0x58] to resolve through the Xbox KPCR. Those few
//    instructions are patched (fs_patch_xbe) to read our per-thread XKPCR,
//    reachable through a Windows TLS slot.
//  * fs:[0]/fs:[4]/fs:[8] (exception list, stack base/limit) to follow the
//    NT_TIB layout, which Windows' TEB shares, so SEH works natively.
//  * Thread-local storage to sit directly below the stack base: XAPI reads
//    [StackBase + tls_index*4]. Each Xbox thread therefore runs on a stack we
//    allocate ourselves with the TLS block at its top, and the TEB stack
//    bounds are pointed at it.
#include "thread.h"
#include "../log.h"
#include "../crash.h"
#include "sync.h"

#include <intrin.h>
#include <mutex>
#include <vector>

namespace rt {

static DWORD g_tls_slot = TLS_OUT_OF_INDEXES;
static std::mutex g_threads_mutex;
static std::vector<XThread *> g_threads;

void kthread_init()
{
    g_tls_slot = TlsAlloc();
    if (g_tls_slot >= 64)
        fatal("TLS slot %lu is not directly addressable from fs:", g_tls_slot);
}

XThread *xt_current()
{
    void *pcr = TlsGetValue(g_tls_slot);
    if (!pcr)
        return nullptr;
    return reinterpret_cast<XThread *>(static_cast<uint8_t *>(pcr) - offsetof(XThread, kpcr));
}

XThread *xt_from_kthread(void *kthread)
{
    return static_cast<XThread *>(kthread);
}

void kthread_for_each(void (*fn)(HostThreadInfo &))
{
    std::vector<HostThreadInfo> list;
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        for (XThread *t : g_threads)
            list.push_back({t->host_id, t->name});
    }
    for (auto &i : list)
        fn(i);
}

static void init_kpcr(XThread *t)
{
    memset(&t->kthread, 0, sizeof t->kthread);
    memset(&t->kpcr, 0, sizeof t->kpcr);
    t->kthread.Header.Type = ThreadObject;
    t->kthread.Header.Size = sizeof(XKTHREAD) / 4;
    t->kthread.Header.WaitListHead.Flink = t->kthread.Header.WaitListHead.Blink = &t->kthread.Header.WaitListHead;
    t->kpcr.SelfPcr = &t->kpcr;
    t->kpcr.Prcb = &t->kpcr.PrcbData;
    t->kpcr.Irql = PASSIVE_LEVEL;
    t->kpcr.PrcbData.CurrentThread = &t->kthread;
}

static void alloc_stack(XThread *t, uint32_t stack_size, uint32_t tls_size)
{
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    if (stack_size < 0x40000)
        stack_size = 0x40000; // generous: runtime code (file I/O, GL) runs on it too
    tls_size = (tls_size + 3) & ~3u; // XAPI indexes TLS at exactly StackBase - TlsDataSize
    t->stack_size = stack_size;
    t->stack_alloc = static_cast<uint8_t *>(VirtualAlloc(nullptr, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!t->stack_alloc)
        fatal("out of memory allocating a thread stack");
    t->stack_top = uint32_t(uintptr_t(t->stack_alloc)) + stack_size;
    uint32_t tls = t->stack_top - tls_size;
    t->kthread.TlsData = tls_size ? reinterpret_cast<void *>(uintptr_t(tls)) : nullptr;
    if (tls_size >= 4) // what XapiThreadStartup establishes: [TlsData] = start of TLS data
        *reinterpret_cast<uint32_t *>(uintptr_t(tls)) = tls + 4;
    t->initial_esp = (tls - 16) & ~15u;
}

typedef void(XAPI *SystemRoutine)(void *start_routine, void *start_context);
typedef void(XAPI *StartRoutine)(void *start_context);

static void __cdecl xthread_body(XThread *t)
{
    __try {
        if (t->host_fn) {
            t->host_fn(t->host_arg);
        } else if (t->system_routine) {
            reinterpret_cast<SystemRoutine>(t->system_routine)(t->start_routine, t->start_context);
        } else {
            reinterpret_cast<StartRoutine>(t->start_routine)(t->start_context);
        }
    } __except (crash_filter(GetExceptionInformation())) {
    }
    xt_exit(0);
}

static void *final_seh_handler()
{
    struct Rec {
        Rec *next;
        void *handler;
    };
    Rec *r = reinterpret_cast<Rec *>(__readfsdword(0));
    while (r && r->next != reinterpret_cast<Rec *>(-1))
        r = r->next;
    return r ? r->handler : nullptr;
}

// Switches to the thread's private stack, installs an SEH chain root that
// matches what Windows expects (for SEHOP validation) and runs the body.
static void run_on_xbox_stack(XThread *t)
{
    uint32_t new_sp = t->initial_esp;
    void *fh = final_seh_handler();
    __asm {
        mov ecx, t
        mov eax, new_sp
        mov edx, fh
        mov ebx, esp
        mov esp, eax
        push ebx                     ; host esp
        push dword ptr fs:[0]        ; host SEH head
        push edx                     ; record.Handler
        push -1                      ; record.Next (end of chain)
        mov dword ptr fs:[0], esp
        push ecx
        call xthread_body
        add esp, 4
        add esp, 8
        pop dword ptr fs:[0]
        pop esp
    }
}

static DWORD WINAPI host_thread_proc(void *p)
{
    XThread *t = static_cast<XThread *>(p);
    t->host_id = GetCurrentThreadId();
    TlsSetValue(g_tls_slot, &t->kpcr);
    NT_TIB *tib = reinterpret_cast<NT_TIB *>(NtCurrentTeb());
    tib->StackBase = reinterpret_cast<void *>(uintptr_t(t->stack_top));
    tib->StackLimit = t->stack_alloc;
    run_on_xbox_stack(t);
    return 0;
}

static XThread *new_thread(uint32_t stack_size, uint32_t tls_size, const char *name)
{
    XThread *t = new XThread();
    init_kpcr(t);
    alloc_stack(t, stack_size, tls_size);
    strncpy(t->name, name, sizeof t->name - 1);
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads.push_back(t);
    return t;
}

XThread *xt_create(uint32_t stack_size, uint32_t tls_size, void *system_routine, void *start_routine,
                   void *start_context, bool suspended, const char *name)
{
    XThread *t = new_thread(stack_size, tls_size, name);
    t->system_routine = system_routine;
    t->start_routine = start_routine;
    t->start_context = start_context;
    t->host = CreateThread(nullptr, 0x10000, host_thread_proc, t, CREATE_SUSPENDED, &t->host_id);
    if (!t->host)
        fatal("CreateThread failed (%lu)", GetLastError());
    if (!suspended)
        ResumeThread(t->host);
    LOG_DEBUG("thread", "created Xbox thread %s start=%p ctx=%p", name, start_routine, start_context);
    return t;
}

XThread *xt_create_host(void (*fn)(void *), void *arg, const char *name, int win_priority)
{
    XThread *t = new_thread(0x40000, 0x20, name);
    t->host_fn = fn;
    t->host_arg = arg;
    t->host = CreateThread(nullptr, 0x10000, host_thread_proc, t, CREATE_SUSPENDED, &t->host_id);
    if (!t->host)
        fatal("CreateThread failed (%lu)", GetLastError());
    SetThreadPriority(t->host, win_priority);
    ResumeThread(t->host);
    return t;
}

void xt_exit(NTSTATUS status)
{
    XThread *t = xt_current();
    if (t) {
        t->exit_status = status;
        sync_signal_thread_exit(t);
        LOG_DEBUG("thread", "Xbox thread %s exited (0x%08X)", t->name, status);
    }
    ExitThread(DWORD(status));
}

// ---------------------------------------------------------------------------
// fs: patching
// ---------------------------------------------------------------------------
struct FsSite {
    uint32_t addr;
    uint8_t len;     // 6 = mov eax,fs:[off]  8 = movzx eax,byte ptr fs:[off]
    uint8_t off;
};

// Every fs-relative access in default.xbe that is not NT_TIB compatible
// (found with tools/ghidra/FsScan.java). fs:[0] and fs:[4] are left alone.
static const FsSite k_fs_sites[] = {
    {0x0031fa22, 6, 0x20}, {0x00321265, 6, 0x20}, {0x003212b1, 6, 0x28}, {0x0032137e, 8, 0x24},
    {0x0032138a, 6, 0x28}, {0x00321db7, 8, 0x24}, {0x00321dc3, 6, 0x28}, {0x00321ddf, 8, 0x24},
    {0x00321deb, 6, 0x28}, {0x00321ebf, 6, 0x20}, {0x00321edb, 6, 0x28}, {0x00322971, 6, 0x20},
    {0x003237ec, 6, 0x20}, {0x00325651, 6, 0x20}, {0x003256c5, 6, 0x20}, {0x00325f44, 6, 0x20},
    {0x00325f73, 6, 0x20}, {0x00328951, 6, 0x58}, {0x003290ff, 6, 0x58}, {0x003292a2, 6, 0x58},
    {0x003298cf, 6, 0x58}, {0x0035ec61, 6, 0x20}, {0x0037d527, 6, 0x58}, {0x0037d6e3, 6, 0x58},
    {0x003c316f, 6, 0x20}, {0x003e9639, 6, 0x20}, {0x003efc55, 6, 0x20}, {0x0041ae93, 8, 0x24},
    {0x0041aec9, 6, 0x58}, {0x0041aeee, 6, 0x58}, {0x0041b095, 8, 0x24}, {0x004204ae, 8, 0x24},
    {0x0042b59b, 6, 0x20},
};

void fs_patch_xbe()
{
    uint8_t *stubs = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    uint8_t *stub_for[2][256] = {};
    uint8_t *w = stubs;
    uint32_t slot_off = 0xE10 + 4 * g_tls_slot; // TEB.TlsSlots[g_tls_slot]
    int patched = 0;

    for (const FsSite &s : k_fs_sites) {
        uint8_t *site = reinterpret_cast<uint8_t *>(uintptr_t(s.addr));
        bool is_byte = s.len == 8;
        static const uint8_t mov_eax[6] = {0x64, 0xA1};
        static const uint8_t movzx_eax[4] = {0x64, 0x0F, 0xB6, 0x05};
        bool ok = is_byte ? memcmp(site, movzx_eax, 4) == 0 && site[4] == s.off
                          : memcmp(site, mov_eax, 2) == 0 && site[2] == s.off;
        if (!ok)
            fatal("fs patch site 0x%08X does not match: is default.xbe the expected build?", s.addr);

        uint8_t *&stub = stub_for[is_byte][s.off];
        if (!stub) {
            stub = w;
            // mov eax, fs:[TlsSlots[n]]   -> our XKPCR
            *w++ = 0x64; *w++ = 0xA1;
            memcpy(w, &slot_off, 4); w += 4;
            if (is_byte) { // movzx eax, byte ptr [eax+off]
                *w++ = 0x0F; *w++ = 0xB6; *w++ = 0x80;
            } else {       // mov eax, [eax+off]
                *w++ = 0x8B; *w++ = 0x80;
            }
            uint32_t off = s.off;
            memcpy(w, &off, 4); w += 4;
            *w++ = 0xC3;   // ret (flags untouched, only eax written)
        }
        int32_t rel = int32_t(uintptr_t(stub) - (uintptr_t(site) + 5));
        site[0] = 0xE8;
        memcpy(site + 1, &rel, 4);
        for (int i = 5; i < s.len; i++)
            site[i] = 0x90;
        patched++;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    LOG_INFO("thread", "patched %d fs: accesses (TLS slot %lu)", patched, g_tls_slot);
}

} // namespace rt
