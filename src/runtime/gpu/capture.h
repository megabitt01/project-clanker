// Single-frame capture for debugging the renderer.
//
//   SWBF2_CAPTURE_AT=ms[,ms...]   capture the frame that starts after each time
//   SWBF2_CAPTURE_PROBE=x,y       log the render-target colour at this pixel after each draw
//   SWBF2_CAPTURE_PROBE_PX=x,y    a second probe in render-target pixels (useful with --fullres)
//   SWBF2_CAPTURE_SHOTS=a-b|N     write the render target to PNG after draws a..b (or every Nth)
//   SWBF2_CAPTURE_PEEK=addr,...   log game texture globals (wrapper -> D3D texture -> data address)
//   SWBF2_WATCH_PHYS=addr:len     log the first CPU write to a physical range once capturing starts
//
// Related debug switches: SWBF2_DEBUG_NOZ (no depth/stencil/cull/scissor, to
// see a draw's full coverage) and SWBF2_DEBUG_MAGENTA (clear 640-wide colour
// surfaces to magenta at each flip, to find pixels nothing draws).
//
// Output goes to capture_<n>/ next to the log: draws.txt (state of every
// draw), shaders (one file per program) and the PNGs.
#pragma once
#include <cstdint>
#include <string>

namespace rt {

struct Surface;

void capture_frame_boundary();
bool capture_active();
// Saves a program's sources the first time it is used during a capture.
void capture_program(uint64_t key, const std::string &vs, const std::string &fs);
// Records one draw (call after it was issued, with its target bound).
void capture_draw(const std::string &desc, Surface *color);

} // namespace rt
