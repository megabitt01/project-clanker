// Run-time library exports.
#include "kexports.h"
#include "sync.h"
#include "thread.h"
#include "../log.h"

#include <windows.h>
#include <intrin.h>

using namespace rt;

extern "C" void *__stdcall kx_ExAllocatePoolWithTag(uint32_t, uint32_t);
extern "C" void __stdcall kx_ExFreePool(void *);

// ---- strings ------------------------------------------------------------------
KEXPORT(void) kx_RtlInitAnsiString(XANSI_STRING *DestinationString, const char *SourceString)
{
    DestinationString->Buffer = const_cast<char *>(SourceString);
    if (SourceString) {
        size_t n = strlen(SourceString);
        DestinationString->Length = uint16_t(n);
        DestinationString->MaximumLength = uint16_t(n + 1);
    } else {
        DestinationString->Length = DestinationString->MaximumLength = 0;
    }
}

KEXPORT(NTSTATUS) kx_RtlAnsiStringToUnicodeString(XUNICODE_STRING *DestinationString, const XANSI_STRING *SourceString,
                                                  XBOOLEAN AllocateDestinationString)
{
    uint32_t n = SourceString->Length;
    if (AllocateDestinationString) {
        DestinationString->MaximumLength = uint16_t((n + 1) * 2);
        DestinationString->Buffer = static_cast<wchar_t *>(kx_ExAllocatePoolWithTag((n + 1) * 2, 'grtS'));
        if (!DestinationString->Buffer)
            return X_STATUS_NO_MEMORY;
    } else if (DestinationString->MaximumLength < n * 2) {
        return X_STATUS_BUFFER_OVERFLOW;
    }
    for (uint32_t i = 0; i < n; i++)
        DestinationString->Buffer[i] = wchar_t(uint8_t(SourceString->Buffer[i]));
    DestinationString->Length = uint16_t(n * 2);
    if (DestinationString->MaximumLength >= n * 2 + 2)
        DestinationString->Buffer[n] = 0;
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_RtlUnicodeStringToAnsiString(XANSI_STRING *DestinationString, const XUNICODE_STRING *SourceString,
                                                  XBOOLEAN AllocateDestinationString)
{
    uint32_t n = SourceString->Length / 2;
    if (AllocateDestinationString) {
        DestinationString->MaximumLength = uint16_t(n + 1);
        DestinationString->Buffer = static_cast<char *>(kx_ExAllocatePoolWithTag(n + 1, 'grtS'));
        if (!DestinationString->Buffer)
            return X_STATUS_NO_MEMORY;
    } else if (DestinationString->MaximumLength < n) {
        return X_STATUS_BUFFER_OVERFLOW;
    }
    for (uint32_t i = 0; i < n; i++) {
        wchar_t c = SourceString->Buffer[i];
        DestinationString->Buffer[i] = c < 0x100 ? char(c) : '?';
    }
    DestinationString->Length = uint16_t(n);
    if (DestinationString->MaximumLength > n)
        DestinationString->Buffer[n] = 0;
    return X_STATUS_SUCCESS;
}

KEXPORT(int32_t) kx_RtlCompareString(const XANSI_STRING *String1, const XANSI_STRING *String2, XBOOLEAN CaseInSensitive)
{
    uint32_t n = std::min(String1->Length, String2->Length);
    for (uint32_t i = 0; i < n; i++) {
        int a = uint8_t(String1->Buffer[i]), b = uint8_t(String2->Buffer[i]);
        if (CaseInSensitive) {
            a = toupper(a);
            b = toupper(b);
        }
        if (a != b)
            return a - b;
    }
    return int32_t(String1->Length) - int32_t(String2->Length);
}

KEXPORT(XBOOLEAN) kx_RtlEqualString(const XANSI_STRING *String1, const XANSI_STRING *String2, XBOOLEAN CaseInSensitive)
{
    return String1->Length == String2->Length && kx_RtlCompareString(String1, String2, CaseInSensitive) == 0;
}

KEXPORT(uint32_t) kx_RtlCompareMemoryUlong(const void *Source, uint32_t Length, uint32_t Pattern)
{
    const uint32_t *p = static_cast<const uint32_t *>(Source);
    uint32_t n = Length / 4, i = 0;
    while (i < n && p[i] == Pattern)
        i++;
    return i * 4;
}

KEXPORT(uint32_t) kx_RtlNtStatusToDosError(NTSTATUS Status)
{
    typedef ULONG(WINAPI * Fn)(LONG);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlNtStatusToDosError"));
    return fn ? fn(Status) : ERROR_GEN_FAILURE;
}

// ---- critical sections (NT algorithm, operating on the Xbox structure) --------
KEXPORT(void) kx_RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION *CriticalSection)
{
    memset(CriticalSection, 0, sizeof *CriticalSection);
    CriticalSection->Event.Type = EventSynchronizationObject;
    CriticalSection->Event.Size = sizeof(XKEVENT) / 4;
    CriticalSection->Event.WaitListHead.Flink = CriticalSection->Event.WaitListHead.Blink =
        &CriticalSection->Event.WaitListHead;
    CriticalSection->LockCount = -1;
}

