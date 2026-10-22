#include <algorithm>
// dxbc.cpp -- DXBC (SM4/SM5 subset) to MSL. The shaders a D3D11 title ships are small, so the
// translation is direct: every D3D register is a float4 (integer instructions reinterpret its
// bits), instructions become one statement each, and control flow maps onto structured MSL.
#include "gpu/dxbc.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace utsuro::gpu {

namespace {

// ---- opcodes (D3D10_SB_OPCODE_TYPE / D3D11) ----------------------------------------------
enum Op : uint32_t {
    OP_ADD = 0, OP_AND = 1, OP_BREAK = 2, OP_BREAKC = 3, OP_CALL = 4, OP_CALLC = 5, OP_CASE = 6, OP_CONTINUE = 7,
    OP_CONTINUEC = 8, OP_CUT = 9, OP_DEFAULT = 10, OP_DERIV_RTX = 11, OP_DERIV_RTY = 12, OP_DISCARD = 13, OP_DIV = 14,
    OP_DP2 = 15, OP_DP3 = 16, OP_DP4 = 17, OP_ELSE = 18, OP_EMIT = 19, OP_EMITTHENCUT = 20, OP_ENDIF = 21,
    OP_ENDLOOP = 22, OP_ENDSWITCH = 23, OP_EQ = 24, OP_EXP = 25, OP_FRC = 26, OP_FTOI = 27, OP_FTOU = 28, OP_GE = 29,
    OP_IADD = 30, OP_IF = 31, OP_IEQ = 32, OP_IGE = 33, OP_ILT = 34, OP_IMAD = 35, OP_IMAX = 36, OP_IMIN = 37,
    OP_IMUL = 38, OP_INE = 39, OP_INEG = 40, OP_ISHL = 41, OP_ISHR = 42, OP_ITOF = 43, OP_LABEL = 44, OP_LD = 45,
    OP_LD_MS = 46, OP_LOG = 47, OP_LOOP = 48, OP_LT = 49, OP_MAD = 50, OP_MIN = 51, OP_MAX = 52, OP_CUSTOMDATA = 53,
    OP_MOV = 54, OP_MOVC = 55, OP_MUL = 56, OP_NE = 57, OP_NOP = 58, OP_NOT = 59, OP_OR = 60, OP_RESINFO = 61,
    OP_RET = 62, OP_RETC = 63, OP_ROUND_NE = 64, OP_ROUND_NI = 65, OP_ROUND_PI = 66, OP_ROUND_Z = 67, OP_RSQ = 68,
    OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_C_LZ = 71, OP_SAMPLE_L = 72, OP_SAMPLE_D = 73, OP_SAMPLE_B = 74,
    OP_SQRT = 75, OP_SWITCH = 76, OP_SINCOS = 77, OP_UDIV = 78, OP_ULT = 79, OP_UGE = 80, OP_UMUL = 81, OP_UMAD = 82,
    OP_UMAX = 83, OP_UMIN = 84, OP_USHR = 85, OP_UTOF = 86, OP_XOR = 87,
    OP_DCL_RESOURCE = 88, OP_DCL_CONSTANT_BUFFER = 89, OP_DCL_SAMPLER = 90, OP_DCL_INDEX_RANGE = 91,
    OP_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY = 92, OP_DCL_GS_INPUT_PRIMITIVE = 93, OP_DCL_MAX_OUTPUT_VERTEX_COUNT = 94,
    OP_DCL_INPUT = 95, OP_DCL_INPUT_SGV = 96, OP_DCL_INPUT_SIV = 97, OP_DCL_INPUT_PS = 98, OP_DCL_INPUT_PS_SGV = 99,
    OP_DCL_INPUT_PS_SIV = 100, OP_DCL_OUTPUT = 101, OP_DCL_OUTPUT_SGV = 102, OP_DCL_OUTPUT_SIV = 103, OP_DCL_TEMPS = 104,
    OP_DCL_INDEXABLE_TEMP = 105, OP_DCL_GLOBAL_FLAGS = 106,
    OP_RCP = 129,   // SM5
    OP_EVAL_SAMPLE_INDEX = 204,
    OP_SAMPLEPOS = 110, OP_SAMPLE_INFO = 111, OP_GATHER4 = 109,
    OP_DERIV_RTX_COARSE = 122, OP_DERIV_RTX_FINE = 123, OP_DERIV_RTY_COARSE = 124, OP_DERIV_RTY_FINE = 125,
    OP_GATHER4_ALT = 108, OP_GATHER4_C = 126,
    OP_F32TO16 = 130, OP_F16TOF32 = 131,
    OP_COUNTBITS = 134, OP_FIRSTBIT_HI = 135, OP_FIRSTBIT_LO = 136, OP_FIRSTBIT_SHI = 137,
    OP_UBFE = 138, OP_IBFE = 139, OP_BFI = 140, OP_BFREV = 141, OP_SWAPC = 142,
    OP_DCL_THREAD_GROUP = 155,
    OP_DCL_UAV_TYPED = 156, OP_DCL_UAV_RAW = 157, OP_DCL_UAV_STRUCTURED = 158,
    OP_DCL_TGSM_RAW = 159, OP_DCL_TGSM_STRUCTURED = 160,
    OP_SYNC = 190,
    OP_DCL_RESOURCE_RAW = 161, OP_DCL_RESOURCE_STRUCTURED = 162,
    OP_LD_UAV_TYPED = 163, OP_STORE_UAV_TYPED = 164, OP_LD_RAW = 165, OP_STORE_RAW = 166,
    OP_LD_STRUCTURED = 167, OP_STORE_STRUCTURED = 168,
    OP_ATOMIC_AND = 169, OP_ATOMIC_OR = 170, OP_ATOMIC_XOR = 171, OP_ATOMIC_CMP_STORE = 172,
    OP_ATOMIC_IADD = 173, OP_ATOMIC_IMAX = 174, OP_ATOMIC_IMIN = 175, OP_ATOMIC_UMAX = 176, OP_ATOMIC_UMIN = 177,
    OP_IMM_ATOMIC_ALLOC = 178, OP_IMM_ATOMIC_CONSUME = 179,
    OP_IMM_ATOMIC_IADD = 180, OP_IMM_ATOMIC_AND = 181, OP_IMM_ATOMIC_OR = 182, OP_IMM_ATOMIC_XOR = 183,
    OP_IMM_ATOMIC_EXCH = 184, OP_IMM_ATOMIC_CMP_EXCH = 185, OP_IMM_ATOMIC_IMAX = 186, OP_IMM_ATOMIC_IMIN = 187,
    OP_IMM_ATOMIC_UMAX = 188, OP_IMM_ATOMIC_UMIN = 189,
    OP_SAMPLE_CLAMP_FEEDBACK = 230,   // D3D11.2 tiled-resource sample with a minimum LOD; the feedback status is not tracked
};

enum OperandType : uint32_t {
    T_TEMP = 0, T_INPUT = 1, T_OUTPUT = 2, T_INDEXABLE_TEMP = 3, T_IMM32 = 4, T_IMM64 = 5, T_SAMPLER = 6, T_RESOURCE = 7,
    T_CONSTANT_BUFFER = 8, T_ICB = 9, T_LABEL = 10, T_INPUT_PRIMITIVE_ID = 11, T_OUTPUT_DEPTH = 12, T_NULL = 13,
    T_RASTERIZER = 14, T_OUTPUT_COVERAGE = 15, T_UAV = 30, T_TGSM = 31,
    T_INPUT_THREAD_ID = 32, T_INPUT_THREAD_GROUP_ID = 33, T_INPUT_THREAD_ID_IN_GROUP = 34,
    T_INPUT_COVERAGE_MASK = 35, T_INPUT_THREAD_ID_IN_GROUP_FLATTENED = 36,
};

struct Operand;
using OperandPtr = std::shared_ptr<Operand>;

struct Operand {
    uint32_t type {0};
    uint32_t components {0};     // 0 none, 1 scalar, 4 vector
    uint32_t select_mode {0};    // 0 mask, 1 swizzle, 2 select one
    uint32_t bits {0};           // mask / swizzle / select bits
    uint32_t modifier {0};       // 0 none, 1 neg, 2 abs, 3 abs+neg
    uint32_t index_dim {0};
    uint32_t index[3] {};
    OperandPtr relative[3];
    uint32_t imm[4] {};
};

struct Decoder {
    const uint32_t* p;
    const uint32_t* end;
    bool ok {true};
    std::string error;

    uint32_t Next() {
        if (p >= end) { ok = false; error = "token stream ended inside an instruction"; return 0; }
        return *p++;
    }

