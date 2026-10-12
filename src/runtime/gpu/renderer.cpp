// OpenGL presentation of the emulated display.
#include "renderer.h"
#include "gl.h"
#include "../host/host.h"
#include "../host/png.h"
#include "../log.h"
#include "../memory.h"

#include <SDL.h>
#include <string>
#include <vector>
#include <windows.h>

#define RT_GL_DEFINE(type, name) type rt_##name;
RT_GL_FUNCS(RT_GL_DEFINE)
#undef RT_GL_DEFINE

bool rt_gl_load()
{
    bool ok = true;
#define RT_GL_LOAD(type, name)                                                                                         \
    rt_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                                                  \
    if (!rt_##name) {                                                                                                  \
        LOG_WARN("gl", "missing GL entry point %s", #name);                                                            \
        ok = false;                                                                                                    \
    }
    RT_GL_FUNCS(RT_GL_LOAD)
#undef RT_GL_LOAD
    return ok;
}

namespace rt {

static GLuint g_present_prog, g_present_vao, g_mem_tex;
static double g_shot_interval;   // seconds between debug screenshots (0 = off)
static std::string g_shot_dir;
static uint64_t g_last_shot_ms;
static int g_shot_index;

void renderer_enable_screenshots(const char *dir, double interval_seconds)
{
    if (const char *e = getenv("SWBF2_SHOT_INTERVAL"))
        interval_seconds = atof(e);
    g_shot_dir = dir;
    g_shot_interval = interval_seconds;
    CreateDirectoryA(dir, nullptr);
}

static void maybe_screenshot(GLuint tex, uint32_t width, uint32_t height, bool flip_y)
{
    if (g_shot_interval <= 0)
        return;
    uint64_t now = GetTickCount64();
    if (g_last_shot_ms && now - g_last_shot_ms < uint64_t(g_shot_interval * 1000))
        return;
    g_last_shot_ms = now;
    GLint w = 0, h = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0)
        return;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 3; i < px.size(); i += 4)
        px[i] = 255;
    char path[512];
    snprintf(path, sizeof path, "%s\\shot_%03d.png", g_shot_dir.c_str(), g_shot_index++);
    if (flip_y)
        write_png(path, px.data() + size_t(h - 1) * w * 4, w, h, -w * 4);
    else
        write_png(path, px.data(), w, h, w * 4);
    LOG_INFO("gl", "screenshot %s (%dx%d)", path, w, h);
}
static GLint g_u_flip;

static void APIENTRY gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                              const GLchar *message, const void *user)
{
    if (severity == GL_DEBUG_SEVERITY_HIGH)
        LOG_WARN("gl", "%s", message);
}

unsigned renderer_compile_program(const char *vs_src, const char *fs_src, const char *name)
{
    auto compile = [&](GLenum type, const char *src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            LOG_ERROR("gl", "%s %s shader failed to compile:\n%s\n--- source ---\n%s", name,
                      type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src);
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOG_ERROR("gl", "%s program failed to link:\n%s", name, log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

void renderer_thread_init()
{
    if (SDL_GL_MakeCurrent(host_window(), host_gl_context()) != 0)
        fatal("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(0);
    if (!rt_gl_load())
        LOG_WARN("gl", "some OpenGL functions are missing; rendering may fail");
    LOG_INFO("gl", "OpenGL %s on %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    if (rt_glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(gl_debug, nullptr);
    }
    static const char *vs = R"(#version 330 core
out vec2 uv;
uniform int flip;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = flip != 0 ? vec2(p.x, 1.0 - p.y) : p;
    uv.y = 1.0 - uv.y;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    static const char *fs = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D tex;
void main() { color = vec4(texture(tex, uv).rgb, 1.0); })";
    g_present_prog = renderer_compile_program(vs, fs, "present");
    g_u_flip = glGetUniformLocation(g_present_prog, "flip");
    glGenVertexArrays(1, &g_present_vao);
    glGenTextures(1, &g_mem_tex);
    glBindTexture(GL_TEXTURE_2D, g_mem_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void renderer_present_texture(unsigned tex, uint32_t width, uint32_t height, bool flip_y)
{
    int w, h;
    host_window_size(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glViewport(0, 0, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
    // Letterbox to 4:3.
    int vw = w, vh = h;
    if (vw * 3 > vh * 4) vw = vh * 4 / 3;
    else vh = vw * 3 / 4;
    glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    glUseProgram(g_present_prog);
    glUniform1i(g_u_flip, flip_y ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(g_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    maybe_screenshot(tex, width, height, flip_y);
    SDL_GL_SwapWindow(host_window());
}

void renderer_present_memory(uint32_t phys, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp)
{
    if (!width || !height || phys + pitch * height > XBOX_RAM_SIZE)
        return;
    glBindTexture(GL_TEXTURE_2D, g_mem_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / bpp);
    if (bpp == 4)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, phys_ptr(phys));
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, phys_ptr(phys));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    renderer_present_texture(g_mem_tex, width, height, false);
}

} // namespace rt
