// Command-line options that change how the game runs (beyond the original).
#pragma once

namespace rt {

struct Options {
    // --60fps: lift the in-game 30 fps cap (D3D presentation interval 2 -> 1).
    bool fps60 = false;
    // --fullres: render at the window's resolution and aspect ratio instead
    // of 640x480 4:3. The game's own widescreen mode is enabled for wide
    // windows and its aspect ratio is set to match the window.
    bool fullres = false;
    // --resolution WxH: window (and, with --fullres, render) size.
    int width = 0, height = 0;

    // Derived at startup (with --fullres): the render size, and the scale
    // from the game's 640x480 framebuffer space to it.
    int render_width = 640, render_height = 480;
    float scale_x = 1.0f, scale_y = 1.0f;
};

extern Options g_options;

} // namespace rt
