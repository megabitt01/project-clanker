// HAL, AV, EEPROM settings, XBE section loading and exported kernel data.
#include "kexports.h"
#include "../log.h"
#include "../xbe.h"
#include "../gpu/nv2a.h"

#include <windows.h>
#include <cstdarg>

using namespace rt;

// ---- exported data -------------------------------------------------------------
extern "C" {
XBOX_HARDWARE_INFO g_XboxHardwareInfo = {0x20, 0xD4, 0xC3, {0, 0}};
XBOX_KRNL_VERSION g_XboxKrnlVersion = {1, 0, 5838, 1};
uint8_t g_XboxHDKey[16];
uint8_t g_XboxSignatureKey[16];
uint8_t g_XboxAlternateSignatureKeys[16][16];
uint8_t g_XboxLANKey[16];
uint8_t g_XePublicKeyData[284];
uint32_t g_LaunchDataPage;                  // PLAUNCH_DATA_PAGE, null = cold boot
uint32_t g_HalDiskCachePartitionCount = 3;
uint32_t g_HalBootSMCVideoMode = 1;
uint8_t g_IdexChannelObject[0x100];
static char g_image_file_name_buf[] = "\\Device\\CdRom0\\default.xbe";
XANSI_STRING g_XeImageFileName = {uint16_t(sizeof(g_image_file_name_buf) - 1), uint16_t(sizeof(g_image_file_name_buf)),
                                  g_image_file_name_buf};
static char g_disk_model[] = "SWBF2-PORT-DISK";
static char g_disk_serial[] = "0000000000000000";
XANSI_STRING g_HalDiskModelNumber = {uint16_t(sizeof(g_disk_model) - 1), uint16_t(sizeof(g_disk_model)), g_disk_model};
XANSI_STRING g_HalDiskSerialNumber = {uint16_t(sizeof(g_disk_serial) - 1), uint16_t(sizeof(g_disk_serial)), g_disk_serial};
}

static void init_keys()
{
    // Not the console's real keys: the port only needs them to be stable so
    // that save-game signatures it writes verify when read back.
    for (int i = 0; i < 16; i++) {
        g_XboxHDKey[i] = uint8_t(0x5A ^ (i * 17));
        g_XboxSignatureKey[i] = uint8_t(0xC3 ^ (i * 29));
        g_XboxLANKey[i] = uint8_t(0x3C ^ (i * 7));
        for (int k = 0; k < 16; k++)
            g_XboxAlternateSignatureKeys[k][i] = uint8_t(i * 13 + k * 31);
    }
}

// ---- HAL ------------------------------------------------------------------------
KEXPORT(void) kx_HalReturnToFirmware(uint32_t Routine)
{
    LOG_INFO("hal", "HalReturnToFirmware(%u): the title asked to leave; exiting", Routine);
    log_flush();
    TerminateProcess(GetCurrentProcess(), 0);
}

KEXPORT(void) kx_HalInitiateShutdown()
{
    kx_HalReturnToFirmware(0);
}

KEXPORT(XBOOLEAN) kx_HalIsResetOrShutdownPending() { return 0; }

KEXPORT(void) kx_HalRegisterShutdownNotification(void *ShutdownRegistration, XBOOLEAN Register) {}

KEXPORT(void) kx_HalReadWritePCISpace(uint32_t BusNumber, uint32_t SlotNumber, uint32_t RegisterNumber, void *Buffer,
                                      uint32_t Length, XBOOLEAN WritePCISpace)
{
    if (!WritePCISpace)
        memset(Buffer, 0, Length);
    LOG_DEBUG("hal", "PCI %s bus %u slot %u reg 0x%X len %u", WritePCISpace ? "write" : "read", BusNumber, SlotNumber,
              RegisterNumber, Length);
}

// ---- AV -----------------------------------------------------------------------
static void *g_av_saved_data;

KEXPORT(void *) kx_AvGetSavedDataAddress() { return g_av_saved_data; }
KEXPORT(void) kx_AvSetSavedDataAddress(void *Address) { g_av_saved_data = Address; }

KEXPORT(void) kx_AvSendTVEncoderOption(void *RegisterBase, uint32_t Option, uint32_t Param, uint32_t *Result)
{
    // AV_QUERY_AV_CAPABILITIES: HDTV pack, NTSC-M, 60 Hz
    constexpr uint32_t AV_PACK_HDTV = 1;
    constexpr uint32_t AV_STANDARD_NTSC_M = 0x00000100u;
    constexpr uint32_t AV_FLAGS_60Hz = 0x00400000u;
    if (Result) {
        if (Option == 6)
            *Result = AV_PACK_HDTV | AV_STANDARD_NTSC_M | AV_FLAGS_60Hz;
        else
            *Result = 0;
    }
    LOG_DEBUG("av", "AvSendTVEncoderOption(%u, %u)", Option, Param);
}

