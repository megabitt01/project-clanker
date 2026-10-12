// XAPI functions that would touch hardware or the raw disk.
#include "hook.h"
#include "../kernel/kfuncs.h"
#include "../log.h"
#include "../crash.h"

#include <windows.h>

using namespace rt;

extern "C" {

// XMountUtilityDrive picks one of the HDD cache partitions (reading the
// partition headers from the raw disk) and maps it to Z:. We always use
// Partition3.
static int __stdcall hle_XMountUtilityDrive(int fFormatClean)
{
    static char link[] = "\\??\\Z:";
    static char target[] = "\\Device\\Harddisk0\\Partition3";
    XANSI_STRING l = {uint16_t(sizeof link - 1), uint16_t(sizeof link), link};
    XANSI_STRING t = {uint16_t(sizeof target - 1), uint16_t(sizeof target), target};
    kx_IoCreateSymbolicLink(&l, &t);
    LOG_INFO("xapi", "XMountUtilityDrive(%d): Z: -> Partition3", fFormatClean);
    return 1;
}

static void __stdcall hle_XapiBootToDash(uint32_t dwReason, uint32_t dwParameter1, uint32_t dwParameter2)
{
    dump_all_threads("XapiBootToDash");
    fatal("The game asked to return to the Xbox dashboard (reason %u, %u, %u)", dwReason, dwParameter1, dwParameter2);
}

static uint32_t __stdcall hle_XLaunchNewImageA(const char *lpTitlePath, void *pLaunchData)
{
    LOG_INFO("xapi", "XLaunchNewImage(%s): exiting", lpTitlePath ? lpTitlePath : "(dashboard)");
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    dump_context("XLaunchNewImage caller", &ctx);
    dump_all_threads("XLaunchNewImage");
    log_flush();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void __stdcall hle_XAutoPowerDownResetTimer() {}

// XAPI's QueryPerformanceCounter reads the CPU timestamp counter directly and
// QueryPerformanceFrequency reports the Xbox's 733 MHz. On a PC the TSC runs
// several times faster, which would speed up every game timer, so both the
// API and D3D's internal TSC read use the kernel's scaled counter instead.
static int __stdcall hle_QueryPerformanceCounter(int64_t *count)
{
    *count = kx_KeQueryPerformanceCounter();
    return 1;
}

// D3D's `rdtsc; ret` helper: returns EDX:EAX, preserves every other register.
static __declspec(naked) void hle_D3D_ReadTsc()
{
    __asm {
        push ecx
        call kx_KeQueryPerformanceCounter
        pop ecx
        ret
    }
}

static void __stdcall hle_OutputDebugStringA(const char *s)
{
    if (s)
        kx_DbgPrint("%s", s);
}

} // extern "C"

void rt::hle_install_xapi()
{
    HOOK(0x0032189E, hle_XMountUtilityDrive);
    HOOK(0x0032311D, hle_XapiBootToDash);
    HOOK(0x0031F87E, hle_XLaunchNewImageA);
    HOOK(0x00325FB2, hle_XAutoPowerDownResetTimer);
    HOOK(0x00323626, hle_OutputDebugStringA);
    HOOK(0x00320963, hle_QueryPerformanceCounter);
    HOOK(0x0042EBA0, hle_D3D_ReadTsc);
}
