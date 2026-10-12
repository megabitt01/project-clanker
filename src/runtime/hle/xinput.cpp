// XPP / XInput replacement: Xbox controller ports backed by SDL2 game
// controllers.
//
// The XDK input stack is a full USB host driver (OHCI + hub + XID class
// drivers) talking to MCPX hardware. Rather than emulating USB, the public
// XAPI entry points are replaced: device enumeration reports SDL controllers
// as gamepads on ports 1-4, and XInputGetState reads their current state.
#include "hook.h"
#include "../host/host.h"
#include "../kernel/kfuncs.h"
#include "../log.h"

#include <mutex>
#include <unordered_map>

using namespace rt;

namespace {

// XPP device type tables in default.xbe (identified by XbSymbolDatabase).
constexpr uint32_t XDEVICE_TYPE_GAMEPAD = 0x0043CE98;
constexpr uint32_t XDEVICE_TYPE_MEMORY_UNIT = 0x0043CD40;

constexpr uint32_t ERROR_SUCCESS_ = 0;
constexpr uint32_t ERROR_INVALID_PARAMETER_ = 0x57;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_ = 0x48F;

#pragma pack(push, 1)
struct XINPUT_GAMEPAD {
    uint16_t wButtons;
    uint8_t bAnalogButtons[8];
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XINPUT_STATE {
    uint32_t dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};
struct XINPUT_RUMBLE {
    uint16_t wLeftMotorSpeed, wRightMotorSpeed;
};
struct XINPUT_CAPABILITIES {
    uint8_t SubType;
    uint16_t Reserved;
    XINPUT_GAMEPAD In;
    XINPUT_RUMBLE Out;
};
struct XINPUT_FEEDBACK {
    uint32_t dwStatus;
    XHANDLE hEvent;
    uint8_t Reserved[58];
    XINPUT_RUMBLE Rumble;
};
#pragma pack(pop)
static_assert(sizeof(XINPUT_STATE) == 22);
static_assert(offsetof(XINPUT_FEEDBACK, Rumble) == 0x42);

struct OpenPad {
    uint32_t port;
    uint32_t packet;
    XINPUT_GAMEPAD last;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_reported; // device type -> last reported mask
OpenPad g_open[4];
bool g_is_open[4];

uint32_t connected_mask(uint32_t type)
{
    if (type != XDEVICE_TYPE_GAMEPAD)
        return 0; // no memory units, headsets or IR dongles
    uint32_t mask = 0;
    for (int i = 0; i < MAX_PADS; i++)
        if (host_pad_state(i).connected)
            mask |= 1u << i;
    return mask;
}

uint32_t handle_for(uint32_t port) { return 0x58500000u | port; }   // "XP" + port
OpenPad *pad_from_handle(uint32_t h)
{
    if ((h & 0xFFFF0000u) != 0x58500000u || (h & 0xFFFF) >= 4 || !g_is_open[h & 0xFFFF])
        return nullptr;
    return &g_open[h & 0xFFFF];
}

} // namespace

extern "C" {

static void __stdcall hle_XInitDevices(uint32_t dwPreallocTypeCount, void *PreallocTypes)
{
    LOG_INFO("xinput", "XInitDevices: USB stack replaced by SDL game controllers");
}

static uint32_t __stdcall hle_XGetDevices(uint32_t DeviceType)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t mask = connected_mask(DeviceType);
    g_reported[DeviceType] = mask;
    return mask;
}

static int __stdcall hle_XGetDeviceChanges(uint32_t DeviceType, uint32_t *pdwInsertions, uint32_t *pdwRemovals)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t mask = connected_mask(DeviceType);
    uint32_t &last = g_reported[DeviceType];
    *pdwInsertions = mask & ~last;
    *pdwRemovals = last & ~mask;
    last = mask;
    if (*pdwInsertions || *pdwRemovals)
        LOG_INFO("xinput", "device changes for type %08X: +%X -%X", DeviceType, *pdwInsertions, *pdwRemovals);
    return (*pdwInsertions | *pdwRemovals) != 0;
}

static uint32_t __stdcall hle_XPeekDevices(uint32_t DeviceType, uint32_t *pdwInsertions, uint32_t *pdwRemovals)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t mask = connected_mask(DeviceType);
    uint32_t last = g_reported[DeviceType];
    if (pdwInsertions) *pdwInsertions = mask & ~last;
    if (pdwRemovals) *pdwRemovals = last & ~mask;
    return mask;
}

