// Game controller object (one per port), as laid out by the game.
#pragma once
#include <cstddef>
#include <cstdint>

namespace game {

#pragma pack(push, 1)
struct XInputGamepad {
    uint16_t buttons;          // XINPUT_GAMEPAD_* digital bits
    uint8_t analog[8];         // A, B, X, Y, Black, White, LT, RT
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};

struct XInputState {
    uint32_t packet;
    XInputGamepad gamepad;
};

struct GamePad {
    uint8_t unk00[4];
    uint8_t port;              // 0x004
    uint8_t opened;            // 0x005
    uint8_t unk06[2];
    float stick[4];            // 0x008 left x/y, right x/y, normalised and dead-zoned
    // 0x018: per stick axis, a pair of "pinned" trackers. [0] follows the
    // axis minimum and, once the axis sits at -1, keeps sliding below -1 by
    // 0.01 per frame; [1] mirrors that for +1. Values past +/-1.04 mean the
    // stick has been held fully over for several frames, which narrows the
    // dead zone of the other axis.
    float pinned[4][2];
    float digital[8];          // 0x038 mapped digital button bits 0..7 as 0/1
    float analog[8];           // 0x058 White, Black, LT, RT, Y, B, A, X in 0..1
    uint8_t unk78[0x40];
    int32_t rumble_target;     // 0x0B8
    int32_t rumble_base;       // 0x0BC
    uint32_t buttons;          // 0x0C0 mapped button bits (see pad.cpp)
    uint32_t pressed;          // 0x0C4 bits that went down this frame
    uint32_t released;         // 0x0C8 bits that went up this frame
    uint8_t feedback[0x42];    // 0x0CC XINPUT_FEEDBACK header (dwStatus first)
    uint16_t rumble_left;      // 0x10E
    uint16_t rumble_right;     // 0x110
    uint8_t capabilities[0x1A];// 0x112
    uint32_t device;           // 0x12C XInput handle
    XInputGamepad current;     // 0x130 copy of the latest state
    XInputState state;         // 0x142 XInputGetState destination
    uint8_t analog_ref[8];     // 0x158 hysteresis reference per analog button
};
#pragma pack(pop)

static_assert(offsetof(GamePad, stick) == 0x08);
static_assert(offsetof(GamePad, digital) == 0x38);
static_assert(offsetof(GamePad, analog) == 0x58);
static_assert(offsetof(GamePad, rumble_target) == 0xB8);
static_assert(offsetof(GamePad, buttons) == 0xC0);
static_assert(offsetof(GamePad, rumble_left) == 0x10E);
static_assert(offsetof(GamePad, device) == 0x12C);
static_assert(offsetof(GamePad, current) == 0x130);
static_assert(offsetof(GamePad, state) == 0x142);
static_assert(offsetof(GamePad, analog_ref) == 0x158);

// Mapped button bits in GamePad::buttons.
enum : uint32_t {
    PAD_BACK = 0x0001,
    PAD_LTHUMB = 0x0002,
    PAD_RTHUMB = 0x0004,
    PAD_START = 0x0008,
    PAD_DPAD_UP = 0x0010,
    PAD_DPAD_RIGHT = 0x0020,
    PAD_DPAD_DOWN = 0x0040,
    PAD_DPAD_LEFT = 0x0080,
    PAD_WHITE = 0x0100,
    PAD_BLACK = 0x0200,
    PAD_LTRIGGER = 0x0400,
    PAD_RTRIGGER = 0x0800,
    PAD_Y = 0x1000,
    PAD_B = 0x2000,
    PAD_A = 0x4000,
    PAD_X = 0x8000,
};

void pad_update_buttons(GamePad *pad);   // was FUN_002fb800
void pad_update(GamePad *pad);           // was FUN_002fbb80
void pad_install_hooks();

} // namespace game
