// Draw submission: turns a Kelvin BEGIN/END batch into OpenGL draws.
#include "draw.h"
#include "gl.h"
#include "kelvin.h"
#include "kelvin_state.h"
#include "nv2a_internal.h"
#include "psh.h"
#include "renderer.h"
#include "surface.h"
#include "texture.h"
#include "vsh.h"
#include "../log.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace rt {

namespace {

GLuint g_vao, g_vbo, g_ibo;

struct Program {
    GLuint prog = 0;
    GLint c, surfaceInfo, fogInfo, psConst0, psConst1, finalConst0, finalConst1, fogColor, texScale, bumpMat,
        bumpScale, bumpOffset, alphaRef, eyeVec, compositeMat, texMat, texMatEnable;
};
std::unordered_map<uint64_t, Program> g_programs;

const char *k_vs_helpers = R"(
vec4 vsh_dst(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }
float vsh_rcp(float x) { return 1.0 / x; }
float vsh_rcc(float x) {
    float r = 1.0 / x;
    return r >= 0.0 ? clamp(r, 5.42101e-20, 1.84467e+19) : clamp(r, -1.84467e+19, -5.42101e-20);
}
vec4 vsh_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }
vec4 vsh_log(float x) {
    float a = abs(x);
    if (a == 0.0) return vec4(-3.4e38, 1.0, -3.4e38, 1.0);
    float e = floor(log2(a));
    return vec4(e, a / exp2(e), log2(a), 1.0);
}
vec4 vsh_lit(vec4 s) {
    float d = max(s.x, 0.0);
    float sp = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -128.0, 128.0)) : 0.0;
    return vec4(1.0, d, sp, 1.0);
}
)";

std::string vs_header(bool flat)
{
    std::string s = "#version 330 core\n";
    for (int i = 0; i < 16; i++)
        s += "layout(location = " + std::to_string(i) + ") in vec4 v" + std::to_string(i) + ";\n";
    std::string q = flat ? "flat " : "";
    s += "uniform vec4 c[192];\nuniform vec4 surfaceInfo;\nuniform vec4 fogInfo;\n"
         "uniform mat4 compositeMat;\nuniform mat4 texMat[4];\nuniform int texMatEnable[4];\n";
    s += q + "out vec4 vtx_D0;\n" + q + "out vec4 vtx_D1;\n" + q + "out vec4 vtx_B0;\n" + q + "out vec4 vtx_B1;\n";
    s += "out float vtx_Fog;\nout vec4 vtx_T0;\nout vec4 vtx_T1;\nout vec4 vtx_T2;\nout vec4 vtx_T3;\n";
    s += k_vs_helpers;
    s += "void main() {\n"
         "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0), R4 = vec4(0.0), R5 = vec4(0.0);\n"
         "  vec4 R6 = vec4(0.0), R7 = vec4(0.0), R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);\n"
         "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
         "  vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
         "  vec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
         "  vec4 oFog = vec4(0.0, 0.0, 0.0, 1.0), oPts = vec4(1.0);\n"
         "  vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
         "  vec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
         "  vec4 oUnk1, oUnk2, oUnk13, oUnk14, oUnk15, cwrite;\n"
         "  int A0 = 0;\n";
    return s;
}

