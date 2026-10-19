// d3d_gpu.inc -- included into d3d.cpp's anonymous namespace: the part of the D3D11.X device that
// reaches the Metal renderer (resources, views, state objects, draws, clears, present).

// ---- per-object host state ------------------------------------------------------------------

struct Res {
    std::shared_ptr<gpu::Buffer> buffer;
    std::shared_ptr<gpu::Texture> texture;
    std::shared_ptr<gpu::TextureView> view;
    std::shared_ptr<gpu::Shader> shader;
    std::shared_ptr<gpu::InputLayout> layout;
    gpu::StateBlob state;
    // A texture mapped for CPU access: its staging memory until Unmap.
    std::vector<uint8_t> staging;
    uint32_t map_subresource {0};
    bool map_write {false};
    uint32_t usage {0}, bind {0};
    // Memory the title's inline descriptors point at: a buffer's data, as host memory (placement memory, or the buffer's own storage).
    uint64_t host_ptr {0};
    uint32_t host_size {0};
    // A placement texture: where the title keeps its texels (in the console GPU's tiled layout), and whether they were written since the last upload.
    uint64_t placement {0};
    uint32_t tile_mode {0}, pitch {0};
    bool placement_dirty {false};
    uint64_t next_try_frame {0};                  ///< a texture with no readable pages yet is looked at again from this frame on
    uint64_t placement_bytes {0};                ///< how much of the placement memory the texels occupy
    uint64_t content_hash {0}, checked_frame {~0ull};   ///< to notice texels the CPU rewrote without telling anyone
    Res* base {nullptr};                         ///< a view's resource
    std::vector<RecordedDraw> recorded;          ///< a command list: what to replay
    std::vector<gpu::InputElement> elements;     ///< an input layout's elements
};

Res& R(Obj& o) {
    if (!o.user) o.user = std::make_shared<Res>();
    return *static_cast<Res*>(o.user.get());
}

std::mutex g_res_mutex;
std::unordered_map<uint64_t, Res*> g_res_cache;   // guest pointer -> host state (objects live forever)

Res* ResAt(uint64_t pointer) {
    if (pointer == 0) return nullptr;
    {
        std::scoped_lock lock {g_res_mutex};
        auto it = g_res_cache.find(pointer);
        if (it != g_res_cache.end()) return it->second;
    }
    Obj* o = Runtime::Get().FromPointer(pointer);
    if (o == nullptr || !o->user) return nullptr;
    Res* r = static_cast<Res*>(o->user.get());
    std::scoped_lock lock {g_res_mutex};
    g_res_cache[pointer] = r;
    return r;
}

// ---- descriptors inside objects -----------------------------------------------------------------
// Titles built for console OS 6.2.13xxx emit their resource bindings inline, as packets copying descriptors out of the resource
// objects (found with UTSURO_SENTINEL): a buffer holds its V# at +0x20, a shader resource view its T# at +0x30, a sampler its S#
// at +0x20. Buffers point at host memory (the console's GPU virtual addresses are the guest's); views and samplers carry the id of
// their object, which the packets then carry to the draw.
constexpr uint32_t kDescMagic = 0xD3D1FACEu;
std::unordered_map<uint64_t, Res*> g_by_id;
std::atomic<uint64_t> g_frame {0};   // presented frames
std::vector<Res*> g_placed;   // placement textures, to find which a cache flush covers

uint8_t* ObjMem(Obj& o) { return reinterpret_cast<uint8_t*>(o.blocks[0]); }

void SetBufferDescriptor(Obj& o, uint64_t base, uint32_t size) {
    uint8_t* m = ObjMem(o);
    std::memset(m + 0x20, 0, 0x20);
    *reinterpret_cast<uint64_t*>(m + 0x20) = base & 0xFFFFFFFFFFFFull;
    *reinterpret_cast<uint32_t*>(m + 0x28) = size;
}

void SetIdDescriptor(Obj& o, uint32_t offset, uint32_t bytes) {
    uint8_t* m = ObjMem(o);
    std::memset(m + offset, 0, bytes);
    *reinterpret_cast<uint32_t*>(m + offset) = static_cast<uint32_t>(o.id);
    *reinterpret_cast<uint32_t*>(m + offset + 4) = kDescMagic;
    std::scoped_lock lock {g_res_mutex};
    g_by_id[o.id] = &R(o);
}

Res* ResFromDescriptor(const uint32_t* d) {
    if (d[1] != kDescMagic) return nullptr;
    std::scoped_lock lock {g_res_mutex};
    auto it = g_by_id.find(d[0]);
    return it == g_by_id.end() ? nullptr : it->second;
}

uint32_t U32(const std::string& s, size_t index) {
    uint32_t v = 0;
    if ((index + 1) * 4 <= s.size()) std::memcpy(&v, s.data() + index * 4, 4);
    return v;
}

uint32_t MipCount(uint32_t requested, uint32_t w, uint32_t h, uint32_t d = 1) {
    if (requested != 0) return requested;
    uint32_t n = 1;
    for (uint32_t m = std::max({w, h, d}); m > 1; m >>= 1) ++n;
    return n;
}

// ---- creation ---------------------------------------------------------------------------------

