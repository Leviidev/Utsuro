// d3d.cpp -- the host's D3D11.X: D3D11CreateDevice and every object the title gets from it.
// The objects are plain COM objects over the Runtime's stub vtables (layouts generated from
// WinDurango's headers for this Era's ABI); methods that need real behaviour are overridden by
// name, the rest succeed and hand out new objects where they return one.
#include "nt/d3d.h"

#include "nt/winrt.h"

#include "engine/log.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace utsuro::nt::d3d {

namespace {

#include "nt/d3d_generated.inc"

using winrt::Call;
using winrt::Guid;
using winrt::Iface;
using winrt::Obj;
using winrt::Runtime;

constexpr uint32_t kSOk = 0;
constexpr uint32_t kENoInterface = 0x80004002u;
constexpr uint32_t kFeatureLevel11_1 = 0xB100;

std::map<std::string, const D3dIfaceDef*> g_defs;
std::map<std::string, Iface*> g_ifaces;

/// The interfaces an object of `type` has: the Xbox classes are single-inheritance, so one
/// pointer with the most-derived vtable answers for the type and all its bases (see aliases).
std::vector<Iface*> InterfacesFor(const std::string& type) {
    auto one = [](std::initializer_list<const char*> names) {
        std::vector<Iface*> v;
        for (const char* n : names) {
            auto it = g_ifaces.find(n);
            if (it != g_ifaces.end()) v.push_back(it->second);
        }
        return v;
    };
    if (type == "ID3D11Device" || type == "ID3D11Device1" || type == "ID3D11Device2" || type == "ID3D11DeviceX" ||
        type == "ID3D11PerformanceDeviceX" || type == "IDXGIDevice" || type == "IDXGIDevice1" || type == "IDXGIDevice2")
        return one({"ID3D11PerformanceDeviceX", "IDXGIDevice2"});
    if (type == "ID3D11DeviceContext" || type == "ID3D11DeviceContext1" || type == "ID3D11DeviceContext2" || type == "ID3D11DeviceContextX" ||
        type == "ID3D11PerformanceContextX" || type == "ID3D11UserDefinedAnnotationX")
        return one({"ID3D11PerformanceContextX"});
    if (type == "IDXGIAdapter" || type == "IDXGIAdapter1") return one({"IDXGIAdapter1"});
    if (type == "IDXGIFactory" || type == "IDXGIFactory1" || type == "IDXGIFactory2") return one({"IDXGIFactory2"});
    if (type == "IDXGISwapChain" || type == "IDXGISwapChain1") return one({"IDXGISwapChain1"});
    return one({type.c_str()});
}

Obj* New(const std::string& type) {
    auto ifaces = InterfacesFor(type);
    if (ifaces.empty()) return nullptr;
    return Runtime::Get().NewObject("D3D:" + type, ifaces);
}

/// New object of `type`; `*pointer` receives it as the interface called `type` (or the first one).
Obj* Create(const std::string& type, uint64_t* pointer) {
    Obj* o = New(type);
    if (o == nullptr) { *pointer = 0; return nullptr; }
    auto it = g_ifaces.find(type);
    uint64_t p = it != g_ifaces.end() ? Runtime::Get().Pointer(*o, *it->second) : 0;
    *pointer = p != 0 ? p : o->blocks[0];
    return o;
}

uint64_t NewPointer(const std::string& type) {
    uint64_t p = 0;
    Create(type, &p);
    return p;
}

/// Creation methods whose description the object keeps for its GetDesc.
struct DescSpec { const char* method; int param; size_t size; };
const DescSpec kDescs[] = {
    {"CreateBuffer", 0, 24},          {"CreateTexture1D", 0, 28},       {"CreateTexture2D", 0, 44},
    {"CreateTexture3D", 0, 44},       {"CreateShaderResourceView", 1, 24}, {"CreateUnorderedAccessView", 1, 20},
    {"CreateRenderTargetView", 1, 20}, {"CreateDepthStencilView", 1, 24}, {"CreateBlendState", 0, 264},
    {"CreateDepthStencilState", 0, 52}, {"CreateRasterizerState", 0, 40}, {"CreateSamplerState", 0, 52},
    {"CreateSwapChainForCoreWindow", 2, 48}, {"CreatePlacementBuffer", 0, 24}, {"CreatePlacementTexture2D", 0, 44},
};

uint64_t DeviceOf(Call& c) {
    auto it = c.obj.u64.find("device");
    if (it != c.obj.u64.end()) return it->second;
    auto d = g_ifaces.find("ID3D11Device");
    return d != g_ifaces.end() ? Runtime::Get().Pointer(c.obj, *d->second) : 0;
}

// ---- resources: the guest-visible memory behind buffers and textures --------------------

/// Bytes per pixel row of `width` texels, and how many rows `height` texels take, for a DXGI format.
void FormatShape(uint32_t fmt, uint32_t width, uint32_t height, uint32_t* pitch, uint32_t* rows) {
    auto bc = [&](uint32_t block_bytes) {
        *pitch = std::max(1u, (width + 3) / 4) * block_bytes;
        *rows = std::max(1u, (height + 3) / 4);
    };
    if ((fmt >= 70 && fmt <= 72) || (fmt >= 79 && fmt <= 81)) return bc(8);
    if ((fmt >= 73 && fmt <= 78) || (fmt >= 82 && fmt <= 84) || (fmt >= 94 && fmt <= 99)) return bc(16);
    uint32_t bits = 32;
    if (fmt >= 1 && fmt <= 4) bits = 128;
    else if (fmt >= 5 && fmt <= 8) bits = 96;
    else if ((fmt >= 9 && fmt <= 18) || (fmt >= 19 && fmt <= 22)) bits = 64;
    else if ((fmt >= 48 && fmt <= 52) || (fmt >= 53 && fmt <= 59) || fmt == 68 || fmt == 69 || fmt == 85 || fmt == 86) bits = 16;
    else if (fmt >= 60 && fmt <= 65) bits = 8;
    else if (fmt == 66) bits = 1;
    *pitch = (width * bits + 7) / 8;
    *rows = height;
}

struct SubLayout { uint64_t offset; uint32_t row_pitch, slice_pitch; };

std::vector<SubLayout> LayoutOf(Obj& o, uint64_t* total) {
    std::vector<SubLayout> subs;
    uint64_t at = 0;
    const std::string& d = o.str["desc"];
    auto u32 = [&](size_t i) { uint32_t v = 0; if ((i + 1) * 4 <= d.size()) std::memcpy(&v, d.data() + i * 4, 4); return v; };
    if (o.class_name == "D3D:ID3D11Buffer") {
        subs.push_back({0, u32(0), u32(0)});
        at = u32(0);
    } else if (o.class_name == "D3D:ID3D11Texture1D" || o.class_name == "D3D:ID3D11Texture2D" || o.class_name == "D3D:ID3D11Texture3D") {
        const bool t3 = o.class_name == "D3D:ID3D11Texture3D", t1 = o.class_name == "D3D:ID3D11Texture1D";
        // 1D: Width,MipLevels,ArraySize,Format ; 2D: Width,Height,MipLevels,ArraySize,Format ; 3D: Width,Height,Depth,MipLevels,Format
        const uint32_t w = u32(0), h = t1 ? 1 : u32(1);
        uint32_t mips = t1 ? u32(1) : t3 ? u32(3) : u32(2), array = t1 ? u32(2) : t3 ? 1 : u32(3);
        const uint32_t depth = t3 ? u32(2) : 1, fmt = t1 ? u32(3) : t3 ? u32(4) : u32(4);
        if (array == 0) array = 1;
        if (mips == 0) { mips = 1; for (uint32_t m = std::max(w, h); m > 1; m >>= 1) ++mips; }
        for (uint32_t a = 0; a < array; ++a) {
            for (uint32_t m = 0; m < mips; ++m) {
                uint32_t pitch, rows;
                FormatShape(fmt, std::max(1u, w >> m), std::max(1u, h >> m), &pitch, &rows);
                const uint32_t slice = pitch * rows, dm = std::max(1u, depth >> m);
                subs.push_back({at, pitch, slice});
                at += static_cast<uint64_t>(slice) * dm;
            }
        }
    }
    *total = at;
    return subs;
}

/// Gives a freshly created buffer/texture its memory and copies the initial data in.
void InitResource(Obj& o, uint64_t initial) {
    uint64_t total = 0;
    const auto subs = LayoutOf(o, &total);
    if (total == 0 || total > (1ull << 31)) return;
    uint64_t base = 0, size = (total + 0xFFF) & ~0xFFFull;
    if (Runtime::Get().kernel().Vm().Allocate(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, RegionKind::Private, "d3d resource") != STATUS_SUCCESS) {
        UT_WARN("d3d: no memory for a %llu byte resource", (unsigned long long)total);
        return;
    }
    o.u64["data"] = base;
    if (initial == 0) return;
    struct Init { uint64_t mem; uint32_t pitch, slice; };
    const auto* in = reinterpret_cast<const Init*>(initial);
    for (size_t i = 0; i < subs.size(); ++i) {
        if (in[i].mem == 0) continue;
        if (o.class_name == "D3D:ID3D11Buffer") { std::memcpy(reinterpret_cast<void*>(base), reinterpret_cast<void*>(in[i].mem), subs[i].slice_pitch); continue; }
        const uint32_t pitch = std::min(subs[i].row_pitch, in[i].pitch == 0 ? subs[i].row_pitch : in[i].pitch);
        const uint32_t rows = subs[i].row_pitch ? subs[i].slice_pitch / subs[i].row_pitch : 0;
        for (uint32_t r = 0; r < rows; ++r)
            std::memcpy(reinterpret_cast<void*>(base + subs[i].offset + static_cast<uint64_t>(r) * subs[i].row_pitch),
                        reinterpret_cast<void*>(in[i].mem + static_cast<uint64_t>(r) * in[i].pitch), pitch);
    }
}

// ---- device context state -------------------------------------------------------------

/// What the title has bound on a context. Getters read it back; a renderer reads it at draw time.
struct Viewport { float x, y, w, h, min_depth, max_depth; };
struct CtxState {
    uint64_t cb[6][16] {}, srv[6][128] {}, sampler[6][16] {}, shader[6] {};
    uint64_t rtv[8] {}, dsv {};
    uint64_t blend {}, depth_stencil {}, raster {};
    float blend_factor[4] {1, 1, 1, 1};
    uint32_t sample_mask {0xFFFFFFFF}, stencil_ref {0};
    Viewport viewports[16] {};
    uint32_t num_viewports {0};
    int32_t scissors[16][4] {};
    uint32_t num_scissors {0};
    uint64_t layout {}, vb[16] {}, ib {};
    uint32_t stride[16] {}, voffset[16] {}, ib_format {0}, ib_offset {0}, topology {0};
};

std::map<uint64_t, CtxState> g_ctx;
CtxState& StateOf(Obj& o) { return g_ctx[o.id]; }

uint32_t Fallback(Call& c) {
    const winrt::Method& m = c.iface.methods[c.slot];
    {
        static std::map<std::string, bool> seen;
        if (seen.emplace(c.iface.name + "::" + m.name, true).second) UT_INFO("d3d: first call of %s::%s", c.iface.name.c_str(), m.name.c_str());
    }
    if (m.out_index >= 0 && g_defs.count(m.out_type)) {
        auto* out = c.Ptr<uint64_t>(m.out_index);
        if (out != nullptr) {
            Obj* o = Create(m.out_type, out);
            if (o != nullptr) {
                o->u64["device"] = DeviceOf(c);
                for (const DescSpec& d : kDescs) {
                    if (m.name != d.method) continue;
                    if (c.Arg(d.param) != 0) o->str["desc"].assign(c.Ptr<char>(d.param), d.size);
                    if (m.name.find("View") != std::string::npos) o->u64["resource"] = c.Arg(0);
                }
                if (m.name == "CreateBuffer" || m.name == "CreateTexture1D" || m.name == "CreateTexture2D" || m.name == "CreateTexture3D")
                    InitResource(*o, c.Arg(1));
                UT_DEBUG("d3d: %s::%s -> new %s", c.iface.name.c_str(), m.name.c_str(), m.out_type.c_str());
            }
        }
    } else if (m.out_index >= 0 && m.out_type != "void") {
        UT_DEBUG("d3d: %s::%s returns an object of unknown type %s", c.iface.name.c_str(), m.name.c_str(), m.out_type.c_str());
    }
    return kSOk;
}

void OverrideAll(const char* method, winrt::HostFn fn, const char* only_iface_prefix = nullptr) {
    for (auto& [name, iface] : g_ifaces) {
        if (only_iface_prefix != nullptr && name.rfind(only_iface_prefix, 0) != 0) continue;
        for (auto& m : iface->methods) if (m.name == method) m.fn = fn;
    }
}

} // namespace