// Converts screen-space oPos (as the NV2A expects) to GL clip space.
// Rendering is done in memory orientation (row 0 = top = GL row 0).
const char *k_vs_epilogue = R"(
  oPos.x = oPos.x * (2.0 / surfaceInfo.x) - 1.0;
  oPos.y = oPos.y * (2.0 / surfaceInfo.y) - 1.0;
  if (surfaceInfo.w != surfaceInfo.z)
    oPos.z = (oPos.z - surfaceInfo.z) / (0.5 * (surfaceInfo.w - surfaceInfo.z)) - 1.0;
  if (oPos.w < 0.0) oPos.w = clamp(oPos.w, -1.84467e+19, -5.42101e-20);
  else oPos.w = clamp(oPos.w, 5.42101e-20, 1.84467e+19);
  oPos.xyz *= oPos.w;
  gl_Position = oPos;
  gl_PointSize = max(oPts.x, 1.0);
  vtx_D0 = clamp(oD0, 0.0, 1.0);
  vtx_D1 = clamp(oD1, 0.0, 1.0);
  vtx_B0 = clamp(oB0, 0.0, 1.0);
  vtx_B1 = clamp(oB1, 0.0, 1.0);
  vtx_T0 = oT0; vtx_T1 = oT1; vtx_T2 = oT2; vtx_T3 = oT3;
  float fogDistance = oFog.x;
  float fogFactor = 1.0;
  if (fogInfo.w > 0.5) {
    if (fogInfo.z < 1.5) fogFactor = fogInfo.x + fogDistance * fogInfo.y;
    else if (fogInfo.z < 2.5) fogFactor = fogInfo.x + exp2(fogDistance * fogInfo.y * 16.0) - 1.5;
    else fogFactor = fogInfo.x + exp2(-fogDistance * fogDistance * fogInfo.y * fogInfo.y * 32.0) - 1.5;
  }
  vtx_Fog = fogFactor;
}
)";

// Fixed-function transform & lighting (minimal: no hardware lighting).
const char *k_ff_body = R"(
  vec4 s = vec4(v0.xyz, 1.0) * compositeMat;
  if (v0.w != 1.0 && v0.w != 0.0) s = v0 * compositeMat;
  oPos = vec4(s.xyz / s.w, s.w);
  oD0 = v3;
  oD1 = v4;
  oB0 = v7;
  oB1 = v8;
  oFog = vec4(v5.x);
  oT0 = texMatEnable[0] != 0 ? v9 * texMat[0] : v9;
  oT1 = texMatEnable[1] != 0 ? v10 * texMat[1] : v10;
  oT2 = texMatEnable[2] != 0 ? v11 * texMat[2] : v11;
  oT3 = texMatEnable[3] != 0 ? v12 * texMat[3] : v12;
)";

uint64_t fnv(const void *p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool vertex_program_mode() { return (kr(NV097_SET_TRANSFORM_EXECUTION_MODE) & 3) == 2; }

uint64_t vs_program_hash()
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t pc = g_kd.program_start; pc < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; pc++) {
        h = fnv(&g_kd.program[pc][1], 12, h);
        if (g_kd.program[pc][3] & 1)
            break;
    }
    return h;
}

GLint loc(GLuint p, const char *n) { return glGetUniformLocation(p, n); }

Program &get_program(const PshState &ps, bool programmable, bool flat)
{
    uint64_t vs_key = programmable ? vs_program_hash() : 0x1234567ull;
    uint64_t key = fnv(&ps, sizeof ps, vs_key ^ (flat ? 0x55 : 0));
    auto it = g_programs.find(key);
    if (it != g_programs.end())
        return it->second;
    std::string vs = vs_header(flat);
    if (programmable)
        vs += vsh_translate(g_kd.program, g_kd.program_start, NV2A_MAX_TRANSFORM_PROGRAM_LENGTH, nullptr);
    else
        vs += k_ff_body;
    vs += k_vs_epilogue;
    std::string fs = psh_generate(ps);
    Program p;
    p.prog = renderer_compile_program(vs.c_str(), fs.c_str(), "kelvin");
    glUseProgram(p.prog);
    for (int i = 0; i < 4; i++)
        glUniform1i(loc(p.prog, ("tex" + std::to_string(i)).c_str()), i);
    p.c = loc(p.prog, "c");
    p.surfaceInfo = loc(p.prog, "surfaceInfo");
    p.fogInfo = loc(p.prog, "fogInfo");
    p.psConst0 = loc(p.prog, "psConst0");
    p.psConst1 = loc(p.prog, "psConst1");
    p.finalConst0 = loc(p.prog, "finalConst0");
    p.finalConst1 = loc(p.prog, "finalConst1");
    p.fogColor = loc(p.prog, "fogColor");
    p.texScale = loc(p.prog, "texScale");
    p.bumpMat = loc(p.prog, "bumpMat");
    p.bumpScale = loc(p.prog, "bumpScale");
    p.bumpOffset = loc(p.prog, "bumpOffset");
    p.alphaRef = loc(p.prog, "alphaRef");
    p.eyeVec = loc(p.prog, "eyeVec");
    p.compositeMat = loc(p.prog, "compositeMat");
    p.texMat = loc(p.prog, "texMat");
    p.texMatEnable = loc(p.prog, "texMatEnable");
    LOG_DEBUG("draw", "compiled program %016llX (%s, %zu programs)", (unsigned long long)key,
              programmable ? "vertex program" : "fixed function", g_programs.size() + 1);
    return g_programs.emplace(key, p).first->second;
}

