#pragma once
#include "xboxkrnl.h"

#include <windows.h>
#include <deque>

namespace rt {

struct HostThreadInfo {
    DWORD host_id;
    const char *name;
};

struct XApc {
    void *routine;     // PIO_APC_ROUTINE / KNORMAL_ROUTINE
    void *ctx;
    void *arg1;
    void *arg2;
};

// One per thread that can execute Xbox code (title threads and the runtime's
// own DPC / interrupt / GPU threads).
struct XThread {
    XKTHREAD kthread;          // what ObReferenceObjectByHandle hands out (first!)
    XKPCR kpcr;                // what fs:[0x20..0x58] reads resolve to
    HANDLE host = nullptr;
    DWORD host_id = 0;
    uint8_t *stack_alloc = nullptr;
    uint32_t stack_size = 0;
    uint32_t stack_top = 0;    // TEB StackBase while running Xbox code
    uint32_t initial_esp = 0;
    void *system_routine = nullptr;
    void *start_routine = nullptr;
    void *start_context = nullptr;
    void (*host_fn)(void *) = nullptr;  // runtime-internal threads
    void *host_arg = nullptr;
    int base_priority = 0;
    NTSTATUS exit_status = 0;
    bool alerted = false;
    std::deque<XApc> apcs;     // guarded by the dispatcher lock
    char name[32] = {};
};

void kthread_init();
XThread *xt_current();               // nullptr on threads that never ran Xbox code
XThread *xt_from_kthread(void *kthread);

// Creates a Windows thread that runs an Xbox start routine.
XThread *xt_create(uint32_t stack_size, uint32_t tls_size, void *system_routine, void *start_routine,
                   void *start_context, bool suspended, const char *name);
// Creates a runtime-internal thread that still has a valid Xbox thread context
// (so DPCs / ISRs written for the Xbox can run on it).
XThread *xt_create_host(void (*fn)(void *), void *arg, const char *name, int win_priority);
[[noreturn]] void xt_exit(NTSTATUS status);

void kthread_for_each(void (*fn)(HostThreadInfo &));

// Patch the title's fs:[...] accesses (Xbox KPCR layout) so they read our
// per-thread XKPCR instead of the Windows TEB.
void fs_patch_xbe();

} // namespace rt
