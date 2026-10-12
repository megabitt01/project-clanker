// Texture decoding and caching.
#include "texture.h"
#include "gl.h"
#include "kelvin.h"
#include "kelvin_state.h"
#include "nv2a_internal.h"
#include "capture.h"
#include "renderer.h"
#include "surface.h"
#include "../log.h"

#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

namespace {

enum Kind { K_RGBA8 = 0, K_DXT1, K_DXT3, K_DXT5 };

struct FormatInfo {
    uint8_t bpp;       // bytes per texel (0 = compressed)
    bool swizzled;
    bool linear;
    Kind kind;
};

FormatInfo format_info(uint32_t f)
{
    switch (f) {
    case 0x00: case 0x01: case 0x0B: case 0x19: return {1, true, false, K_RGBA8};
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x27: case 0x28: case 0x29: case 0x2C: case 0x2D:
        return {2, true, false, K_RGBA8};
    case 0x06: case 0x07: case 0x2A: case 0x2B: case 0x3A: case 0x3B: case 0x3C: return {4, true, false, K_RGBA8};
    case 0x0C: return {0, false, false, K_DXT1};
    case 0x0E: return {0, false, false, K_DXT3};
    case 0x0F: return {0, false, false, K_DXT5};
    case 0x13: case 0x14: case 0x1B: case 0x1F: return {1, false, true, K_RGBA8};
    case 0x10: case 0x11: case 0x15: case 0x16: case 0x17: case 0x18: case 0x1C: case 0x1D: case 0x20: case 0x24:
    case 0x25: case 0x30: case 0x31: return {2, false, true, K_RGBA8};
    case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x3F: case 0x40: case 0x41: return {4, false, true, K_RGBA8};
    default: return {0, false, false, K_RGBA8};
    }
}

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }
inline uint32_t e5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t e6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t e4(uint32_t v) { return v | (v << 4); }