void unpack_argb(uint32_t c, float *out)
{
    out[0] = ((c >> 16) & 255) / 255.0f;
    out[1] = ((c >> 8) & 255) / 255.0f;
    out[2] = (c & 255) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

void set_uniforms(Program &p, uint32_t sw, uint32_t sh, const TextureBinding *tb)
{
    if (p.c >= 0)
        glUniform4fv(p.c, NV2A_VERTEXSHADER_CONSTANTS, &g_kd.constants[0][0]);
    glUniform4f(p.surfaceInfo, float(sw), float(sh), kf(NV097_SET_CLIP_MIN), kf(NV097_SET_CLIP_MAX));
    float fogmode = 0;
    switch (kr(NV097_SET_FOG_MODE)) {
    case 0x2601: case 0x804: fogmode = 1; break;
    case 0x800: case 0x802: fogmode = 2; break;
    case 0x801: case 0x803: fogmode = 3; break;
    }
    glUniform4f(p.fogInfo, kf(NV097_SET_FOG_PARAMS), kf(NV097_SET_FOG_PARAMS + 4), fogmode,
                kr(NV097_SET_FOG_ENABLE) ? 1.0f : 0.0f);
    float c0[8][4], c1[8][4];
    for (int i = 0; i < 8; i++) {
        unpack_argb(kr(NV097_SET_COMBINER_FACTOR0 + 4 * i), c0[i]);
        unpack_argb(kr(NV097_SET_COMBINER_FACTOR1 + 4 * i), c1[i]);
    }
    glUniform4fv(p.psConst0, 8, &c0[0][0]);
    glUniform4fv(p.psConst1, 8, &c1[0][0]);
    float f0[4], f1[4], fc[4];
    unpack_argb(kr(NV097_SET_SPECULAR_FOG_FACTOR), f0);
    unpack_argb(kr(NV097_SET_SPECULAR_FOG_FACTOR + 4), f1);
    glUniform4fv(p.finalConst0, 1, f0);
    glUniform4fv(p.finalConst1, 1, f1);
    // Fog colour is stored ABGR (red in the low byte).
    uint32_t fcw = kr(NV097_SET_FOG_COLOR);
    fc[0] = (fcw & 255) / 255.0f;
    fc[1] = ((fcw >> 8) & 255) / 255.0f;
    fc[2] = ((fcw >> 16) & 255) / 255.0f;
    fc[3] = (fcw >> 24) / 255.0f;
    glUniform4fv(p.fogColor, 1, fc);
    float ts[4][2], bm[4][4], bs[4], bo[4];
    for (int i = 0; i < 4; i++) {
        ts[i][0] = tb[i].scale_x;
        ts[i][1] = tb[i].scale_y;
        for (int j = 0; j < 4; j++)
            bm[i][j] = kf(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + i * 0x40 + j * 4);
        bs[i] = kf(NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE + i * 0x40);
        bo[i] = kf(NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET + i * 0x40);
    }
    glUniform2fv(p.texScale, 4, &ts[0][0]);
    glUniform4fv(p.bumpMat, 4, &bm[0][0]);
    glUniform1fv(p.bumpScale, 4, bs);
    glUniform1fv(p.bumpOffset, 4, bo);
    glUniform1f(p.alphaRef, (kr(NV097_SET_ALPHA_REF) & 0xFF) / 255.0f);
    glUniform3f(p.eyeVec, kf(NV097_SET_EYE_VECTOR), kf(NV097_SET_EYE_VECTOR + 4), kf(NV097_SET_EYE_VECTOR + 8));
    if (p.compositeMat >= 0) {
        glUniformMatrix4fv(p.compositeMat, 1, GL_FALSE, reinterpret_cast<const float *>(&kr(NV097_SET_COMPOSITE_MATRIX)));
        glUniformMatrix4fv(p.texMat, 4, GL_FALSE, reinterpret_cast<const float *>(&kr(NV097_SET_TEXTURE_MATRIX)));
        GLint en[4];
        for (int i = 0; i < 4; i++)
            en[i] = kr(NV097_SET_TEXTURE_MATRIX_ENABLE + 4 * i) ? 1 : 0;
        glUniform1iv(p.texMatEnable, 4, en);
    }
}

void apply_state(Surface *color, Surface *zeta)
{
    // Blending
    if (kr(NV097_SET_BLEND_ENABLE) && color) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(kr(NV097_SET_BLEND_FUNC_SFACTOR), kr(NV097_SET_BLEND_FUNC_DFACTOR),
                            kr(NV097_SET_BLEND_FUNC_SFACTOR), kr(NV097_SET_BLEND_FUNC_DFACTOR));
        GLenum eq = kr(NV097_SET_BLEND_EQUATION);
        glBlendEquationSeparate(eq ? eq : GL_FUNC_ADD, eq ? eq : GL_FUNC_ADD);
        float bc[4];
        unpack_argb(kr(NV097_SET_BLEND_COLOR), bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else {
        glDisable(GL_BLEND);
    }
    // Depth
    bool depth_test = kr(NV097_SET_DEPTH_TEST_ENABLE) != 0 && zeta;
    bool depth_write = kr(NV097_SET_DEPTH_MASK) != 0 && zeta;
    if (depth_test || depth_write) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(depth_test ? kr(NV097_SET_DEPTH_FUNC) : GL_ALWAYS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
    // Stencil
    if (kr(NV097_SET_STENCIL_TEST_ENABLE) && zeta && zeta->format == 2) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(kr(NV097_SET_STENCIL_FUNC), kr(NV097_SET_STENCIL_FUNC_REF), kr(NV097_SET_STENCIL_FUNC_MASK));
        glStencilOp(kr(NV097_SET_STENCIL_OP_FAIL), kr(NV097_SET_STENCIL_OP_ZFAIL), kr(NV097_SET_STENCIL_OP_ZPASS));
        glStencilMask(kr(NV097_SET_STENCIL_MASK));
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    // Culling. We render with Y pointing down (memory order), which flips
    // the apparent winding, so the front face is inverted.
    if (kr(NV097_SET_CULL_FACE_ENABLE)) {
        glEnable(GL_CULL_FACE);
        glCullFace(kr(NV097_SET_CULL_FACE));
    } else {
        glDisable(GL_CULL_FACE);
    }
    glFrontFace(kr(NV097_SET_FRONT_FACE) == GL_CW ? GL_CCW : GL_CW);
    // Colour mask
    uint32_t cm = kr(NV097_SET_COLOR_MASK);
    glColorMask((cm >> 16) & 1, (cm >> 8) & 1, cm & 1, (cm >> 24) & 1);
    // Polygon offset / mode
    if (kr(NV097_SET_POLY_OFFSET_FILL_ENABLE)) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(kf(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR), kf(NV097_SET_POLYGON_OFFSET_BIAS));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    uint32_t pm = kr(NV097_SET_FRONT_POLYGON_MODE);
    glPolygonMode(GL_FRONT_AND_BACK, (pm == GL_POINT || pm == GL_LINE) ? pm : GL_FILL);
    // Scissor from window clip rectangle 0 (inclusive coordinates).
    Surface *s = color ? color : zeta;
    uint32_t wh = kr(NV097_SET_WINDOW_CLIP_HORIZONTAL), wv = kr(NV097_SET_WINDOW_CLIP_VERTICAL);
    int x0 = wh & 0xFFFF, x1 = wh >> 16, y0 = wv & 0xFFFF, y1 = wv >> 16;
    if ((wh || wv) && x1 >= x0 && y1 >= y0 && (x0 > 0 || y0 > 0 || x1 + 1 < int(s->width) || y1 + 1 < int(s->height))) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glEnable(GL_PROGRAM_POINT_SIZE);
}

struct AttrInfo {
    bool enabled;
    uint32_t type, count, stride, addr;
    GLenum gltype;
    GLboolean norm;
    uint32_t elem;    // bytes per component
};

void decode_attrs(AttrInfo *a)
{
    DmaObject da = nv_dma_load(kr(NV097_SET_CONTEXT_DMA_VERTEX_A));
    DmaObject db = nv_dma_load(kr(NV097_SET_CONTEXT_DMA_VERTEX_B));
    for (int i = 0; i < 16; i++) {
        uint32_t f = kr(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * i);
        uint32_t off = kr(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * i);
        AttrInfo &x = a[i];
        x.type = f & 0xF;
        x.count = (f >> 4) & 0xF;
        x.stride = f >> 8;
        x.enabled = x.count != 0;
        x.addr = ((off & 0x80000000) ? db.address : da.address) + (off & 0x7FFFFFFF);
        switch (x.type) {
        case NV097_VTX_TYPE_UB_D3D: case NV097_VTX_TYPE_UB_OGL: x.gltype = GL_UNSIGNED_BYTE; x.norm = GL_TRUE; x.elem = 1; break;
        case NV097_VTX_TYPE_S1: x.gltype = GL_SHORT; x.norm = GL_TRUE; x.elem = 2; break;
        case NV097_VTX_TYPE_S32K: x.gltype = GL_SHORT; x.norm = GL_FALSE; x.elem = 2; break;
        case NV097_VTX_TYPE_CMP: x.gltype = GL_FLOAT; x.norm = GL_FALSE; x.elem = 4; break;
        default: x.gltype = GL_FLOAT; x.norm = GL_FALSE; x.elem = 4; break;
        }
    }
}

float cmp_component(uint32_t v, int shift, int bits)
{
    int32_t s = int32_t(v << (32 - shift - bits)) >> (32 - bits);
    return float(s) / float((1 << (bits - 1)) - 1);
}

std::vector<uint8_t> g_staging;

// Uploads attribute data for vertices [first, last] from guest memory and
// sets up attribute pointers. Returns false if nothing is drawable.
bool setup_array_attrs(const AttrInfo *a, uint32_t first, uint32_t last)
{
    g_staging.clear();
    struct Ptr {
        int idx;
        size_t offset;
        GLint size;
        GLenum type;
        GLboolean norm;
        GLsizei stride;
    };
    std::vector<Ptr> ptrs;
    uint32_t n = last - first + 1;
    for (int i = 0; i < 16; i++) {
        const AttrInfo &x = a[i];
        if (!x.enabled) {
            glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, g_kd.current_attr[i]);
            continue;
        }
        if (x.stride == 0) {
            // Constant attribute: the same element for every vertex.
            float v[4] = {0, 0, 0, 1};
            const uint8_t *p = phys_ptr(x.addr);
            for (uint32_t c = 0; c < x.count && c < 4; c++) {
                if (x.type == NV097_VTX_TYPE_F) memcpy(&v[c], p + c * 4, 4);
                else if (x.elem == 1) v[c] = p[c] / 255.0f;
                else v[c] = int16_t(p[c * 2] | (p[c * 2 + 1] << 8)) / (x.norm ? 32767.0f : 1.0f);
            }
            if (x.type == NV097_VTX_TYPE_UB_D3D) std::swap(v[0], v[2]);
            glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, v);
            continue;
        }
        size_t off = g_staging.size();
        if (x.type == NV097_VTX_TYPE_CMP) {
            g_staging.resize(off + size_t(n) * 12);
            float *dst = reinterpret_cast<float *>(&g_staging[off]);
            for (uint32_t v = 0; v < n; v++) {
                uint32_t pk = *reinterpret_cast<const uint32_t *>(phys_ptr(x.addr + (first + v) * x.stride));
                dst[v * 3 + 0] = cmp_component(pk, 0, 11);
                dst[v * 3 + 1] = cmp_component(pk, 11, 11);
                dst[v * 3 + 2] = cmp_component(pk, 22, 10);
            }
            ptrs.push_back({i, off, 3, GL_FLOAT, GL_FALSE, 12});
            continue;
        }
        uint32_t bytes = (n - 1) * x.stride + x.count * x.elem;
        uint32_t src = x.addr + first * x.stride;
        if (src + bytes > XBOX_RAM_SIZE)
            return false;
        g_staging.resize(off + bytes);
        memcpy(&g_staging[off], phys_ptr(src), bytes);
        GLint size = x.type == NV097_VTX_TYPE_UB_D3D && x.count == 4 ? GL_BGRA : GLint(x.count);
        ptrs.push_back({i, off, size, x.gltype, x.norm, GLsizei(x.stride)});
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, g_staging.size(), g_staging.data(), GL_STREAM_DRAW);
    for (const Ptr &p : ptrs) {
        glEnableVertexAttribArray(p.idx);
        glVertexAttribPointer(p.idx, p.size, p.type, p.norm, p.stride, reinterpret_cast<void *>(p.offset));
    }
    return true;
}

GLenum gl_mode(uint32_t prim)
{
    switch (prim) {
    case PRIM_POINTS: return GL_POINTS;
    case PRIM_LINES: return GL_LINES;
    case PRIM_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TRIANGLE_STRIP: case PRIM_QUAD_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TRIANGLE_FAN: case PRIM_POLYGON: return GL_TRIANGLE_FAN;
    case PRIM_QUADS: return GL_TRIANGLES;
    default: return GL_POINTS;
    }
}

// Draws `count` vertices numbered base..base+count-1 in the uploaded buffer.
void draw_range(uint32_t prim, uint32_t base, uint32_t count)
{
    if (prim == PRIM_QUADS) {
        std::vector<uint32_t> idx;
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            uint32_t a = base + q;
            idx.insert(idx.end(), {a, a + 1, a + 2, a, a + 2, a + 3});
        }
        if (idx.empty()) return;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, GLsizei(idx.size()), GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(gl_mode(prim), base, count);
    }
}

