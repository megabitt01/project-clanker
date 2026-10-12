// NV2A pixel pipeline translation.
//
// Texture shader stages fetch t0..t3 (with dependent modes such as bump
// mapping and dot products), then up to eight general register combiner
// stages operate on the register file {zero, c0, c1, fog, v0, v1, t0-3, r0,
// r1}, and the final combiner computes A*B + (1-A)*C + D with alpha G.
#include "psh.h"

#include <cstdio>

namespace rt {

namespace {

// Register numbers in combiner input/output fields.
enum {
    REG_ZERO = 0, REG_C0 = 1, REG_C1 = 2, REG_FOG = 3, REG_V0 = 4, REG_V1 = 5,
    REG_T0 = 8, REG_T1 = 9, REG_T2 = 10, REG_T3 = 11, REG_R0 = 12, REG_R1 = 13,
    REG_V1R0_SUM = 14, REG_EF_PROD = 15,
};

const char *reg_name(int r)
{
    switch (r) {
    case REG_C0: return "c0";
    case REG_C1: return "c1";
    case REG_FOG: return "fog";
    case REG_V0: return "v0";
    case REG_V1: return "v1";
    case REG_T0: return "t0";
    case REG_T1: return "t1";
    case REG_T2: return "t2";
    case REG_T3: return "t3";
    case REG_R0: return "r0";
    case REG_R1: return "r1";
    case REG_V1R0_SUM: return "sumVR";
    case REG_EF_PROD: return "prodEF";
    default: return nullptr;
    }
}

// Applies an input mapping (bits 5-7 of an input byte).
std::string map_input(const std::string &x, int mapping, bool vec)
{
    std::string z = vec ? "vec3(0.0)" : "0.0";
    std::string o = vec ? "vec3(1.0)" : "1.0";
    switch (mapping) {
    case 0: return "max(" + x + ", " + z + ")";                                  // unsigned identity
    case 1: return "(" + o + " - clamp(" + x + ", 0.0, 1.0))";                     // unsigned invert
    case 2: return "(2.0 * max(" + x + ", " + z + ") - " + o + ")";                // expand normal
    case 3: return "(-2.0 * max(" + x + ", " + z + ") + " + o + ")";               // expand negate
    case 4: return "(max(" + x + ", " + z + ") - 0.5)";                            // half-bias normal
    case 5: return "(-max(" + x + ", " + z + ") + 0.5)";                           // half-bias negate
    case 6: return x;                                                              // signed identity
    default: return "(-" + x + ")";                                                // signed negate
    }
}

// An RGB input (vec3) or alpha input (float) from an 8-bit input selector.
std::string input(uint32_t sel, bool alpha_portion)
{
    int reg = sel & 0xF;
    bool alpha_channel = (sel & 0x10) != 0;
    int mapping = (sel >> 5) & 7;
    const char *name = reg_name(reg);
    std::string v;
    if (!name) {
        v = alpha_portion ? "0.0" : "vec3(0.0)";
    } else if (alpha_portion) {
        v = std::string(name) + (alpha_channel ? ".a" : ".b");
    } else {
        v = alpha_channel ? "vec3(" + std::string(name) + ".a)" : std::string(name) + ".rgb";
    }
    return map_input(v, mapping, !alpha_portion);
}

// Output scale/bias from the combiner output flags (bits 3-5 of flags).
std::string apply_op(const std::string &x, uint32_t flags)
{
    bool bias = (flags & 0x08) != 0;
    std::string v = bias ? "(" + x + " - 0.5)" : x;
    switch ((flags >> 4) & 3) {
    case 1: v = "(" + v + " * 2.0)"; break;
    case 2: v = "(" + v + " * 4.0)"; break;
    case 3: v = "(" + v + " * 0.5)"; break;
    default: break;
    }
    return v;
}

const char *sampler_type(uint8_t t)
{
    switch (t) {
    case PSH_TEX_CUBE: return "samplerCube";
    case PSH_TEX_3D: return "sampler3D";
    default: return "sampler2D";
    }
}

} // namespace

std::string psh_generate(const PshState &s)
{
    std::string o;
    char buf[1024];
    std::string q = s.flat_shade ? "flat " : "";
    o += "#version 330 core\n";
    o += q + "in vec4 vtx_D0;\n" + q + "in vec4 vtx_D1;\n" + q + "in vec4 vtx_B0;\n" + q + "in vec4 vtx_B1;\n";
    o += "in float vtx_Fog;\nin vec4 vtx_T0;\nin vec4 vtx_T1;\nin vec4 vtx_T2;\nin vec4 vtx_T3;\n";
    for (int i = 0; i < 4; i++) {
        snprintf(buf, sizeof buf, "uniform %s tex%d;\n", sampler_type(s.tex_type[i]), i);
        o += buf;
    }
    o += "uniform vec4 psConst0[8];\nuniform vec4 psConst1[8];\nuniform vec4 finalConst0;\nuniform vec4 finalConst1;\n"
         "uniform vec4 fogColor;\nuniform vec2 texScale[4];\nuniform vec4 bumpMat[4];\nuniform float bumpScale[4];\n"
         "uniform float bumpOffset[4];\nuniform float alphaRef;\nuniform vec3 eyeVec;\n"
         "out vec4 fragColor;\n\n"
         "void main() {\n";
    if (s.two_sided)
        o += "  vec4 v0 = gl_FrontFacing ? vtx_D0 : vtx_B0;\n  vec4 v1 = gl_FrontFacing ? vtx_D1 : vtx_B1;\n";
    else
        o += "  vec4 v0 = vtx_D0;\n  vec4 v1 = vtx_D1;\n";
    o += s.fog_enable ? "  vec4 fog = vec4(fogColor.rgb, clamp(vtx_Fog, 0.0, 1.0));\n"
                      : "  vec4 fog = vec4(fogColor.rgb, 1.0);\n";

    // ---- texture shader stages ---------------------------------------------------
    o += "  vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);\n";
    o += "  float dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n";
    const char *tc[4] = {"vtx_T0", "vtx_T1", "vtx_T2", "vtx_T3"};
    for (int i = 0; i < 4; i++) {
        uint32_t mode = (s.stage_program >> (5 * i)) & 0x1F;
        uint32_t in_stage = i == 0 ? 0 : (s.other_stage_input >> (12 + 4 * i)) & 3;
        if (i > 0 && in_stage >= uint32_t(i))
            in_stage = i - 1;
        bool tex = s.tex_type[i] != PSH_TEX_NONE;
        std::string prev = "t" + std::to_string(in_stage);
        uint32_t dm = i > 0 ? (s.dot_mapping >> (4 * (i - 1))) & 7 : 0;
        std::string prev_mapped = dm == 0 ? prev + ".rgb" : "(" + prev + ".rgb * 2.0 - 1.0)";
        auto sample2d = [&](const std::string &coord) {
            if (!tex)
                return std::string("vec4(0.0)");
            if (s.tex_type[i] == PSH_TEX_CUBE)
                return "texture(tex" + std::to_string(i) + ", vec3(" + coord + ", 0.0))";
            if (s.tex_type[i] == PSH_TEX_3D)
                return "texture(tex" + std::to_string(i) + ", vec3(" + coord + ", 0.0))";
            return "texture(tex" + std::to_string(i) + ", (" + coord + ") * texScale[" + std::to_string(i) + "])";
        };
        std::string t = "t" + std::to_string(i), T = tc[i];
        switch (mode) {
        case 0: // none
            break;
        case 1: // projective 2D
            if (s.tex_type[i] == PSH_TEX_CUBE)
                o += "  " + t + " = texture(tex" + std::to_string(i) + ", " + T + ".xyz);\n";
            else if (s.tex_type[i] == PSH_TEX_3D)
                o += "  " + t + " = texture(tex" + std::to_string(i) + ", " + T + ".xyz / " + T + ".w);\n";
            else
                o += "  " + t + " = " + sample2d(T + ".xy / " + T + ".w") + ";\n";
            break;
        case 2: // projective 3D (or shadow-map compare on a 2D depth texture)
            if (s.tex_type[i] == PSH_TEX_3D)
                o += "  " + t + " = texture(tex" + std::to_string(i) + ", " + T + ".xyz / " + T + ".w);\n";
            else if (tex)
                o += "  { vec4 d = " + sample2d(T + ".xy / " + T + ".w") + "; " + t + " = vec4((" + T + ".z / " + T +
                     ".w) <= d.r ? 1.0 : 0.0); }\n";
            break;
        case 3: // cube map
            if (s.tex_type[i] == PSH_TEX_CUBE)
                o += "  " + t + " = texture(tex" + std::to_string(i) + ", " + T + ".xyz);\n";
            else
                o += "  " + t + " = " + sample2d(T + ".xy") + ";\n";
            break;
        case 4: // pass-through
            o += "  " + t + " = clamp(" + T + ", 0.0, 1.0);\n";
            break;
        case 5: // clip plane
            o += "  if (any(lessThan(" + T + ", vec4(0.0)))) discard;\n";
            break;
        case 6: // bump env map
        case 7: // bump env map + luminance
            o += "  { vec2 dsdt = " + prev + ".rg * 2.0 - 1.0;\n    vec2 off = mat2(bumpMat[" + std::to_string(i) +
                 "]) * dsdt;\n    " + t + " = " + sample2d(T + ".xy + off / texScale[" + std::to_string(i) + "]") +
                 ";\n";
            if (mode == 7)
                o += "    " + t + ".rgb *= " + prev + ".b * bumpScale[" + std::to_string(i) + "] + bumpOffset[" +
                     std::to_string(i) + "];\n";
            o += "  }\n";
            break;
        case 9:  // dot ST
        case 10: // dot ZW
        case 11: // dot reflect diffuse
        case 12: // dot reflect specular
        case 13: // dot STR 3D
        case 14: // dot STR cube
        case 17: // dot product
        case 18: // dot reflect specular (constant eye)
        {
            std::string d = "dot" + std::to_string(i);
            if (i == 0) break;
            o += "  " + d + " = dot(" + T + ".xyz, " + prev_mapped + ");\n";
            if (mode == 9)
                o += "  " + t + " = " + sample2d("vec2(dot" + std::to_string(i - 1) + ", " + d + ")") + ";\n";
            else if (mode == 10)
                o += "  gl_FragDepth = clamp(dot" + std::to_string(i - 1) + " / " + d + ", 0.0, 1.0);\n";
            else if (mode == 11 || mode == 14)
                o += "  " + t + " = texture(tex" + std::to_string(i) + ", vec3(dot1, dot2, dot3));\n";
            else if (mode == 13)
                o += "  " + t + " = texture(tex" + std::to_string(i) + ", vec3(dot1, dot2, dot3));\n";
            else if (mode == 12 || mode == 18) {
                std::string e = mode == 12 ? "vec3(vtx_T1.w, vtx_T2.w, vtx_T3.w)" : "eyeVec";
                o += "  { vec3 n = vec3(dot1, dot2, dot3); vec3 e = " + e +
                     "; vec3 r = 2.0 * n * dot(n, e) / max(dot(n, n), 1e-6) - e; " + t + " = texture(tex" +
                     std::to_string(i) + ", r); }\n";
            }
            break;
        }
        case 15: // dependent AR
            o += "  " + t + " = " + sample2d(prev + ".ar") + ";\n";
            break;
        case 16: // dependent GB
            o += "  " + t + " = " + sample2d(prev + ".gb") + ";\n";
            break;
        default:
            o += "  // unsupported texture stage mode " + std::to_string(mode) + "\n";
            break;
        }
    }

    // ---- general combiner stages ----------------------------------------------------
    o += "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);\n  vec4 r1 = vec4(0.0);\n";
    uint32_t stages = s.control & 0xFF;
    if (stages > 8)
        stages = 8;
    bool mux_msb = (s.control & 0x100) != 0;
    bool unique_c0 = (s.control & 0x1000) != 0, unique_c1 = (s.control & 0x10000) != 0;
    for (uint32_t i = 0; i < stages; i++) {
        snprintf(buf, sizeof buf, "  { // stage %u\n    vec4 c0 = psConst0[%u];\n    vec4 c1 = psConst1[%u];\n", i,
                 unique_c0 ? i : 0, unique_c1 ? i : 0);
        o += buf;
        uint32_t ci = s.color_icw[i], co = s.color_ocw[i], ai = s.alpha_icw[i], ao = s.alpha_ocw[i];
        // RGB portion
        std::string A = input(ci >> 24, false), B = input((ci >> 16) & 0xFF, false), C = input((ci >> 8) & 0xFF, false),
                    D = input(ci & 0xFF, false);
        uint32_t cflags = co >> 12;
        std::string ab = (cflags & 0x2) ? "vec3(dot(" + A + ", " + B + "))" : A + " * " + B;
        std::string cd = (cflags & 0x1) ? "vec3(dot(" + C + ", " + D + "))" : C + " * " + D;
        std::string mux = mux_msb ? "(r0.a >= 0.5)" : "((int(r0.a * 255.0 + 0.5) & 1) != 0)";
        o += "    vec3 ab = clamp(" + apply_op(ab, cflags) + ", -1.0, 1.0);\n";
        o += "    vec3 cd = clamp(" + apply_op(cd, cflags) + ", -1.0, 1.0);\n";
        if (cflags & 0x4)
            o += "    vec3 sm = clamp(" + apply_op("(" + mux + " ? (" + cd + ") : (" + ab + "))", cflags) +
                 ", -1.0, 1.0);\n";
        else
            o += "    vec3 sm = clamp(" + apply_op("(" + ab + " + " + cd + ")", cflags) + ", -1.0, 1.0);\n";
        // Alpha portion
        std::string aA = input(ai >> 24, true), aB = input((ai >> 16) & 0xFF, true), aC = input((ai >> 8) & 0xFF, true),
                    aD = input(ai & 0xFF, true);
        uint32_t aflags = ao >> 12;
        o += "    float aab = clamp(" + apply_op(aA + " * " + aB, aflags) + ", -1.0, 1.0);\n";
        o += "    float acd = clamp(" + apply_op(aC + " * " + aD, aflags) + ", -1.0, 1.0);\n";
        if (aflags & 0x4)
            o += "    float asm_ = clamp(" + apply_op("(" + mux + " ? (" + aC + " * " + aD + ") : (" + aA + " * " + aB + "))", aflags) +
                 ", -1.0, 1.0);\n";
        else
            o += "    float asm_ = clamp(" + apply_op("(" + aA + " * " + aB + " + " + aC + " * " + aD + ")", aflags) +
                 ", -1.0, 1.0);\n";
        // Writes: RGB then alpha (all inputs were read above).
        auto dst = [](uint32_t r) -> const char * { return reg_name(int(r)); };
        if (const char *d = dst(co & 0xF)) o += std::string("    ") + d + ".rgb = cd;\n";
        if (const char *d = dst((co >> 4) & 0xF)) o += std::string("    ") + d + ".rgb = ab;\n";
        if (const char *d = dst((co >> 8) & 0xF)) o += std::string("    ") + d + ".rgb = sm;\n";
        if (const char *d = dst(ao & 0xF)) o += std::string("    ") + d + ".a = acd;\n";
        if (const char *d = dst((ao >> 4) & 0xF)) o += std::string("    ") + d + ".a = aab;\n";
        if (const char *d = dst((ao >> 8) & 0xF)) o += std::string("    ") + d + ".a = asm_;\n";
        if (cflags & 0x80)
            if (const char *d = dst((co >> 4) & 0xF)) o += std::string("    ") + d + ".a = ab.b;\n";
        if (cflags & 0x40)
            if (const char *d = dst(co & 0xF)) o += std::string("    ") + d + ".a = cd.b;\n";
        o += "  }\n";
    }

    // ---- final combiner ----------------------------------------------------------------
    if (s.final0 == 0 && s.final1 == 0) {
        o += "  fragColor = r0;\n";
    } else {
        o += "  {\n    vec4 c0 = finalConst0;\n    vec4 c1 = finalConst1;\n";
        uint32_t f0 = s.final0, f1 = s.final1;
        bool clamp_sum = (f1 & 0x80) != 0, comp_v1 = (f1 & 0x40) != 0, comp_r0 = (f1 & 0x20) != 0;
        o += std::string("    vec3 sumVRv = ") + (comp_v1 ? "(1.0 - v1.rgb)" : "v1.rgb") + " + " +
             (comp_r0 ? "(1.0 - r0.rgb)" : "r0.rgb") + ";\n";
        o += std::string("    vec4 sumVR = vec4(") + (clamp_sum ? "clamp(sumVRv, 0.0, 1.0)" : "sumVRv") + ", 0.0);\n";
        auto fin = [&](uint32_t sel, bool alpha) {
            int reg = sel & 0xF;
            bool ach = (sel & 0x10) != 0;
            bool inv = (sel & 0x20) != 0;
            const char *name = reg_name(reg);
            std::string v;
            if (!name)
                v = alpha ? "0.0" : "vec3(0.0)";
            else if (alpha)
                v = std::string(name) + (ach ? ".a" : ".b");
            else
                v = ach ? "vec3(" + std::string(name) + ".a)" : std::string(name) + ".rgb";
            v = "clamp(" + v + ", 0.0, 1.0)";
            if (inv)
                v = (alpha ? "(1.0 - " : "(vec3(1.0) - ") + v + ")";
            return v;
        };
        o += "    vec3 fE = " + fin(f1 >> 24, false) + ";\n";
        o += "    vec3 fF = " + fin((f1 >> 16) & 0xFF, false) + ";\n";
        o += "    vec4 prodEF = vec4(fE * fF, 0.0);\n";
        o += "    vec3 fA = " + fin(f0 >> 24, false) + ";\n";
        o += "    vec3 fB = " + fin((f0 >> 16) & 0xFF, false) + ";\n";
        o += "    vec3 fC = " + fin((f0 >> 8) & 0xFF, false) + ";\n";
        o += "    vec3 fD = " + fin(f0 & 0xFF, false) + ";\n";
        o += "    float fG = " + fin((f1 >> 8) & 0xFF, true) + ";\n";
        o += "    fragColor = vec4(fA * fB + (vec3(1.0) - fA) * fC + fD, fG);\n  }\n";
    }

    // ---- alpha test -------------------------------------------------------------------
    if (s.alpha_func) {
        static const char *ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        int f = s.alpha_func - 1;
        if (f == 0)
            o += "  discard;\n";
        else if (f != 7) {
            snprintf(buf, sizeof buf, "  if (!(fragColor.a %s alphaRef)) discard;\n", ops[f]);
            o += buf;
        }
    }
    o += "}\n";
    return o;
}

} // namespace rt