// Converts one texel at p into RGBA8.
uint32_t decode_texel(uint32_t f, const uint8_t *p, const uint32_t *palette)
{
    uint32_t v8 = p[0], v16 = p[0] | (p[1] << 8), v32 = v16 | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    switch (f) {
    case 0x00: case 0x13: return rgba(v8, v8, v8, 255);                     // Y8
    case 0x01: case 0x1B: return rgba(v8, v8, v8, v8);                      // AY8
    case 0x19: case 0x1F: return rgba(255, 255, 255, v8);                   // A8
    case 0x0B: {                                                            // P8
        uint32_t c = palette ? palette[v8] : 0;
        return rgba((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24);
    }
    case 0x02: case 0x10:                                                   // A1R5G5B5
        return rgba(e5((v16 >> 10) & 31), e5((v16 >> 5) & 31), e5(v16 & 31), (v16 & 0x8000) ? 255 : 0);
    case 0x03: case 0x1C:                                                   // X1R5G5B5
        return rgba(e5((v16 >> 10) & 31), e5((v16 >> 5) & 31), e5(v16 & 31), 255);
    case 0x04: case 0x1D:                                                   // A4R4G4B4
        return rgba(e4((v16 >> 8) & 15), e4((v16 >> 4) & 15), e4(v16 & 15), e4(v16 >> 12));
    case 0x05: case 0x11:                                                   // R5G6B5
        return rgba(e5(v16 >> 11), e6((v16 >> 5) & 63), e5(v16 & 31), 255);
    case 0x27:                                                              // R6G5B5 (bump + luminance)
        return rgba(e6(v16 >> 10), e5((v16 >> 5) & 31), e5(v16 & 31), 255);
    case 0x1A: case 0x20: return rgba(p[0], p[0], p[0], p[1]);              // A8Y8
    case 0x28: case 0x17: return rgba(p[0], p[1], 255, 255);                // G8B8 (V8U8 bump: du, dv)
    case 0x29: case 0x16: return rgba(p[1], 255, p[0], 255);                // R8B8
    case 0x18: return rgba(p[0], p[1], 255, 255);                           // SG8SB8
    case 0x06: case 0x12: return rgba(p[2], p[1], p[0], p[3]);              // A8R8G8B8
    case 0x07: case 0x1E: return rgba(p[2], p[1], p[0], 255);               // X8R8G8B8
    case 0x3A: case 0x3F: return rgba(p[0], p[1], p[2], p[3]);              // A8B8G8R8
    case 0x3B: case 0x40: return rgba(p[1], p[2], p[3], p[0]);              // B8G8R8A8
    case 0x3C: case 0x41: return rgba(p[3], p[2], p[1], p[0]);              // R8G8B8A8
    case 0x2C: case 0x2D: case 0x30: case 0x31: {                           // depth Y16
        uint32_t d = v16 >> 8;
        return rgba(d, d, d, 255);
    }
    case 0x2A: case 0x2B: case 0x2E: case 0x2F: {                           // depth X8Y24
        uint32_t d = (v32 >> 24) & 255;
        return rgba(d, d, d, 255);
    }
    default: return rgba(255, 0, 255, 255);
    }
}

uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

struct CachedTexture {
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    uint64_t hash = 0;
    uint64_t checked_frame = 0;
    uint32_t levels = 1;
    float sx = 1, sy = 1;
};

struct Key {
    uint32_t addr, format, rect, pitch, palette;
    bool operator==(const Key &o) const
    {
        return addr == o.addr && format == o.format && rect == o.rect && pitch == o.pitch && palette == o.palette;
    }
};
struct KeyHash {
    size_t operator()(const Key &k) const
    {
        return size_t(k.addr * 2654435761u ^ k.format * 40503u ^ k.rect ^ (k.pitch << 7) ^ k.palette);
    }
};

std::unordered_map<Key, CachedTexture, KeyHash> g_cache;
std::unordered_set<uint32_t> g_unknown_logged;
uint64_t g_tex_frame = 1;

uint32_t dxt_block_bytes(Kind k) { return k == K_DXT1 ? 8 : 16; }

uint32_t level_size(const FormatInfo &fi, uint32_t w, uint32_t h)
{
    if (fi.kind != K_RGBA8)
        return ((w + 3) / 4) * ((h + 3) / 4) * dxt_block_bytes(fi.kind);
    return w * h * fi.bpp;
}

GLenum dxt_gl(Kind k)
{
    switch (k) {
    case K_DXT1: return 0x83F1; // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
    case K_DXT3: return 0x83F2;
    default: return 0x83F3;
    }
}

void upload_level(GLenum target, uint32_t level, uint32_t fmt, const FormatInfo &fi, const uint8_t *src, uint32_t w,
                  uint32_t h, uint32_t pitch, const uint32_t *palette, std::vector<uint32_t> &scratch)
{
    if (fi.kind != K_RGBA8) {
        glCompressedTexImage2D(target, level, dxt_gl(fi.kind), w, h, 0, level_size(fi, w, h), src);
        return;
    }
    scratch.resize(size_t(w) * h);
    if (fi.swizzled) {
        static std::vector<uint32_t> xo, yo;
        xo.resize(w);
        yo.resize(h);
        swizzle_tables(w, h, xo.data(), yo.data());
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                scratch[size_t(y) * w + x] = decode_texel(fmt, src + (xo[x] | yo[y]) * fi.bpp, palette);
    } else if (fmt == 0x24 || fmt == 0x25) {
        // YUY2 / UYVY: two pixels per 32-bit word.
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = src + size_t(y) * pitch;
            for (uint32_t x = 0; x + 1 < w + 1; x += 2) {
                const uint8_t *q = row + x * 2;
                int y0, y1, u, v;
                if (fmt == 0x24) { y0 = q[0]; u = q[1]; y1 = q[2]; v = q[3]; }
                else { u = q[0]; y0 = q[1]; v = q[2]; y1 = q[3]; }
                for (int k = 0; k < 2 && x + k < w; k++) {
                    int yy = (k ? y1 : y0) - 16, uu = u - 128, vv = v - 128;
                    auto clampi = [](int c) { return uint32_t(c < 0 ? 0 : c > 255 ? 255 : c); };
                    uint32_t r = clampi((298 * yy + 409 * vv + 128) >> 8);
                    uint32_t g = clampi((298 * yy - 100 * uu - 208 * vv + 128) >> 8);
                    uint32_t b = clampi((298 * yy + 516 * uu + 128) >> 8);
                    scratch[size_t(y) * w + x + k] = rgba(r, g, b, 255);
                }
            }
        }
    } else {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = src + size_t(y) * (pitch ? pitch : w * fi.bpp);
            for (uint32_t x = 0; x < w; x++)
                scratch[size_t(y) * w + x] = decode_texel(fmt, row + x * fi.bpp, palette);
        }
    }
    glTexImage2D(target, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
}

