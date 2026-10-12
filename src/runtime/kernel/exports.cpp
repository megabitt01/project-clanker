// xboxkrnl.exe export table: ordinal -> implementation, and the code that
// patches the XBE's kernel thunk table at load time.
#include "kexports.h"
#include "object.h"
#include "sync.h"
#include "../log.h"

#include <windows.h>

using namespace rt;

#include "kfuncs.h"

extern "C" {
extern XBOX_HARDWARE_INFO g_XboxHardwareInfo;
extern XBOX_KRNL_VERSION g_XboxKrnlVersion;
extern uint8_t g_XboxHDKey[16], g_XboxSignatureKey[16], g_XboxAlternateSignatureKeys[16][16], g_XboxLANKey[16];
extern uint8_t g_XePublicKeyData[284];
extern uint32_t g_LaunchDataPage, g_HalDiskCachePartitionCount, g_HalBootSMCVideoMode;
extern uint8_t g_IdexChannelObject[0x100];
extern XANSI_STRING g_XeImageFileName, g_HalDiskModelNumber, g_HalDiskSerialNumber;
}

#define F(ord, name) {ord, #name, reinterpret_cast<void *>(&kx_##name)}
#define D(ord, name, sym) {ord, #name, (void *)&sym}

static void *ntdll(const char *name)
{
    return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("ntdll.dll"), name));
}