    OperandPtr ReadOperand() {
        auto o = std::make_shared<Operand>();
        const uint32_t t = Next();
        o->components = (t & 3) == 0 ? 0 : (t & 3) == 1 ? 1 : 4;
        o->type = (t >> 12) & 0xFF;
        if (o->type == 39 || o->type == 40) o->type = T_OUTPUT_DEPTH;   // oDepthGE / oDepthLE: written like oDepth
        o->index_dim = (t >> 20) & 3;
        if (o->components == 4) {
            o->select_mode = (t >> 2) & 3;
            o->bits = (t >> 4) & 0xFF;
        }
        if (t & 0x80000000u) {   // extended operand token
            uint32_t e = Next();
            while (true) {
                if ((e & 0x3F) == 1) o->modifier = (e >> 6) & 0xFF;
                if (!(e & 0x80000000u)) break;
                e = Next();
            }
        }
        if (o->type == T_IMM32) {
            const int n = o->components == 1 ? 1 : 4;
            for (int i = 0; i < n; ++i) o->imm[i] = Next();
        } else if (o->type == T_IMM64) {
            const int n = o->components == 1 ? 2 : 8;
            for (int i = 0; i < n; ++i) Next();
        }
        for (uint32_t d = 0; d < o->index_dim; ++d) {
            const uint32_t repr = (t >> (22 + 3 * d)) & 7;
            if (repr == 0) o->index[d] = Next();
            else if (repr == 1) { o->index[d] = Next(); Next(); }
            else if (repr == 2) o->relative[d] = ReadOperand();
            else if (repr == 3) { o->index[d] = Next(); o->relative[d] = ReadOperand(); }
            else if (repr == 4) { o->index[d] = Next(); Next(); o->relative[d] = ReadOperand(); }
        }
        return o;
    }
};

const char kComp[] = "xyzw";

struct SigRegister {
    // The signature elements packed into one register (each owns some components).
    std::vector<const SignatureElement*> elements;
};

// Metal gives a stage 31 buffer slots. Constant buffers take 0..13, the vertex stage's vertex buffers 15..23; buffer
// SRVs (structured and raw) get the rest in declaration order, and the renderer binds them through
// ShaderReflection::buffer_index. Compute keeps its fixed 14 + slot layout.
template <typename T>
uint32_t NextBufferIndex(T& t) {
    uint32_t first = t.stage == ShaderStage::Vertex ? 24u : 14u;
    for (uint32_t b = 14; b < 16; ++b) if (t.refl.constant_buffers & (1u << b)) first = std::max(first, b + 1);   // cb14/cb15 exist on the console
    uint32_t used = 0;
    for (int i = 0; i < 128; ++i) if (t.refl.buffer_index[i] != 0) ++used;
    for (int i = 0; i < 64; ++i) if (t.refl.uav_index[i] != 0) ++used;
    for (int i = 0; i < 64; ++i) if (t.refl.counter_index[i] != 0) ++used;
    return std::min(first + used, 30u);
}
template <typename T>
uint32_t BufferIndexFor(T& t, uint32_t slot) {
    if (slot < 128 && t.refl.buffer_index[slot] != 0) return t.refl.buffer_index[slot];
    const uint32_t index = NextBufferIndex(t);
    if (slot < 128) t.refl.buffer_index[slot] = static_cast<uint8_t>(index);
    return index;
}
template <typename T>
uint32_t CounterIndexFor(T& t, uint32_t slot) {
    if (slot < 64 && t.refl.counter_index[slot] != 0) return t.refl.counter_index[slot];
    const uint32_t index = NextBufferIndex(t);
    if (slot < 64) t.refl.counter_index[slot] = static_cast<uint8_t>(index);
    return index;
}
template <typename T>
uint32_t UavBufferIndexFor(T& t, uint32_t slot) {
    if (slot < 64 && t.refl.uav_index[slot] != 0) return t.refl.uav_index[slot];
    const uint32_t index = NextBufferIndex(t);
    if (slot < 64) t.refl.uav_index[slot] = static_cast<uint8_t>(index);
    return index;
}

struct Translator {
    ShaderStage stage {ShaderStage::Unknown};
    ShaderReflection refl;
    std::ostringstream body;
    std::ostringstream globals;
    std::string error;
    int indent {1};
    uint32_t temps {0};
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> indexable;   // reg -> (size, components)
    std::map<uint32_t, uint32_t> resource_dim;                      // t# -> dimension
    std::map<uint32_t, uint32_t> resource_stride;                   // t# -> byte stride
    std::map<uint32_t, uint32_t> tgsm_stride, tgsm_count;
    std::set<uint32_t> tgsm_raw;   ///< raw (byte-addressed) thread group memory, as opposed to structured
    std::map<uint32_t, uint32_t> uav_stride;
    std::map<uint32_t, uint32_t> uav_dim;
    bool uses_structured {false}, uses_f32to16 {false}, uses_f16tof32 {false};
    std::map<uint32_t, uint32_t> input_sgv;                          // register -> sysval
    std::map<uint32_t, uint32_t> output_sgv;
    std::set<uint32_t> input_regs, output_regs;
    std::map<uint32_t, uint32_t> input_masks, output_masks;
    std::map<uint32_t, SigRegister> sig_in, sig_out;
    std::vector<uint32_t> icb;                                       // immediate constant buffer, in dwords
    bool uses_vertex_id {false}, uses_instance_id {false}, uses_front_face {false}, uses_prim_id {false};
    uint32_t pos_input_reg {~0u};
    uint32_t pos_output_reg {~0u};
    int loop_depth {0};

    void Line(const std::string& s) { body << std::string(static_cast<size_t>(indent) * 4, ' ') << s << "\n"; }
    void Fail(const std::string& e) { if (error.empty()) error = e; }

    // ---- operand text ------------------------------------------------------------------
    static std::string Swizzle(const Operand& o, int count = 4) {
        std::string s;
        if (o.components == 4) {
            if (o.select_mode == 1) {
                for (int i = 0; i < count; ++i) s += kComp[(o.bits >> (2 * i)) & 3];
            } else if (o.select_mode == 2) {
                for (int i = 0; i < count; ++i) s += kComp[o.bits & 3];
            } else {
                // A source with a write mask: use the components in order.
                int taken = 0;
                for (int i = 0; i < 4 && taken < count; ++i) if (o.bits & (1u << i)) { s += kComp[i]; ++taken; }
                while (static_cast<int>(s.size()) < count) s += s.empty() ? 'x' : s.back();
            }
        } else {
            for (int i = 0; i < count; ++i) s += 'x';
        }
        return s;
    }

    std::string RelIndex(const Operand& rel) {
        // The relative register's first selected component, as an int.
        return "as_type<int>(" + RegisterBase(rel) + "." + Swizzle(rel, 1) + ")";
    }

    std::string RegisterBase(const Operand& o) {
        char buf[64];
        switch (o.type) {
        case T_TEMP: std::snprintf(buf, sizeof buf, "r%u", o.index[0]); return buf;
        case T_INPUT: std::snprintf(buf, sizeof buf, "v%u", o.index[0]); return buf;
        case T_OUTPUT: std::snprintf(buf, sizeof buf, "o%u", o.index[0]); return buf;
        case T_OUTPUT_DEPTH: return "float4(oDepth)";
        case T_UAV: std::snprintf(buf, sizeof buf, "u%u", o.index[0]); return buf;
        case T_INDEXABLE_TEMP: {
            std::snprintf(buf, sizeof buf, "x%u", o.index[0]);
            std::string s = buf;
            std::string idx = o.relative[1] ? "(" + RelIndex(*o.relative[1]) + " + " + std::to_string(o.index[1]) + ")" : std::to_string(o.index[1]);
            return s + "[" + idx + "]";
        }
        case T_CONSTANT_BUFFER: {
            std::snprintf(buf, sizeof buf, "cb%u", o.index[0]);
            std::string idx = o.relative[1] ? "(" + RelIndex(*o.relative[1]) + " + " + std::to_string(o.index[1]) + ")" : std::to_string(o.index[1]);
            return std::string(buf) + "[" + idx + "]";
        }
        case T_INPUT_COVERAGE_MASK: return "as_type<float4>(uint4(0xFFFFFFFFu))";   // every sample covered
        case T_INPUT_THREAD_ID: return "as_type<float4>(uint4(ut_tid, 0u))";
        case T_INPUT_THREAD_GROUP_ID: return "as_type<float4>(uint4(ut_gid, 0u))";
        case T_INPUT_THREAD_ID_IN_GROUP: return "as_type<float4>(uint4(ut_gtid, 0u))";
        case T_INPUT_THREAD_ID_IN_GROUP_FLATTENED: return "as_type<float4>(uint4(ut_gidx, 0u, 0u, 0u))";
        case T_ICB: {
            std::string idx = o.relative[0] ? "(" + RelIndex(*o.relative[0]) + " + " + std::to_string(o.index[0]) + ")" : std::to_string(o.index[0]);
            return "icb[" + idx + "]";
        }
        default: return "float4(0)";
        }
    }

    /// A source as a float4 expression (raw bits; modifiers applied as floats).
    std::string SrcF(const Operand& o) {
        std::string e;
        if (o.type == T_IMM32) {
            char buf[160];
            if (o.components == 1) {
                std::snprintf(buf, sizeof buf, "as_type<float4>(uint4(0x%xu))", o.imm[0]);
                e = buf;
            } else {
                std::snprintf(buf, sizeof buf, "as_type<float4>(uint4(0x%xu, 0x%xu, 0x%xu, 0x%xu))", o.imm[0], o.imm[1], o.imm[2], o.imm[3]);
                e = buf;
                // An immediate vector has no swizzle: its four values are used in order.
            }
        } else if (o.type == T_OUTPUT_DEPTH) {
            e = "float4(oDepth)";
        } else {
            e = RegisterBase(o) + "." + Swizzle(o);
        }
        if (o.modifier & 2) e = "abs(" + e + ")";
        if (o.modifier & 1) e = "-" + e;
        return e;
    }
    /// A source reinterpreted as int4 / uint4 (integer modifiers).
    std::string SrcI(const Operand& o, bool is_unsigned) {
        Operand raw = o;
        raw.modifier = 0;
        std::string e = std::string("as_type<") + (is_unsigned ? "uint4" : "int4") + ">(" + SrcF(raw) + ")";
        if (o.modifier & 2) e = "abs(" + e + ")";
        if (o.modifier & 1) e = "-" + e;
        return e;
    }
    std::string Scalar(const Operand& o) { Operand c = o; return SrcF(c) + ".x"; }
    std::string SrcU(const Operand& o) { return SrcI(o, true); }

    // ---- destinations ------------------------------------------------------------------
    struct Dest { std::string reg; std::string mask; bool valid {false}; };
    Dest DestOf(const Operand& o) {
        Dest d;
        if (o.type == T_NULL || o.type == T_OUTPUT_COVERAGE || o.type == T_UAV) return d;   // oMask and UAV writes not modelled directly
        d.valid = true;
        if (o.type == T_OUTPUT_DEPTH) { d.reg = "oDepth"; d.mask = ""; return d; }
        d.reg = RegisterBase(o);
        uint32_t m = o.components == 4 ? (o.select_mode == 0 ? o.bits : 0xF) : 0xF;
        if (o.components == 4 && o.select_mode == 2) m = 1u << (o.bits & 3);
        for (int i = 0; i < 4; ++i) if (m & (1u << i)) d.mask += kComp[i];
        return d;
    }
    void Write(const Operand& dst, bool saturate, const std::string& value_f4) {
        Dest d = DestOf(dst);
        if (!d.valid) return;
        std::string v = value_f4;
        if (saturate) v = "saturate(" + v + ")";
        if (d.mask.empty()) { Line(d.reg + " = (" + v + ").x;"); return; }
        if (d.mask == "xyzw") { Line(d.reg + " = " + v + ";"); return; }
        Line("{ float4 t_ = " + v + "; " + d.reg + "." + d.mask + " = t_." + d.mask + "; }");
    }
    /// Writes an integer-typed (int4/uint4) result.
    void WriteBits(const Operand& dst, const std::string& value_int4) { Write(dst, false, "as_type<float4>(" + value_int4 + ")"); }

    // ---- declarations ------------------------------------------------------------------
    std::string TextureType(uint32_t dim) {
        switch (dim) {
        case 2: return "texture1d<float>";
        case 3: return "texture2d<float>";
        case 4: return "texture2d_ms<float>";
        case 5: return "texture3d<float>";
        case 6: return "texturecube<float>";
        case 7: return "texture1d_array<float>";
        case 8: return "texture2d_array<float>";
        case 9: return "texture2d_ms_array<float>";
        case 10: return "texturecube_array<float>";
        default: return "texture2d<float>";
        }
    }

    /// The coordinate expression for sampling `dim` from float4 `c`.
    std::string Coord(uint32_t dim, const std::string& c) {
        switch (dim) {
        case 2: return "(" + c + ").x";
        case 5: case 6: return "(" + c + ").xyz";
        default: return "(" + c + ").xy";
        }
    }
    std::string ArraySlice(uint32_t dim, const std::string& c) {
        if (dim == 8 || dim == 10) return ", uint(rint((" + c + ").z))";
        if (dim == 7) return ", uint(rint((" + c + ").y))";
        return "";
    }
};

std::string FloatLiteral(float f) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s + "f";
}

// ---- container parsing ----------------------------------------------------------------
struct Chunk { uint32_t tag; const uint8_t* data; uint32_t size; };