// One mip level of a swizzled volume texture.
void upload_level_3d(uint32_t level, uint32_t fmt, const FormatInfo &fi, const uint8_t *src, uint32_t w, uint32_t h,
                     uint32_t d, const uint32_t *palette, std::vector<uint32_t> &scratch)
{
    scratch.resize(size_t(w) * h * d);
    static std::vector<uint32_t> xo, yo, zo;
    xo.resize(w);
    yo.resize(h);
    zo.resize(d);
    swizzle_tables_3d(w, h, d, xo.data(), yo.data(), zo.data());
    for (uint32_t z = 0; z < d; z++)
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                scratch[(size_t(z) * h + y) * w + x] = decode_texel(fmt, src + (xo[x] | yo[y] | zo[z]) * fi.bpp, palette);
    glTexImage3D(GL_TEXTURE_3D, level, GL_RGBA8, w, h, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
}

GLenum wrap_mode(uint32_t m)
{
    switch (m) {
    case 1: return GL_REPEAT;
    case 2: return GL_MIRRORED_REPEAT;
    case 4: return GL_CLAMP_TO_BORDER;
    default: return GL_CLAMP_TO_EDGE;
    }
}

void apply_sampler(GLenum target, int stage, uint32_t levels)
{
    uint32_t base = stage * 0x40;
    uint32_t filter = kr(NV097_SET_TEXTURE_FILTER + base);
    uint32_t addr = kr(NV097_SET_TEXTURE_ADDRESS + base);
    uint32_t minf = (filter >> 16) & 0xFF, magf = (filter >> 24) & 0xF;
    static const GLenum min_map[8] = {GL_LINEAR, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                      GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR,
                                      GL_LINEAR};
    GLenum gmin = min_map[minf & 7];
    if (levels <= 1) {
        if (gmin == GL_NEAREST_MIPMAP_NEAREST || gmin == GL_NEAREST_MIPMAP_LINEAR) gmin = GL_NEAREST;
        else if (gmin != GL_NEAREST) gmin = GL_LINEAR;
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, gmin);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magf == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_mode(addr & 0xF));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_mode((addr >> 8) & 0xF));
    glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap_mode((addr >> 16) & 0xF));
    uint32_t bc = kr(NV097_SET_TEXTURE_BORDER_COLOR + base);
    float border[4] = {((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, (bc & 255) / 255.0f, (bc >> 24) / 255.0f};
    glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, border);
    // LOD bias is a signed 5.8 fixed-point value.
    int32_t bias = int32_t(filter << 19) >> 19;
    glTexParameterf(target, GL_TEXTURE_LOD_BIAS, bias / 256.0f);
}

#ifndef GL_DEPTH_STENCIL_TEXTURE_MODE
#define GL_DEPTH_STENCIL_TEXTURE_MODE 0x90EA
#endif

// A depth/stencil surface sampled with a colour format: the shader expects
// the raw bytes of guest memory (Z24S8 is stencil in byte 0 and depth in
// bytes 1-3; Z16 is depth in bytes 0-1). The surface lives in a GL depth
// texture, so it is converted on the GPU into an RGBA8 texture holding those
// bytes in memory order, which is then swizzled for the requested format.
struct DepthView {
    GLuint tex = 0, fbo = 0;
    uint32_t w = 0, h = 0;
    uint64_t gen = ~0ull;
};
std::unordered_map<GLuint, DepthView> g_depth_views;
bool g_bindings_disturbed;
GLuint g_conv_prog, g_conv_vao;
GLint g_conv_mode, g_conv_depth, g_conv_stencil;

void init_depth_conversion()
{
    static const char *vs = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    static const char *fs = R"(#version 330 core
uniform sampler2D depthTex;
uniform usampler2D stencilTex;
uniform int mode;   // 0 = Z24S8 depth bytes, 1 = Z24S8 stencil byte, 2 = Z16
out vec4 color;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    if (mode == 1) {
        color = vec4(float(texelFetch(stencilTex, c, 0).r) / 255.0, 0.0, 0.0, 0.0);
        return;
    }
    float d = texelFetch(depthTex, c, 0).r;
    if (mode == 0) {
        uint z = min(uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5), 16777215u);
        color = vec4(0.0, float(z & 255u), float((z >> 8) & 255u), float(z >> 16)) / 255.0;
    } else {
        uint z = min(uint(clamp(d, 0.0, 1.0) * 65535.0 + 0.5), 65535u);
        color = vec4(float(z & 255u), float(z >> 8), 0.0, 0.0) / 255.0;
    }
})";
    g_conv_prog = renderer_compile_program(vs, fs, "depth-to-color");
    g_conv_mode = glGetUniformLocation(g_conv_prog, "mode");
    g_conv_depth = glGetUniformLocation(g_conv_prog, "depthTex");
    g_conv_stencil = glGetUniformLocation(g_conv_prog, "stencilTex");
    glGenVertexArrays(1, &g_conv_vao);
}

