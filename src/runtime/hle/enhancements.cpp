// Optional changes beyond the original game (see options.h).
//
// --60fps: D3D flips no sooner than the presentation interval after the
// previous flip. The game asks for "two, or immediately if late" in missions
// (30 fps) and one in menus; its simulation is time based, so asking for 1
// throughout runs missions at 60 fps.
//
// --fullres: the game supports the console's 16:9 widescreen setting (it
// renders anamorphically into its 640x480 buffer and lays the HUD out for
// it). For any window wider than 4:3 that setting is reported as on, and the
// two values the game derives from it (the screen's height/width ratio and
// the horizontal scale of screen-space effects) are set to match the window
// exactly. The renderer then scales the 640x480 buffer to the window.
#include "hook.h"
#include "../log.h"
#include "../options.h"

#include <windows.h>
#include <cstring>

namespace rt {

namespace {

constexpr uint32_t D3D_PRESENTATION_INTERVAL = 0x00439CB4; // D3DRS_PRESENTATIONINTERVAL

// `mov dword ptr [addr], imm32` instructions (C7 05 addr imm) that set the
// aspect-dependent globals, with the immediate the original stores.
struct ImmPatch {
    uint32_t insn, global, original;
};
const ImmPatch k_aspect_ratio[] = {           // DAT_00b6beb8: screen height / width
    {0x002C0B46, 0x00B6BEB8, 0x3F100000},     // 0.5625 (16:9)
    {0x002C0B57, 0x00B6BEB8, 0x3F400000},     // 0.75 (4:3)
};
const ImmPatch k_effect_scale[] = {           // DAT_004ac520: x scale of screen-space effects
    {0x002C8DD6, 0x004AC520, 0x3F400000},     // 0.75 (16:9)
    {0x002C8DE2, 0x004AC520, 0x3F800000},     // 1.0 (4:3)
};

bool patch_imm(const ImmPatch &p, float value)
{
    const uint8_t *b = reinterpret_cast<const uint8_t *>(uintptr_t(p.insn));
    uint32_t global, imm;
    memcpy(&global, b + 2, 4);
    memcpy(&imm, b + 6, 4);
    if (b[0] != 0xC7 || b[1] != 0x05 || global != p.global || imm != p.original) {
        LOG_WARN("enh", "unexpected code at %08X; aspect patch skipped", p.insn);
        return false;
    }
    patch_bytes(p.insn + 6, &value, 4);
    return true;
}

DWORD WINAPI interval_thread(void *)
{
    volatile uint32_t *interval = reinterpret_cast<volatile uint32_t *>(uintptr_t(D3D_PRESENTATION_INTERVAL));
    for (;;) {
        // Only read when a frame is presented, so rewriting it is race free.
        // INTERVAL_TWO -> ONE, TWO_OR_IMMEDIATE -> ONE_OR_IMMEDIATE (used in missions).
        uint32_t v = *interval;
        if ((v & 0x7FFFFFFF) == 2)
            *interval = (v & 0x80000000) | 1;
        Sleep(2);
    }
}

} // namespace

bool enh_widescreen()
{
    return g_options.fullres && g_options.render_width * 3 > g_options.render_height * 4 + 3;
}

void hle_install_enhancements()
{
    if (g_options.fps60) {
        CreateThread(nullptr, 0, interval_thread, nullptr, 0, nullptr);
        LOG_INFO("enh", "--60fps: presentation interval forced to 1");
    }
    if (g_options.fullres) {
        float hw = float(g_options.render_height) / float(g_options.render_width);
        bool ok = true;
        for (const ImmPatch &p : k_aspect_ratio)
            ok &= patch_imm(p, hw);
        for (const ImmPatch &p : k_effect_scale)
            ok &= patch_imm(p, hw * (4.0f / 3.0f));
        LOG_INFO("enh", "--fullres: aspect %.4f%s, widescreen mode %s", 1.0f / hw, ok ? "" : " (patch failed)",
                 enh_widescreen() ? "on" : "off");
    }
}

} // namespace rt
