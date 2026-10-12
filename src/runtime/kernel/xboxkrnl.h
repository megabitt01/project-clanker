// Xbox kernel (xboxkrnl.exe, build 5838 era) data structures as the title sees
// them. Layouts must match the Xbox exactly because game/XDK code reads and
// writes these structures directly.
#pragma once
#include <cstddef>
#include <cstdint>

typedef int32_t NTSTATUS;
typedef uint32_t XHANDLE;   // handles are opaque 32-bit values on the Xbox
typedef uint8_t XBOOLEAN;
typedef uint8_t KIRQL;

#define XAPI __stdcall
#define XFASTCALL __fastcall

// ---- NTSTATUS values -------------------------------------------------------
constexpr NTSTATUS X_STATUS_SUCCESS = 0x00000000;
constexpr NTSTATUS X_STATUS_WAIT_0 = 0x00000000;
constexpr NTSTATUS X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr NTSTATUS X_STATUS_USER_APC = 0x000000C0;
constexpr NTSTATUS X_STATUS_ALERTED = 0x00000101;
constexpr NTSTATUS X_STATUS_TIMEOUT = 0x00000102;
constexpr NTSTATUS X_STATUS_PENDING = 0x00000103;
constexpr NTSTATUS X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
constexpr NTSTATUS X_STATUS_BUFFER_OVERFLOW = int32_t(0x80000005);
constexpr NTSTATUS X_STATUS_NO_MORE_FILES = int32_t(0x80000006);
constexpr NTSTATUS X_STATUS_UNSUCCESSFUL = int32_t(0xC0000001);
constexpr NTSTATUS X_STATUS_NOT_IMPLEMENTED = int32_t(0xC0000002);
constexpr NTSTATUS X_STATUS_INVALID_HANDLE = int32_t(0xC0000008);
constexpr NTSTATUS X_STATUS_INVALID_PARAMETER = int32_t(0xC000000D);
constexpr NTSTATUS X_STATUS_NO_SUCH_FILE = int32_t(0xC000000F);
constexpr NTSTATUS X_STATUS_END_OF_FILE = int32_t(0xC0000011);
constexpr NTSTATUS X_STATUS_NO_MEMORY = int32_t(0xC0000017);
constexpr NTSTATUS X_STATUS_CONFLICTING_ADDRESSES = int32_t(0xC0000018);
constexpr NTSTATUS X_STATUS_MEMORY_NOT_ALLOCATED = int32_t(0xC00000A0);
constexpr NTSTATUS X_STATUS_ACCESS_DENIED = int32_t(0xC0000022);
constexpr NTSTATUS X_STATUS_BUFFER_TOO_SMALL = int32_t(0xC0000023);
constexpr NTSTATUS X_STATUS_OBJECT_TYPE_MISMATCH = int32_t(0xC0000024);
constexpr NTSTATUS X_STATUS_OBJECT_NAME_INVALID = int32_t(0xC0000033);
constexpr NTSTATUS X_STATUS_OBJECT_NAME_NOT_FOUND = int32_t(0xC0000034);
constexpr NTSTATUS X_STATUS_OBJECT_NAME_COLLISION = int32_t(0xC0000035);
constexpr NTSTATUS X_STATUS_OBJECT_PATH_NOT_FOUND = int32_t(0xC000003A);
constexpr NTSTATUS X_STATUS_SHARING_VIOLATION = int32_t(0xC0000043);
constexpr NTSTATUS X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = int32_t(0xC0000047);
constexpr NTSTATUS X_STATUS_DISK_FULL = int32_t(0xC000007F);
constexpr NTSTATUS X_STATUS_INSUFFICIENT_RESOURCES = int32_t(0xC000009A);
constexpr NTSTATUS X_STATUS_DEVICE_NOT_CONNECTED = int32_t(0xC000009D);
constexpr NTSTATUS X_STATUS_FILE_IS_A_DIRECTORY = int32_t(0xC00000BA);
constexpr NTSTATUS X_STATUS_NOT_SUPPORTED = int32_t(0xC00000BB);
constexpr NTSTATUS X_STATUS_DIRECTORY_NOT_EMPTY = int32_t(0xC0000101);
constexpr NTSTATUS X_STATUS_NOT_A_DIRECTORY = int32_t(0xC0000103);
constexpr NTSTATUS X_STATUS_CANCELLED = int32_t(0xC0000120);
constexpr NTSTATUS X_STATUS_UNRECOGNIZED_VOLUME = int32_t(0xC000014F);
constexpr NTSTATUS X_STATUS_DEVICE_DOES_NOT_EXIST = int32_t(0xC00000C0);
constexpr NTSTATUS X_STATUS_INVALID_DEVICE_REQUEST = int32_t(0xC0000010);
constexpr NTSTATUS X_STATUS_IO_DEVICE_ERROR = int32_t(0xC0000185);

#define X_NT_SUCCESS(s) (int32_t(s) >= 0)

// ---- Basic structures ------------------------------------------------------
struct XLIST_ENTRY {
    XLIST_ENTRY *Flink;
    XLIST_ENTRY *Blink;
};

struct XANSI_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    char *Buffer;
};

struct XUNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    wchar_t *Buffer;
};

struct XOBJECT_ATTRIBUTES {
    XHANDLE RootDirectory;
    XANSI_STRING *ObjectName;
    uint32_t Attributes;
};

struct XIO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        void *Pointer;
    };
    uint32_t Information;
};

