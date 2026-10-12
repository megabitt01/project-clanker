#include "surface.h"
#include "gl.h"
#include "kelvin.h"
#include "kelvin_state.h"
#include "nv2a_internal.h"
#include "../log.h"
#include "../options.h"

#include <algorithm>
#include <map>
#include <vector>

namespace rt {

static std::vector<Surface *> g_surfaces;
static std::map<std::pair<unsigned, unsigned>, GLuint> g_fbos;
static uint64_t g_frame = 1;

uint64_t surface_frame() { return g_frame; }
void surface_new_frame()
{
    g_frame++;
    static bool magenta = getenv("SWBF2_DEBUG_MAGENTA") != nullptr;
    if (magenta)
        for (Surface *s : g_surfaces)
            if (!s->depth && s->width == 640) {
                glBindFramebuffer(GL_FRAMEBUFFER, surface_fbo(s));
                glDisable(GL_SCISSOR_TEST);
                glColorMask(1, 1, 1, 1);
                glClearColor(1, 0, 1, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
}

static uint64_t sparse_hash(const Surface *s)
{
    uint32_t bpp = s->depth ? (s->format == 1 ? 2 : 4) : ((s->format >= 4 && s->format <= 8) ? 4 : 2);
    uint32_t bytes = s->swizzled ? s->width * s->height * bpp : s->pitch * s->height;
    if (!bytes || s->addr + bytes > XBOX_RAM_SIZE)
        return 0;
    const uint32_t *p = reinterpret_cast<const uint32_t *>(phys_ptr(s->addr));
    uint32_t words = bytes / 4, step = words / 1024 ? words / 1024 : 1;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < words; i += step)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void surface_mark_written(Surface *s)
{
    if (s) {
        s->last_write_frame = g_frame;
        s->mem_sig = sparse_hash(s);
        s->write_gen++;
    }
}

bool surface_is_stale(Surface *s)
{
    return s->mem_sig != sparse_hash(s);
}

static void destroy(Surface *s)
{
    for (auto it = g_fbos.begin(); it != g_fbos.end();) {
        if (it->first.first == s->tex || it->first.second == s->tex) {
            glDeleteFramebuffers(1, &it->second);
            it = g_fbos.erase(it);
        } else {
            ++it;
        }
    }
    glDeleteTextures(1, &s->tex);
    delete s;
}

float surface_scale_x() { return g_options.scale_x; }
float surface_scale_y() { return g_options.scale_y; }

static uint32_t scaled(uint32_t v, float scale)
{
    uint32_t r = uint32_t(float(v) * scale + 0.5f);
    return r ? r : 1;
}

static void set_size(Surface *s, uint32_t w, uint32_t h)
{
    s->width = w;
    s->height = h;
    s->pw = scaled(w, g_options.scale_x);
    s->ph = scaled(h, g_options.scale_y);
}

void surface_scissor(int x0, int y0, int x1, int y1)
{
    float sx = g_options.scale_x, sy = g_options.scale_y;
    int px0 = int(x0 * sx + 0.5f), py0 = int(y0 * sy + 0.5f);
    int px1 = int((x1 + 1) * sx + 0.5f), py1 = int((y1 + 1) * sy + 0.5f);
    glScissor(px0, py0, px1 - px0, py1 - py0);
}

static GLuint g_xfer_tex, g_xfer_fbo;
static uint32_t g_xfer_w, g_xfer_h;

// A scratch RGBA8 texture/FBO at game resolution for resampled transfers.
static void xfer_target(uint32_t w, uint32_t h)
{
    if (!g_xfer_tex) {
        glGenTextures(1, &g_xfer_tex);
        glGenFramebuffers(1, &g_xfer_fbo);
    }
    if (w > g_xfer_w || h > g_xfer_h) {
        g_xfer_w = std::max(w, g_xfer_w);
        g_xfer_h = std::max(h, g_xfer_h);
        glBindTexture(GL_TEXTURE_2D, g_xfer_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_xfer_w, g_xfer_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_xfer_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_xfer_tex, 0);
    }
}

static bool is_scaled() { return g_options.scale_x != 1.0f || g_options.scale_y != 1.0f; }

void surface_read(Surface *s, int x, int y, int w, int h, unsigned format, unsigned type, void *dst, int row_length)
{
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    if (!is_scaled()) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, surface_fbo(s));
        glReadPixels(x, y, w, h, format, type, dst);
    } else {
        xfer_target(w, h);
        float sx = g_options.scale_x, sy = g_options.scale_y;
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, surface_fbo(s));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_xfer_fbo);
        glBlitFramebuffer(int(x * sx + 0.5f), int(y * sy + 0.5f), int((x + w) * sx + 0.5f), int((y + h) * sy + 0.5f), 0,
                          0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_xfer_fbo);
        glReadPixels(0, 0, w, h, format, type, dst);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, surface_fbo(s));
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