void Register(Runtime& rt) {
    for (const D3dIfaceDef& def : kD3dIfaces) {
        std::vector<winrt::Method> methods;
        for (const D3dMethodDef& m : def.methods) {
            winrt::Method mm;
            mm.name = m.name;
            mm.params = m.params;
            mm.out_index = m.out_index;
            mm.out_type = m.out_type;
            methods.push_back(std::move(mm));
        }
        g_defs[def.name] = &def;
        g_ifaces[def.name] = &rt.DefineComInterface(def.name, def.iid, std::move(methods), Fallback);
    }

    // The Xbox device context embeds its vtable (m_Function) after the device-child fields, the
    // draw-state blocks and the 64-byte out-of-line flags. Offsets are for ABI >= 6.2.11294.
    constexpr uint32_t kTinyDevice = 0x20, kUserData = kTinyDevice + 0x148, kFlags = kUserData + 0x228, kFunction = kFlags + 64;
    for (const char* n : {"ID3D11PerformanceContextX", "ID3D11DeviceContextX", "ID3D11UserDefinedAnnotationX"})
        if (g_ifaces.count(n)) g_ifaces[n]->inline_table = kFunction;

    // Every interface also answers to the IIDs of the ones it derives from.
    for (auto& [name, iface] : g_ifaces) {
        std::string t = g_defs[name]->base;
        while (true) {
            auto it = g_ifaces.find(t);
            if (it == g_ifaces.end()) break;
            iface->aliases.push_back(it->second->iid);
            t = g_defs[t]->base;
        }
    }

    // IDXGIOutput is missing from the headers the tables come from: the PC layout, after IDXGIObject.
    {
        static D3dIfaceDef output;
        output.name = "IDXGIOutput";
        output.iid = "00CDDEA8-939B-4B83-A340-A685226666CC";
        output.base = "IDXGIObject";
        output.methods = g_defs["IDXGIObject"]->methods;
        for (const char* n : {"GetDesc", "GetDisplayModeList", "FindClosestMatchingMode", "WaitForVBlank", "TakeOwnership", "ReleaseOwnership",
                              "GetGammaControlCapabilities", "SetGammaControl", "GetGammaControl", "SetDisplaySurface", "GetDisplaySurfaceData", "GetFrameStatistics"})
            output.methods.push_back({n, 0, -1, ""});
        std::vector<winrt::Method> methods;
        for (const D3dMethodDef& m : output.methods) {
            winrt::Method mm;
            mm.name = m.name;
            mm.params = m.params;
            mm.out_index = m.out_index;
            mm.out_type = m.out_type;
            methods.push_back(std::move(mm));
        }
        g_defs["IDXGIOutput"] = &output;
        g_ifaces["IDXGIOutput"] = &rt.DefineComInterface("IDXGIOutput", output.iid, std::move(methods), Fallback);
    }
    // The PIX annotation interface is the context itself.
    if (g_ifaces.count("ID3D11PerformanceContextX") && g_ifaces.count("ID3D11UserDefinedAnnotationX"))
        g_ifaces["ID3D11PerformanceContextX"]->aliases.push_back(g_ifaces["ID3D11UserDefinedAnnotationX"]->iid);

    OverrideAll("GetFeatureLevel", [](Call&) -> uint32_t { return kFeatureLevel11_1; });
    OverrideAll("GetCreationFlags", [](Call&) -> uint32_t { return 0; });
    OverrideAll("GetImmediateContext", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = c.obj.u64["context"];
        return kSOk;
    });
    OverrideAll("GetImmediateContext1", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = c.obj.u64["context"];
        return kSOk;
    });
    OverrideAll("GetImmediateContext2", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = c.obj.u64["context"];
        return kSOk;
    });
    OverrideAll("GetImmediateContextX", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = c.obj.u64["context"];
        return kSOk;
    });
    // IDXGIObject::GetParent(riid, void** parent): the object the IID names.
    OverrideAll("GetParent", [](Call& c) -> uint32_t {
        Guid iid;
        std::memcpy(iid.b, c.Ptr<uint8_t>(0), 16);
        for (auto& [name, iface] : g_ifaces) {
            if (iface->iid == iid) {
                *c.Ptr<uint64_t>(1) = NewPointer(name);
                return kSOk;
            }
        }
        UT_WARN("d3d: GetParent for unknown %s", iid.ToString().c_str());
        *c.Ptr<uint64_t>(1) = 0;
        return kENoInterface;
    });
    // ---- context state: Set* stores, Get* reads back ----
    static const char* const kStages[6] = {"VS", "HS", "DS", "GS", "PS", "CS"};
    for (int st = 0; st < 6; ++st) {
        const std::string p = kStages[st];
        auto bind = [&](const std::string& name, winrt::HostFn fn) { OverrideAll(name.c_str(), std::move(fn)); };
        bind(p + "SetConstantBuffers", [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) s.cb[st][c.Arg(0) + i] = c.Arg(2) ? c.Ptr<uint64_t>(2)[i] : 0;
            return 0;
        });
        bind(p + "GetConstantBuffers", [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) c.Ptr<uint64_t>(2)[i] = s.cb[st][c.Arg(0) + i];
            return 0;
        });
        bind(p + "SetShaderResources", [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 128; ++i) s.srv[st][c.Arg(0) + i] = c.Arg(2) ? c.Ptr<uint64_t>(2)[i] : 0;
            return 0;
        });
        bind(p + "GetShaderResources", [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 128; ++i) c.Ptr<uint64_t>(2)[i] = s.srv[st][c.Arg(0) + i];
            return 0;
        });
        bind(p + "SetSamplers", [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) s.sampler[st][c.Arg(0) + i] = c.Arg(2) ? c.Ptr<uint64_t>(2)[i] : 0;
            return 0;
        });
        bind(p + "GetSamplers", [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) c.Ptr<uint64_t>(2)[i] = s.sampler[st][c.Arg(0) + i];
            return 0;
        });
        bind(p + "SetShader", [st](Call& c) -> uint32_t { StateOf(c.obj).shader[st] = c.Arg(0); return 0; });
        bind(p + "GetShader", [st](Call& c) -> uint32_t {
            if (c.Arg(0)) *c.Ptr<uint64_t>(0) = StateOf(c.obj).shader[st];
            if (c.Arg(2)) *c.Ptr<uint32_t>(2) = 0;
            return 0;
        });
    }
    OverrideAll("OMSetRenderTargets", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        for (uint64_t i = 0; i < 8; ++i) s.rtv[i] = (i < c.Arg(0) && c.Arg(1)) ? c.Ptr<uint64_t>(1)[i] : 0;
        s.dsv = c.Arg(2);
        return 0;
    });
    OverrideAll("OMSetRenderTargetsAndUnorderedAccessViews", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(0) != 0xFFFFFFFFu) {   // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL
            for (uint64_t i = 0; i < 8; ++i) s.rtv[i] = (i < c.Arg(0) && c.Arg(1)) ? c.Ptr<uint64_t>(1)[i] : 0;
            s.dsv = c.Arg(2);
        }
        return 0;
    });
    OverrideAll("OMGetRenderTargets", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(1)) for (uint64_t i = 0; i < c.Arg(0) && i < 8; ++i) c.Ptr<uint64_t>(1)[i] = s.rtv[i];
        if (c.Arg(2)) *c.Ptr<uint64_t>(2) = s.dsv;
        return 0;
    });
    OverrideAll("OMGetRenderTargetsAndUnorderedAccessViews", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(1)) for (uint64_t i = 0; i < c.Arg(0) && i < 8; ++i) c.Ptr<uint64_t>(1)[i] = s.rtv[i];
        if (c.Arg(2)) *c.Ptr<uint64_t>(2) = s.dsv;
        if (c.Arg(5)) for (uint64_t i = 0; i < c.Arg(4); ++i) c.Ptr<uint64_t>(5)[i] = 0;
        return 0;
    });
    OverrideAll("OMSetBlendState", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s.blend = c.Arg(0);
        if (c.Arg(1)) std::memcpy(s.blend_factor, c.Ptr<float>(1), 16); else s.blend_factor[0] = s.blend_factor[1] = s.blend_factor[2] = s.blend_factor[3] = 1;
        s.sample_mask = static_cast<uint32_t>(c.Arg(2));
        return 0;
    });
    OverrideAll("OMGetBlendState", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(0)) *c.Ptr<uint64_t>(0) = s.blend;
        if (c.Arg(1)) std::memcpy(c.Ptr<float>(1), s.blend_factor, 16);
        if (c.Arg(2)) *c.Ptr<uint32_t>(2) = s.sample_mask;
        return 0;
    });
    OverrideAll("OMSetDepthStencilState", [](Call& c) -> uint32_t { auto& s = StateOf(c.obj); s.depth_stencil = c.Arg(0); s.stencil_ref = static_cast<uint32_t>(c.Arg(1)); return 0; });
    OverrideAll("OMGetDepthStencilState", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(0)) *c.Ptr<uint64_t>(0) = s.depth_stencil;
        if (c.Arg(1)) *c.Ptr<uint32_t>(1) = s.stencil_ref;
        return 0;
    });
    OverrideAll("RSSetState", [](Call& c) -> uint32_t { StateOf(c.obj).raster = c.Arg(0); return 0; });
    OverrideAll("RSGetState", [](Call& c) -> uint32_t { if (c.Arg(0)) *c.Ptr<uint64_t>(0) = StateOf(c.obj).raster; return 0; });
    OverrideAll("RSSetViewports", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s.num_viewports = static_cast<uint32_t>(std::min<uint64_t>(c.Arg(0), 16));
        if (c.Arg(1)) std::memcpy(s.viewports, c.Ptr<void>(1), s.num_viewports * sizeof(Viewport));
        return 0;
    });
    OverrideAll("RSGetViewports", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(0)) {
            const uint32_t room = *c.Ptr<uint32_t>(0);
            if (c.Arg(1)) std::memcpy(c.Ptr<void>(1), s.viewports, std::min(room, s.num_viewports) * sizeof(Viewport));
            *c.Ptr<uint32_t>(0) = s.num_viewports;
        }
        return 0;
    });
    OverrideAll("RSSetScissorRects", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s.num_scissors = static_cast<uint32_t>(std::min<uint64_t>(c.Arg(0), 16));
        if (c.Arg(1)) std::memcpy(s.scissors, c.Ptr<void>(1), s.num_scissors * 16);
        return 0;
    });
    OverrideAll("RSGetScissorRects", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(0)) {
            const uint32_t room = *c.Ptr<uint32_t>(0);
            if (c.Arg(1)) std::memcpy(c.Ptr<void>(1), s.scissors, std::min(room, s.num_scissors) * 16);
            *c.Ptr<uint32_t>(0) = s.num_scissors;
        }
        return 0;
    });
    OverrideAll("IASetInputLayout", [](Call& c) -> uint32_t { StateOf(c.obj).layout = c.Arg(0); return 0; });
    OverrideAll("IAGetInputLayout", [](Call& c) -> uint32_t { if (c.Arg(0)) *c.Ptr<uint64_t>(0) = StateOf(c.obj).layout; return 0; });
    OverrideAll("IASetVertexBuffers", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) {
            const uint64_t k = c.Arg(0) + i;
            s.vb[k] = c.Arg(2) ? c.Ptr<uint64_t>(2)[i] : 0;
            s.stride[k] = c.Arg(3) ? c.Ptr<uint32_t>(3)[i] : 0;
            s.voffset[k] = c.Arg(4) ? c.Ptr<uint32_t>(4)[i] : 0;
        }
        return 0;
    });
    OverrideAll("IAGetVertexBuffers", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) {
            const uint64_t k = c.Arg(0) + i;
            if (c.Arg(2)) c.Ptr<uint64_t>(2)[i] = s.vb[k];
            if (c.Arg(3)) c.Ptr<uint32_t>(3)[i] = s.stride[k];
            if (c.Arg(4)) c.Ptr<uint32_t>(4)[i] = s.voffset[k];
        }
        return 0;
    });
    OverrideAll("IASetIndexBuffer", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s.ib = c.Arg(0); s.ib_format = static_cast<uint32_t>(c.Arg(1)); s.ib_offset = static_cast<uint32_t>(c.Arg(2));
        return 0;
    });
    OverrideAll("IAGetIndexBuffer", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        if (c.Arg(0)) *c.Ptr<uint64_t>(0) = s.ib;
        if (c.Arg(1)) *c.Ptr<uint32_t>(1) = s.ib_format;
        if (c.Arg(2)) *c.Ptr<uint32_t>(2) = s.ib_offset;
        return 0;
    });
    OverrideAll("IASetPrimitiveTopology", [](Call& c) -> uint32_t { StateOf(c.obj).topology = static_cast<uint32_t>(c.Arg(0)); return 0; });
    OverrideAll("IAGetPrimitiveTopology", [](Call& c) -> uint32_t { if (c.Arg(0)) *c.Ptr<uint32_t>(0) = StateOf(c.obj).topology; return 0; });

    // The one display: 1920x1080 at 60 Hz.
    OverrideAll("EnumOutputs", [](Call& c) -> uint32_t {
        if (c.Arg(0) != 0) { *c.Ptr<uint64_t>(1) = 0; return 0x887A0003u; }   // DXGI_ERROR_NOT_FOUND
        *c.Ptr<uint64_t>(1) = NewPointer("IDXGIOutput");
        return 0;
    });
    OverrideAll("GetDisplayModeList", [](Call& c) -> uint32_t {
        // (format, flags, UINT* count, DXGI_MODE_DESC* modes)
        if (c.Arg(3) != 0 && *c.Ptr<uint32_t>(2) >= 1) {
            uint32_t mode[7] {1920, 1080, 60, 1, static_cast<uint32_t>(c.Arg(0)), 0, 0};
            std::memcpy(c.Ptr<void>(3), mode, sizeof mode);
        }
        if (c.Arg(2) != 0) *c.Ptr<uint32_t>(2) = 1;
        return 0;
    });
    OverrideAll("GetDesc", [](Call& c) -> uint32_t {
        if (c.iface.name == "IDXGIOutput" && c.Arg(0) != 0) {   // DXGI_OUTPUT_DESC
            auto* d = c.Ptr<uint8_t>(0);
            std::memset(d, 0, 96);
            const char16_t name[] = u"\\\\.\\DISPLAY1";
            std::memcpy(d, name, sizeof name);
            const int32_t rect[4] {0, 0, 1920, 1080};
            std::memcpy(d + 64, rect, 16);
            d[80] = 1;   // AttachedToDesktop
            return 0;
        }
        auto it = c.obj.str.find("desc");
        if (it != c.obj.str.end() && c.Arg(0) != 0) std::memcpy(c.Ptr<void>(0), it->second.data(), it->second.size());
        return 0;
    });
    // Map(resource, subresource, type, flags, D3D11_MAPPED_SUBRESOURCE*)
    OverrideAll("Map", [](Call& c) -> uint32_t {
        Obj* r = Runtime::Get().FromPointer(c.Arg(0));
        auto* out = c.Ptr<uint64_t>(4);
        if (r == nullptr || out == nullptr || r->u64["data"] == 0) {
            UT_WARN("d3d: Map of a resource without memory (%s)", r ? r->class_name.c_str() : "unknown");
            if (out) std::memset(out, 0, 16);
            return 0x80004005u;
        }
        uint64_t total;
        const auto subs = LayoutOf(*r, &total);
        const size_t sub = std::min<size_t>(static_cast<size_t>(c.Arg(1)), subs.empty() ? 0 : subs.size() - 1);
        out[0] = r->u64["data"] + (subs.empty() ? 0 : subs[sub].offset);
        UT_INFO("d3d: Map %s sub %zu -> %#llx (resource memory %#llx, %llu bytes) type=%llu", r->class_name.c_str(), sub, (unsigned long long)out[0],
                (unsigned long long)r->u64["data"], (unsigned long long)total, (unsigned long long)c.Arg(2));
        reinterpret_cast<uint32_t*>(out)[2] = subs.empty() ? 0 : subs[sub].row_pitch;
        reinterpret_cast<uint32_t*>(out)[3] = subs.empty() ? 0 : subs[sub].slice_pitch;
        return 0;
    });
    // GetDesc / GetResource / GetDevice on resources and views read back what was stashed.

    OverrideAll("GetResource", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = c.obj.u64["resource"];
        return kSOk;
    });
    OverrideAll("GetDevice", [](Call& c) -> uint32_t {
        *c.Ptr<uint64_t>(0) = DeviceOf(c);
        return kSOk;
    });
    // IDXGISwapChain::GetBuffer(index, riid, void** surface): a texture shaped like the swap chain.
    OverrideAll("GetBuffer", [](Call& c) -> uint32_t {
        const std::string key = "buffer" + std::to_string(c.Arg(0));
        uint64_t p = c.obj.u64[key];
        if (p == 0) {
            Obj* t = Create("ID3D11Texture2D", &p);
            if (t == nullptr) return 0x80004005u;
            uint32_t sc[12] {};
            auto d = c.obj.str.find("desc");
            if (d != c.obj.str.end()) std::memcpy(sc, d->second.data(), std::min<size_t>(d->second.size(), sizeof sc));
            uint32_t td[11] {sc[0], sc[1], 1, 1, sc[2], sc[4], sc[5], 0 /*usage*/, 0x28 /*SRV|RTV*/, 0, 0};
            t->str["desc"].assign(reinterpret_cast<char*>(td), sizeof td);
            t->u64["device"] = DeviceOf(c);
            c.obj.u64[key] = p;
            UT_INFO("d3d: swap chain buffer %llu is %ux%u format %u", (unsigned long long)c.Arg(0), sc[0], sc[1], sc[2]);
        }
        *c.Ptr<uint64_t>(2) = p;
        return kSOk;
    });
    OverrideAll("CheckFormatSupport", [](Call& c) -> uint32_t {
        *c.Ptr<uint32_t>(1) = 0x7FFFFFFF;
        return kSOk;
    });
    OverrideAll("CheckMultisampleQualityLevels", [](Call& c) -> uint32_t {
        *c.Ptr<uint32_t>(2) = 1;
        return kSOk;
    });
    OverrideAll("CheckFeatureSupport", [](Call& c) -> uint32_t {
        if (c.Arg(1) != 0) std::memset(c.Ptr<void>(1), 0, static_cast<uint32_t>(c.Arg(2)));
        return kSOk;
    });
    UT_INFO("d3d: %zu interfaces", g_ifaces.size());
}