/// The resource descriptions the creation methods stash (see kDescs) turned into GPU objects.
void OnCreate(Call& c, const winrt::Method& m, Obj& o) {
    const std::string& n = m.name;
    Res& r = R(o);
    const std::string& desc = o.str["desc"];
    if (n == "CreateBuffer") {
        const uint32_t size = U32(desc, 0);
        r.usage = U32(desc, 1);
        r.bind = U32(desc, 2);
        const void* initial = nullptr;
        if (c.Arg(1) != 0) initial = reinterpret_cast<const void*>(*c.Ptr<uint64_t>(1));
        r.buffer = gpu::CreateBuffer(std::max<uint32_t>(size, 16), r.usage == 2, initial);
        r.host_ptr = reinterpret_cast<uint64_t>(gpu::BufferData(*r.buffer));
        r.host_size = size;
        SetBufferDescriptor(o, r.host_ptr, size);
    } else if (n == "CreateTexture1D" || n == "CreateTexture2D" || n == "CreateTexture3D" || n == "CreatePlacementTexture2D") {
        const bool placed = n == "CreatePlacementTexture2D";   // (desc, tile mode, pitch, placement address, ppTexture)
        gpu::TextureDesc d;
        uint32_t bind, misc, usage;
        if (n == "CreateTexture1D") {
            d.width = U32(desc, 0); d.height = 1; d.mips = MipCount(U32(desc, 1), d.width, 1); d.array_size = std::max(1u, U32(desc, 2)); d.format = U32(desc, 3);
            usage = U32(desc, 4); bind = U32(desc, 5); misc = U32(desc, 7);
        } else if (n == "CreateTexture2D" || placed) {
            d.width = U32(desc, 0); d.height = U32(desc, 1); d.mips = MipCount(U32(desc, 2), d.width, d.height); d.array_size = std::max(1u, U32(desc, 3));
            d.format = U32(desc, 4); d.samples = std::max(1u, U32(desc, 5)); usage = U32(desc, 7); bind = U32(desc, 8); misc = U32(desc, 10);
        } else {
            d.width = U32(desc, 0); d.height = U32(desc, 1); d.depth = std::max(1u, U32(desc, 2)); d.mips = MipCount(U32(desc, 3), d.width, d.height, d.depth);
            d.format = U32(desc, 4); usage = U32(desc, 5); bind = U32(desc, 6); misc = U32(desc, 8);
        }
        d.shader_resource = (bind & 0x8) != 0 || bind == 0;
        d.render_target = (bind & 0x20) != 0;
        d.depth_stencil = (bind & 0x40) != 0;
        d.cube = (misc & 0x4) != 0;
        r.usage = usage;
        r.bind = bind;
        r.texture = gpu::CreateTexture(d);
        if (placed) {
            r.tile_mode = static_cast<uint32_t>(c.Arg(1));
            r.pitch = static_cast<uint32_t>(c.Arg(2));
            if (static const bool log = std::getenv("UTSURO_PLACE") != nullptr; log) {
                static std::mutex m;
                static std::map<std::string, int> seen;
                char key[160];
                std::snprintf(key, sizeof key, "tile %u fmt %u %ux%u mips %u arr %u bind %#x pitch %u", r.tile_mode, d.format, d.width, d.height, d.mips, d.array_size, bind, r.pitch);
                std::scoped_lock lock {m};
                if (seen[key]++ == 0 && seen.size() < 200) UT_INFO("d3d: placement texture: %s", key);
            }
            r.placement = c.Arg(3);
            r.placement_dirty = true;
            {
                uint32_t row, rows;
                FormatShape(d.format, d.width, d.height, &row, &rows);
                r.host_size = std::max(r.pitch, row) * rows * d.array_size * 2;   // generous: room for the mips
                std::scoped_lock lock {g_res_mutex};
                g_placed.push_back(&r);
            }
            return;
        }
        if (d.width <= 4 && d.height <= 4) {
            const auto* init0 = c.Ptr<uint8_t>(1);
            uint64_t mem = 0;
            if (init0) std::memcpy(&mem, init0, 8);
            UT_INFO("d3d: small texture %ux%u fmt %u mips %u array %u bind %#x usage %u, initial data %s (first bytes %02x%02x%02x%02x %02x%02x%02x%02x)", d.width, d.height, d.format, d.mips, d.array_size, bind, usage,
                    init0 ? "yes" : "no", mem ? reinterpret_cast<uint8_t*>(mem)[0] : 0, mem ? reinterpret_cast<uint8_t*>(mem)[1] : 0, mem ? reinterpret_cast<uint8_t*>(mem)[2] : 0, mem ? reinterpret_cast<uint8_t*>(mem)[3] : 0,
                    mem ? reinterpret_cast<uint8_t*>(mem)[4] : 0, mem ? reinterpret_cast<uint8_t*>(mem)[5] : 0, mem ? reinterpret_cast<uint8_t*>(mem)[6] : 0, mem ? reinterpret_cast<uint8_t*>(mem)[7] : 0);
        }
        if (c.Arg(1) != 0 && r.texture) {
            const auto* init = c.Ptr<uint8_t>(1);   // D3D11_SUBRESOURCE_DATA[]: pSysMem, row pitch, slice pitch
            for (uint32_t a = 0; a < d.array_size; ++a) {
                for (uint32_t mip = 0; mip < d.mips; ++mip) {
                    const uint8_t* e = init + 16 * (a * d.mips + mip);
                    uint64_t mem;
                    uint32_t row, slice;
                    std::memcpy(&mem, e, 8);
                    std::memcpy(&row, e + 8, 4);
                    std::memcpy(&slice, e + 12, 4);
                    if (mem != 0) gpu::UploadTexture(*r.texture, mip, a, nullptr, reinterpret_cast<const void*>(mem), row, slice);
                }
            }
        }
    } else if (n == "CreateShaderResourceView" || n == "CreateRenderTargetView" || n == "CreateDepthStencilView" || n == "CreateUnorderedAccessView") {
        Res* base = ResAt(c.Arg(0));
        if (base == nullptr || !base->texture) return;   // buffer views are not drawn from yet
        auto v = std::make_shared<gpu::TextureView>();
        v->texture = base->texture;
        r.base = base;
        if (n == "CreateShaderResourceView") SetIdDescriptor(o, 0x30, 0x20);
        const bool have = c.Arg(1) != 0;
        gpu::ViewDesc& vd = v->desc;
        if (have) {
            static int shown = 0;
            if (shown++ < 12) UT_INFO("d3d: %s desc %u %u %u %u %u %u (texture %ux%u fmt %u)", n.c_str(), U32(desc, 0), U32(desc, 1), U32(desc, 2), U32(desc, 3), U32(desc, 4), U32(desc, 5),
                                      gpu::TextureInfo(*base->texture).width, gpu::TextureInfo(*base->texture).height, gpu::TextureInfo(*base->texture).format);
        }
        if (have) {
            vd.format = U32(desc, 0);
            vd.dimension = U32(desc, 1);
            if (n == "CreateShaderResourceView") {
                switch (vd.dimension) {
                case 2: case 4: case 8: case 9: vd.first_mip = U32(desc, 2); vd.mip_count = U32(desc, 3); break;
                case 3: case 5: vd.first_mip = U32(desc, 2); vd.mip_count = U32(desc, 3); vd.first_slice = U32(desc, 4); vd.slice_count = U32(desc, 5); break;
                case 10: vd.first_mip = U32(desc, 2); vd.mip_count = U32(desc, 3); vd.first_slice = U32(desc, 4) * 6; vd.slice_count = U32(desc, 5) * 6; break;
                case 6: break;
                case 7: vd.first_slice = U32(desc, 2); vd.slice_count = U32(desc, 3); break;
                }
                if (vd.mip_count == 0xFFFFFFFFu) vd.mip_count = 0;
            } else if (n == "CreateRenderTargetView") {
                switch (vd.dimension) {
                case 2: case 4: vd.first_mip = U32(desc, 2); vd.mip_count = 1; break;
                case 3: case 5: vd.first_mip = U32(desc, 2); vd.mip_count = 1; vd.first_slice = U32(desc, 3); vd.slice_count = U32(desc, 4); break;
                case 8: vd.first_mip = U32(desc, 2); vd.mip_count = 1; vd.first_slice = U32(desc, 3); vd.slice_count = 1; break;
                case 7: vd.first_slice = U32(desc, 2); vd.slice_count = U32(desc, 3); break;
                }
            } else if (n == "CreateDepthStencilView") {
                switch (vd.dimension) {
                case 3: case 5: vd.first_mip = U32(desc, 3); vd.mip_count = 1; vd.first_slice = U32(desc, 4); vd.slice_count = U32(desc, 5); break;
                case 2: case 4: vd.first_mip = U32(desc, 3); vd.mip_count = 1; break;
                case 7: vd.first_slice = U32(desc, 3); vd.slice_count = U32(desc, 4); break;
                }
            }
            // A format of 0 keeps the texture's own.
        }
        r.view = v;
    } else if (n.size() > 12 && n.compare(0, 6, "Create") == 0 && n.find("Shader") != std::string::npos && n != "CreateShaderResourceView") {
        if (c.Arg(0) == 0) return;
        const auto* code = c.Ptr<uint8_t>(0);
        const size_t length = static_cast<size_t>(c.Arg(1));
        const gpu::ShaderStage stage = n == "CreateVertexShader" ? gpu::ShaderStage::Vertex : n == "CreatePixelShader" ? gpu::ShaderStage::Pixel : gpu::ShaderStage::Unknown;
        r.shader = gpu::CreateShader(code, length, stage);
    } else if (n == "CreateInputLayout") {
        // (elements, count, bytecode, length)
        std::vector<gpu::InputElement> elements;
        const auto* e = c.Ptr<uint8_t>(0);
        for (uint64_t i = 0; i < c.Arg(1) && e != nullptr; ++i) {
            const uint8_t* p = e + 32 * i;
            uint64_t name_ptr;
            std::memcpy(&name_ptr, p, 8);
            gpu::InputElement el;
            if (name_ptr != 0) el.semantic = reinterpret_cast<const char*>(name_ptr);
            std::memcpy(&el.semantic_index, p + 8, 4);
            std::memcpy(&el.format, p + 12, 4);
            std::memcpy(&el.slot, p + 16, 4);
            std::memcpy(&el.offset, p + 20, 4);
            std::memcpy(&el.slot_class, p + 24, 4);
            std::memcpy(&el.step_rate, p + 28, 4);
            elements.push_back(std::move(el));
        }
        r.layout = gpu::CreateInputLayout(elements, c.Ptr<uint8_t>(2), static_cast<size_t>(c.Arg(3)));
        r.elements = elements;
    } else if (n == "CreateBlendState" || n == "CreateDepthStencilState" || n == "CreateRasterizerState" || n == "CreateSamplerState" || n == "CreateDepthStencilStateX") {
        r.state.bytes = desc;
        if (n == "CreateSamplerState") SetIdDescriptor(o, 0x20, 0x10);
    } else if (n == "CreateDeferredContext" || n == "CreateDeferredContext1" || n == "CreateDeferredContext2") {
        o.u64["deferred"] = 1;
    } else if (n == "FinishCommandList") {
        r.recorded = std::move(StateOf(c.obj).recorded);
        StateOf(c.obj).recorded.clear();
    } else if (n == "CreatePlacementBuffer") {
        // (desc, placement address, ppBuffer): the title keeps the data; descriptors point straight at it.
        r.usage = U32(desc, 1);
        r.bind = U32(desc, 2);
        r.host_ptr = c.Arg(1);
        r.host_size = U32(desc, 0);
        SetBufferDescriptor(o, 0, r.host_size);   // the title adds the buffer's address itself (found by watching what its packets hold)
    } else if (n == "CreateSwapChainForCoreWindow") {
        gpu::TextureDesc d;
        d.width = U32(desc, 0);
        d.height = U32(desc, 1);
        d.format = U32(desc, 2);
        d.render_target = true;
        d.shader_resource = true;
        gpu::Init();
        r.texture = gpu::CreateTexture(d);
        UT_INFO("d3d: swap chain %ux%u format %u", d.width, d.height, d.format);
    }
}

// ---- drawing ------------------------------------------------------------------------------------

/// The Xbox context's inline Set* functions only store into m_ShaderUserDataManagerDraw (inside the
/// context object) and the real work happens at Draw: read the shaders, layout and topology from there.
constexpr uint32_t kUserDataOffset = 0x20 + 0x148;

/// The hardware primitive-type words the Xbox headers store in m_Topology, as D3D_PRIMITIVE_TOPOLOGY.
uint32_t TopologyOf(uint64_t hardware) {
    switch (hardware) {
    case 0x000003ffc0009e00ull: return 1;   // point list
    case 0x000005ffc0009e00ull: return 2;   // line list
    case 0x000007ffc0009e00ull: return 3;   // line strip
    case 0x000009ffc0009e00ull: return 4;   // triangle list
    case 0x00000dffc0009e00ull: return 5;   // triangle strip
    default: return 0;
    }
}

/// The constant-engine buffer the title's inline code appends packets to, and the resource batch it is recording
/// (m_pCeCurrent / m_pCeBiasedLimit / m_pCeLimit and m_pBatchCurrent / m_pBatchLimit of the context's tiny device).
constexpr uint32_t kBatchCurrent = 0x20 + 0x20, kBatchLimit = 0x20 + 0x28, kCeCurrent = 0x20 + 0x30, kCeBiasedLimit = 0x20 + 0x38, kCeLimit = 0x20 + 0x40;


Kernel* g_kernel {nullptr};
bool HostRange(uint64_t address, uint64_t bytes) { return g_kernel != nullptr && g_kernel->Vm().Contains(address, bytes); }

/// Copies guest memory the title's descriptors point at, without trusting them: reserved-but-uncommitted pages fault, so the kernel does the read.
bool SafeCopy(uint64_t address, void* destination, uint64_t bytes) { return SafeRead(address, destination, bytes); }

