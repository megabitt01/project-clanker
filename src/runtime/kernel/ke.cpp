// Kernel executive exports: synchronization, timers, DPCs, interrupts,
// threads, object manager.
#include "kexports.h"
#include "object.h"
#include "sync.h"
#include "thread.h"
#include "../log.h"

#include <windows.h>
#include <intrin.h>

using namespace rt;

// ---- DPCs -------------------------------------------------------------------
KEXPORT(void) kx_KeInitializeDpc(XKDPC *Dpc, XPKDEFERRED_ROUTINE DeferredRoutine, void *DeferredContext)
{
    Dpc->Type = DpcObject;
    Dpc->Inserted = 0;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

KEXPORT(XBOOLEAN) kx_KeInsertQueueDpc(XKDPC *Dpc, void *SystemArgument1, void *SystemArgument2)
{
    return sync_queue_dpc(Dpc, SystemArgument1, SystemArgument2);
}

KEXPORT(XBOOLEAN) kx_KeRemoveQueueDpc(XKDPC *Dpc)
{
    return sync_remove_dpc(Dpc);
}

// ---- timers -----------------------------------------------------------------
KEXPORT(void) kx_KeInitializeTimerEx(XKTIMER *Timer, uint32_t Type)
{
    memset(Timer, 0, sizeof *Timer);
    Timer->Header.Type = uint8_t(TimerNotificationObject + Type);
    Timer->Header.Size = sizeof(XKTIMER) / 4;
    Timer->Header.WaitListHead.Flink = Timer->Header.WaitListHead.Blink = &Timer->Header.WaitListHead;
}

KEXPORT(XBOOLEAN) kx_KeSetTimerEx(XKTIMER *Timer, int64_t DueTime, int32_t Period, XKDPC *Dpc)
{
    return sync_set_timer(Timer, DueTime, Period, Dpc);
}

KEXPORT(XBOOLEAN) kx_KeSetTimer(XKTIMER *Timer, int64_t DueTime, XKDPC *Dpc)
{
    return sync_set_timer(Timer, DueTime, 0, Dpc);
}

KEXPORT(XBOOLEAN) kx_KeCancelTimer(XKTIMER *Timer)
{
    return sync_cancel_timer(Timer);
}

// ---- events / waits ---------------------------------------------------------
KEXPORT(int32_t) kx_KeSetEvent(XKEVENT *Event, int32_t Increment, XBOOLEAN Wait)
{
    return sync_set_event(Event);
}

KEXPORT(NTSTATUS) kx_KeWaitForSingleObject(void *Object, uint32_t WaitReason, uint8_t WaitMode, XBOOLEAN Alertable,
                                           int64_t *Timeout)
{
    XDISPATCHER_HEADER *h = static_cast<XDISPATCHER_HEADER *>(Object);
    return sync_wait(1, &h, false, Alertable != 0, Timeout);
}

KEXPORT(NTSTATUS) kx_KeWaitForMultipleObjects(uint32_t Count, void **Object, uint32_t WaitType, uint32_t WaitReason,
                                              uint8_t WaitMode, XBOOLEAN Alertable, int64_t *Timeout,
                                              void *WaitBlockArray)
{
    return sync_wait(Count, reinterpret_cast<XDISPATCHER_HEADER *const *>(Object), WaitType == 0, Alertable != 0,
                     Timeout);
}

KEXPORT(NTSTATUS) kx_KeDelayExecutionThread(uint8_t WaitMode, XBOOLEAN Alertable, int64_t *Interval)
{
    NTSTATUS s;
    sync_delay(Alertable != 0, Interval, &s);
    return s;
}

// ---- time ---------------------------------------------------------------------
KEXPORT(void) kx_KeQuerySystemTime(int64_t *CurrentTime)
{
    *CurrentTime = system_time_now();
}

static constexpr int64_t XBOX_PERF_FREQUENCY = 733333333;

KEXPORT(int64_t) kx_KeQueryPerformanceFrequency()
{
    return XBOX_PERF_FREQUENCY;
}

KEXPORT(int64_t) kx_KeQueryPerformanceCounter()
{
    static int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t whole = c.QuadPart / freq, rem = c.QuadPart % freq;
    return whole * XBOX_PERF_FREQUENCY + rem * XBOX_PERF_FREQUENCY / freq;
}

KEXPORT(void) kx_KeStallExecutionProcessor(uint32_t MicroSeconds)
{
    static int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    int64_t ticks = int64_t(MicroSeconds) * freq / 1000000;
    do {
        _mm_pause();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

KEXPORT(NTSTATUS) kx_NtSetSystemTime(int64_t *SystemTime, int64_t *PreviousTime)
{
    if (PreviousTime)
        *PreviousTime = system_time_now();
    LOG_INFO("ke", "NtSetSystemTime ignored");
    return X_STATUS_SUCCESS;
}

// ---- IRQL -----------------------------------------------------------------------
KEXPORT(KIRQL) kx_KeRaiseIrqlToDpcLevel()
{
    return irql_raise(DISPATCH_LEVEL);
}

KEXPORT_FASTCALL(KIRQL) kx_KfRaiseIrql(KIRQL NewIrql)
{
    return irql_raise(NewIrql);
}

KEXPORT_FASTCALL(void) kx_KfLowerIrql(KIRQL NewIrql)
{
    irql_lower(NewIrql);
}

// ---- interrupts ---------------------------------------------------------------
KEXPORT(uint32_t) kx_HalGetInterruptVector(uint32_t BusInterruptLevel, KIRQL *Irql)
{
    uint32_t vector = 0x30 + BusInterruptLevel;
    if (Irql)
        *Irql = KIRQL(26 - BusInterruptLevel);
    return vector;
}

KEXPORT(void) kx_KeInitializeInterrupt(XKINTERRUPT *Interrupt, XPKSERVICE_ROUTINE ServiceRoutine, void *ServiceContext,
                                       uint32_t Vector, KIRQL Irql, uint32_t InterruptMode, XBOOLEAN ShareVector)
{
    memset(Interrupt, 0, sizeof *Interrupt);
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->BusInterruptLevel = Vector - 0x30;
    Interrupt->Irql = Irql;
    Interrupt->Mode = uint8_t(InterruptMode);
    Interrupt->ShareVector = ShareVector;
}

KEXPORT(XBOOLEAN) kx_KeConnectInterrupt(XKINTERRUPT *Interrupt)
{
    return sync_connect_interrupt(Interrupt);
}

KEXPORT(XBOOLEAN) kx_KeDisconnectInterrupt(XKINTERRUPT *Interrupt)
{
    sync_disconnect_interrupt(Interrupt);
    return 1;
}

KEXPORT(XBOOLEAN) kx_KeSynchronizeExecution(XKINTERRUPT *Interrupt, void *SynchronizeRoutine, void *SynchronizeContext)
{
    return sync_synchronize_execution(Interrupt, SynchronizeRoutine, SynchronizeContext);
}

// ---- misc ----------------------------------------------------------------------
KEXPORT(void) kx_KeBugCheck(uint32_t BugCheckCode)
{
    fatal("KeBugCheck(0x%08X)", BugCheckCode);
}

KEXPORT(NTSTATUS) kx_KeSaveFloatingPointState(void *FloatSave) { return X_STATUS_SUCCESS; }
KEXPORT(NTSTATUS) kx_KeRestoreFloatingPointState(void *FloatSave) { return X_STATUS_SUCCESS; }

// ---- threads ---------------------------------------------------------------------
static int map_priority(int p)
{
    if (p >= 15) return THREAD_PRIORITY_TIME_CRITICAL;
    if (p <= -15) return THREAD_PRIORITY_IDLE;
    if (p > 2) return THREAD_PRIORITY_HIGHEST;
    if (p < -2) return THREAD_PRIORITY_LOWEST;
    return p;
}

KEXPORT(int32_t) kx_KeQueryBasePriorityThread(XKTHREAD *Thread)
{
    return xt_from_kthread(Thread)->base_priority;
}

KEXPORT(int32_t) kx_KeSetBasePriorityThread(XKTHREAD *Thread, int32_t Increment)
{
    XThread *t = xt_from_kthread(Thread);
    int32_t old = t->base_priority;
    t->base_priority = Increment;
    if (t->host)
        SetThreadPriority(t->host, map_priority(Increment));
    return old;
}

KEXPORT(NTSTATUS) kx_PsCreateSystemThreadEx(XHANDLE *ThreadHandle, uint32_t ThreadExtensionSize, uint32_t KernelStackSize,
                                            uint32_t TlsDataSize, XHANDLE *ThreadId, void *StartRoutine,
                                            void *StartContext, XBOOLEAN CreateSuspended, XBOOLEAN DebuggerThread,
                                            void *SystemRoutine)
{
    static int counter;
    char name[32];
    snprintf(name, sizeof name, "xbox-%d", ++counter);
    XThread *t = xt_create(KernelStackSize, TlsDataSize, SystemRoutine, StartRoutine, StartContext,
                           CreateSuspended != 0, name);
    LOG_INFO("ke", "PsCreateSystemThreadEx: %s start=%p ctx=%p stack=0x%X tls=%u", name, StartRoutine, StartContext,
             KernelStackSize, TlsDataSize);
    if (ThreadHandle)
        *ThreadHandle = ob_insert(new ThreadObj(t));
    if (ThreadId)
        *ThreadId = t->host_id;
    return X_STATUS_SUCCESS;
}

KEXPORT(void) kx_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    xt_exit(ExitStatus);
}

// ---- object manager ----------------------------------------------------------------
KEXPORT(NTSTATUS) kx_ObReferenceObjectByHandle(XHANDLE Handle, void *ObjectType, void **ReturnedObject)
{
    if (Handle == X_NtCurrentThread) {
        *ReturnedObject = &xt_current()->kthread;
        return X_STATUS_SUCCESS;
    }
    XObject *o = ob_lookup(Handle);
    if (!o) {
        *ReturnedObject = nullptr;
        return X_STATUS_INVALID_HANDLE;
    }
    if (ObjectType == g_PsThreadObjectType && o->kind != ObjKind::Thread)
        return X_STATUS_OBJECT_TYPE_MISMATCH;
    if (ObjectType == g_ExEventObjectType && o->kind != ObjKind::Event)
        return X_STATUS_OBJECT_TYPE_MISMATCH;
    *ReturnedObject = o->body();
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_ObReferenceObjectByName(XANSI_STRING *ObjectName, uint32_t Attributes, void *ObjectType,
                                             void *ParseContext, void **Object)
{
    LOG_WARN("ob", "ObReferenceObjectByName(%s) not supported", ansi_to_string(ObjectName).c_str());
    *Object = nullptr;
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
}

KEXPORT_FASTCALL(void) kx_ObfDereferenceObject(void *Object) {}

KEXPORT(NTSTATUS) kx_NtClose(XHANDLE Handle)
{
    return ob_close(Handle) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

KEXPORT(NTSTATUS) kx_NtDuplicateObject(XHANDLE SourceHandle, XHANDLE *TargetHandle, uint32_t Options)
{
    if (SourceHandle == X_NtCurrentThread) {
        *TargetHandle = ob_insert(new ThreadObj(xt_current()));
        return X_STATUS_SUCCESS;
    }
    XHANDLE h = ob_duplicate(SourceHandle);
    if (!h)
        return X_STATUS_INVALID_HANDLE;
    *TargetHandle = h;
    if (Options & 1) // DUPLICATE_CLOSE_SOURCE
        ob_close(SourceHandle);
    return X_STATUS_SUCCESS;
}

// Named-object helper for NtCreateEvent / NtCreateSemaphore.
template <class T>
static NTSTATUS create_named(XHANDLE *out, XOBJECT_ATTRIBUTES *oa, ObjKind kind, T *(*make)())
{
    std::string name = oa && oa->ObjectName ? ansi_to_string(oa->ObjectName) : std::string();
    if (!name.empty()) {
        if (XObject *o = ob_find_named(name)) {
            if (o->kind != kind)
                return X_STATUS_OBJECT_TYPE_MISMATCH;
            *out = ob_insert(o);
            return X_STATUS_OBJECT_NAME_EXISTS;
        }
    }
    T *obj = make();
    if (!name.empty()) {
        obj->name = name;
        ob_add_named(obj);
    }
    *out = ob_insert(obj);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtCreateEvent(XHANDLE *EventHandle, XOBJECT_ATTRIBUTES *ObjectAttributes, uint32_t EventType,
                                   XBOOLEAN InitialState)
{
    EventObj *created = nullptr;
    NTSTATUS s = create_named<EventObj>(EventHandle, ObjectAttributes, ObjKind::Event, [] { return new EventObj(); });
    if (s == X_STATUS_SUCCESS) {
        created = static_cast<EventObj *>(ob_lookup(*EventHandle));
        created->ev.Header.Type = uint8_t(EventType);
        created->ev.Header.Size = sizeof(XKEVENT) / 4;
        created->ev.Header.SignalState = InitialState;
        created->ev.Header.WaitListHead.Flink = created->ev.Header.WaitListHead.Blink = &created->ev.Header.WaitListHead;
    }
    return s;
}

KEXPORT(NTSTATUS) kx_NtSetEvent(XHANDLE EventHandle, int32_t *PreviousState)
{
    EventObj *e = ob_lookup_as<EventObj>(EventHandle, ObjKind::Event);
    if (!e)
        return X_STATUS_INVALID_HANDLE;
    int32_t old = sync_set_event(&e->ev);
    if (PreviousState)
        *PreviousState = old;
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtCreateSemaphore(XHANDLE *SemaphoreHandle, XOBJECT_ATTRIBUTES *ObjectAttributes,
                                       int32_t InitialCount, int32_t MaximumCount)
{
    NTSTATUS s = create_named<SemaphoreObj>(SemaphoreHandle, ObjectAttributes, ObjKind::Semaphore,
                                            [] { return new SemaphoreObj(); });
    if (s == X_STATUS_SUCCESS) {
        SemaphoreObj *o = static_cast<SemaphoreObj *>(ob_lookup(*SemaphoreHandle));
        o->sem.Header.Type = SemaphoreObject;
        o->sem.Header.Size = sizeof(XKSEMAPHORE) / 4;
        o->sem.Header.SignalState = InitialCount;
        o->sem.Limit = MaximumCount;
    }
    return s;
}

KEXPORT(NTSTATUS) kx_NtReleaseSemaphore(XHANDLE SemaphoreHandle, int32_t ReleaseCount, int32_t *PreviousCount)
{
    SemaphoreObj *o = ob_lookup_as<SemaphoreObj>(SemaphoreHandle, ObjKind::Semaphore);
    if (!o)
        return X_STATUS_INVALID_HANDLE;
    int32_t old = sync_release_semaphore(&o->sem, ReleaseCount);
    LOG_TRACE("sem", "release %X +%d (was %d) from %p", SemaphoreHandle, ReleaseCount, old, _ReturnAddress());
    if (old < 0)
        return X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    if (PreviousCount)
        *PreviousCount = old;
    return X_STATUS_SUCCESS;
}

static NTSTATUS wait_handle(XHANDLE Handle, bool alertable, int64_t *Timeout)
{
    XDISPATCHER_HEADER *h = nullptr;
    if (Handle == X_NtCurrentThread)
        h = &xt_current()->kthread.Header;
    else if (XObject *o = ob_lookup(Handle))
        h = o->dispatcher();
    else
        return X_STATUS_INVALID_HANDLE;
    if (!h) // files etc.: all I/O completes synchronously
        return X_STATUS_SUCCESS;
    if (h->Type == SemaphoreObject)
        LOG_TRACE("sem", "wait %X (count %d)", Handle, h->SignalState);
    NTSTATUS st = sync_wait(1, &h, false, alertable, Timeout);
    if (h->Type == SemaphoreObject)
        LOG_TRACE("sem", "woke %X -> %X (count %d)", Handle, st, h->SignalState);
    return st;
}

KEXPORT(NTSTATUS) kx_NtWaitForSingleObject(XHANDLE Handle, XBOOLEAN Alertable, int64_t *Timeout)
{
    return wait_handle(Handle, Alertable != 0, Timeout);
}

KEXPORT(NTSTATUS) kx_NtWaitForSingleObjectEx(XHANDLE Handle, uint8_t WaitMode, XBOOLEAN Alertable, int64_t *Timeout)
{
    return wait_handle(Handle, Alertable != 0, Timeout);
}