// ---- Dispatcher objects ----------------------------------------------------
enum XKOBJECTS : uint8_t {
    EventNotificationObject = 0,
    EventSynchronizationObject = 1,
    MutantObject = 2,
    ProcessObject = 3,
    QueueObject = 4,
    SemaphoreObject = 5,
    ThreadObject = 6,
    TimerNotificationObject = 8,
    TimerSynchronizationObject = 9,
    ApcObject = 18,
    DpcObject = 19,
    DeviceQueueObject = 20,
    EventPairObject = 21,
    InterruptObject = 22,
};

struct XDISPATCHER_HEADER {
    uint8_t Type;
    uint8_t Absolute;
    uint8_t Size;
    uint8_t Inserted;
    int32_t SignalState;
    XLIST_ENTRY WaitListHead;
};
static_assert(sizeof(XDISPATCHER_HEADER) == 0x10);

struct XKEVENT {
    XDISPATCHER_HEADER Header;
};

struct XKSEMAPHORE {
    XDISPATCHER_HEADER Header;
    int32_t Limit;
};

struct XKMUTANT {
    XDISPATCHER_HEADER Header;
    XLIST_ENTRY MutantListEntry;
    void *OwnerThread;
    uint8_t Abandoned;
};

struct XKDPC;
typedef void(XAPI *XPKDEFERRED_ROUTINE)(XKDPC *Dpc, void *DeferredContext, void *SystemArgument1,
                                        void *SystemArgument2);
struct XKDPC {
    int16_t Type;
    uint8_t Inserted;
    uint8_t Padding;
    XLIST_ENTRY DpcListEntry;
    XPKDEFERRED_ROUTINE DeferredRoutine;
    void *DeferredContext;
    void *SystemArgument1;
    void *SystemArgument2;
};
static_assert(sizeof(XKDPC) == 0x1C);

struct XKTIMER {
    XDISPATCHER_HEADER Header;
    uint64_t DueTime;
    XLIST_ENTRY TimerListEntry;
    XKDPC *Dpc;
    int32_t Period;
};
static_assert(sizeof(XKTIMER) == 0x28);

struct XKINTERRUPT;
typedef XBOOLEAN(XAPI *XPKSERVICE_ROUTINE)(XKINTERRUPT *Interrupt, void *ServiceContext);
struct XKINTERRUPT {
    XPKSERVICE_ROUTINE ServiceRoutine;
    void *ServiceContext;
    uint32_t BusInterruptLevel;
    uint32_t Irql;
    XBOOLEAN Connected;
    XBOOLEAN ShareVector;
    uint8_t Mode;
    uint8_t Padding;
    uint32_t ServiceCount;
    uint32_t DispatchCode[22];
};
static_assert(sizeof(XKINTERRUPT) == 0x70);

// Xbox RTL_CRITICAL_SECTION: an embedded synchronization event plus counters.
struct XRTL_CRITICAL_SECTION {
    XDISPATCHER_HEADER Event;
    int32_t LockCount;
    int32_t RecursionCount;
    void *OwningThread;
};
static_assert(sizeof(XRTL_CRITICAL_SECTION) == 0x1C);

// ---- Threads / processor control region ------------------------------------
// Only the fields the title reads are meaningful; the rest is padding so that
// offsets line up with the real kernel.
struct XKTHREAD {
    XDISPATCHER_HEADER Header;   // 0x00
    uint8_t pad10[0x18];         // 0x10
    void *TlsData;               // 0x28
    uint8_t pad2C[0x200 - 0x2C];
};
static_assert(offsetof(XKTHREAD, TlsData) == 0x28);

struct XKPRCB {
    XKTHREAD *CurrentThread;     // 0x00
    XKTHREAD *NextThread;        // 0x04
    XKTHREAD *IdleThread;        // 0x08
    uint8_t pad0C[0x30 - 0x0C];
    uint32_t DpcRoutineActive;   // 0x30
    uint8_t pad34[0x250 - 0x34];
    void *DebugMonitorData;      // 0x250 (non-null only under the debug monitor)
    uint8_t pad254[0x300 - 0x254];
};

struct XKPCR {
    uint8_t NtTib[0x1C];         // 0x00 (the real one is the Windows TEB)
    XKPCR *SelfPcr;              // 0x1C
    XKPRCB *Prcb;                // 0x20
    KIRQL Irql;                  // 0x24
    uint8_t pad25[3];
    XKPRCB PrcbData;             // 0x28
};
static_assert(offsetof(XKPCR, Prcb) == 0x20);
static_assert(offsetof(XKPCR, Irql) == 0x24);
static_assert(offsetof(XKPCR, PrcbData) == 0x28);
static_assert(offsetof(XKPCR, PrcbData) + offsetof(XKPRCB, DpcRoutineActive) == 0x58);

constexpr KIRQL PASSIVE_LEVEL = 0;
constexpr KIRQL APC_LEVEL = 1;
constexpr KIRQL DISPATCH_LEVEL = 2;

// ---- Misc exported data ----------------------------------------------------
struct XBOX_HARDWARE_INFO {
    uint32_t Flags;
    uint8_t GpuRevision;
    uint8_t McpRevision;
    uint8_t reserved[2];
};

struct XBOX_KRNL_VERSION {
    uint16_t Major, Minor, Build, Qfe;
};

struct XMM_STATISTICS {
    uint32_t Length;
    uint32_t TotalPhysicalPages;
    uint32_t AvailablePages;
    uint32_t VirtualMemoryBytesCommitted;
    uint32_t VirtualMemoryBytesReserved;
    uint32_t CachePagesCommitted;
    uint32_t PoolPagesCommitted;
    uint32_t StackPagesCommitted;
    uint32_t ImagePagesCommitted;
};

struct XTIME_FIELDS {
    int16_t Year, Month, Day, Hour, Minute, Second, Millisecond, Weekday;
};