KEXPORT(void) kx_RtlEnterCriticalSection(XRTL_CRITICAL_SECTION *CriticalSection)
{
    void *self = &xt_current()->kthread;
    if (_InterlockedIncrement(reinterpret_cast<volatile long *>(&CriticalSection->LockCount)) == 0) {
        CriticalSection->OwningThread = self;
        CriticalSection->RecursionCount = 1;
        return;
    }
    if (CriticalSection->OwningThread == self) {
        CriticalSection->RecursionCount++;
        return;
    }
    XDISPATCHER_HEADER *h = &CriticalSection->Event;
    sync_wait(1, &h, false, false, nullptr);
    CriticalSection->OwningThread = self;
    CriticalSection->RecursionCount = 1;
}

KEXPORT(void) kx_RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION *CriticalSection)
{
    if (--CriticalSection->RecursionCount != 0) {
        _InterlockedDecrement(reinterpret_cast<volatile long *>(&CriticalSection->LockCount));
        return;
    }
    CriticalSection->OwningThread = nullptr;
    if (_InterlockedDecrement(reinterpret_cast<volatile long *>(&CriticalSection->LockCount)) >= 0)
        sync_set_event(reinterpret_cast<XKEVENT *>(&CriticalSection->Event));
}

// ---- exceptions: identical record layout, defer to Windows SEH -------------------
KEXPORT(void) kx_RtlRaiseException(EXCEPTION_RECORD *ExceptionRecord)
{
    typedef VOID(WINAPI * Fn)(PEXCEPTION_RECORD);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlRaiseException"));
    fn(ExceptionRecord);
}

// RtlUnwind is bound directly to ntdll!RtlUnwind in the export table: it must
// not have a frame of ours between it and the title's frames.

// ---- time ------------------------------------------------------------------------
KEXPORT(void) kx_RtlTimeToTimeFields(const int64_t *Time, XTIME_FIELDS *TimeFields)
{
    FILETIME ft;
    ft.dwLowDateTime = DWORD(*Time);
    ft.dwHighDateTime = DWORD(*Time >> 32);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    TimeFields->Year = int16_t(st.wYear);
    TimeFields->Month = int16_t(st.wMonth);
    TimeFields->Day = int16_t(st.wDay);
    TimeFields->Hour = int16_t(st.wHour);
    TimeFields->Minute = int16_t(st.wMinute);
    TimeFields->Second = int16_t(st.wSecond);
    TimeFields->Millisecond = int16_t(st.wMilliseconds);
    TimeFields->Weekday = int16_t(st.wDayOfWeek);
}

KEXPORT(XBOOLEAN) kx_RtlTimeFieldsToTime(const XTIME_FIELDS *TimeFields, int64_t *Time)
{
    SYSTEMTIME st = {};
    st.wYear = WORD(TimeFields->Year);
    st.wMonth = WORD(TimeFields->Month);
    st.wDay = WORD(TimeFields->Day);
    st.wHour = WORD(TimeFields->Hour);
    st.wMinute = WORD(TimeFields->Minute);
    st.wSecond = WORD(TimeFields->Second);
    st.wMilliseconds = WORD(TimeFields->Millisecond);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
        return 0;
    *Time = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return 1;
}
