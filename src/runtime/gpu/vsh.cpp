// NV2A vertex program translation.
//
// Each instruction is 128 bits and pairs a MAC (multiply/accumulate) op with
// an ILU (special function) op. Operands A, B, C each select a temporary
// register, a vertex attribute or a constant (all three share one attribute
// index and one constant index), with per-operand swizzle and negation.
#include "vsh.h"

#include <cstdio>

namespace rt {

namespace {

struct Field {
    int word, pos, bits;
};
// Field layout of an instruction (word 0 is unused).
constexpr Field F_ILU{1, 25, 3}, F_MAC{1, 21, 4}, F_CONST{1, 13, 8}, F_V{1, 9, 4};
constexpr Field F_A_NEG{1, 8, 1}, F_A_SWZ_X{1, 6, 2}, F_A_SWZ_Y{1, 4, 2}, F_A_SWZ_Z{1, 2, 2}, F_A_SWZ_W{1, 0, 2};
constexpr Field F_A_R{2, 28, 4}, F_A_MUX{2, 26, 2};
constexpr Field F_B_NEG{2, 25, 1}, F_B_SWZ_X{2, 23, 2}, F_B_SWZ_Y{2, 21, 2}, F_B_SWZ_Z{2, 19, 2}, F_B_SWZ_W{2, 17, 2};
constexpr Field F_B_R{2, 13, 4}, F_B_MUX{2, 11, 2};
constexpr Field F_C_NEG{2, 10, 1}, F_C_SWZ_X{2, 8, 2}, F_C_SWZ_Y{2, 6, 2}, F_C_SWZ_Z{2, 4, 2}, F_C_SWZ_W{2, 2, 2};
constexpr Field F_C_R_HIGH{2, 0, 2}, F_C_R_LOW{3, 30, 2}, F_C_MUX{3, 28, 2};
constexpr Field F_OUT_MAC_MASK{3, 24, 4}, F_OUT_R{3, 20, 4}, F_OUT_ILU_MASK{3, 16, 4}, F_OUT_O_MASK{3, 12, 4};
constexpr Field F_OUT_ORB{3, 11, 1}, F_OUT_ADDRESS{3, 3, 8}, F_OUT_MUX{3, 2, 1}, F_A0X{3, 1, 1}, F_FINAL{3, 0, 1};

uint32_t get(const uint32_t *ins, Field f)
{
    return (ins[f.word] >> f.pos) & ((1u << f.bits) - 1);
}

enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT,
       MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

const char *k_out_reg[16] = {"oPos", "oUnk1", "oUnk2", "oD0", "oD1", "oFog", "oPts", "oB0",
                             "oB1",  "oT0",   "oT1",   "oT2", "oT3", "oUnk13", "oUnk14", "oUnk15"};

std::string swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    static const char c[] = "xyzw";
    if (x == 0 && y == 1 && z == 2 && w == 3)
        return "";
    std::string s = ".";
    s += c[x];
    s += c[y];
    s += c[z];
    s += c[w];
    return s;
}

std::string mask_str(uint32_t m)
{
    std::string s = ".";
    if (m & 8) s += 'x';
    if (m & 4) s += 'y';
    if (m & 2) s += 'z';
    if (m & 1) s += 'w';
    return s;
}

std::string operand(const uint32_t *ins, Field mux, Field r, Field neg, Field sx, Field sy, Field sz, Field sw,
                    uint32_t c_reg)
{
    std::string base;
    uint32_t m = get(ins, mux);
    switch (m) {
    case MUX_R: {
        uint32_t reg = c_reg != 0xFFFFFFFF ? c_reg : get(ins, r);
        base = reg == 12 ? "oPos" : "R" + std::to_string(reg);
        break;
    }
    case MUX_V:
        base = "v" + std::to_string(get(ins, F_V));
        break;
    case MUX_C: {
        uint32_t ci = get(ins, F_CONST);
        if (get(ins, F_A0X))
            base = "c[clamp(A0 + " + std::to_string(ci) + ", 0, 191)]";
        else
            base = "c[" + std::to_string(ci) + "]";
        break;
    }
    default:
        base = "vec4(0.0)";
        break;
    }
    std::string s = base + swizzle(get(ins, sx), get(ins, sy), get(ins, sz), get(ins, sw));
    if (get(ins, neg))
        s = "-" + s;
    return "(" + s + ")";
}

} // namespace

