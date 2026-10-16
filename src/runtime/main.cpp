// Runtime entry point, called by swbf2.exe (src/loader/loader.c).
#include "crash.h"
#include "gpu/nv2a.h"
#include "gpu/renderer.h"
#include "hle/hook.h"
#include "host/host.h"
#include "kernel/kexports.h"
#include "kernel/sync.h"
#include "kernel/thread.h"
#include "log.h"
#include "memory.h"
#include "mmio.h"
#include "options.h"
#include "xbe.h"

#include <windows.h>
#include <shellapi.h>
#include <SDL.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace rt;

Options rt::g_options;

static std::string exe_dir()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s = path;
    return s.substr(0, s.find_last_of("\\/"));
}

static std::string arg_value(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return {};
}

static bool has_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

// The XBE entry point is `void entry(void)`; run it like the kernel's
// first title thread would.
static void __stdcall xbe_entry_routine(void *entry, void *)
{
    reinterpret_cast<void(__cdecl *)()>(entry)();
}

extern "C" __declspec(dllexport) void __cdecl swbf2_runtime_main(void *space, unsigned size)
{
    SDL_SetMainReady();
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    std::vector<char *> argv;
    for (int i = 0; i < argc; i++) {
        char buf[1024];
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, sizeof buf, nullptr, nullptr);
        args.push_back(buf);
    }
    for (auto &a : args)
        argv.push_back(a.data());

    std::string dir = exe_dir();
    log_init((dir + "\\swbf2.log").c_str());
    if (has_flag(argc, argv.data(), "--debug"))
        log_set_level(LogLevel::Debug);
    if (has_flag(argc, argv.data(), "--trace"))
        log_set_level(LogLevel::Trace);
    LOG_INFO("main", "Star Wars Battlefront II native runtime starting");

    // Game files: --rom, $SWBF2_ROM, a "rom" folder next to swbf2.exe (for a
    // copied build, e.g. on Linux with Proton), then the build's src/rom.
    std::string rom = arg_value(argc, argv.data(), "--rom");
    if (rom.empty()) {
        if (const char *env = getenv("SWBF2_ROM"))
            rom = env;
        else if (GetFileAttributesA((dir + "\\rom\\default.xbe").c_str()) != INVALID_FILE_ATTRIBUTES)
            rom = dir + "\\rom";
        else
            rom = SWBF2_DEFAULT_ROM_DIR;
    }
    std::string userdata = arg_value(argc, argv.data(), "--userdata");
    if (userdata.empty())
        userdata = dir + "\\userdata";

    exceptions_init();
    symbols_load((dir + "\\swbf2.sym").c_str());

    kthread_init(); // first: its TLS slot must be < 64 (GL drivers allocate many)

    // The window and GL context are created up front, on the main thread.
    g_options.fps60 = has_flag(argc, argv.data(), "--60fps");
    g_options.fullres = has_flag(argc, argv.data(), "--fullres");
    {
        std::string res = arg_value(argc, argv.data(), "--resolution");
        if (!res.empty() && sscanf(res.c_str(), "%dx%d", &g_options.width, &g_options.height) != 2)
            g_options.width = g_options.height = 0;
    }
    HostConfig hc;
    hc.fullscreen = has_flag(argc, argv.data(), "--fullscreen");
    hc.width = g_options.width;
    hc.height = g_options.height;
    if (g_options.fullres && !hc.width) {
        hc.width = 1280; // a 16:9 window by default
        hc.height = 720;
    }
    host_init(hc);
    if (g_options.fullres) {
        int w = g_options.width, h = g_options.height;
        if (!w || hc.fullscreen)
            host_window_size(w, h);
        g_options.render_width = w;
        g_options.render_height = h;
        g_options.scale_x = w / 640.0f;
        g_options.scale_y = h / 480.0f;
        LOG_INFO("main", "--fullres: rendering at %dx%d", w, h);
    }
    {
        std::string script = arg_value(argc, argv.data(), "--press");
        if (!script.empty())
            host_set_input_script(script.c_str());
    }
    mem_init(space, size);

    XbeInfo xbe;
    if (!xbe_load((rom + "\\default.xbe").c_str(), xbe))
        fatal("Could not load %s\\default.xbe.\nPlace your copy of the game in src/rom (see src/rom/README.md) "
              "or pass --rom <dir>.",
              rom.c_str());
    fs_patch_xbe();
    hal_init();
    io_init(rom.c_str(), userdata.c_str());
    kernel_resolve_imports(xbe.kernel_thunk);
    hle_install_all();

    if (has_flag(argc, argv.data(), "--shots"))
        renderer_enable_screenshots((dir + "\\shots").c_str(), 3.0);
    nv2a_init();
    sync_init();

    if (const char *dump = getenv("SWBF2_DUMP_AT")) {
        // Debug aid: dump every thread's stack at the given times (ms, comma-separated).
        static std::string times = dump;
        CreateThread(nullptr, 0, [](void *) -> DWORD {
            DWORD start = GetTickCount();
            const char *p = times.c_str();
            while (*p) {
                DWORD at = DWORD(atoi(p));
                while (GetTickCount() - start < at) Sleep(10);
                dump_all_threads("SWBF2_DUMP_AT");
                while (*p && *p != ',') p++;
                if (*p) p++;
            }
            return 0;
        }, nullptr, 0, nullptr);
    }
    if (const char *prof = getenv("SWBF2_PROFILE_AT")) {
        // Debug aid: "<start ms>:<duration ms>" sampling profile of Xbox threads.
        static uint32_t at = 0, dur = 1000;
        sscanf(prof, "%u:%u", &at, &dur);
        CreateThread(nullptr, 0, [](void *) -> DWORD {
            Sleep(at);
            profile_threads(dur);
            return 0;
        }, nullptr, 0, nullptr);
    }
    LOG_INFO("main", "starting title at %08X", xbe.entry_point);
    xt_create(xbe.stack_size ? xbe.stack_size : 0x10000, 0x20, reinterpret_cast<void *>(&xbe_entry_routine),
              reinterpret_cast<void *>(uintptr_t(xbe.entry_point)), nullptr, false, "xbe-entry");
    host_run_event_loop();
}
