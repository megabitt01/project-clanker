// Host platform layer: SDL2 window + OpenGL context, event pump and gamepads.
#include "host.h"
#include "../log.h"
#include "../crash.h"

#include <windows.h>
#include <SDL.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

static SDL_Window *g_window;
static SDL_GLContext g_gl;
static std::mutex g_pad_mutex;
static PadState g_pads[MAX_PADS];
static SDL_GameController *g_controllers[MAX_PADS];
static std::atomic<uint32_t> g_pad_generation{1};

struct ScriptedPress {
    uint32_t at_ms;
    uint16_t buttons;
    int analog;  // index into PadState::analog, or -1
    int stick;   // 0 none, 1 lx, 2 ly, 3 rx, 4 ry (held for 1 s)
    int16_t value;
};
static std::vector<ScriptedPress> g_script;
static bool g_script_active;
static uint64_t g_script_start;

SDL_Window *host_window() { return g_window; }
void *host_gl_context() { return g_gl; }

void host_window_size(int &w, int &h)
{
    SDL_GL_GetDrawableSize(g_window, &w, &h);
}

void host_init(const HostConfig &cfg)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_HAPTIC) != 0)
        fatal("SDL_Init failed: %s", SDL_GetError());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_window = SDL_CreateWindow("Star Wars Battlefront II", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                cfg.width ? cfg.width : 640 * cfg.window_scale,
                                cfg.height ? cfg.height : 480 * cfg.window_scale, flags);
    if (!g_window)
        fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        g_gl = SDL_GL_CreateContext(g_window);
    }
    if (!g_gl)
        fatal("Could not create an OpenGL 3.3+ context: %s", SDL_GetError());
    // The GPU thread owns the context from here on.
    SDL_GL_MakeCurrent(g_window, nullptr);
    {
        int ww, wh;
        SDL_GetWindowSize(g_window, &ww, &wh);
        LOG_INFO("host", "window %dx%d created", ww, wh);
    }
}

// ---- gamepads ---------------------------------------------------------------------
enum : uint16_t {
    XPAD_DPAD_UP = 0x0001,
    XPAD_DPAD_DOWN = 0x0002,
    XPAD_DPAD_LEFT = 0x0004,
    XPAD_DPAD_RIGHT = 0x0008,
    XPAD_START = 0x0010,
    XPAD_BACK = 0x0020,
    XPAD_LEFT_THUMB = 0x0040,
    XPAD_RIGHT_THUMB = 0x0080,
};

static void open_controller(int device_index)
{
    if (!SDL_IsGameController(device_index))
        return;
    for (int port = 0; port < MAX_PADS; port++) {
        if (g_controllers[port])
            continue;
        g_controllers[port] = SDL_GameControllerOpen(device_index);
        if (g_controllers[port]) {
            LOG_INFO("input", "controller '%s' -> port %d", SDL_GameControllerName(g_controllers[port]), port + 1);
            g_pad_generation++;
        }
        return;
    }
}

static void close_controller(SDL_JoystickID id)
{
    for (int port = 0; port < MAX_PADS; port++) {
        SDL_GameController *c = g_controllers[port];
        if (c && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) {
            SDL_GameControllerClose(c);
            g_controllers[port] = nullptr;
            LOG_INFO("input", "controller on port %d removed", port + 1);
            g_pad_generation++;
        }
    }
}

static uint8_t axis_to_trigger(Sint16 v)
{
    return uint8_t(v <= 0 ? 0 : (v * 255) / 32767);
}

static int16_t flip_y(Sint16 v)
{
    return v == -32768 ? 32767 : int16_t(-v);
}

void host_set_input_script(const char *script)
{
    static const struct { const char *name; uint16_t bit; int analog; } names[] = {
        {"up", 0x0001, -1}, {"down", 0x0002, -1}, {"left", 0x0004, -1}, {"right", 0x0008, -1},
        {"start", 0x0010, -1}, {"back", 0x0020, -1}, {"lthumb", 0x0040, -1}, {"rthumb", 0x0080, -1},
        {"a", 0, 0}, {"b", 0, 1}, {"x", 0, 2}, {"y", 0, 3}, {"black", 0, 4}, {"white", 0, 5}, {"lt", 0, 6}, {"rt", 0, 7},
    };
    std::string s = script;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            uint32_t at = uint32_t(atoi(item.c_str()));
            std::string b = item.substr(colon + 1);
            for (auto &n : names)
                if (b == n.name)
                    g_script.push_back({at, n.bit, n.analog, 0, 0});
            static const char *sticks[] = {"lx", "ly", "rx", "ry"};
            for (int k = 0; k < 4; k++)
                if (b.size() == 3 && b.compare(0, 2, sticks[k]) == 0)
                    g_script.push_back({at, 0, -1, k + 1, int16_t(b[2] == '-' ? -32767 : 32767)});
        }
        pos = end + 1;
    }
    g_script_active = !g_script.empty();
    g_script_start = SDL_GetTicks64();
    LOG_INFO("input", "input script with %zu presses", g_script.size());
}