/// Bytes per element (a texel, or a 4x4 block of a compressed format) and the element's size in texels.
void ElementOf(uint32_t fmt, uint32_t* bytes, uint32_t* block) {
    *block = 1;
    if ((fmt >= 70 && fmt <= 72) || (fmt >= 79 && fmt <= 81)) { *bytes = 8; *block = 4; return; }
    if ((fmt >= 73 && fmt <= 78) || (fmt >= 82 && fmt <= 84) || (fmt >= 94 && fmt <= 99)) { *bytes = 16; *block = 4; return; }
    uint32_t bits = 32;
    if (fmt >= 1 && fmt <= 4) bits = 128;
    else if (fmt >= 5 && fmt <= 8) bits = 96;
    else if ((fmt >= 9 && fmt <= 18) || (fmt >= 19 && fmt <= 22)) bits = 64;
    else if ((fmt >= 48 && fmt <= 52) || (fmt >= 53 && fmt <= 59) || fmt == 68 || fmt == 69 || fmt == 85 || fmt == 86) bits = 16;
    else if (fmt >= 60 && fmt <= 65) bits = 8;
    *bytes = std::max(1u, bits / 8);
}

/// How a tiled placement texture is laid out, learned from xg_x.dll (the console's own tiling code) by asking it where texels are:
/// the console's thin 1D tiling is rows of 8x8-element micro tiles, each in Morton order, and what varies is where the levels start
/// and how far apart the micro tiles are.
struct TileLevel { uint64_t base {0}, row_stride {0}, tile_bytes {0}; uint32_t width {0}, height {0}; };
struct TileModel {
    bool ok {false};
    bool linear {false};           ///< rows (of elements) one pitch apart, rather than micro tiles
    uint32_t bytes {0};            ///< per element
    uint32_t levels_per_slice {0};
    std::vector<TileLevel> levels; ///< [slice * levels_per_slice + mip]
};
std::mutex g_tile_mutex;
std::map<std::string, TileModel> g_tile_models;

uint32_t Morton8(uint32_t x, uint32_t y) {   // bit i of x and y interleaved: x0 y0 x1 y1 x2 y2
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

TileModel BuildTileModel(const gpu::TextureDesc& info, uint32_t tile_mode, uint32_t pitch) {
    TileModel m;
    if (g_kernel == nullptr) return m;
    uint32_t block;
    ElementOf(info.format, &m.bytes, &block);
    const uint64_t create = xg::Export("XGCreateTexture2DComputer");
    if (create == 0) return m;
    const uint64_t desc = Runtime::Get().AllocGuest(0x40, 16), out = Runtime::Get().AllocGuest(0x10, 16);
    auto* d = reinterpret_cast<uint32_t*>(desc);
    std::memset(d, 0, 0x40);
    d[0] = info.width; d[1] = info.height; d[2] = info.mips; d[3] = info.array_size; d[4] = info.format; d[5] = 1; d[8] = 8; d[10] = info.cube ? 4 : 0; d[13] = tile_mode; d[14] = pitch;
    *reinterpret_cast<uint64_t*>(out) = 0;
    if (xg::Call(*g_kernel, create, {desc, out}) != 0 || *reinterpret_cast<uint64_t*>(out) == 0) return m;
    const uint64_t comp = *reinterpret_cast<uint64_t*>(out);
    const uint64_t* vtable = *reinterpret_cast<uint64_t**>(comp);
    const uint64_t offset_fn = vtable[6];
    auto at = [&](uint32_t mip, uint32_t x, uint32_t y, uint32_t slice) { return xg::Call(*g_kernel, offset_fn, {comp, 0, mip, x, y, slice, 0}); };
    m.levels_per_slice = info.mips;
    // Is it a plain row-by-row layout (the console's linear modes, tile mode 0, 1 and, it turns out, 8)?
    bool linear_layout = tile_mode == 0 || tile_mode == 1;
    if (!linear_layout && info.width * info.height > 1) {
        const uint32_t w = std::max(1u, (info.width + block - 1) / block), h = std::max(1u, (info.height + block - 1) / block);
        const uint64_t p00 = at(0, 0, 0, 0), p10 = w > 1 ? at(0, 1, 0, 0) : p00 + m.bytes, p01 = h > 1 ? at(0, 0, 1, 0) : p00;
        const uint64_t stride = p01 - p00;
        linear_layout = p00 != ~0ull && p10 == p00 + m.bytes && (h == 1 || stride >= static_cast<uint64_t>(w) * m.bytes) &&
                        at(0, w - 1, h - 1, 0) == p00 + (h - 1) * stride + static_cast<uint64_t>(w - 1) * m.bytes;
    }
    bool ok = true;
    for (uint32_t slice = 0; slice < info.array_size && ok; ++slice) {
        for (uint32_t mip = 0; mip < info.mips && ok; ++mip) {
            TileLevel lv;
            lv.width = std::max(1u, ((info.width >> mip) + block - 1) / block);
            lv.height = std::max(1u, ((info.height >> mip) + block - 1) / block);
            lv.base = at(mip, 0, 0, slice);
            if (lv.base == ~0ull) { ok = false; break; }
            if (linear_layout) {   // linear: rows one pitch apart
                m.linear = true;
                lv.tile_bytes = 0;
                lv.row_stride = lv.height > 1 ? at(mip, 0, 1, slice) - lv.base : static_cast<uint64_t>(lv.width) * m.bytes;
                m.levels.push_back(lv);
                continue;
            }
            lv.tile_bytes = lv.width > 8 ? at(mip, 8, 0, slice) - lv.base : 64ull * m.bytes;
            lv.row_stride = lv.height > 8 ? at(mip, 0, 8, slice) - lv.base : 0;
            // The model must reproduce what the library says at a few more texels.
            const uint32_t probes[][2] = {{1, 0}, {0, 1}, {2, 0}, {0, 2}, {4, 0}, {0, 4}, {7, 7}, {lv.width - 1, lv.height - 1}, {lv.width / 2, lv.height / 2}};
            for (const auto& pt : probes) {
                if (pt[0] >= lv.width || pt[1] >= lv.height) continue;
                const uint64_t expect = lv.base + (pt[1] >> 3) * lv.row_stride + (pt[0] >> 3) * lv.tile_bytes + static_cast<uint64_t>(Morton8(pt[0] & 7, pt[1] & 7)) * m.bytes;
                if (at(mip, pt[0], pt[1], slice) != expect) { ok = false; break; }
            }
            m.levels.push_back(lv);
        }
    }
    xg::Call(*g_kernel, vtable[1], {comp});   // Release
    m.ok = ok;
    return m;
}

uint64_t HashWords(const uint8_t* p, size_t bytes, uint64_t h) {
    const auto* w = reinterpret_cast<const uint64_t*>(p);
    uint64_t a = h, b = h ^ 0x9E3779B97F4A7C15ull, c = h + 0xC2B2AE3D27D4EB4Full, d = h ^ 0x165667B19E3779F9ull;
    size_t n = bytes / 8;
    for (; n >= 4; n -= 4, w += 4) {
        a = (a ^ w[0]) * 0x100000001B3ull; b = (b ^ w[1]) * 0x100000001B3ull; c = (c ^ w[2]) * 0x100000001B3ull; d = (d ^ w[3]) * 0x100000001B3ull;
        a ^= a >> 29; b ^= b >> 31; c ^= c >> 27; d ^= d >> 33;
    }
    for (; n > 0; --n, ++w) a = (a ^ *w) * 0x100000001B3ull;
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3);
}

/// Whether a placement texture's memory differs from what was last uploaded (checked at most once per frame; the CPU may rewrite
/// dynamic textures, such as glyph caches, without any call the host sees).
bool PlacementChanged(Res& r) {
    if (r.placement == 0 || r.placement_bytes == 0 || r.placement_bytes > (4u << 20) || (r.bind & 0x60) != 0) return false;
    const uint64_t frame = g_frame.load();
    if (r.checked_frame == frame) return false;
    r.checked_frame = frame;
    static thread_local std::vector<uint8_t> chunk(64 << 10);
    uint64_t h = 0x84222325CBF29CE4ull;
    for (uint64_t at = 0; at < r.placement_bytes; at += chunk.size()) {
        const uint64_t n = std::min<uint64_t>(chunk.size(), r.placement_bytes - at);
        if (!SafeCopy(r.placement + at, chunk.data(), n)) {
            // Some pages are not committed (yet): what is there, and zeros for the rest, as the upload read it.
            std::fill(chunk.begin(), chunk.begin() + n, 0);
            for (uint64_t o = 0; o < n;) {
                const uint64_t step = std::min<uint64_t>(n - o, 4096 - ((r.placement + at + o) & 4095));
                SafeCopy(r.placement + at + o, chunk.data() + o, step);
                o += step;
            }
        }
        h = HashWords(chunk.data(), static_cast<size_t>(n & ~7ull), h);
    }
    if (h == r.content_hash) return false;
    r.placement_dirty = true;
    return true;
}

/// The layout of a placement texture's subresources (null while xg_x.dll cannot say).
const TileModel* ModelFor(Res& r) {
    if (!r.texture) return nullptr;
    const auto& info = gpu::TextureInfo(*r.texture);
    char key[96];
    std::snprintf(key, sizeof key, "%u %u %u %u %u %u %u %u", info.width, info.height, info.mips, info.array_size, info.format, info.cube ? 1 : 0, r.tile_mode, r.pitch);
    std::scoped_lock lock {g_tile_mutex};
    auto it = g_tile_models.find(key);
    if (it == g_tile_models.end()) {
        it = g_tile_models.emplace(key, BuildTileModel(info, r.tile_mode, r.pitch)).first;
        if (!it->second.ok) UT_INFO("d3d: no layout for a texture (%s): %ux%u fmt %u mips %u array %u tile mode %u pitch %u", it->second.levels.empty() ? "xg_x.dll did not answer" : "not a layout the host knows", info.width, info.height, info.format, info.mips, info.array_size, r.tile_mode, r.pitch);
    }
    return it->second.ok ? &it->second : nullptr;
}

