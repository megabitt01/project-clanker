#pragma once
#include "xboxkrnl.h"

namespace rt {

struct XThread;

void sync_init();

// --- dispatcher objects (operate on Xbox structures in place) --------------
// timeout: nullptr = infinite, otherwise Xbox semantics (100ns units,
// negative = relative, positive = absolute system time).
NTSTATUS sync_wait(uint32_t count, XDISPATCHER_HEADER *const *objects, bool wait_all, bool alertable,
                   const int64_t *timeout);
int32_t sync_set_event(XKEVENT *ev);
int32_t sync_reset_event(XKEVENT *ev);
int32_t sync_pulse_event(XKEVENT *ev);
// Returns previous count, or -1 if the limit would be exceeded.
int32_t sync_release_semaphore(XKSEMAPHORE *sem, int32_t count);
int32_t sync_release_mutant(XKMUTANT *m);
void sync_signal_thread_exit(XThread *t);
void sync_notify();  // wake waiters after an object was modified directly

// --- timers -----------------------------------------------------------------
bool sync_set_timer(XKTIMER *timer, int64_t due, int32_t period_ms, XKDPC *dpc);
bool sync_cancel_timer(XKTIMER *timer);
void sync_delay(bool alertable, const int64_t *interval, NTSTATUS *status);

// --- DPCs / interrupts / IRQL -------------------------------------------------
bool sync_queue_dpc(XKDPC *dpc, void *arg1, void *arg2);
bool sync_remove_dpc(XKDPC *dpc);
KIRQL irql_raise(KIRQL new_irql);
void irql_lower(KIRQL new_irql);
KIRQL irql_current();
bool sync_connect_interrupt(XKINTERRUPT *intr);
void sync_disconnect_interrupt(XKINTERRUPT *intr);
// Called by device models: runs the ISR connected to this bus IRQ (if any) on
// the interrupt thread. Returns immediately.
void sync_raise_interrupt(uint32_t bus_irq);
bool sync_interrupt_connected(uint32_t bus_irq);
uint8_t sync_synchronize_execution(XKINTERRUPT *intr, void *routine, void *ctx);

// --- APCs ---------------------------------------------------------------------
void sync_queue_user_apc(XThread *t, void *routine, void *ctx, void *arg1, void *arg2);

// Millisecond tick count since boot (KeTickCount).
extern volatile uint32_t g_KeTickCount;
int64_t system_time_now();   // 100ns units since 1601 (FILETIME)

} // namespace rt