// Returns an RGBA8 texture with the surface's bytes in memory order. Changes
// the bound framebuffer, program, VAO, texture units 0-1 and raster state, so
// it must run before the draw sets those up.
GLuint depth_as_color(Surface *s)
{
    if (!g_conv_prog)
        init_depth_conversion();
    DepthView &v = g_depth_views[s->tex];
    if (!v.tex || v.w != s->pw || v.h != s->ph) {
        if (!v.tex) {
            glGenTextures(1, &v.tex);
            glGenFramebuffers(1, &v.fbo);
        }
        glBindTexture(GL_TEXTURE_2D, v.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->pw, s->ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, v.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v.tex, 0);
        v.w = s->pw;
        v.h = s->ph;
        v.gen = ~0ull;
    }
    if (v.gen == s->write_gen)
        return v.tex;
    v.gen = s->write_gen;
    g_bindings_disturbed = true;
    glBindFramebuffer(GL_FRAMEBUFFER, v.fbo);
    glViewport(0, 0, v.w, v.h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(g_conv_prog);
    glBindVertexArray(g_conv_vao);
    glUniform1i(g_conv_depth, 0);
    glUniform1i(g_conv_stencil, 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    if (s->format == 1) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUniform1i(g_conv_mode, 2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
        glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUniform1i(g_conv_mode, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        // Stencil: a second pass through the same texture in stencil mode.
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
        glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUniform1i(g_conv_mode, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (capture_active()) {
        std::vector<uint8_t> px(size_t(v.w) * v.h * 4);
        glReadPixels(0, 0, v.w, v.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        char b[96];
        snprintf(b, sizeof b, "depth view %08X: centre %02X%02X%02X%02X corner %02X%02X%02X%02X", s->addr,
                 px[(v.h / 2 * v.w + v.w / 2) * 4], px[(v.h / 2 * v.w + v.w / 2) * 4 + 1],
                 px[(v.h / 2 * v.w + v.w / 2) * 4 + 2], px[(v.h / 2 * v.w + v.w / 2) * 4 + 3], px[(40 * v.w + 100) * 4],
                 px[(40 * v.w + 100) * 4 + 1], px[(40 * v.w + 100) * 4 + 2], px[(40 * v.w + 100) * 4 + 3]);
        capture_draw(b, nullptr);
    }
    return v.tex;
}

// Swizzle mapping memory-order bytes (R = byte 0 ... A = byte 3) to the
// channels a 32-bit colour format reads (see decode_texel).
bool raw_swizzle(uint32_t fmt, GLint *sw)
{
    switch (fmt) {
    case 0x06: case 0x07: case 0x12: case 0x1E:   // A8R8G8B8 / X8R8G8B8
        sw[0] = GL_BLUE; sw[1] = GL_GREEN; sw[2] = GL_RED;
        sw[3] = (fmt == 0x07 || fmt == 0x1E) ? GL_ONE : GL_ALPHA;
        return true;
    case 0x3A: case 0x3F:                         // A8B8G8R8
        sw[0] = GL_RED; sw[1] = GL_GREEN; sw[2] = GL_BLUE; sw[3] = GL_ALPHA;
        return true;
    case 0x3B: case 0x40:                         // B8G8R8A8
        sw[0] = GL_GREEN; sw[1] = GL_BLUE; sw[2] = GL_ALPHA; sw[3] = GL_RED;
        return true;
    case 0x3C: case 0x41:                         // R8G8B8A8
        sw[0] = GL_ALPHA; sw[1] = GL_BLUE; sw[2] = GL_GREEN; sw[3] = GL_RED;
        return true;
    default:
        return false;
    }
}

} // namespace

void swizzle_tables(uint32_t w, uint32_t h, uint32_t *xoff, uint32_t *yoff)
{
    swizzle_tables_3d(w, h, 1, xoff, yoff, nullptr);
}

void swizzle_tables_3d(uint32_t w, uint32_t h, uint32_t d, uint32_t *xoff, uint32_t *yoff, uint32_t *zoff)
{
    uint32_t mx = 0, my = 0, mz = 0, bit = 1, mbit = 1;
    for (;;) {
        bool done = true;
        if (bit < w) { mx |= mbit; mbit <<= 1; done = false; }
        if (bit < h) { my |= mbit; mbit <<= 1; done = false; }
        if (bit < d) { mz |= mbit; mbit <<= 1; done = false; }
        bit <<= 1;
        if (done) break;
    }
    auto fill = [](uint32_t pattern, uint32_t value) {
        uint32_t r = 0, b = 1;
        while (value) {
            if (pattern & b) {
                if (value & 1) r |= b;
                value >>= 1;
            }
            b <<= 1;
            if (!b) break;
        }
        return r;
    };
    for (uint32_t x = 0; x < w; x++) xoff[x] = fill(mx, x);
    for (uint32_t y = 0; y < h; y++) yoff[y] = fill(my, y);
    if (zoff)
        for (uint32_t z = 0; z < d; z++) zoff[z] = fill(mz, z);
}

void texture_new_frame() { g_tex_frame++; }

bool texture_take_disturbed()
{
    bool d = g_bindings_disturbed;
    g_bindings_disturbed = false;
    return d;
}

TextureBinding texture_bind_stage(int stage)
{
    TextureBinding b;
    uint32_t base = stage * 0x40;
    uint32_t control0 = kr(NV097_SET_TEXTURE_CONTROL0 + base);
    glActiveTexture(GL_TEXTURE0 + stage);
    if (!(control0 & 0x40000000)) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return b;
    }
    uint32_t format = kr(NV097_SET_TEXTURE_FORMAT + base);
    uint32_t offset = kr(NV097_SET_TEXTURE_OFFSET + base);
    uint32_t dma_sel = format & 3;
    DmaObject dma = nv_dma_load(kr(dma_sel == 2 ? NV097_SET_CONTEXT_DMA_B : NV097_SET_CONTEXT_DMA_A));
    uint32_t addr = (dma.address + offset) & 0x07FFFFFF;
    uint32_t fmt = (format >> 8) & 0xFF;
    bool cube = (format & 4) != 0;
    uint32_t dims = (format >> 4) & 0xF;
    uint32_t levels = (format >> 16) & 0xF;
    if (!levels) levels = 1;
    uint32_t w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
    bool volume = dims == 3 && !cube;
    uint32_t d = volume ? 1u << ((format >> 28) & 0xF) : 1;
    FormatInfo fi = format_info(fmt);
    uint32_t pitch = 0, rect = 0;
    if (fi.linear) {
        rect = kr(NV097_SET_TEXTURE_IMAGE_RECT + base);
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = kr(NV097_SET_TEXTURE_CONTROL1 + base) >> 16;
        levels = 1;
    }

    // Render-to-texture: use the GL surface directly, unless the CPU has since
    // written new data to that memory.
    for (int depth = 0; depth < 2; depth++)
        if (Surface *s = surface_find(addr, depth != 0))
            if (surface_is_stale(s))
                surface_discard(s);
    if (Surface *s = surface_find(addr, false)) {
        b.tex = s->tex;
        b.target = GL_TEXTURE_2D;
        b.type = fi.linear ? PSH_TEX_RECT : PSH_TEX_2D;
        if (fi.linear) { b.scale_x = 1.0f / s->width; b.scale_y = 1.0f / s->height; }
        glBindTexture(GL_TEXTURE_2D, s->tex);
        apply_sampler(GL_TEXTURE_2D, stage, 1);
        return b;
    }
    if (Surface *s = surface_find(addr, true)) {
        GLint sw[4];
        if (!s->swizzled && raw_swizzle(fmt, sw)) {
            GLuint t = depth_as_color(s);
            glActiveTexture(GL_TEXTURE0 + stage);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
            apply_sampler(GL_TEXTURE_2D, stage, 1);
            // Packed depth bytes cannot be interpolated (matters when scaled).
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            b.tex = t;
            b.target = GL_TEXTURE_2D;
            b.type = fi.linear ? PSH_TEX_RECT : PSH_TEX_2D;
            if (fi.linear) { b.scale_x = 1.0f / s->width; b.scale_y = 1.0f / s->height; }
            return b;
        }
        b.tex = s->tex;
        b.target = GL_TEXTURE_2D;
        b.type = fi.linear ? PSH_TEX_RECT : PSH_TEX_2D;
        if (fi.linear) { b.scale_x = 1.0f / s->width; b.scale_y = 1.0f / s->height; }
        glBindTexture(GL_TEXTURE_2D, s->tex);
        apply_sampler(GL_TEXTURE_2D, stage, 1);
        return b;
    }

    if (!fi.bpp && fi.kind == K_RGBA8) {
        if (g_unknown_logged.insert(fmt).second)
            LOG_WARN("texture", "unsupported texture format 0x%02X", fmt);
        glBindTexture(GL_TEXTURE_2D, 0);
        return b;
    }
    if (!w || !h || w > 4096 || h > 4096) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return b;
    }
    if (volume && (fi.kind != K_RGBA8 || !fi.swizzled)) {
        if (g_unknown_logged.insert(0x100 | fmt).second)
            LOG_WARN("texture", "unsupported volume texture format 0x%02X", fmt);
        glBindTexture(GL_TEXTURE_3D, 0);
        return b;
    }

    // Total size of the image data (all faces and levels).
    uint32_t face_size = 0;
    for (uint32_t l = 0, lw = w, lh = h, ld = d; l < levels; l++) {
        face_size += fi.linear ? pitch * lh : level_size(fi, lw, lh) * ld;
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
        ld = ld > 1 ? ld / 2 : 1;
    }
    if (cube)
        face_size = (face_size + 127) & ~127u;
    uint32_t total = face_size * (cube ? 6 : 1);
    if (addr + total > XBOX_RAM_SIZE) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return b;
    }
    const uint8_t *src = phys_ptr(addr);

    const uint32_t *palette = nullptr;
    uint32_t pal_word = 0;
    if (fmt == 0x0B) {
        pal_word = kr(NV097_SET_TEXTURE_PALETTE + base);
        DmaObject pd = nv_dma_load(kr((pal_word & 3) == 2 ? NV097_SET_CONTEXT_DMA_B : NV097_SET_CONTEXT_DMA_A));
        palette = reinterpret_cast<const uint32_t *>(phys_ptr(pd.address + (pal_word & 0xFFFFFFC0)));
    }

    Key key{addr, format, rect, pitch, pal_word};
    CachedTexture &ct = g_cache[key];
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : volume ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    bool need_upload = ct.tex == 0;
    if (ct.checked_frame != g_tex_frame) {
        uint64_t hsh = hash_bytes(src, total);
        if (palette)
            hsh ^= hash_bytes(reinterpret_cast<const uint8_t *>(palette), 1024) * 31;
        if (hsh != ct.hash)
            need_upload = true;
        ct.hash = hsh;
        ct.checked_frame = g_tex_frame;
    }
    if (!ct.tex)
        glGenTextures(1, &ct.tex);
    ct.target = target;
    ct.levels = levels;
    glBindTexture(target, ct.tex);
    if (need_upload) {
        static std::vector<uint32_t> scratch;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
        if (volume) {
            const uint8_t *p = src;
            for (uint32_t l = 0, lw = w, lh = h, ld = d; l < levels; l++) {
                upload_level_3d(l, fmt, fi, p, lw, lh, ld, palette, scratch);
                p += level_size(fi, lw, lh) * ld;
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
                ld = ld > 1 ? ld / 2 : 1;
            }
        }
        for (uint32_t face = 0; !volume && face < (cube ? 6u : 1u); face++) {
            const uint8_t *p = src + face * face_size;
            GLenum t = cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
            for (uint32_t l = 0, lw = w, lh = h; l < levels; l++) {
                upload_level(t, l, fmt, fi, p, lw, lh, pitch, palette, scratch);
                p += fi.linear ? pitch * lh : level_size(fi, lw, lh);
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
        }
    }
    apply_sampler(target, stage, levels);
    b.tex = ct.tex;
    b.target = target;
    b.type = cube ? PSH_TEX_CUBE : volume ? PSH_TEX_3D : (fi.linear ? PSH_TEX_RECT : PSH_TEX_2D);
    if (fi.linear) {
        b.scale_x = 1.0f / w;
        b.scale_y = 1.0f / h;
    }
    return b;
}

} // namespace rt