/// Copies a placement texture's texels into the host texture.
void UploadPlacement(Res& r) {
    r.placement_dirty = false;
    if (static const bool stats = std::getenv("UTSURO_UPLOADSTATS") != nullptr; stats) {
        static std::atomic<uint64_t> uploads {0}, bytes {0};
        static std::atomic<int64_t> last_report {0};
        ++uploads;
        if (r.texture) { const auto& i = gpu::TextureInfo(*r.texture); bytes += static_cast<uint64_t>(i.width) * i.height * 2; }
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - last_report.load() >= 3) { last_report = now; UT_INFO("d3d: placement uploads so far: %llu textures, about %llu MB", (unsigned long long)uploads.load(), (unsigned long long)(bytes.load() >> 20)); }
    }
    if (!r.texture || r.placement == 0 || (r.bind & 0x60) != 0) return;   // render targets and depth buffers are written by the GPU
    const auto& info = gpu::TextureInfo(*r.texture);
    uint32_t bytes, block;
    ElementOf(info.format, &bytes, &block);
    auto fail = [&](const char* why) {
        static std::atomic<int> shown {0};
        if (shown++ < 10) UT_INFO("d3d: placement texture at %#llx (%ux%u fmt %u mips %u tile %u): %s", (unsigned long long)r.placement, info.width, info.height, info.format, info.mips, r.tile_mode, why);
    };
    const TileModel* model = ModelFor(r);
    if (model == nullptr) return fail("its layout is not a thin 1D tiling or a linear one (or xg_x.dll did not answer)");
    uint64_t need = 0;
    for (const TileLevel& lv : model->levels)
        need = std::max<uint64_t>(need, model->linear ? lv.base + static_cast<uint64_t>(lv.height) * lv.row_stride
                                                      : lv.base + std::max<uint64_t>((lv.height + 7) / 8, 1) * std::max<uint64_t>(lv.row_stride, ((lv.width + 7) / 8) * lv.tile_bytes));
    // A streamed texture has only some of its pages committed: read what is there, page by page, and upload the levels that are whole.
    std::vector<uint8_t> raw(static_cast<size_t>(need));
    std::vector<uint8_t> present((raw.size() + 4095) / 4096 + 1, 0);
    {
        size_t got = 0;
        for (size_t at = 0; at < raw.size();) {
            const size_t step = std::min<size_t>(raw.size() - at, 4096 - ((r.placement + at) & 4095));
            if (SafeCopy(r.placement + at, raw.data() + at, step)) { for (size_t k = at / 4096; k <= (at + step - 1) / 4096; ++k) present[k] = 1; got += step; }
            at += step;
        }
        if (got == 0) { r.placement_dirty = true; r.next_try_frame = g_frame.load() + 20; return fail("not readable"); }
    }
    r.placement_bytes = raw.size();
    {
        uint64_t h = 0x84222325CBF29CE4ull;
        for (size_t at = 0; at < raw.size(); at += 64 << 10) h = HashWords(raw.data() + at, std::min<size_t>(64 << 10, raw.size() - at) & ~7ull, h);
        r.content_hash = h;
    }
    auto level_present = [&](const TileLevel& lv) {
        const uint64_t end = std::min<uint64_t>(raw.size(), model->linear ? lv.base + static_cast<uint64_t>(lv.height) * lv.row_stride
                                                                              : lv.base + std::max<uint64_t>((lv.height + 7) / 8, 1) * std::max<uint64_t>(lv.row_stride, ((lv.width + 7) / 8) * lv.tile_bytes));
        for (uint64_t k = lv.base / 4096; k * 4096 < end; ++k) if (!present[k]) return false;
        return true;
    };
    std::vector<uint8_t> linear;
    uint32_t lowest_missing = 0;   // sampled views start below the last level that is missing
    for (uint32_t slice = 0; slice < info.array_size; ++slice) {
        for (uint32_t mip = 0; mip < info.mips; ++mip) {
            const TileLevel& lv = model->levels[slice * model->levels_per_slice + mip];
            if (!level_present(lv)) { if (slice == 0 && mip + 1 > lowest_missing) lowest_missing = mip + 1; continue; }
            const uint32_t row_pitch = lv.width * bytes;
            linear.resize(static_cast<size_t>(row_pitch) * lv.height);
            for (uint32_t y = 0; y < lv.height; ++y) {
                if (model->linear) {
                    const uint64_t src = lv.base + static_cast<uint64_t>(y) * lv.row_stride;
                    if (src + row_pitch <= raw.size()) std::memcpy(linear.data() + static_cast<size_t>(y) * row_pitch, raw.data() + src, row_pitch);
                    continue;
                }
                for (uint32_t x = 0; x < lv.width; ++x) {
                    const uint64_t src = lv.base + (y >> 3) * lv.row_stride + (x >> 3) * lv.tile_bytes + static_cast<uint64_t>(Morton8(x & 7, y & 7)) * bytes;
                    if (src + bytes > raw.size()) continue;
                    std::memcpy(linear.data() + static_cast<size_t>(y) * row_pitch + static_cast<size_t>(x) * bytes, raw.data() + src, bytes);
                }
            }
            {
                // Committed but never filled (a level the streamer has not delivered): not a level the shader may sample yet.
                const uint64_t* w = reinterpret_cast<const uint64_t*>(linear.data());
                const size_t words = linear.size() / 8;
                size_t k = 0;
                while (k < words && w[k] == 0) ++k;
                if (k == words && linear.size() >= 64) { if (slice == 0 && mip + 1 > lowest_missing) lowest_missing = mip + 1; continue; }
            }
            gpu::UploadTexture(*r.texture, mip, slice, nullptr, linear.data(), row_pitch, row_pitch * lv.height);
            if (const char* dump = std::getenv("UTSURO_DUMPR8"); dump != nullptr && info.format == 61 && mip == 0 && info.width >= 512) {
                static std::atomic<int> n {0};
                if (n < 12) { char path[512]; std::snprintf(path, sizeof path, "%s/r8_%02d_%ux%u.png", dump, n++, info.width, info.height); gpu::SavePng(*r.texture, path); }
            }
            if (static const bool log = std::getenv("UTSURO_UPLOADLOG") != nullptr; log && info.width >= 256 && lv.width >= 16) {
                size_t nonzero = 0;
                for (uint8_t b : linear) nonzero += b != 0;
                static std::atomic<int> shown {0};
                if (shown++ < 300) UT_INFO("d3d: uploaded %ux%u fmt %u mip %u (%ux%u) tile %u: %zu non-zero bytes of %zu", info.width, info.height, info.format, mip, lv.width, lv.height, r.tile_mode, nonzero, linear.size());
            }
            if (static const bool log = std::getenv("UTSURO_UPLOADLOG") != nullptr; log && info.format == 61 && mip == 0) {
                size_t nonzero = 0;
                for (uint8_t b : linear) nonzero += b != 0;
                static std::atomic<int> shown {0};
                if (shown++ < 40) UT_INFO("d3d: uploaded R8 texture %ux%u (mips %u, tile %u) with %zu non-zero bytes of %zu", info.width, info.height, info.mips, r.tile_mode, nonzero, linear.size());
            }
        }
    }
    gpu::SetMinMip(*r.texture, std::min<uint32_t>(lowest_missing, info.mips > 0 ? info.mips - 1 : 0));
}

// ---- the title's inline descriptor stream ----------------------------------------------------------
// Packets: a header (register | (dwords - 1) << 16) and the dwords. Registers are byte offsets of resource slots: textures (T#, 8 dwords)
// from 0x6b40 in 0x20 steps, samplers (S#, 4) from 0x7b40 in 0x10 steps, vertex streams (V#, 4) from 0x7d70, and constant buffers
// (V#, 4) from 0x1c40 (vertex stage) and from 0x6a00 (pixel stage).
constexpr uint32_t kRegTexture = 0x6b40, kRegSampler = 0x7b40, kRegVertex = 0x7d70, kRegCbA = 0x1c40, kRegCbB = 0x6a00;

std::shared_ptr<gpu::Buffer> CopyOfHost(uint64_t address, uint32_t bytes) {
    if (address == 0 || bytes == 0) return nullptr;
    std::vector<uint8_t> data(std::max<uint32_t>(bytes, 16));
    if (!SafeCopy(address, data.data(), bytes)) {
        // A constant buffer's size is its slot's maximum, not what was allocated: keep the readable part, page by page.
        uint32_t got = 0;
        while (got < bytes) {
            const uint64_t at = address + got;
            const uint32_t step = std::min<uint32_t>(bytes - got, static_cast<uint32_t>(4096 - (at & 4095)));
            if (!SafeCopy(at, data.data() + got, step)) break;
            got += step;
        }
        if (got == 0) {
            static std::atomic<int> shown {0};
            if (shown++ < 8) UT_INFO("d3d: a descriptor points at %#llx (%u bytes), which is not readable", (unsigned long long)address, bytes);
            return nullptr;
        }
    }
    return gpu::CreateBuffer(static_cast<uint32_t>(data.size()), false, data.data());
}

/// Reads the packets the title appended to the context's constant-engine buffer since the last read.
void ParseStream(Obj& ctx, CtxState& s) {
    if (ctx.blocks.empty()) return;
    const uint64_t buffer = ctx.u64["ce_buffer"];
    if (buffer == 0) return;
    const uint64_t current = *reinterpret_cast<const uint64_t*>(ctx.blocks[0] + kCeCurrent);
    if (current < buffer || current > buffer + 16384 * 4) {   // not the buffer MakeCeSpace gave out
        static std::atomic<int> shown {0};
        if (shown++ < 4) UT_INFO("d3d: ctx %llu has its constant-engine cursor at %#llx, outside its buffer %#llx", (unsigned long long)ctx.id, (unsigned long long)current, (unsigned long long)buffer);
        return;
    }
    if (s.stream_parsed < buffer || s.stream_parsed > current) s.stream_parsed = buffer;
    uint64_t q = s.stream_parsed;
    while (q + 4 <= current) {
        const uint32_t header = *reinterpret_cast<const uint32_t*>(q);
        const uint32_t n = (header >> 16) + 1;
        if (n > 8) { static bool shown = false; if (!shown) { shown = true; UT_INFO("d3d: unknown packet %08x in a command stream", header); } q = current; break; }
        if (q + 4 + 4ull * n > current) break;
        auto& r = s.regs[header & 0xFFFF];
        r.n = n;
        std::memcpy(r.dw, reinterpret_cast<const void*>(q + 4), 4ull * n);
        q += 4 + 4ull * n;
    }
    s.stream_parsed = q;
}

