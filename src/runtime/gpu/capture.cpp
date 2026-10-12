#include "capture.h"
#include "gl.h"
#include "surface.h"
#include "../host/png.h"
#include "../log.h"
#include "../mmio.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
#include <windows.h>

namespace rt {

namespace {

std::vector<uint64_t> g_times;
uint64_t g_start_ms;
bool g_armed, g_active;
int g_index, g_draw;
int g_probe_x = -1, g_probe_y = -1;
int g_probe2_x = -1, g_probe2_y = -1; // second probe, in render-target (scaled) pixels
int g_shot_lo = -1, g_shot_hi = -1, g_shot_step = 0;
FILE *g_log;
std::string g_dir;
std::set<uint64_t> g_programs;

void init_once()
{
    static bool done;
    if (done)
        return;
    done = true;
    g_start_ms = GetTickCount64();
    if (const char *e = getenv("SWBF2_CAPTURE_AT")) {
        for (const char *p = e; *p;) {
            g_times.push_back(strtoull(p, const_cast<char **>(&p), 10));
            if (*p == ',') p++;
            else break;
        }
    }
    if (const char *e = getenv("SWBF2_CAPTURE_PROBE"))
        sscanf(e, "%d,%d", &g_probe_x, &g_probe_y);
    if (const char *e = getenv("SWBF2_CAPTURE_PROBE_PX"))
        sscanf(e, "%d,%d", &g_probe2_x, &g_probe2_y);
    if (const char *e = getenv("SWBF2_CAPTURE_SHOTS")) {
        if (sscanf(e, "%d-%d", &g_shot_lo, &g_shot_hi) != 2) {
            g_shot_lo = g_shot_hi = -1;
            g_shot_step = atoi(e);
        }
    }
}

} // namespace

void capture_frame_boundary()
{
    init_once();
    if (g_active) {
        fclose(g_log);
        g_log = nullptr;
        g_active = false;
        LOG_INFO("capture", "frame capture %d done: %d draws in %s", g_index, g_draw, g_dir.c_str());
        g_index++;
    }
    if (!g_times.empty() && GetTickCount64() - g_start_ms >= g_times.front()) {
        g_times.erase(g_times.begin());
        char dir[64];
        snprintf(dir, sizeof dir, "capture_%u_%d", unsigned(GetCurrentProcessId()), g_index);
        g_dir = dir;
        CreateDirectoryA(dir, nullptr);
        g_log = fopen((g_dir + "\\draws.txt").c_str(), "w");
        g_active = g_log != nullptr;
        g_draw = 0;
        g_programs.clear();
        LOG_INFO("capture", "capturing a frame into %s", dir);
        if (const char *e = getenv("SWBF2_WATCH_PHYS")) {
            unsigned lo = 0, len = 0x1000;
            sscanf(e, "%x:%x", &lo, &len);
            debug_watch_phys(lo, len);
        }
        // SWBF2_CAPTURE_PEEK=addr,...: log game texture globals (wrapper -> +0x20 D3DTexture -> +4 data).
        if (const char *e = getenv("SWBF2_CAPTURE_PEEK")) {
            for (const char *p = e; *p;) {
                uint32_t g = strtoul(p, const_cast<char **>(&p), 16);
                uint32_t v = *reinterpret_cast<uint32_t *>(uintptr_t(g)), tex = 0, data = 0;
                if (v && !IsBadReadPtr(reinterpret_cast<void *>(uintptr_t(v + 0x20)), 4))
                    tex = *reinterpret_cast<uint32_t *>(uintptr_t(v + 0x20));
                if (tex && !IsBadReadPtr(reinterpret_cast<void *>(uintptr_t(tex + 4)), 4))
                    data = *reinterpret_cast<uint32_t *>(uintptr_t(tex + 4));
                fprintf(g_log, "peek %08X = %08X tex %08X data %08X\n", g, v, tex, data);
                if (*p == ',') p++;
                else break;
            }
        }
    }
}

bool capture_active() { return g_active; }

void capture_program(uint64_t key, const std::string &vs, const std::string &fs)
{
    if (!g_active || !g_programs.insert(key).second)
        return;
    char path[128];
    snprintf(path, sizeof path, "%s\\prog_%016llX.glsl", g_dir.c_str(), (unsigned long long)key);
    if (FILE *f = fopen(path, "w")) {
        fprintf(f, "// ---- vertex ----\n%s\n// ---- fragment ----\n%s\n", vs.c_str(), fs.c_str());
        fclose(f);
    }
}

void capture_draw(const std::string &desc, Surface *color)
{
    if (!g_active)
        return;
    int n = g_draw++;
    fprintf(g_log, "#%d %s", n, desc.c_str());
    if (color && g_probe_x >= 0 && g_probe_x < int(color->width) && g_probe_y < int(color->height)) {
        uint8_t px[4];
        glReadPixels(int(g_probe_x * surface_scale_x()), int(g_probe_y * surface_scale_y()), 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, px);
        fprintf(g_log, " probe=%02X%02X%02X%02X", px[0], px[1], px[2], px[3]);
    }
    if (color && g_probe2_x >= 0 && g_probe2_x < int(color->pw) && g_probe2_y < int(color->ph)) {
        uint8_t px[4];
        glReadPixels(g_probe2_x, g_probe2_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        fprintf(g_log, " probe2=%02X%02X%02X%02X", px[0], px[1], px[2], px[3]);
        uint32_t zs = 0xDEADBEEF; // unchanged when the framebuffer has no depth/stencil attachment
        glReadPixels(g_probe2_x, g_probe2_y, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, &zs);
        while (glGetError() != GL_NO_ERROR) {}
        if (zs != 0xDEADBEEF)
            fprintf(g_log, " z2=%06X", zs >> 8);
    }
    fprintf(g_log, "\n");
    bool shot = (g_shot_step > 0 && n % g_shot_step == 0) || (n >= g_shot_lo && n <= g_shot_hi);
    if (shot && color) {
        std::vector<uint8_t> px(size_t(color->pw) * color->ph * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, color->pw, color->ph, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (size_t i = 3; i < px.size(); i += 4)
            px[i] = 255;
        char path[128];
        snprintf(path, sizeof path, "%s\\draw_%05d.png", g_dir.c_str(), n);
        write_png(path, px.data(), color->pw, color->ph, color->pw * 4);
    }
}

} // namespace rt