static uint32_t __stdcall hle_XInputOpen(uint32_t DeviceType, uint32_t dwPort, uint32_t dwSlot, void *pPollingParameters)
{
    if (DeviceType != XDEVICE_TYPE_GAMEPAD || dwPort >= 4 || !host_pad_state(dwPort).connected) {
        kx_DbgPrint("XInputOpen(type %08X, port %u) failed\n", DeviceType, dwPort);
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_open[dwPort] = OpenPad{dwPort, 1, {}};
    g_is_open[dwPort] = true;
    LOG_INFO("xinput", "XInputOpen(port %u)", dwPort + 1);
    return handle_for(dwPort);
}

static void __stdcall hle_XInputClose(uint32_t hDevice)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (OpenPad *p = pad_from_handle(hDevice))
        g_is_open[p->port] = false;
}

static uint32_t __stdcall hle_XInputGetState(uint32_t hDevice, XINPUT_STATE *pState)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    OpenPad *p = pad_from_handle(hDevice);
    if (!p)
        return ERROR_INVALID_PARAMETER_;
    PadState s = host_pad_state(p->port);
    if (!s.connected)
        return ERROR_DEVICE_NOT_CONNECTED_;
    XINPUT_GAMEPAD g;
    g.wButtons = s.buttons;
    memcpy(g.bAnalogButtons, s.analog, 8);
    g.sThumbLX = s.lx;
    g.sThumbLY = s.ly;
    g.sThumbRX = s.rx;
    g.sThumbRY = s.ry;
    if (memcmp(&g, &p->last, sizeof g) != 0) {
        p->last = g;
        p->packet++;
    }
    pState->dwPacketNumber = p->packet;
    pState->Gamepad = g;
    return ERROR_SUCCESS_;
}

static uint32_t __stdcall hle_XInputGetCapabilities(uint32_t hDevice, XINPUT_CAPABILITIES *pCapabilities)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!pad_from_handle(hDevice))
        return ERROR_INVALID_PARAMETER_;
    memset(pCapabilities, 0, sizeof *pCapabilities);
    pCapabilities->SubType = 1; // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    pCapabilities->In.wButtons = 0xFF;
    memset(pCapabilities->In.bAnalogButtons, 0xFF, 8);
    pCapabilities->In.sThumbLX = pCapabilities->In.sThumbLY = int16_t(0xFFC0);
    pCapabilities->In.sThumbRX = pCapabilities->In.sThumbRY = int16_t(0xFFC0);
    pCapabilities->Out.wLeftMotorSpeed = pCapabilities->Out.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS_;
}

static uint32_t __stdcall hle_XInputSetState(uint32_t hDevice, XINPUT_FEEDBACK *pFeedback)
{
    OpenPad *p;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        p = pad_from_handle(hDevice);
    }
    if (!p)
        return ERROR_INVALID_PARAMETER_;
    host_pad_rumble(int(p->port), pFeedback->Rumble.wLeftMotorSpeed, pFeedback->Rumble.wRightMotorSpeed);
    // Completes immediately: report success and signal the caller's event.
    pFeedback->dwStatus = ERROR_SUCCESS_;
    if (pFeedback->hEvent)
        kx_NtSetEvent(pFeedback->hEvent, nullptr);
    return 997; // ERROR_IO_PENDING, as the real driver returns
}

} // extern "C"

void rt::hle_install_xinput()
{
    HOOK(0x0043D090, hle_XInitDevices);
    HOOK(0x0043DEA7, hle_XGetDevices);
    HOOK(0x0043DEC9, hle_XGetDeviceChanges);
    HOOK(0x0043DE66, hle_XPeekDevices);
    HOOK(0x004449AE, hle_XInputOpen);
    HOOK(0x00444A04, hle_XInputClose);
    HOOK(0x00444A10, hle_XInputGetCapabilities);
    HOOK(0x00444BE8, hle_XInputGetState);
    HOOK(0x00444C5B, hle_XInputSetState);
}