std::string vsh_translate(const uint32_t (*program)[4], uint32_t start, uint32_t length, uint64_t *hash_out)
{
    std::string out;
    uint64_t h = 0xcbf29ce484222325ull;
    char buf[512];
    for (uint32_t pc = start; pc < length; pc++) {
        const uint32_t *ins = program[pc];
        for (int i = 0; i < 4; i++) {
            h ^= ins[i];
            h *= 0x100000001b3ull;
        }
        uint32_t mac = get(ins, F_MAC), ilu = get(ins, F_ILU);
        std::string a = operand(ins, F_A_MUX, F_A_R, F_A_NEG, F_A_SWZ_X, F_A_SWZ_Y, F_A_SWZ_Z, F_A_SWZ_W, 0xFFFFFFFF);
        std::string b = operand(ins, F_B_MUX, F_B_R, F_B_NEG, F_B_SWZ_X, F_B_SWZ_Y, F_B_SWZ_Z, F_B_SWZ_W, 0xFFFFFFFF);
        uint32_t c_r = (get(ins, F_C_R_HIGH) << 2) | get(ins, F_C_R_LOW);
        std::string c = operand(ins, F_C_MUX, F_C_R_HIGH, F_C_NEG, F_C_SWZ_X, F_C_SWZ_Y, F_C_SWZ_Z, F_C_SWZ_W, c_r);

        snprintf(buf, sizeof buf, "  // %03u: %08X %08X %08X\n", pc, ins[1], ins[2], ins[3]);
        out += buf;
        std::string mac_expr, ilu_expr;
        switch (mac) {
        case MAC_NOP: break;
        case MAC_MOV: mac_expr = a; break;
        case MAC_MUL: mac_expr = a + " * " + b; break;
        case MAC_ADD: mac_expr = a + " + " + c; break;
        case MAC_MAD: mac_expr = a + " * " + b + " + " + c; break;
        case MAC_DP3: mac_expr = "vec4(dot(" + a + ".xyz, " + b + ".xyz))"; break;
        case MAC_DPH: mac_expr = "vec4(dot(vec4(" + a + ".xyz, 1.0), " + b + "))"; break;
        case MAC_DP4: mac_expr = "vec4(dot(" + a + ", " + b + "))"; break;
        case MAC_DST: mac_expr = "vsh_dst(" + a + ", " + b + ")"; break;
        case MAC_MIN: mac_expr = "min(" + a + ", " + b + ")"; break;
        case MAC_MAX: mac_expr = "max(" + a + ", " + b + ")"; break;
        case MAC_SLT: mac_expr = "vec4(lessThan(" + a + ", " + b + "))"; break;
        case MAC_SGE: mac_expr = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
        case MAC_ARL: break;
        default: mac_expr = "vec4(0.0)"; break;
        }
        switch (ilu) {
        case ILU_NOP: break;
        case ILU_MOV: ilu_expr = c; break;
        case ILU_RCP: ilu_expr = "vec4(vsh_rcp(" + c + ".x))"; break;
        case ILU_RCC: ilu_expr = "vec4(vsh_rcc(" + c + ".x))"; break;
        case ILU_RSQ: ilu_expr = "vec4(inversesqrt(abs(" + c + ".x)))"; break;
        case ILU_EXP: ilu_expr = "vsh_exp(" + c + ".x)"; break;
        case ILU_LOG: ilu_expr = "vsh_log(" + c + ".x)"; break;
        case ILU_LIT: ilu_expr = "vsh_lit(" + c + ")"; break;
        default: break;
        }

        out += "  {\n";
        if (!mac_expr.empty())
            out += "    vec4 mac = " + mac_expr + ";\n";
        if (!ilu_expr.empty())
            out += "    vec4 ilu = " + ilu_expr + ";\n";
        if (mac == MAC_ARL)
            out += "    A0 = int(floor(" + a + ".x + 0.001));\n";

        uint32_t mac_mask = get(ins, F_OUT_MAC_MASK), ilu_mask = get(ins, F_OUT_ILU_MASK);
        uint32_t o_mask = get(ins, F_OUT_O_MASK), out_r = get(ins, F_OUT_R);
        if (!mac_expr.empty() && mac_mask) {
            std::string dst = out_r == 12 ? "oPos" : "R" + std::to_string(out_r);
            out += "    " + dst + mask_str(mac_mask) + " = mac" + mask_str(mac_mask) + ";\n";
        }
        if (!ilu_expr.empty() && ilu_mask) {
            // When paired with a MAC op the ILU result goes to R1.
            uint32_t r = (mac != MAC_NOP) ? 1 : out_r;
            std::string dst = r == 12 ? "oPos" : "R" + std::to_string(r);
            out += "    " + dst + mask_str(ilu_mask) + " = ilu" + mask_str(ilu_mask) + ";\n";
        }
        if (o_mask) {
            bool use_ilu = get(ins, F_OUT_MUX) != 0;
            const std::string &src = use_ilu ? ilu_expr : mac_expr;
            if (!src.empty()) {
                uint32_t addr = get(ins, F_OUT_ADDRESS);
                std::string dst;
                if (get(ins, F_OUT_ORB))
                    dst = k_out_reg[addr & 15];
                else
                    dst = "cwrite"; // constant write-back (uniforms are read-only; rarely used)
                out += "    " + dst + mask_str(o_mask) + " = " + (use_ilu ? "ilu" : "mac") + mask_str(o_mask) + ";\n";
            }
        }
        out += "  }\n";
        if (get(ins, F_FINAL))
            break;
    }
    if (hash_out)
        *hash_out = h;
    return out;
}

} // namespace rt