std::vector<Chunk> ParseContainer(const uint8_t* d, size_t n, bool* is_container) {
    std::vector<Chunk> out;
    *is_container = n >= 32 && std::memcmp(d, "DXBC", 4) == 0;
    if (!*is_container) return out;
    uint32_t count;
    std::memcpy(&count, d + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * (i + 1) <= n; ++i) {
        uint32_t off;
        std::memcpy(&off, d + 32 + 4 * i, 4);
        if (off + 8 > n) continue;
        Chunk c;
        std::memcpy(&c.tag, d + off, 4);
        std::memcpy(&c.size, d + off + 4, 4);
        c.data = d + off + 8;
        if (off + 8 + c.size > n) continue;
        out.push_back(c);
    }
    return out;
}

constexpr uint32_t Tag(const char (&s)[5]) { return static_cast<uint32_t>(s[0]) | (static_cast<uint32_t>(s[1]) << 8) | (static_cast<uint32_t>(s[2]) << 16) | (static_cast<uint32_t>(s[3]) << 24); }

std::vector<SignatureElement> ParseSignature(const Chunk& c) {
    std::vector<SignatureElement> out;
    if (c.size < 8) return out;
    uint32_t count;
    std::memcpy(&count, c.data, 4);
    const bool has_stream = c.tag == Tag("OSG5");
    const uint32_t stride = has_stream ? 32 : 24;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = c.data + 8 + i * stride;
        if (e + stride > c.data + c.size) break;
        uint32_t base = has_stream ? 4 : 0;
        uint32_t name_off, index, sysval, comp, reg, masks;
        std::memcpy(&name_off, e + base, 4);
        std::memcpy(&index, e + base + 4, 4);
        std::memcpy(&sysval, e + base + 8, 4);
        std::memcpy(&comp, e + base + 12, 4);
        std::memcpy(&reg, e + base + 16, 4);
        std::memcpy(&masks, e + base + 20, 4);
        SignatureElement s;
        if (name_off < c.size) {
            for (const char* q = reinterpret_cast<const char*>(c.data) + name_off; *q && q < reinterpret_cast<const char*>(c.data) + c.size; ++q)
                s.semantic += static_cast<char>(std::toupper(static_cast<unsigned char>(*q)));
        }
        s.index = index;
        s.reg = reg;
        s.mask = masks & 0xF;
        s.system_value = sysval;
        // Older compilers name the front-face input VFACE without marking it as a system value.
        if (s.system_value == 0 && (s.semantic == "VFACE" || s.semantic == "SV_ISFRONTFACE")) s.system_value = 9;
        s.component_type = comp;
        out.push_back(s);
    }
    return out;
}

} // namespace