void rt::kernel_resolve_imports(uint32_t thunk_va)
{
    const KernelExport table[] = {
        F(1, AvGetSavedDataAddress), F(2, AvSendTVEncoderOption), F(3, AvSetDisplayMode),
        F(4, AvSetSavedDataAddress), F(8, DbgPrint), F(15, ExAllocatePoolWithTag),
        D(16, ExEventObjectType, g_ExEventObjectType), F(17, ExFreePool), F(23, ExQueryPoolBlockSize),
        F(24, ExQueryNonVolatileSetting), D(40, HalDiskCachePartitionCount, g_HalDiskCachePartitionCount),
        D(41, HalDiskModelNumber, g_HalDiskModelNumber), D(42, HalDiskSerialNumber, g_HalDiskSerialNumber),
        F(44, HalGetInterruptVector), F(46, HalReadWritePCISpace), F(47, HalRegisterShutdownNotification),
        F(49, HalReturnToFirmware), F(62, IoBuildSynchronousFsdRequest), F(65, IoCreateDevice),
        F(67, IoCreateSymbolicLink), F(69, IoDeleteSymbolicLink), F(74, IoInvalidDeviceRequest),
        F(81, IoStartNextPacket), F(83, IoStartPacket), F(84, IoSynchronousDeviceIoControlRequest),
        F(85, IoSynchronousFsdRequest), F(86, IofCallDriver), F(87, IofCompleteRequest), F(95, KeBugCheck),
        F(97, KeCancelTimer), F(98, KeConnectInterrupt), F(99, KeDelayExecutionThread),
        F(100, KeDisconnectInterrupt), F(107, KeInitializeDpc), F(109, KeInitializeInterrupt),
        F(113, KeInitializeTimerEx), F(119, KeInsertQueueDpc), F(124, KeQueryBasePriorityThread),
        F(126, KeQueryPerformanceCounter), F(127, KeQueryPerformanceFrequency), F(128, KeQuerySystemTime),
        F(129, KeRaiseIrqlToDpcLevel), F(137, KeRemoveQueueDpc), F(139, KeRestoreFloatingPointState),
        F(142, KeSaveFloatingPointState), F(143, KeSetBasePriorityThread), F(145, KeSetEvent), F(149, KeSetTimer),
        F(150, KeSetTimerEx), F(151, KeStallExecutionProcessor), F(153, KeSynchronizeExecution),
        D(156, KeTickCount, g_KeTickCount), F(158, KeWaitForMultipleObjects), F(159, KeWaitForSingleObject),
        F(160, KfRaiseIrql), F(161, KfLowerIrql), D(164, LaunchDataPage, g_LaunchDataPage),
        F(165, MmAllocateContiguousMemory), F(166, MmAllocateContiguousMemoryEx), F(168, MmClaimGpuInstanceMemory),
        F(169, MmCreateKernelStack), F(170, MmDeleteKernelStack), F(171, MmFreeContiguousMemory),
        F(173, MmGetPhysicalAddress), F(175, MmLockUnlockBufferPages), F(176, MmLockUnlockPhysicalPage),
        F(178, MmPersistContiguousMemory), F(179, MmQueryAddressProtect), F(180, MmQueryAllocationSize),
        F(181, MmQueryStatistics), F(182, MmSetAddressProtect), F(184, NtAllocateVirtualMemory), F(187, NtClose),
        F(189, NtCreateEvent), F(190, NtCreateFile), F(193, NtCreateSemaphore), F(195, NtDeleteFile),
        F(196, NtDeviceIoControlFile), F(197, NtDuplicateObject), F(198, NtFlushBuffersFile),
        F(199, NtFreeVirtualMemory), F(200, NtFsControlFile), F(202, NtOpenFile), F(203, NtOpenSymbolicLinkObject),
        F(207, NtQueryDirectoryFile), F(210, NtQueryFullAttributesFile), F(211, NtQueryInformationFile),
        F(215, NtQuerySymbolicLinkObject), F(217, NtQueryVirtualMemory), F(218, NtQueryVolumeInformationFile),
        F(219, NtReadFile), F(222, NtReleaseSemaphore), F(225, NtSetEvent), F(226, NtSetInformationFile),
        F(228, NtSetSystemTime), F(233, NtWaitForSingleObject), F(234, NtWaitForSingleObjectEx), F(236, NtWriteFile),
        F(246, ObReferenceObjectByHandle), F(247, ObReferenceObjectByName), F(250, ObfDereferenceObject),
        F(252, PhyGetLinkState), F(253, PhyInitialize), F(255, PsCreateSystemThreadEx),
        F(258, PsTerminateSystemThread), D(259, PsThreadObjectType, g_PsThreadObjectType),
        F(260, RtlAnsiStringToUnicodeString), F(269, RtlCompareMemoryUlong), F(270, RtlCompareString),
        F(277, RtlEnterCriticalSection), F(279, RtlEqualString), F(289, RtlInitAnsiString),
        F(291, RtlInitializeCriticalSection), F(294, RtlLeaveCriticalSection), F(301, RtlNtStatusToDosError),
        {302, "RtlRaiseException", ntdll("RtlRaiseException")}, F(304, RtlTimeFieldsToTime),
        F(305, RtlTimeToTimeFields), F(308, RtlUnicodeStringToAnsiString), {312, "RtlUnwind", ntdll("RtlUnwind")},
        D(322, XboxHardwareInfo, g_XboxHardwareInfo), D(323, XboxHDKey, g_XboxHDKey),
        D(324, XboxKrnlVersion, g_XboxKrnlVersion), D(325, XboxSignatureKey, g_XboxSignatureKey),
        D(326, XeImageFileName, g_XeImageFileName), F(327, XeLoadSection), F(328, XeUnloadSection),
        F(335, XcSHAInit), F(336, XcSHAUpdate), F(337, XcSHAFinal), F(338, XcRC4Key), F(339, XcRC4Crypt),
        F(340, XcHMAC), F(344, XcVerifyPKCS1Signature), F(345, XcModExp), F(346, XcDESKeyParity),
        F(347, XcKeyTable), F(349, XcBlockCryptCBC), D(353, XboxLANKey, g_XboxLANKey),
        D(354, XboxAlternateSignatureKeys, g_XboxAlternateSignatureKeys), D(355, XePublicKeyData, g_XePublicKeyData),
        D(356, HalBootSMCVideoMode, g_HalBootSMCVideoMode), D(357, IdexChannelObject, g_IdexChannelObject),
        F(358, HalIsResetOrShutdownPending), F(359, IoMarkIrpMustComplete), F(360, HalInitiateShutdown),
    };

    uint32_t *thunk = reinterpret_cast<uint32_t *>(uintptr_t(thunk_va));
    int resolved = 0;
    for (; *thunk; thunk++) {
        uint32_t ord = *thunk & 0x7FFFFFFF;
        const KernelExport *e = nullptr;
        for (const auto &k : table)
            if (k.ordinal == ord)
                e = &k;
        if (!e || !e->address)
            fatal("The XBE imports kernel ordinal %u, which is not implemented", ord);
        *thunk = uint32_t(uintptr_t(e->address));
        resolved++;
    }
    LOG_INFO("kernel", "resolved %d kernel imports", resolved);
}