void draw_indexed(uint32_t prim, const std::vector<uint32_t> &indices, uint32_t bias)
{
    std::vector<uint32_t> idx;
    if (prim == PRIM_QUADS) {
        for (size_t q = 0; q + 3 < indices.size(); q += 4) {
            uint32_t a = indices[q] - bias, b = indices[q + 1] - bias, c = indices[q + 2] - bias, d = indices[q + 3] - bias;
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    } else {
        idx.resize(indices.size());
        for (size_t i = 0; i < indices.size(); i++)
            idx[i] = indices[i] - bias;
    }
    if (idx.empty()) return;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STREAM_DRAW);
    glDrawElements(gl_mode(prim), GLsizei(idx.size()), GL_UNSIGNED_INT, nullptr);
}

} // namespace

void draw_init()
{
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glGenBuffers(1, &g_ibo);
    for (int i = 0; i < 16; i++) {
        g_kd.current_attr[i][0] = g_kd.current_attr[i][1] = g_kd.current_attr[i][2] = 0.0f;
        g_kd.current_attr[i][3] = 1.0f;
    }
    // Default diffuse/specular are opaque white/black.
    for (int c = 0; c < 4; c++)
        g_kd.current_attr[3][c] = 1.0f;
}

void draw_query_begin();
void draw_query_end();