TranslatedShader TranslateDxbc(const uint8_t* data, size_t size, const std::vector<uint8_t>* input_classes) {
    auto input_class = [&](uint32_t reg) -> int { return input_classes != nullptr && reg < input_classes->size() ? (*input_classes)[reg] : 0; };
    TranslatedShader result;
    bool container = false;
    const auto chunks = ParseContainer(data, size, &container);
    const uint32_t* tokens = nullptr;
    size_t token_count = 0;
    std::vector<SignatureElement> in_sig, out_sig;
    if (container) {
        for (const Chunk& c : chunks) {
            if (c.tag == Tag("SHDR") || c.tag == Tag("SHEX")) { tokens = reinterpret_cast<const uint32_t*>(c.data); token_count = c.size / 4; }
            else if (c.tag == Tag("ISGN") || c.tag == Tag("ISG1")) in_sig = ParseSignature(c);
            else if (c.tag == Tag("OSGN") || c.tag == Tag("OSG5") || c.tag == Tag("OSG1")) out_sig = ParseSignature(c);
        }
    } else {
        tokens = reinterpret_cast<const uint32_t*>(data);
        token_count = size / 4;
    }
    if (tokens == nullptr || token_count < 2) { result.error = "no shader code"; return result; }

    Translator t;
    const uint32_t version = tokens[0];
    const uint32_t type = version >> 16;
    switch (type) {
    case 0: t.stage = ShaderStage::Pixel; break;
    case 1: t.stage = ShaderStage::Vertex; break;
    case 2: t.stage = ShaderStage::Geometry; break;
    case 3: t.stage = ShaderStage::Hull; break;
    case 4: t.stage = ShaderStage::Domain; break;
    case 5: t.stage = ShaderStage::Compute; break;
    default: t.stage = ShaderStage::Unknown;
    }
    result.stage = t.stage;
    if (t.stage != ShaderStage::Vertex && t.stage != ShaderStage::Pixel && t.stage != ShaderStage::Compute) { result.error = "only vertex, pixel, and compute shaders are translated"; return result; }
    const bool vs = t.stage == ShaderStage::Vertex;
    for (const auto& e : in_sig) {
        t.sig_in[e.reg].elements.push_back(&e);
        if (!vs && e.system_value == 9) { t.input_sgv[e.reg] = 9; t.uses_front_face = true; }
    }
    for (const auto& e : out_sig) t.sig_out[e.reg].elements.push_back(&e);

    const uint32_t total = std::min<uint32_t>(tokens[1], static_cast<uint32_t>(token_count));
    Decoder dec {tokens + 2, tokens + total};
    std::vector<std::string> const_buffer_decls, resource_decls, sampler_decls, uav_decls;

    auto sample_args_for = [&](uint32_t resource_reg) { return t.resource_dim.count(resource_reg) ? t.resource_dim[resource_reg] : 3u; };

    bool ended = false;
    while (dec.p < dec.end && dec.ok && !ended && t.error.empty()) {
        const uint32_t* start = dec.p;
        const uint32_t tok = dec.Next();
        const uint32_t op = tok & 0x7FF;
        uint32_t length = (tok >> 24) & 0x7F;
        if (op == OP_CUSTOMDATA) {
            const uint32_t len = dec.Next();
            const uint32_t klass = tok >> 11;
            if (klass == 3 && len >= 2) {   // DCL_IMMEDIATE_CONSTANT_BUFFER
                const uint32_t count = len - 2;
                for (uint32_t i = 0; i < count; ++i) t.icb.push_back(dec.Next());
            } else {
                for (uint32_t i = 2; i < len; ++i) dec.Next();
            }
            continue;
        }
        const uint32_t* next_instruction = start + length;
        // Extended opcode tokens (sample offsets etc.) are skipped; the offset ones are remembered.
        int offset[3] = {0, 0, 0};
        bool has_offset = false;
        if (tok & 0x80000000u) {
            uint32_t e;
            do {
                e = dec.Next();
                if ((e & 0x3F) == 1) {
                    has_offset = true;
                    for (int i = 0; i < 3; ++i) { int v = (e >> (9 + 4 * i)) & 0xF; offset[i] = v >= 8 ? v - 16 : v; }
                }
            } while (e & 0x80000000u);
        }
        const bool saturate = (tok >> 13) & 1;
        auto operand = [&]() { return dec.ReadOperand(); };
        char buf[512];

        switch (op) {
        // ---------------- declarations ----------------
        case OP_DCL_GLOBAL_FLAGS: break;
        case OP_DCL_TEMPS: t.temps = dec.Next(); break;
        case OP_DCL_INDEXABLE_TEMP: {
            const uint32_t reg = dec.Next(), n = dec.Next(), comps = dec.Next();
            t.indexable[reg] = {n, comps};
            break;
        }
        case OP_DCL_CONSTANT_BUFFER: {
            auto o = operand();
            const uint32_t slot = o->index[0];
            const uint32_t count = o->index_dim > 1 ? o->index[1] : 0;
            t.refl.constant_buffers |= 1u << slot;
            if (slot < 16) t.refl.cb_size[slot] = count;
            break;
        }
        case OP_DCL_SAMPLER: { auto o = operand(); t.refl.samplers |= 1u << o->index[0]; sampler_decls.push_back("sampler s" + std::to_string(o->index[0]) + " [[sampler(" + std::to_string(o->index[0]) + ")]]"); break; }
        case OP_DCL_RESOURCE: {
            const uint32_t dim = (tok >> 11) & 0x1F; (void)dim;
            auto o = operand();
            dec.Next();   // return type
            const uint32_t slot = o->index[0];
            t.resource_dim[slot] = dim;
            if (slot < 32) t.refl.textures |= 1u << slot;
            if (slot >= 16 && slot < 128 && std::find(t.refl.high_slots.begin(), t.refl.high_slots.end(), slot) == t.refl.high_slots.end()) t.refl.high_slots.push_back(static_cast<uint8_t>(slot));
            if (slot < 128) t.refl.texture_dimension[slot] = dim;
            resource_decls.push_back(t.TextureType(dim) + " t" + std::to_string(slot) + " [[texture(" + std::to_string(slot) + ")]]");
            break;
        }
        case OP_DCL_RESOURCE_STRUCTURED: {
            auto o = operand();
            const uint32_t stride = dec.Next(); (void)stride;
            const uint32_t slot = o->index[0];
            t.resource_dim[slot] = 12;
            t.resource_stride[slot] = stride;
            if (slot < 32) t.refl.textures |= 1u << slot;
            if (slot >= 16 && slot < 128 && std::find(t.refl.high_slots.begin(), t.refl.high_slots.end(), slot) == t.refl.high_slots.end()) t.refl.high_slots.push_back(static_cast<uint8_t>(slot));
            if (slot < 128) t.refl.texture_dimension[slot] = 12;
            const uint32_t buf_slot = BufferIndexFor(t, slot);
            resource_decls.push_back("device const uint* t" + std::to_string(slot) + " [[buffer(" + std::to_string(buf_slot) + ")]]");
            break;
        }
        case OP_DCL_UAV_TYPED: {
            const uint32_t dim = (tok >> 11) & 0x1F;
            auto o = operand();
            dec.Next();   // return type
            const uint32_t slot = o->index[0];
            t.refl.uavs |= (1u << slot);
            t.uav_dim[slot] = dim;
            if (dim == 1) {   // RWBuffer<T>: a typed buffer; read and written as raw words here (titles use it for counters)
                t.uav_stride[slot] = 4;
                uav_decls.push_back("device uint* u" + std::to_string(slot) + " [[buffer(" + std::to_string(UavBufferIndexFor(t, slot)) + ")]]");
                break;
            }
            std::string type = (dim == 8) ? "texture2d_array<float, access::read_write>" :
                               (dim == 5) ? "texture3d<float, access::read_write>" :
                               "texture2d<float, access::read_write>";
            uav_decls.push_back(type + " u" + std::to_string(slot) + " [[texture(" + std::to_string(64 + slot) + ")]]");
            break;
        }
        case OP_DCL_UAV_STRUCTURED: {
            auto o = operand();
            const uint32_t stride = dec.Next();
            const uint32_t slot = o->index[0];
            t.refl.uavs |= (1u << slot);
            t.uav_stride[slot] = stride;
            uav_decls.push_back("device uint* u" + std::to_string(slot) + " [[buffer(" + std::to_string(UavBufferIndexFor(t, slot)) + ")]]");
            break;
        }
        case OP_DCL_UAV_RAW: {
            auto o = operand();
            const uint32_t slot = o->index[0];
            t.refl.uavs |= (1u << slot);
            uav_decls.push_back("device uint* u" + std::to_string(slot) + " [[buffer(" + std::to_string(UavBufferIndexFor(t, slot)) + ")]]");
            break;
        }
        case OP_DCL_TGSM_RAW: {
            auto o = operand();
            const uint32_t bytes = dec.Next();
            const uint32_t slot = o->index[0];
            t.tgsm_stride[slot] = 4;
            t.tgsm_count[slot] = (bytes + 3) / 4;
            t.tgsm_raw.insert(slot);
            break;
        }
        case OP_DCL_TGSM_STRUCTURED: {
            auto o = operand();
            const uint32_t stride = dec.Next();
            const uint32_t count = dec.Next();
            const uint32_t slot = o->index[0];
            t.tgsm_stride[slot] = stride;
            t.tgsm_count[slot] = count;
            break;
        }
        case OP_DCL_THREAD_GROUP: {
            t.refl.thread_group_size[0] = dec.Next();
            t.refl.thread_group_size[1] = dec.Next();
            t.refl.thread_group_size[2] = dec.Next();
            break;
        }
        case OP_DCL_RESOURCE_RAW: {
            auto o = operand();
            const uint32_t slot = o->index[0];
            t.resource_dim[slot] = 12;
            t.resource_stride[slot] = 0;
            if (slot < 32) t.refl.textures |= 1u << slot;
            if (slot >= 16 && slot < 128 && std::find(t.refl.high_slots.begin(), t.refl.high_slots.end(), slot) == t.refl.high_slots.end()) t.refl.high_slots.push_back(static_cast<uint8_t>(slot));
            if (slot < 128) t.refl.texture_dimension[slot] = 12;
            resource_decls.push_back("device const uint* t" + std::to_string(slot) + " [[buffer(" + std::to_string(BufferIndexFor(t, slot)) + ")]]");
            break;
        }
        case OP_DCL_INPUT: case OP_DCL_INPUT_PS: {
            auto o = operand();
            if (o->type == T_INPUT) { t.input_regs.insert(o->index[0]); t.input_masks[o->index[0]] |= o->components == 4 ? (o->bits & 0xF) : 1; }
            break;
        }
        case OP_DCL_INPUT_SGV: case OP_DCL_INPUT_PS_SGV: case OP_DCL_INPUT_SIV: case OP_DCL_INPUT_PS_SIV: {
            auto o = operand();
            const uint32_t sv = dec.Next();
            if (o->type == T_INPUT) {
                t.input_regs.insert(o->index[0]);
                t.input_sgv[o->index[0]] = sv;
                t.input_masks[o->index[0]] |= o->components == 4 ? (o->bits & 0xF) : 1;
                if (sv == 6) t.uses_vertex_id = true;
                if (sv == 8) t.uses_instance_id = true;
                if (sv == 9) t.uses_front_face = true;
                if (sv == 1 && !vs) t.pos_input_reg = o->index[0];
            } else if (o->type == T_INPUT_PRIMITIVE_ID) {
                t.uses_prim_id = true;
            }
            break;
        }
        case OP_DCL_OUTPUT: {
            auto o = operand();
            if (o->type == T_OUTPUT) { t.output_regs.insert(o->index[0]); t.output_masks[o->index[0]] |= o->components == 4 ? (o->bits & 0xF) : 1; if (!vs) t.refl.outputs_written |= 1u << o->index[0]; }
            else if (o->type == T_OUTPUT_DEPTH) t.refl.writes_depth = true;
            break;
        }
        case OP_DCL_OUTPUT_SGV: case OP_DCL_OUTPUT_SIV: {
            auto o = operand();
            const uint32_t sv = dec.Next();
            if (o->type == T_OUTPUT) {
                t.output_regs.insert(o->index[0]);
                t.output_sgv[o->index[0]] = sv;
                t.output_masks[o->index[0]] |= o->components == 4 ? (o->bits & 0xF) : 1;
                if (sv == 1 && vs) t.pos_output_reg = o->index[0];
            }
            break;
        }
        case OP_DCL_INDEX_RANGE: case OP_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY: case OP_DCL_GS_INPUT_PRIMITIVE: case OP_DCL_MAX_OUTPUT_VERTEX_COUNT:
            break;

        // ---------------- arithmetic ----------------
        case OP_MOV: { auto d = operand(); auto a = operand(); t.Write(*d, saturate, t.SrcF(*a)); break; }
        case OP_ADD: case OP_MUL: case OP_DIV: case OP_MIN: case OP_MAX: {
            auto d = operand(); auto a = operand(); auto b = operand();
            const char* sym = op == OP_ADD ? "+" : op == OP_MUL ? "*" : "/";
            std::string e;
            if (op == OP_MIN) e = "fmin(" + t.SrcF(*a) + ", " + t.SrcF(*b) + ")";
            else if (op == OP_MAX) e = "fmax(" + t.SrcF(*a) + ", " + t.SrcF(*b) + ")";
            else e = "(" + t.SrcF(*a) + " " + sym + " " + t.SrcF(*b) + ")";
            t.Write(*d, saturate, e);
            break;
        }
        case OP_MAD: { auto d = operand(); auto a = operand(); auto b = operand(); auto c = operand(); t.Write(*d, saturate, "fma(" + t.SrcF(*a) + ", " + t.SrcF(*b) + ", " + t.SrcF(*c) + ")"); break; }
        case OP_DP2: case OP_DP3: case OP_DP4: {
            auto d = operand(); auto a = operand(); auto b = operand();
            const char* sw = op == OP_DP2 ? "xy" : op == OP_DP3 ? "xyz" : "xyzw";
            t.Write(*d, saturate, std::string("float4(dot((") + t.SrcF(*a) + ")." + sw + ", (" + t.SrcF(*b) + ")." + sw + "))");
            break;
        }
        case OP_RSQ: case OP_SQRT: case OP_EXP: case OP_LOG: case OP_FRC: case OP_ROUND_NE: case OP_ROUND_NI: case OP_ROUND_PI: case OP_ROUND_Z: case OP_DERIV_RTX: case OP_DERIV_RTY: case OP_DERIV_RTX_COARSE: case OP_DERIV_RTX_FINE: case OP_DERIV_RTY_COARSE: case OP_DERIV_RTY_FINE: case OP_RCP: {
            auto d = operand(); auto a = operand();
            const char* fn = op == OP_RSQ ? "rsqrt" : op == OP_SQRT ? "sqrt" : op == OP_EXP ? "exp2" : op == OP_LOG ? "log2" : op == OP_FRC ? "fract"
                           : op == OP_ROUND_NE ? "rint" : op == OP_ROUND_NI ? "floor" : op == OP_ROUND_PI ? "ceil" : op == OP_ROUND_Z ? "trunc"
                           : (op == OP_DERIV_RTX || op == OP_DERIV_RTX_COARSE || op == OP_DERIV_RTX_FINE) ? "dfdx" : (op == OP_DERIV_RTY || op == OP_DERIV_RTY_COARSE || op == OP_DERIV_RTY_FINE) ? "dfdy" : "";
            t.Write(*d, saturate, op == OP_RCP ? "(float4(1.0f) / " + t.SrcF(*a) + ")" : std::string(fn) + "(" + t.SrcF(*a) + ")");
            break;
        }
        case OP_SINCOS: {
            auto d0 = operand(); auto d1 = operand(); auto a = operand();
            if (d0->type != T_NULL) t.Write(*d0, saturate, "sin(" + t.SrcF(*a) + ")");
            if (d1->type != T_NULL) t.Write(*d1, saturate, "cos(" + t.SrcF(*a) + ")");
            break;
        }
        // ---------------- comparisons / select ----------------
        case OP_LT: case OP_GE: case OP_EQ: case OP_NE: {
            auto d = operand(); auto a = operand(); auto b = operand();
            const char* c = op == OP_LT ? "<" : op == OP_GE ? ">=" : op == OP_EQ ? "==" : "!=";
            t.WriteBits(*d, "select(uint4(0u), uint4(0xFFFFFFFFu), " + t.SrcF(*a) + " " + c + " " + t.SrcF(*b) + ")");
            break;
        }
        case OP_ILT: case OP_IGE: case OP_IEQ: case OP_INE: case OP_ULT: case OP_UGE: {
            auto d = operand(); auto a = operand(); auto b = operand();
            const bool u = op == OP_ULT || op == OP_UGE;
            const char* c = (op == OP_ILT || op == OP_ULT) ? "<" : (op == OP_IGE || op == OP_UGE) ? ">=" : op == OP_IEQ ? "==" : "!=";
            t.WriteBits(*d, "select(uint4(0u), uint4(0xFFFFFFFFu), " + t.SrcI(*a, u) + " " + c + " " + t.SrcI(*b, u) + ")");
            break;
        }
        case OP_MOVC: {
            auto d = operand(); auto c = operand(); auto a = operand(); auto b = operand();
            t.Write(*d, saturate, "select(" + t.SrcF(*b) + ", " + t.SrcF(*a) + ", " + t.SrcI(*c, true) + " != uint4(0u))");
            break;
        }
        // ---------------- integer ----------------
        case OP_IADD: { auto d = operand(); auto a = operand(); auto b = operand(); t.WriteBits(*d, t.SrcI(*a, false) + " + " + t.SrcI(*b, false)); break; }
        case OP_INEG: { auto d = operand(); auto a = operand(); t.WriteBits(*d, "-" + t.SrcI(*a, false)); break; }
        case OP_IMAX: case OP_IMIN: { auto d = operand(); auto a = operand(); auto b = operand(); t.WriteBits(*d, std::string(op == OP_IMAX ? "max(" : "min(") + t.SrcI(*a, false) + ", " + t.SrcI(*b, false) + ")"); break; }
        case OP_UMAX: case OP_UMIN: { auto d = operand(); auto a = operand(); auto b = operand(); t.WriteBits(*d, std::string(op == OP_UMAX ? "max(" : "min(") + t.SrcI(*a, true) + ", " + t.SrcI(*b, true) + ")"); break; }
        case OP_IMAD: case OP_UMAD: {
            auto d = operand(); auto a = operand(); auto b = operand(); auto c = operand();
            const bool u = op == OP_UMAD;
            t.WriteBits(*d, t.SrcI(*a, u) + " * " + t.SrcI(*b, u) + " + " + t.SrcI(*c, u));
            break;
        }
        case OP_IMUL: case OP_UMUL: {
            auto hi = operand(); auto lo = operand(); auto a = operand(); auto b = operand();
            const bool u = op == OP_UMUL;
            if (hi->type != T_NULL) t.WriteBits(*hi, std::string(u ? "uint4" : "int4") + "(mulhi(" + t.SrcI(*a, u) + ", " + t.SrcI(*b, u) + "))");
            if (lo->type != T_NULL) t.WriteBits(*lo, t.SrcI(*a, u) + " * " + t.SrcI(*b, u));
            break;
        }
        case OP_UDIV: {
            auto q = operand(); auto r = operand(); auto a = operand(); auto b = operand();
            if (q->type != T_NULL) t.WriteBits(*q, "select(uint4(0xFFFFFFFFu), " + t.SrcI(*a, true) + " / " + t.SrcI(*b, true) + ", " + t.SrcI(*b, true) + " != uint4(0u))");
            if (r->type != T_NULL) t.WriteBits(*r, "select(uint4(0xFFFFFFFFu), " + t.SrcI(*a, true) + " % " + t.SrcI(*b, true) + ", " + t.SrcI(*b, true) + " != uint4(0u))");
            break;
        }
        case OP_AND: case OP_OR: case OP_XOR: {
            auto d = operand(); auto a = operand(); auto b = operand();
            const char* s = op == OP_AND ? " & " : op == OP_OR ? " | " : " ^ ";
            t.WriteBits(*d, t.SrcI(*a, true) + s + t.SrcI(*b, true));
            break;
        }
        case OP_NOT: { auto d = operand(); auto a = operand(); t.WriteBits(*d, "~" + t.SrcI(*a, true)); break; }
        case OP_ISHL: case OP_ISHR: case OP_USHR: {
            auto d = operand(); auto a = operand(); auto b = operand();
            const bool u = op != OP_ISHR;
            t.WriteBits(*d, t.SrcI(*a, u) + (op == OP_ISHL ? " << " : " >> ") + "(" + t.SrcI(*b, true) + " & uint4(31u))");
            break;
        }
        case OP_ITOF: { auto d = operand(); auto a = operand(); t.Write(*d, saturate, "float4(" + t.SrcI(*a, false) + ")"); break; }
        case OP_UTOF: { auto d = operand(); auto a = operand(); t.Write(*d, saturate, "float4(" + t.SrcI(*a, true) + ")"); break; }
        case OP_FTOI: { auto d = operand(); auto a = operand(); t.WriteBits(*d, "int4(" + t.SrcF(*a) + ")"); break; }
        case OP_FTOU: { auto d = operand(); auto a = operand(); t.WriteBits(*d, "uint4(max(" + t.SrcF(*a) + ", float4(0.0f)))"); break; }

        // ---------------- bit manipulation (SM5) ----------------
        case OP_BFI: {
            auto d = operand(); auto w = operand(); auto o = operand(); auto ins = operand(); auto base = operand();
            t.WriteBits(*d, "ut_bfi(" + t.SrcI(*w, true) + ", " + t.SrcI(*o, true) + ", " + t.SrcI(*ins, true) + ", " + t.SrcI(*base, true) + ")");
            break;
        }
        case OP_UBFE: case OP_IBFE: {
            auto d = operand(); auto w = operand(); auto o = operand(); auto a = operand();
            const bool u = op == OP_UBFE;
            t.WriteBits(*d, std::string(u ? "ut_ubfe(" : "ut_ibfe(") + t.SrcI(*w, true) + ", " + t.SrcI(*o, true) + ", " + t.SrcI(*a, u) + ")");
            break;
        }
        case OP_BFREV: { auto d = operand(); auto a = operand(); t.WriteBits(*d, "reverse_bits(" + t.SrcI(*a, true) + ")"); break; }
        case OP_SYNC: {
            t.body << "    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);\n";
            break;
        }
        case OP_COUNTBITS: { auto d = operand(); auto a = operand(); t.WriteBits(*d, "popcount(" + t.SrcI(*a, true) + ")"); break; }
        case OP_FIRSTBIT_HI: case OP_FIRSTBIT_LO: case OP_FIRSTBIT_SHI: {
            auto d = operand(); auto a = operand();
            const char* fn = op == OP_FIRSTBIT_HI ? "ut_firstbit_hi(" : op == OP_FIRSTBIT_LO ? "ut_firstbit_lo(" : "ut_firstbit_shi(";
            t.WriteBits(*d, std::string(fn) + t.SrcI(*a, op != OP_FIRSTBIT_SHI) + ")");
            break;
        }
        case OP_EVAL_SAMPLE_INDEX: {
            // An input evaluated at one sample: the interpolated value at the pixel is what a shader that is not
            // running per sample gets anyway.
            auto d = operand(); auto a = operand(); auto idx = operand();
            (void)idx;
            t.Write(*d, false, t.SrcF(*a));
            break;
        }
        case OP_SWAPC: {
            // swapc d0, d1, cond, a, b:  d0 = cond ? b : a;  d1 = cond ? a : b.  Both read their sources before either is written.
            auto d0 = operand(); auto d1 = operand(); auto c = operand(); auto a = operand(); auto b = operand();
            static int swap_id = 0;
            const std::string n = std::to_string(swap_id++);
            t.Line("float4 sw" + n + "a = select(" + t.SrcF(*a) + ", " + t.SrcF(*b) + ", " + t.SrcI(*c, true) + " != uint4(0u));");
            t.Line("float4 sw" + n + "b = select(" + t.SrcF(*b) + ", " + t.SrcF(*a) + ", " + t.SrcI(*c, true) + " != uint4(0u));");
            t.Write(*d0, false, "sw" + n + "a");
            t.Write(*d1, false, "sw" + n + "b");
            break;
        }

        // ---------------- texture sampling ----------------
        case OP_SAMPLE: case OP_SAMPLE_L: case OP_SAMPLE_B: case OP_SAMPLE_D: {
            auto d = operand(); auto coord = operand(); auto res = operand(); auto smp = operand();
            std::shared_ptr<Operand> extra1, extra2;
            if (op == OP_SAMPLE_L || op == OP_SAMPLE_B) extra1 = operand();
            if (op == OP_SAMPLE_D) { extra1 = operand(); extra2 = operand(); }
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            std::string c = t.SrcF(*coord);
            std::string args = "s" + std::to_string(smp->index[0]) + ", " + t.Coord(dim, c) + t.ArraySlice(dim, c);
            if (op == OP_SAMPLE_L) args += ", level(" + t.Scalar(*extra1) + ")";
            else if (op == OP_SAMPLE_B) args += ", bias(" + t.Scalar(*extra1) + ")";
            else if (op == OP_SAMPLE_D) {
                const char* g = dim == 5 ? "gradient3d" : dim == 6 ? "gradientcube" : "gradient2d";
                std::string gx = "(" + t.SrcF(*extra1) + ")", gy = "(" + t.SrcF(*extra2) + ")";
                const char* comps = (dim == 5 || dim == 6) ? ".xyz" : ".xy";
                args += std::string(", ") + g + "(" + gx + comps + ", " + gy + comps + ")";
            }
            if (vs && op == OP_SAMPLE) args += ", level(0)";   // a vertex shader has no derivatives
            if (has_offset && (dim == 3 || dim == 8)) args += ", int2(" + std::to_string(offset[0]) + ", " + std::to_string(offset[1]) + ")";
            else if (has_offset && dim == 5) args += ", int3(" + std::to_string(offset[0]) + ", " + std::to_string(offset[1]) + ", " + std::to_string(offset[2]) + ")";
            t.Write(*d, saturate, "t" + std::to_string(slot) + ".sample(" + args + ")." + Translator::Swizzle(*res));
            break;
        }
        case OP_SAMPLE_C: case OP_SAMPLE_C_LZ: {
            // Comparison sampling on an ordinary float texture: the reference is compared with the (unfiltered-by-compare) depth read.
            auto d = operand(); auto coord = operand(); auto res = operand(); auto smp = operand(); auto ref = operand();
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            std::string c = t.SrcF(*coord);
            std::string args = "s" + std::to_string(smp->index[0]) + ", " + t.Coord(dim, c) + t.ArraySlice(dim, c) + ", level(0)";
            if (has_offset && (dim == 3 || dim == 8)) args += ", int2(" + std::to_string(offset[0]) + ", " + std::to_string(offset[1]) + ")";
            t.Write(*d, saturate, "float4(float4(" + t.Scalar(*ref) + ") <= float4(t" + std::to_string(slot) + ".sample(" + args + ").r)).xxxx");
            break;
        }
        case OP_SAMPLE_CLAMP_FEEDBACK: {
            auto d = operand(); auto status = operand(); auto coord = operand(); auto res = operand(); auto smp = operand(); auto lod = operand();
            (void)status;
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            std::string c = t.SrcF(*coord);
            std::string args = "s" + std::to_string(smp->index[0]) + ", " + t.Coord(dim, c) + t.ArraySlice(dim, c) + ", min_lod_clamp(" + t.Scalar(*lod) + ")";
            if (has_offset && (dim == 3 || dim == 8)) args += ", int2(" + std::to_string(offset[0]) + ", " + std::to_string(offset[1]) + ")";
            t.Write(*d, saturate, "t" + std::to_string(slot) + ".sample(" + args + ")." + Translator::Swizzle(*res));
            break;
        }
        case OP_GATHER4: case OP_GATHER4_ALT: {
            auto d = operand(); auto coord = operand(); auto res = operand(); auto smp = operand();
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            const char* comp[4] = {"component::x", "component::y", "component::z", "component::w"};
            const uint32_t pick = smp->components == 4 ? (smp->select_mode == 2 ? (smp->bits & 3) : (smp->bits & 3)) : 0;
            std::string c = t.SrcF(*coord);
            std::string args = "s" + std::to_string(smp->index[0]) + ", " + t.Coord(dim, c) + t.ArraySlice(dim, c);
            // Metal's gather takes the offset before the component: pass int2(0) when there is none.
            if (dim == 3 || dim == 8) args += has_offset ? ", int2(" + std::to_string(offset[0]) + ", " + std::to_string(offset[1]) + ")" : std::string(", int2(0)");
            args += std::string(", ") + comp[pick];
            t.Write(*d, saturate, "t" + std::to_string(slot) + ".gather(" + args + ")." + Translator::Swizzle(*res));
            break;
        }
        case OP_GATHER4_C: {
            auto d = operand(); auto coord = operand(); auto res = operand(); auto smp = operand(); auto ref = operand();
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            std::string c = t.SrcF(*coord);
            std::string args = "s" + std::to_string(smp->index[0]) + ", " + t.Coord(dim, c) + t.ArraySlice(dim, c);
            if (has_offset && (dim == 3 || dim == 8)) args += ", int2(" + std::to_string(offset[0]) + ", " + std::to_string(offset[1]) + ")";
            t.Write(*d, saturate, "float4(float4(" + t.Scalar(*ref) + ") <= float4(t" + std::to_string(slot) + ".gather(" + args + ")))");
            break;
        }
        case OP_LD_MS: {
            auto d = operand(); auto coord = operand(); auto res = operand(); auto idx = operand();
            const uint32_t slot = res->index[0];
            const std::string c = t.SrcI(*coord, true);
            t.Write(*d, false, "t" + std::to_string(slot) + ".read(uint2((" + c + ").xy), " + t.SrcI(*idx, true) + ".x)." + Translator::Swizzle(*res));
            break;
        }
        case OP_SAMPLEPOS: {
            auto dst = operand(); auto src = operand(); auto sample_idx = operand();
            t.Write(*dst, false, "float4(0.5f, 0.5f, 0.0f, 0.0f)");
            break;
        }
        case OP_SAMPLE_INFO: {
            auto d = operand(); auto res = operand();
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            const std::string v = (dim == 4 || dim == 9) ? "float4(float(t" + std::to_string(slot) + ".get_num_samples()))" : "float4(0.0f)";
            t.Write(*d, false, v + "." + Translator::Swizzle(*res));
            break;
        }
        case OP_LD: {
            auto d = operand(); auto coord = operand(); auto res = operand();
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            const std::string c = t.SrcI(*coord, true);
            std::string call;
            if (dim == 2) call = "t" + std::to_string(slot) + ".read((" + c + ").x, (" + c + ").y)";
            else if (dim == 5) call = "t" + std::to_string(slot) + ".read((" + c + ").xyz, (" + c + ").w)";
            else if (dim == 8) call = "t" + std::to_string(slot) + ".read((" + c + ").xy, (" + c + ").z, (" + c + ").w)";
            else call = "t" + std::to_string(slot) + ".read((" + c + ").xy, (" + c + ").z)";
            t.Write(*d, false, call + "." + Translator::Swizzle(*res));
            break;
        }
        case OP_RESINFO: {
            auto d = operand(); auto mip = operand(); auto res = operand();
            const uint32_t slot = res->index[0];
            const uint32_t dim = sample_args_for(slot);
            const std::string tx = "t" + std::to_string(slot);
            const std::string lod = "as_type<uint>(" + t.SrcF(*mip) + ".x)";
            std::string v;
            if (dim == 5) v = "float4(float(" + tx + ".get_width(" + lod + ")), float(" + tx + ".get_height(" + lod + ")), float(" + tx + ".get_depth(" + lod + ")), float(" + tx + ".get_num_mip_levels()))";
            else if (dim == 2) v = "float4(float(" + tx + ".get_width(" + lod + ")), 0.0f, 0.0f, float(" + tx + ".get_num_mip_levels()))";
            else v = "float4(float(" + tx + ".get_width(" + lod + ")), float(" + tx + ".get_height(" + lod + ")), 0.0f, float(" + tx + ".get_num_mip_levels()))";
            t.Write(*d, false, v + "." + Translator::Swizzle(*res));
            break;
        }

        // ---------------- control flow ----------------
        case OP_IF: {
            auto a = operand();
            const bool nonzero = (tok >> 18) & 1;
            t.Line("if (as_type<uint>(" + t.Scalar(*a) + (nonzero ? ") != 0u) {" : ") == 0u) {"));
            ++t.indent;
            break;
        }
        case OP_ELSE: --t.indent; t.Line("} else {"); ++t.indent; break;
        case OP_ENDIF: --t.indent; t.Line("}"); break;
        case OP_LOOP: t.Line("for (;;) {"); ++t.indent; ++t.loop_depth; break;
        case OP_ENDLOOP: --t.indent; --t.loop_depth; t.Line("}"); break;
        case OP_BREAK: t.Line("break;"); break;
        case OP_CONTINUE: t.Line("continue;"); break;
        case OP_BREAKC: case OP_CONTINUEC: {
            auto a = operand();
            const bool nonzero = (tok >> 18) & 1;
            t.Line("if (as_type<uint>(" + t.Scalar(*a) + (nonzero ? ") != 0u) " : ") == 0u) ") + (op == OP_BREAKC ? "break;" : "continue;"));
            break;
        }
        case OP_DISCARD: {
            auto a = operand();
            const bool nonzero = (tok >> 18) & 1;
            t.Line("if (as_type<uint>(" + t.Scalar(*a) + (nonzero ? ") != 0u) " : ") == 0u) ") + "discard_fragment();");
            break;
        }
        case OP_LD_STRUCTURED: {
            auto dst = operand();
            auto s_idx = operand();
            auto byte_off = operand();
            auto res = operand();
            if (res->type == T_TGSM) {
                const uint32_t slot = res->index[0];
                const uint32_t stride = t.tgsm_stride.count(slot) ? t.tgsm_stride[slot] : 4u;
                t.Line("{");
                t.Line("    uint s_idx = " + t.SrcU(*s_idx) + ".x;");
                t.Line("    uint b_off = " + t.SrcU(*byte_off) + ".x;");
                t.Line("    uint w_idx = (s_idx * " + std::to_string(stride) + "u + b_off) / 4u;");
                t.Line("    uint4 raw = uint4(tgsm_g" + std::to_string(slot) + "[w_idx], tgsm_g" + std::to_string(slot) + "[w_idx + 1], tgsm_g" + std::to_string(slot) + "[w_idx + 2], tgsm_g" + std::to_string(slot) + "[w_idx + 3]);");
                t.Write(*dst, false, "as_type<float4>(raw)");
                t.Line("}");
            } else if (res->type == T_RESOURCE) {
                t.uses_structured = true;
                const uint32_t slot = res->index[0];
                const uint32_t stride = t.resource_stride.count(slot) ? t.resource_stride[slot] : 0u;
                const std::string call = "ut_ld_structured(t" + std::to_string(slot) + ", "
                    + "as_type<uint>(" + t.Scalar(*s_idx) + "), "
                    + std::to_string(stride) + "u, "
                    + "as_type<uint>(" + t.Scalar(*byte_off) + "))";
                t.Write(*dst, false, call + "." + Translator::Swizzle(*res));
            } else if (res->type == T_UAV) {
                t.uses_structured = true;
                const uint32_t slot = res->index[0];
                const uint32_t stride = t.uav_stride.count(slot) ? t.uav_stride[slot] : 0u;
                const std::string call = "ut_ld_structured(u" + std::to_string(slot) + ", "
                    + "as_type<uint>(" + t.Scalar(*s_idx) + "), "
                    + std::to_string(stride) + "u, "
                    + "as_type<uint>(" + t.Scalar(*byte_off) + "))";
                t.Write(*dst, false, call + "." + Translator::Swizzle(*res));
            }
            break;
        }
        case OP_STORE_STRUCTURED: {
            auto dst = operand(); auto idx = operand(); auto off = operand(); auto val = operand();
            if (dst->type == T_TGSM) {
                const uint32_t slot = dst->index[0];
                const uint32_t stride = t.tgsm_stride.count(slot) ? t.tgsm_stride[slot] : 4u;
                t.Line("{");
                t.Line("    uint s_idx = " + t.SrcU(*idx) + ".x;");
                t.Line("    uint b_off = " + t.SrcU(*off) + ".x;");
                t.Line("    uint w_idx = (s_idx * " + std::to_string(stride) + "u + b_off) / 4u;");
                t.Line("    uint4 raw = " + t.SrcU(*val) + ";");
                for (int comp = 0; comp < 4; ++comp) {
                    if (dst->bits & (1u << comp)) {
                        t.Line("    tgsm_g" + std::to_string(slot) + "[w_idx + " + std::to_string(comp) + "] = raw[" + std::to_string(comp) + "];");
                    }
                }
                t.Line("}");
            } else if (dst->type == T_UAV) {
                const uint32_t slot = dst->index[0];
                const uint32_t stride = t.uav_stride.count(slot) ? t.uav_stride[slot] : 4u;
                t.Line("{");
                t.Line("    uint s_idx = " + t.SrcU(*idx) + ".x;");
                t.Line("    uint b_off = " + t.SrcU(*off) + ".x;");
                t.Line("    uint w_idx = (s_idx * " + std::to_string(stride) + "u + b_off) / 4u;");
                t.Line("    uint4 raw = " + t.SrcU(*val) + ";");
                for (int comp = 0; comp < 4; ++comp) {
                    if (dst->bits & (1u << comp)) {
                        t.Line("    u" + std::to_string(slot) + "[w_idx + " + std::to_string(comp) + "] = raw[" + std::to_string(comp) + "];");
                    }
                }
                t.Line("}");
            }
            break;
        }
        case OP_STORE_RAW: {
            auto dst = operand(); auto off = operand(); auto val = operand();
            if (dst->type == T_TGSM || dst->type == T_UAV) {
                const std::string g = (dst->type == T_TGSM ? "tgsm_g" : "u") + std::to_string(dst->index[0]);
                t.Line("{");
                t.Line("    uint w_idx = " + t.SrcU(*off) + ".x / 4u;");
                t.Line("    uint4 raw = " + t.SrcU(*val) + ";");
                for (int comp = 0; comp < 4; ++comp)
                    if (dst->bits & (1u << comp)) t.Line("    " + g + "[w_idx + " + std::to_string(comp) + "] = raw[" + std::to_string(comp) + "];");
                t.Line("}");
            }
            break;
        }
        case OP_STORE_UAV_TYPED: {
            auto dst = operand(); auto coord = operand(); auto val = operand();
            const uint32_t slot = dst->index[0];
            if (t.uav_dim[slot] == 1) {
                t.Line("u" + std::to_string(slot) + "[" + t.SrcU(*coord) + ".x] = " + t.SrcU(*val) + ".x;");
            } else if (t.uav_dim[slot] == 8) {
                t.Line("u" + std::to_string(slot) + ".write(" + t.SrcF(*val) + ", uint2(" + t.SrcU(*coord) + ".xy), " + t.SrcU(*coord) + ".z);");
            } else if (t.uav_dim[slot] == 5) {
                t.Line("u" + std::to_string(slot) + ".write(" + t.SrcF(*val) + ", uint3(" + t.SrcU(*coord) + ".xyz));");
            } else {
                t.Line("u" + std::to_string(slot) + ".write(" + t.SrcF(*val) + ", uint2(" + t.SrcU(*coord) + ".xy));");
            }
            break;
        }
        case OP_IMM_ATOMIC_ALLOC: case OP_IMM_ATOMIC_CONSUME: {
            // Append/consume: the UAV's hidden counter, a word the renderer binds next to it (set by the initial count given with the
            // view, read back by CopyStructureCount). Alloc returns the count before the increment, consume the one after the decrement.
            auto dst = operand(); auto uav = operand();
            const uint32_t slot = uav->index[0];
            if (slot < 64 && t.refl.counter_index[slot] == 0)
                uav_decls.push_back("device atomic_uint* uc" + std::to_string(slot) + " [[buffer(" + std::to_string(CounterIndexFor(t, slot)) + ")]]");
            const std::string c = "uc" + std::to_string(slot);
            const std::string v = op == OP_IMM_ATOMIC_ALLOC ? "atomic_fetch_add_explicit(" + c + ", 1u, memory_order_relaxed)"
                                                            : "(atomic_fetch_sub_explicit(" + c + ", 1u, memory_order_relaxed) - 1u)";
            t.Write(*dst, false, "as_type<float4>(uint4(" + v + "))");
            break;
        }
        case OP_IMM_ATOMIC_IADD: case OP_IMM_ATOMIC_AND: case OP_IMM_ATOMIC_OR: case OP_IMM_ATOMIC_XOR:
        case OP_IMM_ATOMIC_EXCH: case OP_IMM_ATOMIC_CMP_EXCH: case OP_IMM_ATOMIC_IMAX: case OP_IMM_ATOMIC_IMIN:
        case OP_IMM_ATOMIC_UMAX: case OP_IMM_ATOMIC_UMIN:
        case OP_ATOMIC_IADD: case OP_ATOMIC_AND: case OP_ATOMIC_OR: case OP_ATOMIC_XOR: case OP_ATOMIC_CMP_STORE:
        case OP_ATOMIC_IMAX: case OP_ATOMIC_IMIN: case OP_ATOMIC_UMAX: case OP_ATOMIC_UMIN: {
            // atomic_*     mem, address, value [, value2]        (compare-store: compare, value)
            // imm_atomic_* out, mem, address, value [, value2]   (out gets the value from before; compare-exchange: compare, value)
            // mem is thread group memory (g#) or a buffer UAV (u#); both are words here. An earlier version always wrote u#,
            // which named a UAV that was not declared for a g# (Forza Horizon 2's luminance average failed to compile).
            const bool imm = op >= OP_IMM_ATOMIC_IADD;
            OperandPtr out = imm ? operand() : nullptr;
            auto mem = operand(); auto addr = operand(); auto v0 = operand();
            const bool two = op == OP_ATOMIC_CMP_STORE || op == OP_IMM_ATOMIC_CMP_EXCH;
            OperandPtr v1 = two ? operand() : nullptr;
            const bool is_signed = op == OP_ATOMIC_IMAX || op == OP_ATOMIC_IMIN || op == OP_IMM_ATOMIC_IMAX || op == OP_IMM_ATOMIC_IMIN;
            const std::string atype = is_signed ? "atomic_int" : "atomic_uint";
            const uint32_t slot = mem->index[0];
            const std::string a = t.SrcU(*addr);
            std::string ptr;
            if (mem->type == T_TGSM) {
                const std::string index = t.tgsm_raw.count(slot) ? "(" + a + ".x / 4u)"
                    : "((" + a + ".x * " + std::to_string(t.tgsm_stride[slot]) + "u + " + a + ".y) / 4u)";
                ptr = "(threadgroup " + atype + "*)&tgsm_g" + std::to_string(slot) + "[" + index + "]";
            } else if (mem->type == T_UAV && (t.uav_dim[slot] == 0 || t.uav_dim[slot] == 1)) {
                const std::string index = t.uav_dim[slot] == 1 ? a + ".x"                          // RWBuffer<uint>: an element
                    : t.uav_stride.count(slot) ? "((" + a + ".x * " + std::to_string(t.uav_stride[slot]) + "u + " + a + ".y) / 4u)"   // structured
                    : "(" + a + ".x / 4u)";                                                         // raw: a byte offset
                ptr = "(device " + atype + "*)&u" + std::to_string(slot) + "[" + index + "]";
            } else {
                static bool shown = false;
                if (!shown) { shown = true; std::fprintf(stderr, "[ut:I] dxbc: an atomic on a texture UAV is not translated\n"); }
                if (out) t.Write(*out, false, "as_type<float4>(uint4(0u))");
                break;
            }
            const std::string val = is_signed ? "as_type<int>(" + t.SrcU(*v0) + ".x)" : t.SrcU(*v0) + ".x";
            std::string fn;
            switch (op) {
            case OP_ATOMIC_IADD: case OP_IMM_ATOMIC_IADD: fn = "atomic_fetch_add_explicit"; break;
            case OP_ATOMIC_AND: case OP_IMM_ATOMIC_AND: fn = "atomic_fetch_and_explicit"; break;
            case OP_ATOMIC_OR: case OP_IMM_ATOMIC_OR: fn = "atomic_fetch_or_explicit"; break;
            case OP_ATOMIC_XOR: case OP_IMM_ATOMIC_XOR: fn = "atomic_fetch_xor_explicit"; break;
            case OP_ATOMIC_IMAX: case OP_IMM_ATOMIC_IMAX: case OP_ATOMIC_UMAX: case OP_IMM_ATOMIC_UMAX: fn = "atomic_fetch_max_explicit"; break;
            case OP_ATOMIC_IMIN: case OP_IMM_ATOMIC_IMIN: case OP_ATOMIC_UMIN: case OP_IMM_ATOMIC_UMIN: fn = "atomic_fetch_min_explicit"; break;
            case OP_IMM_ATOMIC_EXCH: fn = "atomic_exchange_explicit"; break;
            default: break;
            }
            t.Line("{");
            if (two) {
                // Compare-exchange happens once in D3D; Metal's weak form may fail without cause, so it is retried until it either
                // succeeds or sees a value other than the one compared with.
                t.Line("    uint ut_old = " + t.SrcU(*v0) + ".x;");
                t.Line("    while (!atomic_compare_exchange_weak_explicit(" + ptr + ", &ut_old, " + t.SrcU(*v1) + ".x, memory_order_relaxed, memory_order_relaxed)"
                       " && ut_old == " + t.SrcU(*v0) + ".x) {}");
            } else {
                t.Line("    uint ut_old = as_type<uint>(" + fn + "(" + ptr + ", " + val + ", memory_order_relaxed));");
            }
            if (out) t.Write(*out, false, "as_type<float4>(uint4(ut_old))");
            t.Line("}");
            break;
        }
        case OP_LD_UAV_TYPED: {
            auto dst = operand(); auto coord = operand(); auto uav = operand();
            const uint32_t slot = uav->index[0];
            if (t.uav_dim[slot] == 1) {
                t.Write(*dst, false, "as_type<float4>(uint4(u" + std::to_string(slot) + "[" + t.SrcU(*coord) + ".x], 0u, 0u, 0u))");
            } else if (t.uav_dim[slot] == 8) {
                t.Write(*dst, false, "u" + std::to_string(slot) + ".read(uint2(" + t.SrcU(*coord) + ".xy), " + t.SrcU(*coord) + ".z)");
            } else if (t.uav_dim[slot] == 5) {
                t.Write(*dst, false, "u" + std::to_string(slot) + ".read(uint3(" + t.SrcU(*coord) + ".xyz))");
            } else {
                t.Write(*dst, false, "u" + std::to_string(slot) + ".read(uint2(" + t.SrcU(*coord) + ".xy))");
            }
            break;
        }
        case OP_LD_RAW: {
            auto dst = operand(); auto off = operand(); auto src = operand();
            const uint32_t slot = src->index[0];
            if (src->type == T_TGSM) {
                const std::string g = "tgsm_g" + std::to_string(slot);
                t.Line("{");
                t.Line("    uint w_idx = " + t.SrcU(*off) + ".x / 4u;");
                t.Line("    uint4 raw = uint4(" + g + "[w_idx], " + g + "[w_idx + 1], " + g + "[w_idx + 2], " + g + "[w_idx + 3]);");
                t.Write(*dst, false, "as_type<float4>(raw)." + Translator::Swizzle(*src));
                t.Line("}");
            } else if (src->type == T_RESOURCE || src->type == T_UAV) {
                t.uses_structured = true;
                const std::string call = "ut_ld_structured(" + std::string(src->type == T_UAV ? "u" : "t") + std::to_string(slot) + ", 0u, 0u, as_type<uint>(" + t.Scalar(*off) + "))";
                t.Write(*dst, false, call + "." + Translator::Swizzle(*src));
            } else {
                t.Write(*dst, false, "as_type<float4>(uint4(0u))");
            }
            break;
        }
        case OP_F32TO16: {
            auto d = operand(); auto a = operand();
            t.uses_f32to16 = true;
            t.WriteBits(*d, "as_type<int4>(ut_f32to16(" + t.SrcF(*a) + "))");
            break;
        }
        case OP_F16TOF32: {
            auto d = operand(); auto a = operand();
            t.uses_f16tof32 = true;
            t.Write(*d, false, "ut_f16tof32(" + t.SrcI(*a, true) + ")");
            break;
        }
        case OP_SWITCH: {
            auto a = operand();
            t.Line("switch (" + t.SrcI(*a, true) + ".x) {");
            break;
        }
        case OP_CASE: {
            auto a = operand();
            if (a->type == T_IMM32) {
                t.Line("case " + std::to_string(a->imm[0]) + "u:");
            } else {
                t.Line("case " + t.SrcI(*a, true) + ".x:");
            }
            break;
        }
        case OP_DEFAULT: {
            t.Line("default:");
            break;
        }
        case OP_ENDSWITCH: {
            t.Line("}");
            break;
        }
        case OP_RET: ended = (t.indent == 1); if (!ended) t.Line("@RET@"); break;
        case OP_RETC: {
            auto a = operand();
            const bool nonzero = (tok >> 18) & 1;
            t.Line("if (as_type<uint>(" + t.Scalar(*a) + (nonzero ? ") != 0u) " : ") == 0u) ") + "@RET@");
            break;
        }
        case OP_NOP: break;
        default:
            std::snprintf(buf, sizeof buf, "unsupported instruction opcode %u", op);
            t.Fail(buf);
        }
        if (!dec.ok) { t.Fail(dec.error); break; }
        if (length != 0 && dec.p != next_instruction) dec.p = next_instruction;   // skip anything left (resource-return types etc.)
    }
    if (!t.error.empty()) { result.error = t.error; return result; }

    // ---- assemble the MSL ---------------------------------------------------------------
    std::ostringstream src;
    src << "#include <metal_stdlib>\nusing namespace metal;\n\n";
    src << "static inline uint4 ut_bfi(uint4 w, uint4 o, uint4 ins, uint4 base) {\n"
           "    w &= 31u; o &= 31u;\n"
           "    uint4 m = ((uint4(1u) << w) - uint4(1u)) << o;\n"
           "    return ((ins << o) & m) | (base & ~m);\n}\n"
           "static inline uint4 ut_ubfe(uint4 w, uint4 o, uint4 a) {\n"
           "    w &= 31u; o &= 31u;\n"
           "    uint4 left = uint4(32u) - w - o;\n"
           "    uint4 r = select(a >> o, (a << left) >> (uint4(32u) - w), (w + o) < uint4(32u));\n"
           "    return select(r, uint4(0u), w == uint4(0u));\n}\n"
           "static inline int4 ut_ibfe(uint4 w, uint4 o, int4 a) {\n"
           "    w &= 31u; o &= 31u;\n"
           "    uint4 left = uint4(32u) - w - o;\n"
           "    int4 r = select(a >> int4(o), (a << int4(left)) >> int4(uint4(32u) - w), (w + o) < uint4(32u));\n"
           "    return select(r, int4(0), w == uint4(0u));\n}\n"
           "static inline uint4 ut_firstbit_hi(uint4 a) { return select(uint4(31u) - clz(a), uint4(0xFFFFFFFFu), a == uint4(0u)); }\n"
           "static inline uint4 ut_firstbit_lo(uint4 a) { return select(ctz(a), uint4(0xFFFFFFFFu), a == uint4(0u)); }\n"
           "static inline uint4 ut_firstbit_shi(int4 a) {\n"
           "    uint4 u = as_type<uint4>(select(a, ~a, a < int4(0)));\n"
           "    return select(uint4(31u) - clz(u), uint4(0xFFFFFFFFu), u == uint4(0u));\n}\n"
           // D3D stores NaN as 0 (and clamps infinities) in packed and unorm targets; Metal keeps them, and one NaN pixel blurs into a block.
           "static inline float4 ut_clean(float4 v) {\n"
           "    uint4 b = as_type<uint4>(v);\n"
           "    uint4 mag = b & uint4(0x7fffffffu);\n"
           "    float4 r = select(v, as_type<float4>((b & uint4(0x80000000u)) | uint4(0x477fe000u)), mag == uint4(0x7f800000u));\n"
           "    return select(r, float4(0.0f), mag > uint4(0x7f800000u));\n}\n\n";
    if (t.uses_structured) {
        src << "static inline float4 ut_ld_structured(device const uint* buf, uint struct_idx, uint stride, uint byte_offset) {\n"
               "    if (!buf) return float4(0.0f);\n"
               "    uint idx = (struct_idx * stride + byte_offset) / 4;\n"
               "    return float4(as_type<float>(buf[idx]),\n"
               "                  as_type<float>(buf[idx + 1]),\n"
               "                  as_type<float>(buf[idx + 2]),\n"
               "                  as_type<float>(buf[idx + 3]));\n"
               "}\n"
               "static inline float4 ut_ld_structured(device uint* buf, uint struct_idx, uint stride, uint byte_offset) {\n"
               "    return ut_ld_structured((device const uint*)buf, struct_idx, stride, byte_offset);\n"
               "}\n\n";
    }
    if (t.uses_f32to16) {
        src << "static inline uint4 ut_f32to16(float4 v) {\n"
               "    return uint4(as_type<ushort>(half(v.x)),\n"
               "                 as_type<ushort>(half(v.y)),\n"
               "                 as_type<ushort>(half(v.z)),\n"
               "                 as_type<ushort>(half(v.w)));\n"
               "}\n\n";
    }
    if (t.uses_f16tof32) {
        src << "static inline float4 ut_f16tof32(uint4 u) {\n"
               "    return float4(float(as_type<half>(ushort(u.x & 0xffffu))),\n"
               "                 float(as_type<half>(ushort(u.y & 0xffffu))),\n"
               "                 float(as_type<half>(ushort(u.z & 0xffffu))),\n"
               "                 float(as_type<half>(ushort(u.w & 0xffffu))));\n"
               "}\n\n";
    }
    if (!t.icb.empty()) {
        src << "constant float4 icb[" << t.icb.size() / 4 << "] = {\n";
        for (size_t i = 0; i + 3 < t.icb.size(); i += 4) {
            src << "    as_type<float4>(uint4(0x" << std::hex << t.icb[i] << "u, 0x" << t.icb[i + 1] << "u, 0x" << t.icb[i + 2] << "u, 0x" << t.icb[i + 3] << std::dec << "u)),\n";
        }
        src << "};\n\n";
    }

    const bool cs = t.stage == ShaderStage::Compute;
    // Inputs.
    std::string stage_in_struct, output_struct, params;
    if (cs) {
        // Compute shaders do not use stage_in
    } else if (vs) {
        src << "struct VSIn {\n";
        for (uint32_t reg : t.input_regs) {
            if (t.input_sgv.count(reg) && (t.input_sgv[reg] == 6 || t.input_sgv[reg] == 8)) continue;
            src << "    " << (input_class(reg) == 1 ? "uint4" : input_class(reg) == 2 ? "int4" : "float4") << " a" << reg << " [[attribute(" << reg << ")]];\n";
        }
        src << "};\n";
        src << "struct VSOut {\n";
        bool has_pos = false;
        for (const auto& e : out_sig) {
            // Positions are invariant: D3D gives the same position for the same inputs and math in every vertex shader, and titles
            // draw a depth pre-pass and then the scene with an EQUAL depth test (Forza Horizon 2). Metal only promises that for
            // `invariant` outputs (with preserveInvariance on).
            if (e.system_value == 1) { if (!has_pos) src << "    float4 position [[position, invariant]];\n"; has_pos = true; continue; }
            if (e.system_value == 2 || e.system_value == 3) continue;   // clip/cull distance: not forwarded
            if (e.system_value == 4 || e.system_value == 5) continue;
            src << "    float4 " << e.semantic << e.index << " [[user(" << e.semantic << e.index << ")]];\n";
        }
        if (!has_pos) src << "    float4 position [[position, invariant]];\n";
        src << "};\n\n";
    } else {
        src << "struct PSIn {\n";
        bool has_pos = false;
        for (const auto& e : in_sig) {
            if (e.system_value == 1) { if (!has_pos) src << "    float4 position [[position]];\n"; has_pos = true; continue; }
            if (e.system_value == 9) continue;
            if (e.system_value == 4) { src << "    uint rt_index [[render_target_array_index]];\n"; continue; }   // SV_RenderTargetArrayIndex read by the pixel shader
            if (e.system_value != 0) continue;
            src << "    float4 " << e.semantic << e.index << " [[user(" << e.semantic << e.index << ")]];\n";
        }
        if (!has_pos) src << "    float4 position_unused [[position]];\n";
        src << "};\n";
        src << "struct PSOut {\n";
        for (uint32_t reg : t.output_regs) src << "    float4 c" << reg << " [[color(" << reg << ")]];\n";
        if (t.refl.writes_depth) src << "    float depth [[depth(any)]];\n";
        src << "};\n\n";
    }

    // Entry.
    if (cs) {
        src << "kernel void main0(\n"
            << "    uint3 ut_tid [[thread_position_in_grid]],\n"
            << "    uint3 ut_gid [[threadgroup_position_in_grid]],\n"
            << "    uint3 ut_gtid [[thread_position_in_threadgroup]],\n"
            << "    uint ut_gidx [[thread_index_in_threadgroup]]";
        for (uint32_t b = 0; b < 16; ++b)
            if (t.refl.constant_buffers & (1u << b)) src << ",\n    device const float4* cb" << b << " [[buffer(" << b << ")]]";
        for (auto& r : resource_decls) src << ",\n    " << r;
        for (auto& u : uav_decls) src << ",\n    " << u;
        for (auto& s : sampler_decls) src << ",\n    " << s;
        src << ")\n{\n";
        for (auto& [slot, count] : t.tgsm_count) {
            uint32_t stride = t.tgsm_stride[slot];
            uint32_t words = (count * stride + 3) / 4;
            src << "    threadgroup uint tgsm_g" << slot << "[" << words << "];\n";
        }
    } else {
        src << (vs ? "vertex VSOut main0(VSIn in [[stage_in]]" : "fragment PSOut main0(PSIn in [[stage_in]]");
        if (!vs && t.uses_front_face) src << ", bool front_facing [[front_facing]]";
        if (vs && t.uses_vertex_id) src << ", uint vertex_id [[vertex_id]]";
        if (vs && t.uses_instance_id) src << ", uint instance_id [[instance_id]], uint base_instance [[base_instance]]";
        for (uint32_t b = 0; b < 16; ++b)
            if (t.refl.constant_buffers & (1u << b)) src << ", device const float4* cb" << b << " [[buffer(" << b << ")]]";
        for (auto& r : resource_decls) src << ", " << r;
        if (!vs) for (auto& u : uav_decls) src << ", " << u;   // pixel-shader UAVs (bound by the renderer, writes unobserved for now)
        for (auto& s : sampler_decls) src << ", " << s;
        src << ") {\n";
    }

    for (uint32_t i = 0; i < t.temps; ++i) src << "    float4 r" << i << " = float4(0.0f);\n";
    for (auto& [reg, spec] : t.indexable) src << "    float4 x" << reg << "[" << spec.first << "];\n    for (int i_ = 0; i_ < " << spec.first << "; ++i_) x" << reg << "[i_] = float4(0.0f);\n";
    if (!vs) src << "    float oDepth = 0.0f;\n";
    for (uint32_t reg : t.output_regs) src << "    float4 o" << reg << " = float4(0.0f);\n";
    // Input registers.
    for (uint32_t reg : t.input_regs) {
        src << "    float4 v" << reg << " = float4(0.0f);\n";
        const auto sv = t.input_sgv.count(reg) ? t.input_sgv[reg] : 0u;
        if (vs) {
            if (sv == 6) src << "    v" << reg << ".x = as_type<float>(vertex_id);\n";
            else if (sv == 8) src << "    v" << reg << ".x = as_type<float>(instance_id - base_instance);\n";   // D3D counts from the draw's first instance
            else if (input_class(reg) != 0) src << "    v" << reg << " = as_type<float4>(in.a" << reg << ");\n";
            else src << "    v" << reg << " = in.a" << reg << ";\n";
        } else {
            if (sv == 1) src << "    v" << reg << " = in.position;\n";
            else if (sv == 9) src << "    v" << reg << ".x = as_type<float>(front_facing ? 0xFFFFFFFFu : 0u);\n";
            else if (t.sig_in.count(reg)) {
                for (const SignatureElement* e : t.sig_in[reg].elements) {
                    if (e->system_value == 4) { src << "    v" << reg << ".x = as_type<float>(in.rt_index);\n"; continue; }
                    if (e->system_value != 0) continue;
                    for (int c = 0; c < 4; ++c)
                        if (e->mask & (1u << c)) src << "    v" << reg << "." << kComp[c] << " = in." << e->semantic << e->index << "." << kComp[c] << ";\n";
                }
            }
        }
    }
    std::ostringstream epilogue;
    if (cs) {
        epilogue << "    return;\n";
    } else if (vs) {
        epilogue << "    VSOut out;\n    out.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
        for (const auto& e : out_sig) {
            if (e.system_value == 1) { epilogue << "    out.position = o" << e.reg << ";\n"; continue; }
            if (e.system_value >= 2 && e.system_value <= 5) continue;
            epilogue << "    out." << e.semantic << e.index << " = float4(0.0f);\n";
            for (int c = 0; c < 4; ++c)
                if (e.mask & (1u << c)) epilogue << "    out." << e.semantic << e.index << "." << kComp[c] << " = o" << e.reg << "." << kComp[c] << ";\n";
        }
        epilogue << "    return out;\n";
    } else {
        epilogue << "    PSOut out;\n";
        for (uint32_t reg : t.output_regs) epilogue << "    out.c" << reg << " = ut_clean(o" << reg << ");\n";
        if (t.refl.writes_depth) epilogue << "    out.depth = oDepth;\n";
        epilogue << "    return out;\n";
    }
    std::string body_text = t.body.str();
    for (size_t at; (at = body_text.find("@RET@")) != std::string::npos;) body_text.replace(at, 5, "{\n" + epilogue.str() + "}");
    src << body_text << epilogue.str() << "}\n";

    result.msl = src.str();
    result.ok = true;
    result.reflection = t.refl;
    result.reflection.inputs = in_sig;
    result.reflection.outputs = out_sig;
    return result;
}

} // namespace utsuro::gpu