static void update_pads()
{
    PadState snap[MAX_PADS] = {};
    if (g_script_active) {
        PadState &p = snap[0];
        p.connected = true;
        uint64_t now = SDL_GetTicks64() - g_script_start;
        for (const ScriptedPress &sp : g_script) {
            if (sp.stick && now >= sp.at_ms && now < sp.at_ms + 1000) {
                int16_t *axes[] = {&p.lx, &p.ly, &p.rx, &p.ry};
                *axes[sp.stick - 1] = sp.value;
            }
            if (!sp.stick && now >= sp.at_ms && now < sp.at_ms + 150) {
                p.buttons |= sp.buttons;
                if (sp.analog >= 0)
                    p.analog[sp.analog] = 255;
            }
        }
    }
    for (int port = g_script_active ? 1 : 0; port < MAX_PADS; port++) {
        SDL_GameController *c = g_controllers[port];
        if (!c)
            continue;
        PadState &p = snap[port];
        p.connected = true;
        auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) p.buttons |= XPAD_DPAD_UP;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p.buttons |= XPAD_DPAD_DOWN;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) p.buttons |= XPAD_DPAD_LEFT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p.buttons |= XPAD_DPAD_RIGHT;
        if (btn(SDL_CONTROLLER_BUTTON_START)) p.buttons |= XPAD_START;
        if (btn(SDL_CONTROLLER_BUTTON_BACK)) p.buttons |= XPAD_BACK;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK)) p.buttons |= XPAD_LEFT_THUMB;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) p.buttons |= XPAD_RIGHT_THUMB;
        // Analog face buttons are digital on modern pads.
        p.analog[0] = btn(SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
        p.analog[1] = btn(SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
        p.analog[2] = btn(SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
        p.analog[3] = btn(SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
        p.analog[4] = btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0; // Black
        p.analog[5] = btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;  // White
        p.analog[6] = axis_to_trigger(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        p.analog[7] = axis_to_trigger(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        p.lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        p.ly = flip_y(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
        p.rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        p.ry = flip_y(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    }
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    memcpy(g_pads, snap, sizeof snap);
}

PadState host_pad_state(int port)
{
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    return port >= 0 && port < MAX_PADS ? g_pads[port] : PadState{};
}

uint32_t host_pad_generation() { return g_pad_generation; }

static uint16_t g_rumble[MAX_PADS][2];
static bool g_rumble_dirty[MAX_PADS];

void host_pad_rumble(int port, uint16_t left, uint16_t right)
{
    if (port < 0 || port >= MAX_PADS)
        return;
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    g_rumble[port][0] = left;
    g_rumble[port][1] = right;
    g_rumble_dirty[port] = true;
}

static void apply_rumble()
{
    for (int port = 0; port < MAX_PADS; port++) {
        uint16_t l, r;
        {
            std::lock_guard<std::mutex> lock(g_pad_mutex);
            if (!g_rumble_dirty[port])
                continue;
            g_rumble_dirty[port] = false;
            l = g_rumble[port][0];
            r = g_rumble[port][1];
        }
        if (g_controllers[port])
            SDL_GameControllerRumble(g_controllers[port], l, r, (l || r) ? 10000 : 0);
    }
}

void host_run_event_loop()
{
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        open_controller(i);
    for (;;) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 2)) {
            switch (e.type) {
            case SDL_QUIT:
                LOG_INFO("host", "window closed");
                log_flush();
                TerminateProcess(GetCurrentProcess(), 0);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                open_controller(e.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_controller(e.cdevice.which);
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_F12)
                    dump_all_threads("F12");
                if (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                    bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            default:
                break;
            }
        }
        update_pads();
        apply_rumble();
    }
}

} // namespace rt