void draw_flush()
{
    uint32_t prim = g_kd.primitive;
    bool have_work = !g_kd.draw_arrays.empty() || !g_kd.inline_elements.empty() || !g_kd.inline_array.empty() ||
                     !g_kd.immediate.empty();
    if (!have_work || prim == PRIM_END)
        return;
    Surface *color = surface_color_current();
    Surface *zeta = surface_zeta_current();
    if (!surface_bind(color, zeta))
        return;
    Surface *s = color ? color : zeta;
    glBindVertexArray(g_vao);
    apply_state(color, zeta);

    TextureBinding tb[4];
    PshState ps;
    memset(&ps, 0, sizeof ps);
    for (int i = 0; i < 4; i++) {
        tb[i] = texture_bind_stage(i);
        ps.tex_type[i] = tb[i].type;
    }
    for (int i = 0; i < 8; i++) {
        ps.color_icw[i] = kr(NV097_SET_COMBINER_COLOR_ICW + 4 * i);
        ps.color_ocw[i] = kr(NV097_SET_COMBINER_COLOR_OCW + 4 * i);
        ps.alpha_icw[i] = kr(NV097_SET_COMBINER_ALPHA_ICW + 4 * i);
        ps.alpha_ocw[i] = kr(NV097_SET_COMBINER_ALPHA_OCW + 4 * i);
    }
    ps.final0 = kr(NV097_SET_COMBINER_SPECULAR_FOG_CW0);
    ps.final1 = kr(NV097_SET_COMBINER_SPECULAR_FOG_CW1);
    ps.control = kr(NV097_SET_COMBINER_CONTROL);
    ps.stage_program = kr(NV097_SET_SHADER_STAGE_PROGRAM);
    ps.other_stage_input = kr(NV097_SET_SHADER_OTHER_STAGE_INPUT);
    ps.dot_mapping = kr(NV097_SET_DOT_RGBMAPPING);
    ps.alpha_func = kr(NV097_SET_ALPHA_TEST_ENABLE) ? uint8_t(kr(NV097_SET_ALPHA_FUNC) - 0x200 + 1) : 0;
    ps.two_sided = kr(NV097_SET_TWO_SIDE_LIGHT_EN) != 0;
    ps.flat_shade = kr(NV097_SET_SHADE_MODE) == 0x1D00;
    ps.fog_enable = kr(NV097_SET_FOG_ENABLE) != 0;

    bool programmable = vertex_program_mode();
    Program &p = get_program(ps, programmable, ps.flat_shade != 0);
    glUseProgram(p.prog);
    set_uniforms(p, s->width, s->height, tb);

    AttrInfo attrs[16];
    decode_attrs(attrs);
    draw_query_begin();

    if (!g_kd.inline_elements.empty()) {
        auto mm = std::minmax_element(g_kd.inline_elements.begin(), g_kd.inline_elements.end());
        uint32_t lo = *mm.first, hi = *mm.second;
        if (hi - lo < 0x100000 && setup_array_attrs(attrs, lo, hi))
            draw_indexed(prim, g_kd.inline_elements, lo);
    }
    if (!g_kd.draw_arrays.empty()) {
        uint32_t lo = 0xFFFFFFFF, hi = 0;
        for (size_t i = 0; i < g_kd.draw_arrays.size(); i += 2) {
            lo = std::min(lo, g_kd.draw_arrays[i]);
            hi = std::max(hi, g_kd.draw_arrays[i] + g_kd.draw_arrays[i + 1] - 1);
        }
        if (hi - lo < 0x100000 && setup_array_attrs(attrs, lo, hi))
            for (size_t i = 0; i < g_kd.draw_arrays.size(); i += 2)
                draw_range(prim, g_kd.draw_arrays[i] - lo, g_kd.draw_arrays[i + 1]);
    }
    if (!g_kd.inline_array.empty()) {
        // Inline vertices: enabled attributes packed in slot order, each
        // rounded up to a dword.
        uint32_t offs[16], stride = 0;
        for (int i = 0; i < 16; i++) {
            if (!attrs[i].enabled) continue;
            offs[i] = stride;
            uint32_t bytes = attrs[i].type == NV097_VTX_TYPE_CMP ? 4 : attrs[i].count * attrs[i].elem;
            stride += (bytes + 3) & ~3u;
        }
        if (stride) {
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
            glBufferData(GL_ARRAY_BUFFER, g_kd.inline_array.size() * 4, g_kd.inline_array.data(), GL_STREAM_DRAW);
            for (int i = 0; i < 16; i++) {
                const AttrInfo &x = attrs[i];
                if (!x.enabled || x.type == NV097_VTX_TYPE_CMP) {
                    glDisableVertexAttribArray(i);
                    glVertexAttrib4fv(i, g_kd.current_attr[i]);
                    continue;
                }
                GLint size = x.type == NV097_VTX_TYPE_UB_D3D && x.count == 4 ? GL_BGRA : GLint(x.count);
                glEnableVertexAttribArray(i);
                glVertexAttribPointer(i, size, x.gltype, x.norm, stride, reinterpret_cast<void *>(uintptr_t(offs[i])));
            }
            draw_range(prim, 0, uint32_t(g_kd.inline_array.size() * 4 / stride));
        }
    }
    if (!g_kd.immediate.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, g_kd.immediate.size() * 4, g_kd.immediate.data(), GL_STREAM_DRAW);
        for (int i = 0; i < 16; i++) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 16 * 16, reinterpret_cast<void *>(uintptr_t(i * 16)));
        }
        draw_range(prim, 0, uint32_t(g_kd.immediate.size() / 64));
    }
    draw_query_end();
    surface_mark_written(color);
    surface_mark_written(zeta);
    g_nv->draw_calls++;
}

