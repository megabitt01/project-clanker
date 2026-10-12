#pragma once
#include <cstdint>

namespace rt {

void draw_init();
// Executes the batch collected in g_kd (called at SET_BEGIN_END(END)).
void draw_flush();
// NV097_CLEAR_SURFACE.
void draw_clear(uint32_t flags);
// Returns the GL texture holding the surface at `addr` (scan-out), or 0.
// Occlusion reports: CLEAR_REPORT_VALUE / GET_REPORT.
void draw_report_clear();
void draw_report_write(uint32_t parameter);
// Writes queued reports to memory; called wherever the CPU can observe GPU progress.
void draw_reports_resolve();
unsigned draw_find_scanout(uint32_t addr, uint32_t &w, uint32_t &h);

} // namespace rt