void surface_write(Surface *s, int x, int y, int w, int h, unsigned format, unsigned type, const void *src,
                   int row_length)
{
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    if (!is_scaled()) {
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, type, src);
    } else {
        xfer_target(w, h);
        glBindTexture(GL_TEXTURE_2D, g_xfer_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, src);
        float sx = g_options.scale_x, sy = g_options.scale_y;
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_xfer_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, surface_fbo(s));
        glBlitFramebuffer(0, 0, w, h, int(x * sx + 0.5f), int(y * sy + 0.5f), int((x + w) * sx + 0.5f),
                          int((y + h) * sy + 0.5f), GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static GLuint alloc_texture(uint32_t w, uint32_t h, uint32_t fmt, bool depth)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (depth) {
        if (fmt == 1)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return tex;
}

static void drop_fbos(GLuint tex)
{
    for (auto it = g_fbos.begin(); it != g_fbos.end();) {
        if (it->first.first == tex || it->first.second == tex) {
            glDeleteFramebuffers(1, &it->second);
            it = g_fbos.erase(it);
        } else {
            ++it;
        }
    }
}

// On the NV2A a surface is just memory plus a pitch; the clip rectangle only
// bounds rendering. Surfaces are therefore identified by address and format,
// and the backing texture grows to the largest extent ever rendered.
static Surface *get_surface(uint32_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt, bool depth,
                            bool swizzled)
{
    for (size_t i = 0; i < g_surfaces.size(); i++) {
        Surface *s = g_surfaces[i];
        if (s->addr != addr || s->depth != depth)
            continue;
        if (s->format == fmt && s->swizzled == swizzled) {
            s->pitch = pitch;
            if (w <= s->width && h <= s->height)
                return s;
            // Grow, preserving the existing contents.
            uint32_t nw = w > s->width ? w : s->width, nh = h > s->height ? h : s->height;
            uint32_t opw = s->pw, oph = s->ph;
            set_size(s, nw, nh);
            GLuint nt = alloc_texture(s->pw, s->ph, fmt, depth);
            if (rt_glCopyImageSubData)
                glCopyImageSubData(s->tex, GL_TEXTURE_2D, 0, 0, 0, 0, nt, GL_TEXTURE_2D, 0, 0, 0, 0, opw, oph, 1);
            drop_fbos(s->tex);
            glDeleteTextures(1, &s->tex);
            s->tex = nt;
            LOG_DEBUG("surface", "grew %s surface %08X to %ux%u", depth ? "zeta" : "color", addr, nw, nh);
            return s;
        }
        // Same memory reinterpreted with another format: start over.
        destroy(s);
        g_surfaces.erase(g_surfaces.begin() + i);
        break;
    }
    Surface *s = new Surface();
    s->addr = addr;
    s->pitch = pitch;
    set_size(s, w, h);
    s->format = fmt;
    s->depth = depth;
    s->swizzled = swizzled;
    s->tex = alloc_texture(s->pw, s->ph, fmt, depth);
    g_surfaces.push_back(s);
    LOG_DEBUG("surface", "new %s surface %08X %ux%u fmt %u pitch %u%s", depth ? "zeta" : "color", addr, w, h, fmt,
              pitch, swizzled ? " swizzled" : "");
    return s;
}

static void surface_dims(uint32_t &w, uint32_t &h, bool &swizzled)
{
    uint32_t fmt = kr(NV097_SET_SURFACE_FORMAT);
    swizzled = ((fmt >> 8) & 0xF) == 2;
    if (swizzled) {
        w = 1u << ((fmt >> 16) & 0xFF);
        h = 1u << ((fmt >> 24) & 0xFF);
    } else {
        uint32_t ch = kr(NV097_SET_SURFACE_CLIP_HORIZONTAL), cv = kr(NV097_SET_SURFACE_CLIP_VERTICAL);
        w = (ch & 0xFFFF) + (ch >> 16);
        h = (cv & 0xFFFF) + (cv >> 16);
        // Anti-aliased surfaces: the clip is in pixels, memory holds samples.
        uint32_t aa = (fmt >> 12) & 0xF;
        if (aa == 1 || aa == 2) w *= 2;
        if (aa == 2) h *= 2;
    }
    if (w == 0) w = 640;
    if (h == 0) h = 480;
}

Surface *surface_color_current()
{
    uint32_t fmt = kr(NV097_SET_SURFACE_FORMAT) & 0xF;
    uint32_t pitch = kr(NV097_SET_SURFACE_PITCH) & 0xFFFF;
    if (!fmt || !kr(NV097_SET_CONTEXT_DMA_COLOR))
        return nullptr;
    // Colour writes fully masked: treat as no colour target (depth-only pass).
    DmaObject dma = nv_dma_load(kr(NV097_SET_CONTEXT_DMA_COLOR));
    uint32_t addr = (dma.address + kr(NV097_SET_SURFACE_COLOR_OFFSET)) & 0x07FFFFFF;
    uint32_t w, h;
    bool swz;
    surface_dims(w, h, swz);
    uint32_t bpp = (fmt >= 4 && fmt <= 8) ? 4 : (fmt == 9 ? 1 : 2);
    if (!swz && pitch / bpp > w && pitch / bpp <= 4096)
        w = pitch / bpp;
    return get_surface(addr, pitch, w, h, fmt, false, swz);
}

Surface *surface_zeta_current()
{
    uint32_t zfmt = (kr(NV097_SET_SURFACE_FORMAT) >> 4) & 0xF;
    uint32_t pitch = kr(NV097_SET_SURFACE_PITCH) >> 16;
    if (!zfmt || !kr(NV097_SET_CONTEXT_DMA_ZETA) || !pitch)
        return nullptr;
    DmaObject dma = nv_dma_load(kr(NV097_SET_CONTEXT_DMA_ZETA));
    uint32_t addr = (dma.address + kr(NV097_SET_SURFACE_ZETA_OFFSET)) & 0x07FFFFFF;
    uint32_t w, h;
    bool swz;
    surface_dims(w, h, swz);
    uint32_t bpp = zfmt == 1 ? 2 : 4;
    if (!swz && pitch / bpp > w && pitch / bpp <= 4096)
        w = pitch / bpp;
    return get_surface(addr, pitch, w, h, zfmt, true, swz);
}

bool surface_bind(Surface *color, Surface *zeta)
{
    if (!color && !zeta)
        return false;
    // GL requires matching attachment sizes: drop a mismatched zeta buffer.
    if (color && zeta && (color->pw != zeta->pw || color->ph != zeta->ph))
        zeta = nullptr;
    auto key = std::make_pair(color ? color->tex : 0u, zeta ? zeta->tex : 0u);
    auto it = g_fbos.find(key);
    GLuint fbo;
    if (it == g_fbos.end()) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (color) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color->tex, 0);
            GLenum db = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &db);
        } else {
            GLenum none = GL_NONE;
            glDrawBuffers(1, &none);
        }
        if (zeta)
            glFramebufferTexture2D(GL_FRAMEBUFFER, zeta->format == 1 ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, zeta->tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE)
            LOG_WARN("surface", "framebuffer incomplete (0x%X)", st);
        g_fbos[key] = fbo;
    } else {
        fbo = it->second;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    Surface *s = color ? color : zeta;
    glViewport(0, 0, s->pw, s->ph);
    return true;
}

