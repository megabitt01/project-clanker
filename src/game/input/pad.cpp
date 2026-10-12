// Controller polling: decompiled from default.xbe.
//
//   0x002FB800  pad_update_buttons   (register argument: EAX = pad)
//   0x002FBB80  pad_update           (register argument: ESI = pad)
//
// Both were reconstructed from the Ghidra decompilation and disassembly and
// replace the original machine code via hooks. Floating-point results can
// differ from the x87 original in the last bits (SSE arithmetic).
#include "pad.h"
#include "../xbe.h"
#include "../../runtime/hle/hook.h"

#include <algorithm>
#include <cmath>

namespace game {

namespace {

// Globals in .data
uint8_t &g_input_suspended = xbe_global<uint8_t>(0x00B74650);  // skip XInput calls (demo playback)
uint8_t &g_input_activity = xbe_global<uint8_t>(0x00B74651);   // any button pressed this frame
uint8_t &g_analog_threshold = xbe_global<uint8_t>(0x004E0DD0); // analog press hysteresis (32)

typedef uint32_t(__stdcall *XInputGetStateFn)(uint32_t device, XInputState *state);
typedef uint32_t(__stdcall *XInputSetStateFn)(uint32_t device, void *feedback);
const auto XInputGetState = xbe_function<XInputGetStateFn>(0x00444BE8);
const auto XInputSetState = xbe_function<XInputSetStateFn>(0x00444C5B);

constexpr uint32_t ERROR_IO_PENDING_ = 997;

// Turns an analog button value into a digital bit with hysteresis: the bit
// sets once the value climbs `threshold` above the lowest value seen while
// released, and clears once it falls `threshold` below the highest value
// seen while pressed.
void analog_button(GamePad *pad, int index, uint32_t bit, uint32_t threshold)
{
    uint8_t value = pad->current.analog[index];
    uint8_t &ref = pad->analog_ref[index];
    if (!(pad->buttons & bit)) {
        if (value < ref) {
            ref = value;
        } else if (uint32_t(ref) + threshold <= value) {
            pad->buttons |= bit;
            ref = value;
        }
    } else if (value > ref) {
        ref = value;
    } else if (int(ref) - int(threshold) >= int(value)) {
        pad->buttons &= ~bit;
        ref = value;
    }
}

// Rescales an axis value outside a dead zone back to the full -1..1 range.
float apply_dead_zone(float v, float dz)
{
    if (v < -dz)
        return (v + dz) / (1.0f - dz);
    if (v <= dz)
        return 0.0f;
    return (v - dz) / (1.0f - dz);
}

// Updates the pinned trackers for one axis (see GamePad::pinned).
void track_pinned(float axis, float &low, float &high)
{
    if (low > -1.04f) {
        if (low > -1.0f)
            low = std::min(low, axis);
        else if (axis > -1.0f)
            low = 0.0f;
        else
            low -= 0.01f;
    }
    if (high < 1.04f) {
        if (high < 1.0f)
            high = std::max(high, axis);
        else if (axis < 1.0f)
            high = 0.0f;
        else
            high += 0.01f;
    }
}

// An axis is "pinned" once both trackers have run past the end stops.
bool pinned(const float (&p)[2]) { return p[0] <= -1.04f && p[1] >= 1.04f; }

} // namespace

void pad_update_buttons(GamePad *pad)
{
    uint32_t threshold = g_analog_threshold;
    uint32_t old = pad->buttons;
    pad->buttons = old & 0xFF00; // analog-derived bits carry over, digital bits are rebuilt

    analog_button(pad, 0, PAD_A, threshold);
    analog_button(pad, 1, PAD_B, threshold);
    analog_button(pad, 2, PAD_X, threshold);
    analog_button(pad, 3, PAD_Y, threshold);
    analog_button(pad, 4, PAD_BLACK, threshold);
    analog_button(pad, 5, PAD_WHITE, threshold);
    analog_button(pad, 6, PAD_LTRIGGER, threshold);
    analog_button(pad, 7, PAD_RTRIGGER, threshold);

    uint16_t d = pad->current.buttons;
    if (d & 0x0001) pad->buttons |= PAD_DPAD_UP;
    if (d & 0x0002) pad->buttons |= PAD_DPAD_DOWN;
    if (d & 0x0004) pad->buttons |= PAD_DPAD_LEFT;
    if (d & 0x0008) pad->buttons |= PAD_DPAD_RIGHT;
    if (d & 0x0010) pad->buttons |= PAD_START;
    if (d & 0x0020) pad->buttons |= PAD_BACK;
    if (d & 0x0040) pad->buttons |= PAD_LTHUMB;
    if (d & 0x0080) pad->buttons |= PAD_RTHUMB;

    uint32_t changed = pad->buttons ^ old;
    pad->pressed = changed & pad->buttons;
    pad->released = changed & old;
    if (pad->pressed)
        g_input_activity = 1;
}

void pad_update(GamePad *pad)
{
    if (!pad->device)
        return;
    if (!g_input_suspended)
        XInputGetState(pad->device, &pad->state);
    pad->current = pad->state.gamepad;

    // Normalise the sticks (the left stick reaches full scale slightly early,
    // the right one is boosted by 3%) and clamp the larger axis to 1.
    float lx = (float(pad->current.thumb_lx) + 0.5f) * (1.0f / 31812.5f);
    float ly = (float(pad->current.thumb_ly) + 0.5f) * (1.0f / 31812.5f);
    float rx = (float(pad->current.thumb_rx) + 0.5f) * (1.0f / 32767.5f) * 1.03f;
    float ry = (float(pad->current.thumb_ry) + 0.5f) * (1.0f / 32767.5f) * 1.03f;
    auto clamp_pair = [](float &a, float &b) {
        float m = std::max(std::fabs(a), std::fabs(b));
        if (m > 1.0f) {
            a /= m;
            b /= m;
        }
    };
    clamp_pair(lx, ly);
    clamp_pair(rx, ry);

    // Dead zone: 0.4 normally, 0.2 while the axis is held pinned, and
    // interpolated toward 0.2 as the other axis is deflected.
    for (int s = 0; s < 2; s++) {
        float &x = s ? rx : lx, &y = s ? ry : ly;
        float dzx = pinned(pad->pinned[s * 2]) ? 0.2f : 0.4f;
        float dzy = pinned(pad->pinned[s * 2 + 1]) ? 0.2f : 0.4f;
        dzx = (0.2f - dzx) * y * y + dzx;
        dzy = (0.2f - dzy) * x * x + dzy;
        x = apply_dead_zone(x, dzx);
        y = apply_dead_zone(y, dzy);
    }
    pad->stick[0] = lx;
    pad->stick[1] = ly;
    pad->stick[2] = rx;
    pad->stick[3] = ry;
    for (int a = 0; a < 4; a++)
        track_pinned(pad->stick[a], pad->pinned[a][0], pad->pinned[a][1]);

    pad_update_buttons(pad);
    if (pad->pressed)
        g_input_activity = 1;

    constexpr float k = 1.0f / 255.0f;
    pad->analog[7] = pad->current.analog[2] * k; // X
    pad->analog[6] = pad->current.analog[0] * k; // A
    pad->analog[5] = pad->current.analog[1] * k; // B
    pad->analog[4] = pad->current.analog[3] * k; // Y
    pad->analog[3] = pad->current.analog[7] * k; // right trigger
    pad->analog[2] = pad->current.analog[6] * k; // left trigger
    pad->analog[1] = pad->current.analog[4] * k; // black
    pad->analog[0] = pad->current.analog[5] * k; // white
    for (int b = 0; b < 8; b++)
        pad->digital[b] = (pad->buttons & (1u << b)) ? 1.0f : 0.0f;

    // Rumble: blend 90% toward the requested level each frame.
    if (*reinterpret_cast<uint32_t *>(pad->feedback) != ERROR_IO_PENDING_) {
        float level = float(pad->rumble_target - pad->rumble_base) * 0.9f + float(pad->rumble_base);
        uint16_t v = uint16_t(int32_t(level));
        pad->rumble_left = v;
        pad->rumble_right = v;
        if (!g_input_suspended)
            XInputSetState(pad->device, pad->feedback);
    }
    pad->rumble_base = 0;
    pad->rumble_target = 0;
}

// ---- register-convention entry points --------------------------------------
namespace {

void __cdecl pad_update_buttons_c(GamePad *pad) { pad_update_buttons(pad); }
void __cdecl pad_update_c(GamePad *pad) { pad_update(pad); }

// Original: EAX = pad; preserves EBX/ESI/EDI/EBP.
__declspec(naked) void pad_update_buttons_entry()
{
    __asm {
        push eax
        call pad_update_buttons_c
        add esp, 4
        ret
    }
}

// Original: ESI = pad; preserves EBX/ESI/EDI/EBP.
__declspec(naked) void pad_update_entry()
{
    __asm {
        push esi
        call pad_update_c
        add esp, 4
        ret
    }
}

} // namespace

void pad_install_hooks()
{
    HOOK(0x002FB800, pad_update_buttons_entry);
    HOOK(0x002FBB80, pad_update_entry);
}

} // namespace game