KEXPORT(uint32_t) kx_AvSetDisplayMode(void *RegisterBase, uint32_t Step, uint32_t Mode, uint32_t Format, uint32_t Pitch,
                                      uint32_t FrameBuffer)
{
    LOG_INFO("av", "AvSetDisplayMode(step %u, mode %08X, format %X, pitch %u, fb %08X)", Step, Mode, Format, Pitch,
             FrameBuffer);
    nv2a_set_display_mode(Mode, Format, Pitch, FrameBuffer);
    return 0;
}

// ---- EEPROM ("non-volatile settings") -------------------------------------------
KEXPORT(NTSTATUS) kx_ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t *Type, void *Value, uint32_t ValueLength,
                                               uint32_t *ResultLength)
{
    constexpr uint32_t REG_DWORD_ = 4, REG_BINARY_ = 3;
    uint32_t dword = 0;
    bool is_dword = true;
    uint8_t blob[64] = {};
    uint32_t blob_len = 0;
    switch (ValueIndex) {
    case 0x7: dword = 1; break;                 // XC_LANGUAGE: English
    case 0x8: dword = 0; break;                 // XC_VIDEO: flags live in bits 16+; plain 480i
    case 0x9: dword = 0x00010001; break;        // XC_AUDIO: stereo + Dolby Digital allowed
    case 0xA: dword = 0; break;                 // XC_PARENTAL_CONTROL_GAMES: off
    case 0xB: dword = 0; break;                 // XC_PARENTAL_CONTROL_PASSWORD
    case 0xC: dword = 0; break;                 // XC_PARENTAL_CONTROL_MOVIES
    case 0x11: dword = 0; break;                // XC_MISC (auto-off disabled)
    case 0x12: dword = 1; break;                // XC_DVD_REGION
    case 0x0: dword = 0; break;                 // XC_TIMEZONE_BIAS (UTC)
    case 0x1: case 0x4:                         // XC_TZ_STD_NAME / DLT_NAME
        is_dword = false;
        memcpy(blob, "UTC", 4);
        blob_len = 4;
        break;
    case 0x2: case 0x5:                         // XC_TZ_STD_DATE / DLT_DATE
        is_dword = false;
        blob_len = 4;
        break;
    case 0x3: case 0x6: dword = 0; break;       // XC_TZ_STD_BIAS / DLT_BIAS
    case 0x103: dword = 0x00400100; break;      // XC_FACTORY_AV_REGION: NTSC-M 60Hz
    case 0x104: dword = 1; break;               // XC_FACTORY_GAME_REGION: North America
    case 0x101:                                 // XC_FACTORY_ETHERNET_ADDR
        is_dword = false;
        blob[0] = 0x00; blob[1] = 0x50; blob[2] = 0xF2; blob[3] = 0x12; blob[4] = 0x34; blob[5] = 0x56;
        blob_len = 6;
        break;
    default:
        LOG_WARN("hal", "ExQueryNonVolatileSetting(0x%X) unknown, returning zero", ValueIndex);
        break;
    }
    if (is_dword) {
        if (Type) *Type = REG_DWORD_;
        if (ResultLength) *ResultLength = 4;
        if (ValueLength < 4)
            return X_STATUS_BUFFER_TOO_SMALL;
        *static_cast<uint32_t *>(Value) = dword;
    } else {
        if (Type) *Type = REG_BINARY_;
        if (ResultLength) *ResultLength = blob_len;
        if (ValueLength < blob_len)
            return X_STATUS_BUFFER_TOO_SMALL;
        memcpy(Value, blob, blob_len);
    }
    return X_STATUS_SUCCESS;
}

// ---- networking PHY -------------------------------------------------------------
KEXPORT(uint32_t) kx_PhyGetLinkState(XBOOLEAN Update) { return 0; } // no cable
KEXPORT(NTSTATUS) kx_PhyInitialize(XBOOLEAN forceReset, void *param) { return X_STATUS_SUCCESS; }

// ---- XBE sections ----------------------------------------------------------------
KEXPORT(NTSTATUS) kx_XeLoadSection(XbeSectionHeader *Section)
{
    if (Section->SectionReferenceCount++ == 0)
        xbe_load_section(Section);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_XeUnloadSection(XbeSectionHeader *Section)
{
    if (Section->SectionReferenceCount == 0)
        return X_STATUS_INVALID_PARAMETER;
    Section->SectionReferenceCount--;
    return X_STATUS_SUCCESS;
}

// ---- debug -------------------------------------------------------------------------
extern "C" uint32_t __cdecl kx_DbgPrint(const char *Format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, Format);
    vsnprintf(buf, sizeof buf, Format, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    LOG_INFO("dbg", "%s", buf);
    return 0;
}

namespace rt {
void hal_init() { init_keys(); }
}
