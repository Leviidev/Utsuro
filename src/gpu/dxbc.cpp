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
};

enum OperandType : uint32_t {
    T_TEMP = 0, T_INPUT = 1, T_OUTPUT = 2, T_INDEXABLE_TEMP = 3, T_IMM32 = 4, T_IMM64 = 5, T_SAMPLER = 6, T_RESOURCE = 7,
    T_CONSTANT_BUFFER = 8, T_ICB = 9, T_LABEL = 10, T_INPUT_PRIMITIVE_ID = 11, T_OUTPUT_DEPTH = 12, T_NULL = 13,
    T_RASTERIZER = 14, T_OUTPUT_COVERAGE = 15,
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

    // ---- destinations ------------------------------------------------------------------
    struct Dest { std::string reg; std::string mask; bool valid {false}; };
    Dest DestOf(const Operand& o) {
        Dest d;
        if (o.type == T_NULL) return d;
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
    if (t.stage != ShaderStage::Vertex && t.stage != ShaderStage::Pixel) { result.error = "only vertex and pixel shaders are translated"; return result; }
    const bool vs = t.stage == ShaderStage::Vertex;
    for (const auto& e : in_sig) t.sig_in[e.reg].elements.push_back(&e);
    for (const auto& e : out_sig) t.sig_out[e.reg].elements.push_back(&e);

    const uint32_t total = std::min<uint32_t>(tokens[1], static_cast<uint32_t>(token_count));
    Decoder dec {tokens + 2, tokens + total};
    std::vector<std::string> const_buffer_decls, resource_decls, sampler_decls;

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
            const uint32_t dim = (tok >> 11) & 0x1F;
            auto o = operand();
            dec.Next();   // return type
            const uint32_t slot = o->index[0];
            t.resource_dim[slot] = dim;
            if (slot < 32) t.refl.textures |= 1u << slot;
            if (slot < 128) t.refl.texture_dimension[slot] = dim;
            resource_decls.push_back(t.TextureType(dim) + " t" + std::to_string(slot) + " [[texture(" + std::to_string(slot) + ")]]");
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
        case OP_RSQ: case OP_SQRT: case OP_EXP: case OP_LOG: case OP_FRC: case OP_ROUND_NE: case OP_ROUND_NI: case OP_ROUND_PI: case OP_ROUND_Z: case OP_DERIV_RTX: case OP_DERIV_RTY: case OP_RCP: {
            auto d = operand(); auto a = operand();
            const char* fn = op == OP_RSQ ? "rsqrt" : op == OP_SQRT ? "sqrt" : op == OP_EXP ? "exp2" : op == OP_LOG ? "log2" : op == OP_FRC ? "fract"
                           : op == OP_ROUND_NE ? "rint" : op == OP_ROUND_NI ? "floor" : op == OP_ROUND_PI ? "ceil" : op == OP_ROUND_Z ? "trunc"
                           : op == OP_DERIV_RTX ? "dfdx" : op == OP_DERIV_RTY ? "dfdy" : "";
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
    if (!t.icb.empty()) {
        src << "constant float4 icb[" << t.icb.size() / 4 << "] = {\n";
        for (size_t i = 0; i + 3 < t.icb.size(); i += 4) {
            src << "    as_type<float4>(uint4(0x" << std::hex << t.icb[i] << "u, 0x" << t.icb[i + 1] << "u, 0x" << t.icb[i + 2] << "u, 0x" << t.icb[i + 3] << std::dec << "u)),\n";
        }
        src << "};\n\n";
    }

    // Inputs.
    std::string stage_in_struct, output_struct, params;
    if (vs) {
        src << "struct VSIn {\n";
        for (uint32_t reg : t.input_regs) {
            if (t.input_sgv.count(reg) && (t.input_sgv[reg] == 6 || t.input_sgv[reg] == 8)) continue;
            src << "    " << (input_class(reg) == 1 ? "uint4" : input_class(reg) == 2 ? "int4" : "float4") << " a" << reg << " [[attribute(" << reg << ")]];\n";
        }
        src << "};\n";
        src << "struct VSOut {\n";
        bool has_pos = false;
        for (const auto& e : out_sig) {
            if (e.system_value == 1) { if (!has_pos) src << "    float4 position [[position]];\n"; has_pos = true; continue; }
            if (e.system_value == 2 || e.system_value == 3) continue;   // clip/cull distance: not forwarded
            if (e.system_value == 4 || e.system_value == 5) continue;
            src << "    float4 " << e.semantic << e.index << " [[user(" << e.semantic << e.index << ")]];\n";
        }
        if (!has_pos) src << "    float4 position [[position]];\n";
        src << "};\n\n";
    } else {
        src << "struct PSIn {\n";
        bool has_pos = false;
        for (const auto& e : in_sig) {
            if (e.system_value == 1) { if (!has_pos) src << "    float4 position [[position]];\n"; has_pos = true; continue; }
            if (e.system_value == 9) continue;
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
    src << (vs ? "vertex VSOut main0(VSIn in [[stage_in]]" : "fragment PSOut main0(PSIn in [[stage_in]]");
    if (!vs && t.uses_front_face) src << ", bool front_facing [[front_facing]]";
    if (vs && t.uses_vertex_id) src << ", uint vertex_id [[vertex_id]]";
    if (vs && t.uses_instance_id) src << ", uint instance_id [[instance_id]]";
    for (uint32_t b = 0; b < 16; ++b)
        if (t.refl.constant_buffers & (1u << b)) src << ", device const float4* cb" << b << " [[buffer(" << b << ")]]";
    for (auto& r : resource_decls) src << ", " << r;
    for (auto& s : sampler_decls) src << ", " << s;
    src << ") {\n";

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
            else if (sv == 8) src << "    v" << reg << ".x = as_type<float>(instance_id);\n";
            else if (input_class(reg) != 0) src << "    v" << reg << " = as_type<float4>(in.a" << reg << ");\n";
            else src << "    v" << reg << " = in.a" << reg << ";\n";
        } else {
            if (sv == 1) src << "    v" << reg << " = in.position;\n";
            else if (sv == 9) src << "    v" << reg << ".x = as_type<float>(front_facing ? 0xFFFFFFFFu : 0u);\n";
            else if (t.sig_in.count(reg)) {
                for (const SignatureElement* e : t.sig_in[reg].elements)
                    for (int c = 0; c < 4; ++c)
                        if (e->mask & (1u << c)) src << "    v" << reg << "." << kComp[c] << " = in." << e->semantic << e->index << "." << kComp[c] << ";\n";
            }
        }
    }
    std::ostringstream epilogue;
    if (vs) {
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
        for (uint32_t reg : t.output_regs) epilogue << "    out.c" << reg << " = o" << reg << ";\n";
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