// ---- occlusion reports (NV097 ZPASS pixel count) ------------------------------
static std::vector<GLuint> g_queries, g_free_queries;
static GLuint g_active_query;
static uint32_t g_zpass_total;

static void collect_queries()
{
    for (GLuint q : g_queries) {
        GLuint n = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &n);
        g_zpass_total += n;
        g_free_queries.push_back(q);
    }
    g_queries.clear();
}

void draw_query_begin()
{
    if (!kr(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE))
        return;
    if (g_free_queries.empty()) {
        GLuint q;
        glGenQueries(1, &q);
        g_free_queries.push_back(q);
    }
    g_active_query = g_free_queries.back();
    g_free_queries.pop_back();
    glBeginQuery(GL_SAMPLES_PASSED, g_active_query);
}

void draw_query_end()
{
    if (!g_active_query)
        return;
    glEndQuery(GL_SAMPLES_PASSED);
    g_queries.push_back(g_active_query);
    g_active_query = 0;
}

void draw_report_clear()
{
    collect_queries();
    g_zpass_total = 0;
}

void draw_report_write(uint32_t parameter)
{
    collect_queries();
    DmaObject d = nv_dma_load(kr(NV097_SET_CONTEXT_DMA_REPORT));
    uint32_t off = parameter & 0x00FFFFFF;
    if (d.address + off + 16 > XBOX_RAM_SIZE)
        return;
    uint8_t *p = phys_ptr(d.address + off);
    uint64_t timestamp = 0x0011223344556677ull;
    uint32_t done = 0;
    memcpy(p, &timestamp, 8);
    memcpy(p + 8, &g_zpass_total, 4);
    memcpy(p + 12, &done, 4);
}