/// Resolves what the packets bound into the draw: vertex streams, constant buffers, textures and samplers.
void BindStream(CtxState& s, gpu::DrawState& d, gpu::DrawArgs& args) {
    if (s.regs.empty()) return;
    auto get = [&](uint32_t reg) -> const CtxState::Reg* { auto it = s.regs.find(reg); return it == s.regs.end() ? nullptr : &it->second; };
    auto address = [](const CtxState::Reg& r) { return static_cast<uint64_t>(r.dw[0]) | (static_cast<uint64_t>(r.dw[1] & 0xFFFF) << 32); };

    // The indices the draw reads, and with them the vertices.
    int64_t first_vertex = args.indexed ? 0 : args.start;
    int64_t last_vertex = args.indexed ? 0 : static_cast<int64_t>(args.start) + args.count - 1;
    if (args.indexed && s.ib_address != 0) {
        const uint32_t size = s.ib_format == 42 ? 4 : 2;
        const uint64_t at = s.ib_address + static_cast<uint64_t>(args.start) * size;
        std::vector<uint8_t> indices(std::max<uint64_t>(static_cast<uint64_t>(args.count) * size, 16));
        if (SafeCopy(at, indices.data(), static_cast<uint64_t>(args.count) * size)) {
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (uint32_t i = 0; i < args.count; ++i) {
                const uint32_t v = size == 4 ? reinterpret_cast<const uint32_t*>(indices.data())[i] : reinterpret_cast<const uint16_t*>(indices.data())[i];
                if (v == 0xFFFF && size == 2) continue;   // restart index
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
            d.index_buffer = gpu::CreateBuffer(static_cast<uint32_t>(indices.size()), false, indices.data());
            d.index_offset = 0;
            d.index_format = s.ib_format;
            args.start = 0;
            if (lo <= hi) {
                first_vertex = static_cast<int64_t>(lo) + args.base_vertex;
                last_vertex = static_cast<int64_t>(hi) + args.base_vertex;
                args.base_vertex = -static_cast<int32_t>(lo);
            }
        }
    } else if (!args.indexed) {
        args.start = 0;
    }
    Res* layout = ResAt(s.layout);
    for (uint32_t k = 0; k < 8; ++k) {
        const auto* v = get(kRegVertex + 0x10 * k);
        if (v == nullptr || v->n < 3) continue;
        const uint64_t base = address(*v);
        const uint32_t stride = (v->dw[1] >> 16) & 0x3FFF, size = v->dw[2];
        if (stride == 0 || base == 0) continue;
        bool per_instance = false;
        if (layout != nullptr) for (const auto& e : layout->elements) if (e.slot == k && e.slot_class == 1) per_instance = true;
        uint64_t lo, hi;
        if (per_instance) { lo = 0; hi = std::min<uint64_t>(size, (static_cast<uint64_t>(args.start_instance) + args.instances) * stride); }
        else { lo = static_cast<uint64_t>(std::max<int64_t>(first_vertex, 0)) * stride; hi = std::min<uint64_t>(size, static_cast<uint64_t>(std::max<int64_t>(last_vertex + 1, 0)) * stride); }
        if (hi <= lo) continue;
        d.vb[k].buffer = CopyOfHost(base + lo, static_cast<uint32_t>(hi - lo));
        d.vb[k].stride = stride;
        d.vb[k].offset = 0;
    }
    for (int stage = 0; stage < 2; ++stage) {
        const auto& sh = stage == 0 ? d.vs : d.ps;
        if (!sh) continue;
        const auto& rf = gpu::Reflection(*sh);
        for (uint32_t k = 0; k < 14; ++k) {
            if (!(rf.constant_buffers & (1u << k))) continue;
            const auto* v = get((stage == 0 ? kRegCbA : kRegCbB) + 0x10 * k);
            if (v == nullptr || v->n < 3) {
                static std::atomic<int> shown {0};
                if (std::getenv("UTSURO_REGLOG") != nullptr && shown++ < 6) {
                    UT_INFO("d3d: %s cb%u has no packet; %zu registers hold:", stage ? "ps" : "vs", k, s.regs.size());
                    std::vector<uint32_t> keys;
                    for (const auto& [reg, r] : s.regs) keys.push_back(reg);
                    std::sort(keys.begin(), keys.end());
                    for (uint32_t reg : keys) { const auto& r = s.regs[reg]; UT_INFO("d3d:   reg %#x n %u: %08x %08x %08x %08x", reg, r.n, r.dw[0], r.dw[1], r.dw[2], r.dw[3]); }
                }
                continue;
            }
            uint32_t size = std::min<uint32_t>(v->dw[2], 65536);
            if (rf.cb_size[k] != 0) size = std::min<uint32_t>(size, rf.cb_size[k] * 16);   // the shader reads no more than it declared
            d.cb[stage][k].buffer = CopyOfHost(address(*v), size);
            if (!d.cb[stage][k].buffer) {
                static std::atomic<int> shown {0};
                if (std::getenv("UTSURO_REGLOG") != nullptr && shown++ < 12)
                    UT_INFO("d3d: %s cb%u packet (n %u): %08x %08x %08x %08x gives no buffer (address %#llx size %u)", stage ? "ps" : "vs", k, v->n, v->dw[0], v->dw[1], v->dw[2], v->dw[3], (unsigned long long)address(*v), size);
            }
            d.cb[stage][k].offset = 0;
            d.cb[stage][k].size = size;
        }
        for (uint32_t k = 0; k < 16; ++k) {
            if (rf.textures & (1u << k)) {
                if (const auto* t = get(kRegTexture + 0x20 * k)) {
                    if (Res* view = ResFromDescriptor(t->dw)) {
                        if (view->base != nullptr && ((view->base->placement_dirty && g_frame.load() >= view->base->next_try_frame) || PlacementChanged(*view->base))) UploadPlacement(*view->base);
                        d.srv[stage][k] = view->view;
                    }
                }
            }
            if (rf.samplers & (1u << k)) {
                if (const auto* t = get(kRegSampler + 0x10 * k))
                    if (Res* sampler = ResFromDescriptor(t->dw)) d.sampler[stage][k] = sampler->state;
            }
        }
    }
}

gpu::DrawState Snapshot(Obj& ctx, CtxState& s, gpu::DrawArgs& args) {
    gpu::DrawState d;
    if (!ctx.blocks.empty()) {
        const auto* u = reinterpret_cast<const uint8_t*>(ctx.blocks[0] + kUserDataOffset);
        uint64_t topology, layout, vs, ps;
        std::memcpy(&topology, u + 8, 8);
        std::memcpy(&layout, u + 16, 8);
        std::memcpy(&vs, u + 24, 8);
        std::memcpy(&ps, u + 32, 8);
        if (topology != 0) s.topology = TopologyOf(topology);
        if (layout != 0) s.layout = layout;
        if (vs != 0) s.shader[0] = vs;
        s.shader[4] = ps;
    }
    auto shader = [&](uint64_t p) -> std::shared_ptr<gpu::Shader> { Res* r = ResAt(p); return r ? r->shader : nullptr; };
    d.vs = shader(s.shader[0]);
    d.ps = shader(s.shader[4]);
    if (Res* l = ResAt(s.layout)) d.layout = l->layout;
    for (int stage = 0; stage < 2; ++stage) {
        const int src = stage == 0 ? 0 : 4;
        const auto& sh = stage == 0 ? d.vs : d.ps;
        if (!sh) continue;
        const auto& rf = gpu::Reflection(*sh);
        for (int i = 0; i < 14; ++i) {
            if (!(rf.constant_buffers & (1u << i))) continue;
            if (Res* r = ResAt(s.cb[src][i])) {
                d.cb[stage][i].buffer = r->buffer;
                d.cb[stage][i].offset = s.cb_first[src][i] * 16;
                d.cb[stage][i].size = s.cb_count[src][i] * 16;
            }
        }
        for (int i = 0; i < 16; ++i) {
            if (rf.textures & (1u << i)) {
                if (Res* r = ResAt(s.srv[src][i])) d.srv[stage][i] = r->view;
            }
            if (rf.samplers & (1u << i)) {
                if (Res* r = ResAt(s.sampler[src][i])) d.sampler[stage][i] = r->state;
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (Res* r = ResAt(s.vb[i])) {
            d.vb[i].buffer = r->buffer;
            d.vb[i].stride = s.stride[i];
            d.vb[i].offset = s.voffset[i];
        }
    }
    if (Res* r = ResAt(s.ib)) d.index_buffer = r->buffer;
    d.index_format = s.ib_format;
    d.index_offset = s.ib_offset;
    d.topology = s.topology;
    for (int i = 0; i < 8; ++i)
        if (Res* r = ResAt(s.rtv[i])) d.rtv[i] = r->view;
    if (Res* r = ResAt(s.dsv)) d.dsv = r->view;
    if (Res* r = ResAt(s.blend)) d.blend = r->state;
    if (Res* r = ResAt(s.depth_stencil)) d.depth_stencil = r->state;
    if (Res* r = ResAt(s.raster)) d.raster = r->state;
    std::memcpy(d.blend_factor, s.blend_factor, 16);
    d.sample_mask = s.sample_mask;
    d.stencil_ref = s.stencil_ref;
    d.num_viewports = s.num_viewports;
    std::memcpy(d.viewports, s.viewports, sizeof d.viewports);
    d.num_scissors = s.num_scissors;
    std::memcpy(d.scissors, s.scissors, sizeof d.scissors);
    BindStream(s, d, args);
    return d;
}

void DoDraw(Obj& ctx, const gpu::DrawArgs& args_in) {
    if (!gpu::Available()) return;
    gpu::DrawArgs args = args_in;
    CtxState& state = StateOf(ctx);
    ParseStream(ctx, state);
    const gpu::DrawState d = Snapshot(ctx, state, args);
    if (static const bool log = std::getenv("UTSURO_STREAMLOG") != nullptr; log) {
        static std::atomic<int> n {0};
        if (n++ < 14 && d.vs && d.ps) {
            const auto& vr = gpu::Reflection(*d.vs);
            const auto& pr = gpu::Reflection(*d.ps);
            std::string regs;
            for (auto& [reg, v] : state.regs) { char t[64]; std::snprintf(t, sizeof t, " %x(%u)", reg, v.n); regs += t; }
            UT_INFO("d3d: draw %d: vs cb %#x [%u %u %u] tex %#x smp %#x | ps cb %#x [%u %u %u] tex %#x smp %#x | regs%s", n.load(), vr.constant_buffers, vr.cb_size[0] * 16, vr.cb_size[1] * 16, vr.cb_size[2] * 16, vr.textures,
                    vr.samplers, pr.constant_buffers, pr.cb_size[0] * 16, pr.cb_size[1] * 16, pr.cb_size[2] * 16, pr.textures, pr.samplers, regs.c_str());
            for (uint32_t reg : {kRegCbA, kRegCbA + 0x10u, kRegCbB, kRegCbB + 0x10u, kRegCbB + 0x20u}) {
                auto it = state.regs.find(reg);
                if (it != state.regs.end()) UT_INFO("d3d:   reg %x: base %#llx stride %u size %u", reg, (unsigned long long)(it->second.dw[0] | (uint64_t(it->second.dw[1] & 0xFFFF) << 32)), (it->second.dw[1] >> 16) & 0x3FFF, it->second.dw[2]);
            }
        }
    }
    if (!d.vs) {
        static int shown = 0;
        const CtxState& s = state;
        if (shown++ < 5) {
            Res* r = ResAt(s.shader[0]);
            UT_INFO("d3d: draw without a vertex shader: vs=%#llx res=%p shader=%p ps=%#llx", (unsigned long long)s.shader[0], (void*)r, r ? (void*)r->shader.get() : nullptr, (unsigned long long)s.shader[4]);
        }
    }
    if (args.indexed && !d.index_buffer) {
        static int shown = 0;
        const CtxState& s = state;
        if (shown++ < 5) {
            Res* r = ResAt(s.ib);
            Obj* o = Runtime::Get().FromPointer(s.ib);
            UT_INFO("d3d: indexed draw without an index buffer: ib=%#llx res=%p buffer=%p obj=%s fmt=%u address=%#llx", (unsigned long long)s.ib, (void*)r, r ? (void*)r->buffer.get() : nullptr, o ? o->class_name.c_str() : "?", s.ib_format, (unsigned long long)s.ib_address);
        }
    }
    if (ctx.u64["deferred"] != 0) {
        state.recorded.push_back({d, args});
        return;
    }
    gpu::Draw(d, args);
}

// ---- resource contents -----------------------------------------------------------------------------

void TextureShape(const gpu::TextureDesc& d, uint32_t sub, uint32_t* mip, uint32_t* slice, uint32_t* row, uint32_t* rows, uint32_t* depth) {
    *mip = sub % std::max(1u, d.mips);
    *slice = sub / std::max(1u, d.mips);
    uint32_t w = std::max(1u, d.width >> *mip), h = std::max(1u, d.height >> *mip);
    FormatShape(d.format, w, h, row, rows);
    *depth = std::max(1u, d.depth >> *mip);
}

uint32_t MapBuffer(Call& c, Res& r, uint32_t type, uint64_t* out) {
    uint8_t* p;
    if (type == 4) p = gpu::RenameBuffer(r.buffer, false);   // WRITE_DISCARD: new storage, old contents stay with the frames that use them
    else p = gpu::BufferData(*r.buffer);
    const uint32_t size = gpu::BufferSize(*r.buffer);
    if (type == 4) {   // the descriptors the title copies now name the new storage
        r.host_ptr = reinterpret_cast<uint64_t>(p);
        if (Obj* o = Runtime::Get().FromPointer(c.Arg(0))) SetBufferDescriptor(*o, r.host_ptr, size);
    }
    out[0] = reinterpret_cast<uint64_t>(p);
    reinterpret_cast<uint32_t*>(out)[2] = size;
    reinterpret_cast<uint32_t*>(out)[3] = size;
    return 0;
}

/// Inflates `n` bytes at `src` (a zlib stream when `zlib_header`, raw DEFLATE otherwise); false unless the stream ends cleanly.
bool InflateGuest(const uint8_t* src, uint32_t n, bool zlib_header, std::vector<uint8_t>* out) {
    z_stream z {};
    if (inflateInit2(&z, zlib_header ? 15 : -15) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(src);
    z.avail_in = n;
    out->clear();
    uint8_t buf[1 << 16];
    int rc = Z_OK;
    while (rc == Z_OK) {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        out->insert(out->end(), buf, buf + (sizeof buf - z.avail_out));
        if (out->size() > (256u << 20)) break;
    }
    inflateEnd(&z);
    return rc == Z_STREAM_END;
}

/// The constant-engine buffer the title's inline code appends packets to, and the resource batch it is recording
/// (m_pCeCurrent / m_pCeBiasedLimit / m_pCeLimit and m_pBatchCurrent / m_pBatchLimit of the context's tiny device).

void RegisterGpuOverrides() {
    // ---- queries: GPU timing and occlusion answer at once, with plausible values ----
    OverrideAll("GetData", [](Call& c) -> uint32_t {
        // GetData(pAsync, pData, DataSize, Flags)
        uint8_t* data = c.Ptr<uint8_t>(1);
        const uint32_t size = static_cast<uint32_t>(c.Arg(2));
        Obj* q = Runtime::Get().FromPointer(c.Arg(0));
        if (data == nullptr || size == 0) return 0;
        std::memset(data, 0, size);
        uint32_t kind = 0;
        if (q != nullptr && q->str["desc"].size() >= 4) std::memcpy(&kind, q->str["desc"].data(), 4);
        using Clock = std::chrono::steady_clock;
        const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
        switch (kind) {
        case 0: if (size >= 4) { const uint32_t done = 1; std::memcpy(data, &done, 4); } break;          // EVENT: has happened
        case 1: if (size >= 8) { const uint64_t samples = 1; std::memcpy(data, &samples, 8); } break;    // OCCLUSION: something was visible
        case 2: if (size >= 8) std::memcpy(data, &now, 8); break;                                       // TIMESTAMP (nanoseconds)
        case 3: if (size >= 12) { const uint64_t frequency = 1000000000ull; std::memcpy(data, &frequency, 8); } break;   // TIMESTAMP_DISJOINT: 1 GHz, not disjoint
        default: break;                                                                                  // everything else reads as zero
        }
        return 0;
    });
    OverrideAll("MakeCeSpace", [](Call& c) -> uint32_t {
        // The packets already appended are for the real GPU's constant engine; hand out a fresh buffer.
        constexpr uint32_t kDwords = 16384, kBias = 70;
        uint8_t* ctx = reinterpret_cast<uint8_t*>(c.obj.blocks[0]);
        ParseStream(c.obj, StateOf(c.obj));
        StateOf(c.obj).stream_parsed = 0;
        uint64_t& buf = c.obj.u64["ce_buffer"];
        if (buf == 0) buf = Runtime::Get().AllocGuest(kDwords * 4, 64);
        auto* start = reinterpret_cast<uint32_t*>(buf);
        *reinterpret_cast<uint32_t**>(ctx + kCeCurrent) = start;
        *reinterpret_cast<uint32_t**>(ctx + kCeBiasedLimit) = start + kDwords - kBias;
        *reinterpret_cast<uint32_t**>(ctx + kCeLimit) = start + kDwords;
        c.wide_result = reinterpret_cast<uint64_t>(start);
        c.wide_set = true;
        return 0;
    });
    OverrideAll("BeginResourceBatch", [](Call& c) -> uint32_t {
        UT_INFO("d3d: BeginResourceBatch on %s ctx %p buf %#llx size %u", c.obj.class_name.c_str(), (void*)c.obj.blocks[0], (unsigned long long)c.Arg(0), (uint32_t)c.Arg(1));
        uint8_t* ctx = reinterpret_cast<uint8_t*>(c.obj.blocks[0]);
        auto* start = reinterpret_cast<uint32_t*>(c.Arg(0));
        c.obj.u64["batch_start"] = c.Arg(0);
        *reinterpret_cast<uint32_t**>(ctx + kBatchCurrent) = start;
        *reinterpret_cast<uint32_t**>(ctx + kBatchLimit) = start + static_cast<uint32_t>(c.Arg(1)) / 4;
        return 0;
    });
    OverrideAll("EndResourceBatch", [](Call& c) -> uint32_t {
        uint8_t* ctx = reinterpret_cast<uint8_t*>(c.obj.blocks[0]);
        const uint64_t cur = reinterpret_cast<uint64_t>(*reinterpret_cast<uint32_t**>(ctx + kBatchCurrent));
        const uint64_t used = cur > c.obj.u64["batch_start"] ? cur - c.obj.u64["batch_start"] : 0;
        if (auto* out = c.Ptr<uint32_t>(0)) *out = static_cast<uint32_t>(used);
        return 0;
    });
    // ---- the DMA engine's hardware LZ decoder (a DEFLATE stream) ----
    // The title cooks its packages in chunks for it. The call completes at once here, so a fence never stays pending.
    OverrideAll("LZDecompressMemory", [](Call& c) -> uint32_t {
        const uint64_t a = c.Arg(0), b = c.Arg(1);
        const uint32_t n = static_cast<uint32_t>(c.Arg(2));
        static int logged = 0;
        std::vector<uint8_t> data;
        // The documented order is (dst, src, size, flags); accept the other one if only that decodes.
        for (int swap = 0; swap < 2; ++swap) {
            const uint64_t dst = swap ? b : a, src = swap ? a : b;
            for (bool zl : {true, false}) {
                if (InflateGuest(reinterpret_cast<const uint8_t*>(src), n, zl, &data)) {
                    std::memcpy(reinterpret_cast<void*>(dst), data.data(), data.size());
                    if (logged++ < 5) UT_INFO("d3d: LZDecompressMemory %u -> %zu bytes (%s, %s order)", n, data.size(), zl ? "zlib" : "raw", swap ? "swapped" : "dst,src");
                    return 0;
                }
            }
        }
        UT_WARN("d3d: LZDecompressMemory %#llx %#llx size %u flags %#x did not decode (first bytes %02x %02x)", (unsigned long long)a, (unsigned long long)b, n,
                (uint32_t)c.Arg(3), c.Arg(1) > 0x10000 ? *reinterpret_cast<uint8_t*>(c.Arg(1)) : 0, c.Arg(1) > 0x10000 ? *reinterpret_cast<uint8_t*>(c.Arg(1) + 1) : 0);
        return 0;
    });
    // ---- creation hook: Fallback calls OnCreate after it made the object ----
    // ---- Map / Unmap ----
    OverrideAll("Map", [](Call& c) -> uint32_t {
        { static int n = 0; if (n++ < 6) UT_INFO("d3d: Map this=%#llx res=%#llx sub=%#llx type=%#llx flags=%#llx out=%#llx", (unsigned long long)c.args[0], (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2), (unsigned long long)c.Arg(3), (unsigned long long)c.Arg(4)); }
        Res* r = ResAt(c.Arg(0));
        auto* out = c.Ptr<uint64_t>(4);
        if (r == nullptr || out == nullptr) { if (out) std::memset(out, 0, 16); return 0x80004005u; }
        const uint32_t type = static_cast<uint32_t>(c.Arg(2));
        if (r->buffer) return MapBuffer(c, *r, type, out);
        if (r->texture && r->placement == 0 && type == 5 && g_kernel != nullptr && !(r->bind & 0x60)) {
            // WRITE_NO_OVERWRITE on a plain texture: the console keeps the memory mapped, and the title writes into it again later
            // without another Map. Give the texture memory of its own that stays (linear, laid out the way xg_x.dll says) and
            // treat it like a placement texture from here on: its changes are noticed when it is next drawn with.
            r->tile_mode = 0;
            if (const TileModel* model = ModelFor(*r)) {
                uint64_t size = 0;
                for (const TileLevel& lv : model->levels) size = std::max<uint64_t>(size, lv.base + static_cast<uint64_t>(lv.height) * lv.row_stride);
                uint64_t base = 0, length = (size + 0xFFFF) & ~0xFFFFull;
                if (length != 0 && g_kernel->Vm().Allocate(&base, &length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == STATUS_SUCCESS) {
                    r->placement = base;
                    r->host_size = size;
                    r->placement_bytes = size;
                    std::scoped_lock lock {g_res_mutex};
                    g_placed.push_back(r);
                }
            }
        }
        if (r->texture && r->placement != 0) {
            // A placement texture: Map hands out the memory the texture lives in (the title writes its texels there, in the
            // layout the console's GPU reads) and they are picked up at the next draw that uses the texture.
            if (const TileModel* model = ModelFor(*r)) {
                const auto& info = gpu::TextureInfo(*r->texture);
                const uint32_t sub = static_cast<uint32_t>(c.Arg(1));
                const uint32_t slice = sub / std::max(1u, info.mips), mip = sub % std::max(1u, info.mips);
                if (slice < info.array_size && slice * model->levels_per_slice + mip < model->levels.size()) {
                    const TileLevel& lv = model->levels[slice * model->levels_per_slice + mip];
                    const uint64_t size = model->linear ? static_cast<uint64_t>(lv.height) * lv.row_stride : std::max<uint64_t>((lv.height + 7) / 8, 1) * std::max<uint64_t>(lv.row_stride, ((lv.width + 7) / 8) * lv.tile_bytes);
                    out[0] = r->placement + lv.base;
                    reinterpret_cast<uint32_t*>(out)[2] = static_cast<uint32_t>(lv.row_stride);
                    reinterpret_cast<uint32_t*>(out)[3] = static_cast<uint32_t>(size);
                    r->placement_dirty = true;
                    return 0;
                }
            }
        }
        if (r->texture) {
            if (static const bool log = std::getenv("UTSURO_MAPLOG") != nullptr; log) {
                static std::mutex m;
                static std::map<std::string, int> seen;
                const auto& i = gpu::TextureInfo(*r->texture);
                char key[128];
                std::snprintf(key, sizeof key, "texture %ux%u fmt %u mips %u sub %llu type %llu usage %u bind %#x", i.width, i.height, i.format, i.mips, (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2), r->usage, r->bind);
                std::scoped_lock lock {m};
                const int n = ++seen[key];
                if (n == 1 || n % 200 == 0) UT_INFO("d3d: Map %s (call %d)", key, n);
            }
            uint32_t mip, slice, row, rows, depth;
            TextureShape(gpu::TextureInfo(*r->texture), static_cast<uint32_t>(c.Arg(1)), &mip, &slice, &row, &rows, &depth);
            r->staging.assign(static_cast<size_t>(row) * rows * depth, 0);
            r->map_subresource = static_cast<uint32_t>(c.Arg(1));
            r->map_write = type != 1;
            if (type == 1 || type == 3) gpu::ReadbackTexture(*r->texture, mip, slice, r->staging.data(), row);
            out[0] = reinterpret_cast<uint64_t>(r->staging.data());
            reinterpret_cast<uint32_t*>(out)[2] = row;
            reinterpret_cast<uint32_t*>(out)[3] = row * rows;
            return 0;
        }
        std::memset(out, 0, 16);
        return 0x80004005u;
    });
    OverrideAll("Unmap", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->texture || r->staging.empty()) return 0;
        if (r->map_write) {
            uint32_t mip, slice, row, rows, depth;
            TextureShape(gpu::TextureInfo(*r->texture), r->map_subresource, &mip, &slice, &row, &rows, &depth);
            gpu::UploadTexture(*r->texture, mip, slice, nullptr, r->staging.data(), row, row * rows);
        }
        r->staging.clear();
        r->staging.shrink_to_fit();
        return 0;
    });
    // UpdateSubresource(resource, subresource, box*, data, row pitch, depth pitch)
    OverrideAll("UpdateSubresource", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || c.Arg(3) == 0) return 0;
        const auto* box = c.Ptr<uint32_t>(2);
        if (r->buffer) {
            const uint32_t size = gpu::BufferSize(*r->buffer);
            if (box == nullptr) {
                uint8_t* p = gpu::RenameBuffer(r->buffer, false);
                std::memcpy(p, c.Ptr<void>(3), size);
            } else {
                const uint32_t left = std::min(box[0], size), right = std::min(box[3], size);
                uint8_t* p = gpu::RenameBuffer(r->buffer, true);
                if (right > left) std::memcpy(p + left, c.Ptr<void>(3), right - left);
            }
            return 0;
        }
        if (r->texture) {
            uint32_t mip, slice, row, rows, depth;
            TextureShape(gpu::TextureInfo(*r->texture), static_cast<uint32_t>(c.Arg(1)), &mip, &slice, &row, &rows, &depth);
            gpu::UploadTexture(*r->texture, mip, slice, box, c.Ptr<void>(3), static_cast<uint32_t>(c.Arg(4)) ? static_cast<uint32_t>(c.Arg(4)) : row,
                               static_cast<uint32_t>(c.Arg(5)));
        }
        return 0;
    });
    OverrideAll("CopyResource", [](Call& c) -> uint32_t {
        Res* dst = ResAt(c.Arg(0));
        Res* src = ResAt(c.Arg(1));
        if (dst == nullptr || src == nullptr) return 0;
        if (dst->buffer && src->buffer) {
            gpu::CopyBuffer(*dst->buffer, 0, *src->buffer, 0, std::min(gpu::BufferSize(*dst->buffer), gpu::BufferSize(*src->buffer)));
        } else if (dst->texture && src->texture) {
            const auto& d = gpu::TextureInfo(*src->texture);
            for (uint32_t a = 0; a < d.array_size; ++a)
                for (uint32_t m = 0; m < d.mips; ++m) gpu::CopyTexture(*dst->texture, m, a, 0, 0, 0, *src->texture, m, a, nullptr);
        }
        return 0;
    });
    // CopySubresourceRegion(dst, dst sub, x, y, z, src, src sub, box*)
    OverrideAll("CopySubresourceRegion", [](Call& c) -> uint32_t {
        Res* dst = ResAt(c.Arg(0));
        Res* src = ResAt(c.Arg(4));
        if (dst == nullptr || src == nullptr) return 0;
        const auto* box = c.Ptr<uint32_t>(6);
        if (dst->buffer && src->buffer) {
            const uint32_t left = box ? box[0] : 0, right = box ? box[3] : gpu::BufferSize(*src->buffer);
            gpu::CopyBuffer(*dst->buffer, static_cast<uint32_t>(c.Arg(2)), *src->buffer, left, right - left);
        } else if (dst->texture && src->texture) {
            uint32_t sm, ss, sr, srs, sd, dm, ds, dr, drs, dd;
            TextureShape(gpu::TextureInfo(*src->texture), static_cast<uint32_t>(c.Arg(5)), &sm, &ss, &sr, &srs, &sd);
            TextureShape(gpu::TextureInfo(*dst->texture), static_cast<uint32_t>(c.Arg(1)), &dm, &ds, &dr, &drs, &dd);
            gpu::CopyTexture(*dst->texture, dm, ds, static_cast<uint32_t>(c.Arg(2)), static_cast<uint32_t>(c.Arg(3)), 0, *src->texture, sm, ss, box);
        } else if (static const bool log = std::getenv("UTSURO_COPYLOG") != nullptr; log) {
            static std::atomic<int> shown {0};
            if (shown++ < 30) {
                auto describe = [](Res* r) -> std::string {
                    char b[96];
                    if (r->texture) { const auto& i = gpu::TextureInfo(*r->texture); std::snprintf(b, sizeof b, "texture %ux%u fmt %u placed %d", i.width, i.height, i.format, r->placement != 0); }
                    else std::snprintf(b, sizeof b, "buffer %u bytes host %#llx", r->host_size, (unsigned long long)r->host_ptr);
                    return b;
                };
                UT_INFO("d3d: CopySubresourceRegion %s <- %s (box %s, dst sub %llu, src sub %llu)", describe(dst).c_str(), describe(src).c_str(), box ? "yes" : "no", (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(5));
            }
        }
        return 0;
    });
    OverrideAll("GenerateMips", [](Call& c) -> uint32_t {
        if (Res* r = ResAt(c.Arg(0)); r && r->view && r->view->texture) gpu::GenerateMips(*r->view->texture);
        return 0;
    });

    // ---- clears ----
    OverrideAll("ClearRenderTargetView", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->view || c.Arg(1) == 0) return 0;
        gpu::ClearRenderTarget(*r->view, c.Ptr<float>(1));
        return 0;
    });
    // ClearDepthStencilView(dsv, flags, depth (xmm3), stencil)
    OverrideAll("ClearDepthStencilView", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->view) return 0;
        gpu::ClearDepthStencil(*r->view, static_cast<uint32_t>(c.Arg(1)), c.Float(2), static_cast<uint8_t>(c.Arg(3)));
        return 0;
    });
    OverrideAll("ClearState", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s = CtxState {};
        return 0;
    });

    // IASetPlacementIndexBuffer(HardwareIndexFormat (0: 16 bit, 1: 32 bit), buffer, address of the indices)
    OverrideAll("IASetPlacementIndexBuffer", [](Call& c) -> uint32_t {
        auto& st = StateOf(c.obj);
        st.ib = c.Arg(1);
        st.ib_address = c.Arg(2);
        st.ib_format = (c.Arg(0) & 0xFF) == 0 ? 57 : 42;
        st.ib_offset = 0;
        return 0;
    });

    // FlushGpuCacheRange(flags, address, size): the title wrote this memory with the CPU; the GPU's copy of it is stale.
    OverrideAll("FlushGpuCacheRange", [](Call& c) -> uint32_t {
        const uint64_t from = c.Arg(1), to = c.Arg(1) + c.Arg(2);
        static std::atomic<int> shown {0};
        if (std::getenv("UTSURO_PLACE") != nullptr && shown++ < 12) UT_INFO("d3d: FlushGpuCacheRange(%#llx, %#llx, %#llx)", (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2));
        std::scoped_lock lock {g_res_mutex};
        int hit = 0;
        for (Res* r : g_placed)
            if (r->placement < to && r->placement + r->host_size > from) { r->placement_dirty = true; r->next_try_frame = 0; ++hit; }
        if (static const bool log = std::getenv("UTSURO_PLACE") != nullptr; log) {
            static std::atomic<int> total {0}, shown {0};
            ++total;
            if (shown++ < 25) UT_INFO("d3d: FlushGpuCacheRange(%#llx, %#llx): %d placement textures; %d flushes so far", (unsigned long long)from, (unsigned long long)(to - from), hit, total.load());
        }
        return 0;
    });

    // ExecuteCommandList(list, restore state): the recorded draws run now, on the immediate context's render targets.
    OverrideAll("ExecuteCommandList", [](Call& c) -> uint32_t {
        Res* list = ResAt(c.Arg(0));
        if (list == nullptr || !gpu::Available()) return 0;
        const CtxState& imm = StateOf(c.obj);
        for (const RecordedDraw& rd : list->recorded) {
            gpu::DrawState d = rd.state;
            for (int i = 0; i < 8; ++i)
                if (!d.rtv[i]) if (Res* r = ResAt(imm.rtv[i])) d.rtv[i] = r->view;
            if (!d.dsv) if (Res* r = ResAt(imm.dsv)) d.dsv = r->view;
            if (d.num_viewports == 0) {
                d.num_viewports = imm.num_viewports;
                std::memcpy(d.viewports, imm.viewports, sizeof d.viewports);
            }
            gpu::Draw(d, rd.args);
        }
        // The title records a list per frame and runs it once; the host never frees objects, so the recording (with the data each draw
        // copied) goes now.
        std::vector<RecordedDraw>().swap(list->recorded);
        return 0;
    });

    // InsertFence(flags) returns the fence's value; titles spin until it is not zero. All work here is done when the call returns,
    // so no fence stays pending (IsFencePending keeps answering "no").
    OverrideAll("InsertFence", [](Call& c) -> uint32_t {
        static std::atomic<uint64_t> next {1};
        c.wide_result = next++;
        c.wide_set = true;
        return 0;
    });

    // ---- draws ----
    OverrideAll("Draw", [](Call& c) -> uint32_t {
        gpu::DrawArgs a;
        a.count = static_cast<uint32_t>(c.Arg(0));
        a.start = static_cast<uint32_t>(c.Arg(1));
        DoDraw(c.obj, a);
        return 0;
    });
    // The Xbox vtable packs start and count into one 64-bit argument: (count << 32) | start.
    OverrideAll("DrawIndexed", [](Call& c) -> uint32_t {
        gpu::DrawArgs a;
        a.indexed = true;
        a.count = static_cast<uint32_t>(c.Arg(0) >> 32);
        a.start = static_cast<uint32_t>(c.Arg(0));
        a.base_vertex = static_cast<int32_t>(c.Arg(1));
        DoDraw(c.obj, a);
        return 0;
    });
    OverrideAll("DrawInstanced", [](Call& c) -> uint32_t {
        gpu::DrawArgs a;
        a.count = static_cast<uint32_t>(c.Arg(0));
        a.start = static_cast<uint32_t>(c.Arg(1));
        a.start_instance = static_cast<uint32_t>(c.Arg(1) >> 32);
        a.instances = static_cast<uint32_t>(c.Arg(2));
        DoDraw(c.obj, a);
        return 0;
    });
    OverrideAll("DrawIndexedInstanced", [](Call& c) -> uint32_t {
        gpu::DrawArgs a;
        a.indexed = true;
        a.count = static_cast<uint32_t>(c.Arg(0) >> 32);
        a.start = static_cast<uint32_t>(c.Arg(0));
        a.base_vertex = static_cast<int32_t>(c.Arg(1));
        a.start_instance = static_cast<uint32_t>(c.Arg(1) >> 32);
        a.instances = static_cast<uint32_t>(c.Arg(2));
        DoDraw(c.obj, a);
        return 0;
    });

    // ---- constant buffer ranges (D3D11.1) ----
    static const char* const kStages[6] = {"VS", "HS", "DS", "GS", "PS", "CS"};
    for (int st = 0; st < 6; ++st) {
        OverrideAll((std::string(kStages[st]) + "SetConstantBuffers1").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) {
                const uint64_t k = c.Arg(0) + i;
                s.cb[st][k] = c.Arg(2) ? c.Ptr<uint64_t>(2)[i] : 0;
                s.cb_first[st][k] = c.Arg(3) ? c.Ptr<uint32_t>(3)[i] : 0;
                s.cb_count[st][k] = c.Arg(4) ? c.Ptr<uint32_t>(4)[i] : 4096;
            }
            return 0;
        });
    }

    // ---- the swap chain ----
    OverrideAll("Present", [](Call& c) -> uint32_t {
        Res* r = c.obj.user ? static_cast<Res*>(c.obj.user.get()) : nullptr;
        ++g_frame;
        if (gpu::Available()) gpu::Present(r ? r->texture : nullptr, static_cast<uint32_t>(c.Arg(0)));
        return 0;
    });
    // GetBuffer(index, riid, void** surface): the swap chain's back buffer as a texture.
    OverrideAll("GetBuffer", [](Call& c) -> uint32_t {
        Res* sc = c.obj.user ? static_cast<Res*>(c.obj.user.get()) : nullptr;
        uint64_t& cached = c.obj.u64["buffer" + std::to_string(c.Arg(0))];
        if (cached == 0) {
            Obj* t = Create("ID3D11Texture2D", &cached);
            if (t == nullptr) return 0x80004005u;
            Res& tr = R(*t);
            if (sc != nullptr) tr.texture = sc->texture;
            uint32_t sd[12] {};
            auto d = c.obj.str.find("desc");
            if (d != c.obj.str.end()) std::memcpy(sd, d->second.data(), std::min<size_t>(d->second.size(), sizeof sd));
            uint32_t td[11] {sd[0], sd[1], 1, 1, sd[2], sd[4], sd[5], 0 /*usage*/, 0x28 /*SRV|RTV*/, 0, 0};
            t->str["desc"].assign(reinterpret_cast<char*>(td), sizeof td);
            t->u64["device"] = DeviceOf(c);
            UT_INFO("d3d: swap chain buffer %llu is %ux%u format %u", (unsigned long long)c.Arg(0), sd[0], sd[1], sd[2]);
        }
        *c.Ptr<uint64_t>(2) = cached;
        return kSOk;
    });
}
