#pragma once
#include <cstdint>

struct SDL_Window;

namespace rt {

struct HostConfig {
    int window_scale = 2;
    int width = 0, height = 0; // explicit window size (0 = 640x480 * window_scale)
    bool fullscreen = false;
};

// Creates the window and GL context on the calling (main) thread.
void host_init(const HostConfig &cfg);
// Pumps SDL events forever on the main thread.
[[noreturn]] void host_run_event_loop();

SDL_Window *host_window();
// Wine's version string when running under Wine/Proton, otherwise null.
const char *host_wine_version();
void *host_gl_context();
void host_window_size(int &w, int &h);

// ---- gamepads ---------------------------------------------------------------
// Snapshot of one controller in Xbox terms.
struct PadState {
    bool connected;
    uint16_t buttons;        // XINPUT_GAMEPAD_* digital bits
    uint8_t analog[8];       // A, B, X, Y, Black, White, LTrigger, RTrigger
    int16_t lx, ly, rx, ry;  // thumbsticks, +Y is up
};
constexpr int MAX_PADS = 4;
PadState host_pad_state(int port);
void host_pad_rumble(int port, uint16_t left, uint16_t right);
// Incremented whenever a pad is plugged in or removed.
uint32_t host_pad_generation();

// Debug/test helper: virtual pads that tap buttons on a
// timeline, e.g. "5000:start,8000:a" (milliseconds since start). A "pN."
// prefix targets port N ("9000:p2.start"); scripted ports replace real pads.
void host_set_input_script(const char *script);

} // namespace rt