uint64_t CreateDevice(Kernel&, NtThread&, SyscallArgs& a) {
    // D3D11CreateDevice(adapter, driverType, software, flags, levels, nlevels, sdk, ppDevice, pLevel, ppContext)
    Obj* device = New("ID3D11DeviceX");
    Obj* context = New("ID3D11DeviceContextX");
    if (device == nullptr || context == nullptr) return 0x80004005u;
    Runtime& rt = Runtime::Get();
    device->u64["context"] = rt.Pointer(*context, *g_ifaces["ID3D11DeviceContext"]);
    {
        auto* c = reinterpret_cast<uint8_t*>(device->u64["context"]);
        *reinterpret_cast<uint64_t*>(c + 0x10) = rt.Pointer(*device, *g_ifaces["ID3D11Device"]);   // m_pDevice
        c[0x20 + 0x148 + 0x228] = 1;                                                              // m_OutOfLineFlags
    }
    context->u64["device"] = rt.Pointer(*device, *g_ifaces["ID3D11Device"]);
    UT_INFO("d3d: D3D11CreateDevice flags=%#x levels=%llu sdk=%#x", static_cast<uint32_t>(a[3]), (unsigned long long)a[5], static_cast<uint32_t>(a[6]));
    if (a[7] != 0) *a.Ptr<uint64_t>(7) = rt.Pointer(*device, *g_ifaces["ID3D11Device"]);
    if (a[8] != 0) *a.Ptr<uint32_t>(8) = kFeatureLevel11_1;
    if (a[9] != 0) *a.Ptr<uint64_t>(9) = rt.Pointer(*context, *g_ifaces["ID3D11DeviceContext"]);
    return kSOk;
}

} // namespace utsuro::nt::d3d
