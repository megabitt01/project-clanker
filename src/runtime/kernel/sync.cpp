// Kernel dispatcher emulation.
//
// Dispatcher objects (events, semaphores, timers, mutants, threads) are the
// Xbox structures themselves, usually living in title memory; their
// Header.SignalState is the single source of truth. A single host mutex
// protects every state transition and one condition variable wakes all
// waiters, who then re-evaluate their wait condition. With a dozen threads
// this is simple and fast enough.
//
// IRQL is emulated with two host locks: raising to DISPATCH_LEVEL takes the
// "DPC lock" (DPCs run holding it), raising above DISPATCH_LEVEL also takes
// the "interrupt lock" (ISRs run holding it). That reproduces the mutual
// exclusion the single-processor Xbox code relies on.
#include "sync.h"
#include "thread.h"
#include "../log.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <timeapi.h>

namespace rt {

using Clock = std::chrono::steady_clock;

volatile uint32_t g_KeTickCount;

static std::mutex g_m;                 // dispatcher lock
static std::condition_variable g_cv;   // signalled on any state change

static std::recursive_mutex g_dpc_lock;  // held at IRQL >= DISPATCH_LEVEL
static std::recursive_mutex g_int_lock;  // held at IRQL > DISPATCH_LEVEL

// DPC queue
static std::mutex g_dpc_q_m;
static std::condition_variable g_dpc_q_cv;
static std::deque<XKDPC *> g_dpc_queue;

// Interrupts
static XKINTERRUPT *g_interrupts[16];
static std::mutex g_int_q_m;
static std::condition_variable g_int_q_cv;
static uint32_t g_int_pending;

// Timers
struct ActiveTimer {
    XKTIMER *timer;
    Clock::time_point due;
    int32_t period_ms;
    XKDPC *dpc;
};
static std::vector<ActiveTimer> g_timers;  // guarded by g_m
static std::condition_variable g_timer_cv;

int64_t system_time_now()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static Clock::time_point deadline_from(int64_t t)
{
    if (t < 0)
        return Clock::now() + std::chrono::microseconds((-t) / 10);
    int64_t delta = t - system_time_now();
    if (delta < 0)
        delta = 0;
    return Clock::now() + std::chrono::microseconds(delta / 10);
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------
static bool is_signaled(XDISPATCHER_HEADER *h, XThread *t)
{
    if (h->Type == MutantObject) {
        XKMUTANT *m = reinterpret_cast<XKMUTANT *>(h);
        return h->SignalState > 0 || (t && m->OwnerThread == &t->kthread);
    }
    return h->SignalState > 0;
}

static void satisfy(XDISPATCHER_HEADER *h, XThread *t)
{
    switch (h->Type) {
    case EventSynchronizationObject:
    case TimerSynchronizationObject:
        h->SignalState = 0;
        break;
    case SemaphoreObject:
        h->SignalState--;
        break;
    case MutantObject:
        h->SignalState--;
        reinterpret_cast<XKMUTANT *>(h)->OwnerThread = t ? &t->kthread : nullptr;
        break;
    default:
        break; // notification events/timers, threads: stay signalled
    }
}

typedef void(XAPI *ApcFn)(void *ctx, void *a1, void *a2);

NTSTATUS sync_wait(uint32_t count, XDISPATCHER_HEADER *const *objects, bool wait_all, bool alertable,
                   const int64_t *timeout)
{
    XThread *t = xt_current();
    bool has_deadline = timeout != nullptr;
    Clock::time_point deadline = has_deadline ? deadline_from(*timeout) : Clock::time_point();

    std::unique_lock<std::mutex> lk(g_m);
    for (;;) {
        if (alertable && t && !t->apcs.empty()) {
            std::deque<XApc> apcs;
            apcs.swap(t->apcs);
            lk.unlock();
            for (const XApc &a : apcs)
                reinterpret_cast<ApcFn>(a.routine)(a.ctx, a.arg1, a.arg2);
            return X_STATUS_USER_APC;
        }
        if (count) {
            if (wait_all) {
                bool all = true;
                for (uint32_t i = 0; i < count && all; i++)
                    all = is_signaled(objects[i], t);
                if (all) {
                    for (uint32_t i = 0; i < count; i++)
                        satisfy(objects[i], t);
                    return X_STATUS_WAIT_0;
                }
            } else {
                for (uint32_t i = 0; i < count; i++) {
                    if (is_signaled(objects[i], t)) {
                        satisfy(objects[i], t);
                        return X_STATUS_WAIT_0 + NTSTATUS(i);
                    }
                }
            }
        }
        if (has_deadline) {
            if (Clock::now() >= deadline)
                return X_STATUS_TIMEOUT;
            g_cv.wait_until(lk, deadline);
        } else {
            g_cv.wait(lk);
        }
    }
}

void sync_notify()
{
    std::lock_guard<std::mutex> lk(g_m);
    g_cv.notify_all();
}

int32_t sync_set_event(XKEVENT *ev)
{
    std::lock_guard<std::mutex> lk(g_m);
    int32_t old = ev->Header.SignalState;
    ev->Header.SignalState = 1;
    g_cv.notify_all();
    return old;
}

int32_t sync_reset_event(XKEVENT *ev)
{
    std::lock_guard<std::mutex> lk(g_m);
    int32_t old = ev->Header.SignalState;
    ev->Header.SignalState = 0;
    return old;
}

int32_t sync_pulse_event(XKEVENT *ev)
{
    // Approximation: signal, give waiters a moment, reset.
    int32_t old = sync_set_event(ev);
    std::this_thread::yield();
    sync_reset_event(ev);
    return old;
}

int32_t sync_release_semaphore(XKSEMAPHORE *sem, int32_t count)
{
    std::lock_guard<std::mutex> lk(g_m);
    int32_t old = sem->Header.SignalState;
    if (int64_t(old) + count > sem->Limit)
        return -1;
    sem->Header.SignalState = old + count;
    g_cv.notify_all();
    return old;
}

int32_t sync_release_mutant(XKMUTANT *m)
{
    std::lock_guard<std::mutex> lk(g_m);
    int32_t old = m->Header.SignalState;
    m->Header.SignalState++;
    if (m->Header.SignalState > 0)
        m->OwnerThread = nullptr;
    g_cv.notify_all();
    return old;
}

void sync_signal_thread_exit(XThread *t)
{
    std::lock_guard<std::mutex> lk(g_m);
    t->kthread.Header.SignalState = 1;
    g_cv.notify_all();
}

void sync_queue_user_apc(XThread *t, void *routine, void *ctx, void *arg1, void *arg2)
{
    std::lock_guard<std::mutex> lk(g_m);
    t->apcs.push_back({routine, ctx, arg1, arg2});
    g_cv.notify_all();
}

void sync_delay(bool alertable, const int64_t *interval, NTSTATUS *status)
{
    int64_t iv = interval ? *interval : 0;
    if (iv == 0 && !alertable) {
        SwitchToThread();
        *status = X_STATUS_SUCCESS;
        return;
    }
    NTSTATUS s = sync_wait(0, nullptr, false, alertable, &iv);
    *status = s == X_STATUS_TIMEOUT ? X_STATUS_SUCCESS : s;
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------
bool sync_set_timer(XKTIMER *timer, int64_t due, int32_t period_ms, XKDPC *dpc)
{
    std::lock_guard<std::mutex> lk(g_m);
    bool was_set = false;
    for (size_t i = 0; i < g_timers.size(); i++) {
        if (g_timers[i].timer == timer) {
            g_timers.erase(g_timers.begin() + i);
            was_set = true;
            break;
        }
    }
    timer->Header.SignalState = 0;
    timer->Header.Inserted = 1;
    timer->DueTime = uint64_t(due);
    timer->Period = period_ms;
    timer->Dpc = dpc;
    g_timers.push_back({timer, deadline_from(due), period_ms, dpc});
    g_timer_cv.notify_all();
    return was_set;
}

bool sync_cancel_timer(XKTIMER *timer)
{
    std::lock_guard<std::mutex> lk(g_m);
    for (size_t i = 0; i < g_timers.size(); i++) {
        if (g_timers[i].timer == timer) {
            g_timers.erase(g_timers.begin() + i);
            timer->Header.Inserted = 0;
            return true;
        }
    }
    return false;
}

static void timer_thread()
{
    timeBeginPeriod(1);
    Clock::time_point boot = Clock::now();
    std::unique_lock<std::mutex> lk(g_m);
    for (;;) {
        Clock::time_point now = Clock::now();
        g_KeTickCount = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - boot).count());
        Clock::time_point next = now + std::chrono::milliseconds(1);
        std::vector<XKDPC *> fire_dpcs;
        bool fired = false;
        for (size_t i = 0; i < g_timers.size();) {
            ActiveTimer &at = g_timers[i];
            if (at.due <= now) {
                at.timer->Header.SignalState = 1;
                fired = true;
                if (at.dpc)
                    fire_dpcs.push_back(at.dpc);
                if (at.period_ms > 0) {
                    at.due += std::chrono::milliseconds(at.period_ms);
                    if (at.due < now)
                        at.due = now + std::chrono::milliseconds(at.period_ms);
                } else {
                    at.timer->Header.Inserted = 0;
                    g_timers.erase(g_timers.begin() + i);
                    continue;
                }
            }
            if (at.due < next)
                next = at.due;
            i++;
        }
        if (fired)
            g_cv.notify_all();
        if (!fire_dpcs.empty()) {
            lk.unlock();
            for (XKDPC *d : fire_dpcs)
                sync_queue_dpc(d, reinterpret_cast<void *>(uintptr_t(system_time_now())), nullptr);
            lk.lock();
            continue;
        }
        g_timer_cv.wait_until(lk, next);
    }
}

// ---------------------------------------------------------------------------
// DPCs
// ---------------------------------------------------------------------------
bool sync_queue_dpc(XKDPC *dpc, void *arg1, void *arg2)
{
    std::lock_guard<std::mutex> lk(g_dpc_q_m);
    if (dpc->Inserted)
        return false;
    dpc->Inserted = 1;
    dpc->SystemArgument1 = arg1;
    dpc->SystemArgument2 = arg2;
    g_dpc_queue.push_back(dpc);
    g_dpc_q_cv.notify_one();
    return true;
}

bool sync_remove_dpc(XKDPC *dpc)
{
    std::lock_guard<std::mutex> lk(g_dpc_q_m);
    for (auto it = g_dpc_queue.begin(); it != g_dpc_queue.end(); ++it) {
        if (*it == dpc) {
            g_dpc_queue.erase(it);
            dpc->Inserted = 0;
            return true;
        }
    }
    return false;
}

static void dpc_thread(void *)
{
    XThread *self = xt_current();
    for (;;) {
        XKDPC *dpc;
        {
            std::unique_lock<std::mutex> lk(g_dpc_q_m);
            g_dpc_q_cv.wait(lk, [] { return !g_dpc_queue.empty(); });
            dpc = g_dpc_queue.front();
            g_dpc_queue.pop_front();
            dpc->Inserted = 0;
        }
        std::lock_guard<std::recursive_mutex> dl(g_dpc_lock);
        self->kpcr.Irql = DISPATCH_LEVEL;
        self->kpcr.PrcbData.DpcRoutineActive = 1;
        dpc->DeferredRoutine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
        self->kpcr.PrcbData.DpcRoutineActive = 0;
        self->kpcr.Irql = PASSIVE_LEVEL;
    }
}

// ---------------------------------------------------------------------------
// IRQL
// ---------------------------------------------------------------------------
KIRQL irql_current()
{
    XThread *t = xt_current();
    return t ? t->kpcr.Irql : PASSIVE_LEVEL;
}

KIRQL irql_raise(KIRQL n)
{
    XThread *t = xt_current();
    if (!t)
        return PASSIVE_LEVEL;
    KIRQL old = t->kpcr.Irql;
    if (old < DISPATCH_LEVEL && n >= DISPATCH_LEVEL)
        g_dpc_lock.lock();
    if (old <= DISPATCH_LEVEL && n > DISPATCH_LEVEL)
        g_int_lock.lock();
    t->kpcr.Irql = n;
    return old;
}

void irql_lower(KIRQL n)
{
    XThread *t = xt_current();
    if (!t)
        return;
    KIRQL old = t->kpcr.Irql;
    t->kpcr.Irql = n;
    if (old > DISPATCH_LEVEL && n <= DISPATCH_LEVEL)
        g_int_lock.unlock();
    if (old >= DISPATCH_LEVEL && n < DISPATCH_LEVEL)
        g_dpc_lock.unlock();
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
bool sync_connect_interrupt(XKINTERRUPT *intr)
{
    uint32_t irq = intr->BusInterruptLevel;
    if (irq >= 16 || g_interrupts[irq])
        return false;
    intr->Connected = 1;
    g_interrupts[irq] = intr;
    LOG_INFO("intr", "ISR %p connected to IRQ %u", intr->ServiceRoutine, irq);
    return true;
}

void sync_disconnect_interrupt(XKINTERRUPT *intr)
{
    uint32_t irq = intr->BusInterruptLevel;
    if (irq < 16 && g_interrupts[irq] == intr)
        g_interrupts[irq] = nullptr;
    intr->Connected = 0;
}

bool sync_interrupt_connected(uint32_t bus_irq)
{
    return bus_irq < 16 && g_interrupts[bus_irq] != nullptr;
}

void sync_raise_interrupt(uint32_t bus_irq)
{
    std::lock_guard<std::mutex> lk(g_int_q_m);
    g_int_pending |= 1u << bus_irq;
    g_int_q_cv.notify_one();
}

static void interrupt_thread(void *)
{
    XThread *self = xt_current();
    for (;;) {
        uint32_t pending;
        {
            std::unique_lock<std::mutex> lk(g_int_q_m);
            g_int_q_cv.wait(lk, [] { return g_int_pending != 0; });
            pending = g_int_pending;
            g_int_pending = 0;
        }
        for (uint32_t irq = 0; irq < 16; irq++) {
            if (!(pending & (1u << irq)))
                continue;
            XKINTERRUPT *intr = g_interrupts[irq];
            if (!intr)
                continue;
            std::lock_guard<std::recursive_mutex> il(g_int_lock);
            self->kpcr.Irql = KIRQL(intr->Irql);
            intr->ServiceRoutine(intr, intr->ServiceContext);
            self->kpcr.Irql = PASSIVE_LEVEL;
        }
    }
}

typedef uint8_t(XAPI *SyncRoutine)(void *ctx);

uint8_t sync_synchronize_execution(XKINTERRUPT *intr, void *routine, void *ctx)
{
    std::lock_guard<std::recursive_mutex> il(g_int_lock);
    XThread *t = xt_current();
    KIRQL old = t ? t->kpcr.Irql : 0;
    if (t)
        t->kpcr.Irql = KIRQL(intr->Irql);
    uint8_t r = reinterpret_cast<SyncRoutine>(routine)(ctx);
    if (t)
        t->kpcr.Irql = old;
    return r;
}

void sync_init()
{
    std::thread(timer_thread).detach();
    xt_create_host(dpc_thread, nullptr, "dpc", THREAD_PRIORITY_HIGHEST);
    xt_create_host(interrupt_thread, nullptr, "interrupt", THREAD_PRIORITY_TIME_CRITICAL);
}

} // namespace rt