Surface *surface_find(uint32_t addr, bool want_depth)
{
    addr &= 0x07FFFFFF;
    Surface *best = nullptr;
    for (Surface *s : g_surfaces)
        if (s->addr == addr && s->depth == want_depth && (!best || s->last_write_frame > best->last_write_frame))
            best = s;
    return best;
}

} // namespace rt

namespace rt {

void surface_discard(Surface *s)
{
    for (size_t i = 0; i < g_surfaces.size(); i++) {
        if (g_surfaces[i] == s) {
            g_surfaces.erase(g_surfaces.begin() + i);
            LOG_DEBUG("surface", "discarding stale surface %08X (overwritten by CPU)", s->addr);
            destroy(s);
            return;
        }
    }
}

Surface *surface_get_color(uint32_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt)
{
    return get_surface(addr & 0x07FFFFFF, pitch, w, h, fmt, false, false);
}

unsigned surface_fbo(Surface *s)
{
    auto key = s->depth ? std::make_pair(0u, s->tex) : std::make_pair(s->tex, 0u);
    auto it = g_fbos.find(key);
    if (it != g_fbos.end())
        return it->second;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (s->depth)
        glFramebufferTexture2D(GL_FRAMEBUFFER, s->format == 1 ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, s->tex, 0);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    g_fbos[key] = fbo;
    return fbo;
}

Surface *surface_containing(uint32_t addr, uint32_t &x, uint32_t &y)
{
    addr &= 0x07FFFFFF;
    for (Surface *s : g_surfaces) {
        if (s->depth || s->swizzled || !s->pitch || addr < s->addr)
            continue;
        uint32_t off = addr - s->addr;
        uint32_t bpp = (s->format >= 4 && s->format <= 8) ? 4 : 2;
        if (off >= s->pitch * s->height)
            continue;
        y = off / s->pitch;
        x = (off % s->pitch) / bpp;
        if (x < s->width)
            return s;
    }
    return nullptr;
}

} // namespace rt