void draw_clear(uint32_t flags)
{
    Surface *color = (flags & 0xF0) ? surface_color_current() : nullptr;
    Surface *zeta = (flags & 0x3) ? surface_zeta_current() : nullptr;
    if (!surface_bind(color, zeta))
        return;
    uint32_t xr = kr(NV097_SET_CLEAR_RECT_HORIZONTAL), yr = kr(NV097_SET_CLEAR_RECT_VERTICAL);
    int x0 = xr & 0xFFFF, x1 = xr >> 16, y0 = yr & 0xFFFF, y1 = yr >> 16;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    if (color) {
        float c[4];
        unpack_argb(kr(NV097_SET_COLOR_CLEAR_VALUE), c);
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0, (flags & 0x40) != 0, (flags & 0x80) != 0);
        glClearBufferfv(GL_COLOR, 0, c);
        surface_mark_written(color);
    }
    if (zeta) {
        uint32_t v = kr(NV097_SET_ZSTENCIL_CLEAR_VALUE);
        float depth;
        int stencil = 0;
        if (zeta->format == 1) {
            depth = (v & 0xFFFF) / 65535.0f;
        } else {
            depth = (v >> 8) / 16777215.0f;
            stencil = v & 0xFF;
        }
        glDepthMask((flags & 1) ? GL_TRUE : GL_FALSE);
        glStencilMask((flags & 2) ? 0xFF : 0);
        if (zeta->format == 2)
            glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencil);
        else
            glClearBufferfv(GL_DEPTH, 0, &depth);
        surface_mark_written(zeta);
    }
    glDisable(GL_SCISSOR_TEST);
}

unsigned draw_find_scanout(uint32_t addr, uint32_t &w, uint32_t &h)
{
    Surface *s = surface_find(addr, false);
    if (!s)
        return 0;
    if (surface_is_stale(s)) {
        surface_discard(s);
        return 0;
    }
    w = s->width;
    h = s->height;
    return s->tex;
}

} // namespace rt
