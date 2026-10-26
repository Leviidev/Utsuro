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
    // A placement buffer's own memory, given at creation. Binding it with a base address moves host_ptr (that is where draws read),
    // but UpdateSubresource, copies and Map write the buffer itself: writing at the last bound address instead overwrote memory the
    // title had moved on to use for something else.
    uint64_t own_ptr {0};
    // A placement texture: where the title keeps its texels (in the console GPU's tiled layout), and whether they were written since the last upload.
    uint64_t placement {0};
    uint32_t tile_mode {0}, pitch {0};
    bool placement_dirty {false};
    uint64_t next_try_frame {0};                  ///< a texture with no readable pages yet is looked at again from this frame on
    uint64_t placement_bytes {0};                ///< how much of the placement memory the texels occupy
    uint64_t content_hash {0}, checked_frame {~0ull};   ///< to notice texels the CPU rewrote without telling anyone
    uint64_t next_check_frame {0};                ///< PlacementChanged backs off on textures that keep the same contents...
    uint32_t check_interval {1};                  ///< ...doubling up to 8 frames; a Map or a cache flush over them resets it
    Res* base {nullptr};                         ///< a view's resource
    std::vector<RecordedDraw> recorded;          ///< a command list: what to replay
    bool final_om {false}, final_vp {false};     ///< a command list: the targets / viewports it left bound, which stay bound after it runs
    uint64_t final_rtv[8] {}, final_dsv {0};
    uint32_t final_nvp {0};
    Viewport final_vps[16] {};
    std::vector<gpu::InputElement> elements;     ///< an input layout's elements
    bool gpu_written {false};
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
    // Titles bind sentinels such as -1 now and then; a pointer that is not one must not be followed.
    uint64_t probe = 0;
    if ((pointer & 7) != 0 || pointer < 0x10000 || !SafeRead(pointer + 8, &probe, 8)) return nullptr;
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

// The last constant-buffer bind calls (ring): dumped with the race-time draw probe, since the binds that
// matter may happen long before the draws that need them.
struct CbCallLog { uint64_t frame, ctx, start, count, p0; uint32_t stage; };
static CbCallLog g_cb_calls[64];
static std::atomic<uint32_t> g_cb_calls_n {0};
static void LogCbCall(uint32_t stage, uint64_t ctx, uint64_t start, uint64_t count, uint64_t p0) {
    const uint32_t i = g_cb_calls_n.fetch_add(1) % 64;
    g_cb_calls[i] = {g_frame.load(), ctx, start, count, p0, stage};
}
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
    } else if (n == "CreateTexture1D" || n == "CreateTexture2D" || n == "CreateTexture3D" || n == "CreatePlacementTexture2D" || n == "CreatePlacementTexture3D") {
        // Placement textures: (desc, tile mode, pitch, placement address, ppTexture). Forza Horizon 2's colour grading LUTs are
        // 3D placement textures; without them its tonemap read an empty LUT and every frame came out black.
        const bool placed = n == "CreatePlacementTexture2D" || n == "CreatePlacementTexture3D";
        gpu::TextureDesc d;
        uint32_t bind, misc, usage;
        if (n == "CreateTexture1D") {
            d.width = U32(desc, 0); d.height = 1; d.mips = MipCount(U32(desc, 1), d.width, 1); d.array_size = std::max(1u, U32(desc, 2)); d.format = U32(desc, 3);
            usage = U32(desc, 4); bind = U32(desc, 5); misc = U32(desc, 7);
        } else if (n == "CreateTexture2D" || n == "CreatePlacementTexture2D") {
            d.width = U32(desc, 0); d.height = U32(desc, 1); d.mips = MipCount(U32(desc, 2), d.width, d.height); d.array_size = std::max(1u, U32(desc, 3));
            d.format = U32(desc, 4); d.samples = std::max(1u, U32(desc, 5)); usage = U32(desc, 7); bind = U32(desc, 8); misc = U32(desc, 10);
        } else {
            d.width = U32(desc, 0); d.height = U32(desc, 1); d.depth = std::max(1u, U32(desc, 2)); d.mips = MipCount(U32(desc, 3), d.width, d.height, d.depth);
            d.format = U32(desc, 4); usage = U32(desc, 5); bind = U32(desc, 6); misc = U32(desc, 8);
        }
        {
            // MipLevels 0 asks for the whole chain; GetDesc must then report the chain's length, as D3D does (Forza
            // Horizon 2 sizes a per-mip loop from it and a 0 runs that loop off the end of its arrays).
            std::string& stored = o.str["desc"];
            const size_t at = n == "CreateTexture1D" ? 4 : (n == "CreateTexture3D" || n == "CreatePlacementTexture3D" ? 12 : 8);
            if (stored.size() >= at + 4) std::memcpy(&stored[at], &d.mips, 4);
        }
        d.shader_resource = (bind & 0x8) != 0 || bind == 0;
        d.render_target = (bind & 0x20) != 0;
        d.depth_stencil = (bind & 0x40) != 0;
        d.cube = (misc & 0x4) != 0;
        r.usage = usage;
        r.bind = bind;
        r.texture = gpu::CreateTexture(d);
        UT_INFO("d3d: Created %s %ux%u fmt %u placed=%d res=%p native=%p bind=%#x host_ptr=%#llx", n.c_str(), d.width, d.height, d.format, placed, &r, r.texture ? gpu::NativeOf(*r.texture) : nullptr, bind, (unsigned long long)(placed ? c.Arg(3) : 0));
        if (std::getenv("UTSURO_TEXLOG") != nullptr) {
            static std::atomic<int> shown {0};
            if (shown++ < 60) UT_INFO("d3d: %s %ux%u fmt %u mips %u array %u bind %#x usage %u misc %#x -> %s", n.c_str(), d.width, d.height, d.format, d.mips, d.array_size, bind, usage, misc, r.texture ? "ok" : "NO TEXTURE");
        }
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
                r.host_size = std::max(r.pitch, row) * rows * d.array_size * d.depth * 2;   // generous: room for the mips
                std::scoped_lock lock {g_res_mutex};
                g_placed.push_back(&r);
            }
            return;
        }
        if (d.width <= 4 && d.height <= 4 && (bind & 0x60) && std::getenv("UTSURO_RTDIAG") != nullptr) {
            static std::atomic<int> shown {0};
            GuestCpuState st;
            if (shown++ < 24 && c.thread.control != nullptr && c.thread.control->CaptureCpuState(&st)) {
                uint64_t regs[16];
                for (int k = 0; k < 16; ++k) regs[k] = st.gregs[k];
                const auto frames = c.kernel.Backtrace(st.rip, regs, 10);
                std::string chain, words;
                for (size_t k = 1; k < frames.size(); ++k) chain += (k > 1 ? " < " : "") + c.kernel.Describe(frames[k]);
                for (size_t k = 0; k < 11; ++k) { char t[12]; std::snprintf(t, sizeof t, " %x", U32(desc, k)); words += t; }
                UT_INFO("d3d: rtdiag %s desc%s -> %#llx from %s", n.c_str(), words.c_str(), (unsigned long long)reinterpret_cast<uint64_t>(&o), chain.c_str());
            }
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
        if (base != nullptr && !base->texture && !base->buffer && std::getenv("UTSURO_CBDIAG") != nullptr) {
            static std::atomic<int> shown {0};
            if (shown++ < 60) UT_INFO("d3d: cbdiag %s on a placement buffer host %#llx %u B usage %u bind %#x: no view", n.c_str(), (unsigned long long)base->host_ptr, base->host_size, base->usage, base->bind);
        }
        if (base == nullptr || (!base->texture && !base->buffer)) return;
        auto v = std::make_shared<gpu::TextureView>();
        UT_INFO("d3d: %s obj=%p base_res=%p native=%p view fmt %u", n.c_str(), &o, base, (base && base->texture) ? gpu::NativeOf(*base->texture) : nullptr, U32(desc, 0));
        v->texture = base->texture;
        v->buffer = base->buffer;
        r.base = base;
        if (n == "CreateShaderResourceView") SetIdDescriptor(o, 0x30, 0x20);
        const bool have = c.Arg(1) != 0;
        gpu::ViewDesc& vd = v->desc;
        if (have) {
            static int shown = 0;
            if (shown++ < 12) {
                if (base->texture) {
                    UT_INFO("d3d: %s desc %u %u %u %u %u %u (texture %ux%u fmt %u)", n.c_str(), U32(desc, 0), U32(desc, 1), U32(desc, 2), U32(desc, 3), U32(desc, 4), U32(desc, 5),
                            gpu::TextureInfo(*base->texture).width, gpu::TextureInfo(*base->texture).height, gpu::TextureInfo(*base->texture).format);
                } else if (base->buffer) {
                    UT_INFO("d3d: %s desc %u %u %u %u %u %u (buffer %u bytes)", n.c_str(), U32(desc, 0), U32(desc, 1), U32(desc, 2), U32(desc, 3), U32(desc, 4), U32(desc, 5),
                            gpu::BufferSize(*base->buffer));
                }
            }
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
                case 1: case 11: vd.first_slice = U32(desc, 2); vd.slice_count = U32(desc, 3); break;   // a buffer: first element, element count
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
            } else if (n == "CreateUnorderedAccessView") {
                switch (vd.dimension) {
                case 2: case 4: vd.first_mip = U32(desc, 2); vd.mip_count = 1; break;
                case 3: case 5: vd.first_mip = U32(desc, 2); vd.mip_count = 1; vd.first_slice = U32(desc, 3); vd.slice_count = U32(desc, 4); break;
                case 8: vd.first_mip = U32(desc, 2); vd.mip_count = 1; vd.first_slice = U32(desc, 3); vd.slice_count = 1; break;
                }
            }
            // A format of 0 keeps the texture's own.
        }
        if (n == "CreateUnorderedAccessView" && base->buffer) {
            const uint32_t zero = 0;
            v->counter = gpu::CreateBuffer(4, false, &zero);   // the hidden append/consume counter every buffer UAV carries
        }
        if (n == "CreateShaderResourceView" && have && base->texture && gpu::TextureInfo(*base->texture).samples > 1 && vd.dimension != 6 && vd.dimension != 7) {
            // A single-sample view of a multisample texture: on the console that reads the surface's FMASK (which samples hold
            // which fragment). There is no FMASK here: hand out zeros, "every sample holds fragment 0", so a resolve shader
            // takes its uncompressed path (Forza Horizon 2's scene resolve read colour bits as sample indices otherwise, and its
            // auto-exposure saw luminance over 1000 and darkened the frame to black).
            static std::shared_ptr<gpu::Texture> zeros;
            if (!zeros) {
                gpu::TextureDesc zd;
                zd.width = zd.height = 1;
                zd.format = 2;   // R32G32B32A32_FLOAT
                zeros = gpu::CreateTexture(zd);
                const float z[4] = {0, 0, 0, 0};
                if (zeros) gpu::UploadTexture(*zeros, 0, 0, nullptr, z, 16, 16);
            }
            static std::atomic<int> shown {0};
            if (shown++ < 8) UT_INFO("d3d: a %u-sample texture's single-sample view (dimension %u, format %u) reads as FMASK zeros", gpu::TextureInfo(*base->texture).samples, vd.dimension, vd.format);
            v->texture = zeros;
            v->desc = gpu::ViewDesc {};
        }
        r.view = v;
    } else if (n.size() > 12 && n.compare(0, 6, "Create") == 0 && n.find("Shader") != std::string::npos && n != "CreateShaderResourceView") {
        if (c.Arg(0) == 0) return;
        const auto* code = c.Ptr<uint8_t>(0);
        const size_t length = static_cast<size_t>(c.Arg(1));
        const gpu::ShaderStage stage = n == "CreateVertexShader" ? gpu::ShaderStage::Vertex : n == "CreatePixelShader" ? gpu::ShaderStage::Pixel : n == "CreateComputeShader" ? gpu::ShaderStage::Compute : gpu::ShaderStage::Unknown;
        if (n == "CreateComputeShader") {
            static int cs_count = 0;
            char path[256];
            std::snprintf(path, sizeof path, "/tmp/cs_%d.bin", cs_count++);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(code, 1, length, f); std::fclose(f); }
        }
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
    } else if (n == "CreateDeferredContext" || n == "CreateDeferredContext1" || n == "CreateDeferredContext2" || n == "CreateDeferredContextX") {
        o.u64["deferred"] = 1;
    } else if (n == "FinishCommandList") {
        r.recorded = std::move(StateOf(c.obj).recorded);
        StateOf(c.obj).recorded.clear();
        {
            CtxState& ds = StateOf(c.obj);
            r.final_om = ds.om_bound;
            r.final_vp = ds.vp_bound;
            std::memcpy(r.final_rtv, ds.rtv, sizeof r.final_rtv);
            r.final_dsv = ds.dsv;
            r.final_nvp = ds.num_viewports;
            std::memcpy(r.final_vps, ds.viewports, sizeof r.final_vps);
            ds.om_bound = ds.vp_bound = false;   // the next list starts out using the executing context's
        }
        // FinishCommandList(RestoreDeferredContextState, list): on the PC, FALSE leaves the deferred context in its default state.
        // Doing that here (UTSURO_FINISHRESET=1) crashed Forza Horizon 2's game and render threads as its 3D phase began.
        if (static const bool reset = std::getenv("UTSURO_FINISHRESET") != nullptr; reset && (c.Arg(0) & 0xFF) == 0) StateOf(c.obj) = CtxState {};
        Tally("~lists finished");
        Tally("~list draws finished", r.recorded.size());
        static std::atomic<int> shown {0};
        if (shown++ < 24) UT_INFO("d3d: FinishCommandList on %p (deferred %d): %zu draws -> list %p", static_cast<void*>(&c.obj), c.obj.u64["deferred"] != 0 ? 1 : 0, r.recorded.size(), static_cast<void*>(&o));
    } else if (n == "CreatePlacementBuffer") {
        // (desc, placement address, ppBuffer): the title keeps the data; descriptors point straight at it.
        r.usage = U32(desc, 1);
        r.bind = U32(desc, 2);
        r.host_ptr = c.Arg(1);
        r.own_ptr = c.Arg(1);
        r.host_size = U32(desc, 0);
        SetBufferDescriptor(o, 0, r.host_size);
        if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag && (r.bind & 4)) {
            static std::atomic<int> shown {0};
            GuestCpuState st;
            if (shown++ < 40 && c.thread.control != nullptr && c.thread.control->CaptureCpuState(&st)) {
                uint64_t regs[16];
                for (int k = 0; k < 16; ++k) regs[k] = st.gregs[k];
                const auto frames = c.kernel.Backtrace(st.rip, regs, 8);
                std::string chain;
                for (size_t k = 1; k < frames.size(); ++k) chain += (k > 1 ? " < " : "") + c.kernel.Describe(frames[k]);
                UT_INFO("d3d: cbdiag CreatePlacementBuffer %u B usage %u bind %#x cpu %#x at %#llx from %s", r.host_size, r.usage, r.bind, U32(desc, 3), (unsigned long long)r.host_ptr, chain.c_str());
            }
        }   // the title adds the buffer's address itself (found by watching what its packets hold)
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
    uint64_t computer {0};         ///< the XGTextureAddressComputer, kept for 2D tilings, whose CopyFromSubresource (copy_fn) detiles
    uint64_t copy_fn {0};
    uint32_t bytes {0};            ///< per element
    uint32_t levels_per_slice {0};
    std::vector<TileLevel> levels; ///< [slice * levels_per_slice + mip]
};
std::mutex g_tile_mutex;
std::map<std::string, TileModel> g_tile_models;
uint64_t g_probe_dest = 0, g_probe_comp = 0, g_probe_pitch = 0, g_probe_vtable7 = 0; uint32_t g_probe_key = 0;   // UTSURO_XGPROBE

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
    if (static const bool probe = std::getenv("UTSURO_XGPROBE") != nullptr; probe) {
        static std::once_flag once;
        std::call_once(once, [&] { for (int i = 0; i < 16; ++i) UT_INFO("xg: computer vtable[%d] = %s", i, g_kernel->Describe(vtable[i]).c_str()); });
    }
    // The computer's methods moved between xg_x.dll versions: the older one (Forza Horizon 2, Minecraft) has eight, with
    // GetTexelElementOffsetBytes at slot 4 and CopyFromSubresource at 6; newer ones (Rocket League, Sonic Mania) have twelve, the
    // same two at 6 and 9. Past the last method the table runs into strings.
    const bool wide = xg::InImage(vtable[8]) && xg::InImage(vtable[11]);
    const uint64_t offset_fn = vtable[wide ? 6 : 4];
    static const uint64_t bit_offset = Runtime::Get().AllocGuest(16, 16);
    auto at = [&](uint32_t mip, uint32_t x, uint32_t y, uint32_t slice) {
        return xg::Call(*g_kernel, offset_fn, {comp, 0, mip, x, y, slice, 0, bit_offset});
    };
    m.levels_per_slice = info.mips;
    const bool linear_layout = tile_mode == 0 || tile_mode == 1;
    bool ok = true;
    for (uint32_t slice = 0; slice < info.array_size && ok; ++slice) {
        for (uint32_t mip = 0; mip < info.mips && ok; ++mip) {
            TileLevel lv;
            lv.width = std::max(1u, ((info.width >> mip) + block - 1) / block);
            lv.height = std::max(1u, ((info.height >> mip) + block - 1) / block);
            lv.base = at(mip, 0, 0, slice);
            if (lv.base == ~0ull) { ok = false; break; }
            if (linear_layout) {
                m.linear = true;
                lv.tile_bytes = 0;
                lv.row_stride = pitch ? pitch : (lv.height > 1 ? at(mip, 0, 1, slice) - lv.base : static_cast<uint64_t>(lv.width) * m.bytes);
                m.levels.push_back(lv);
                continue;
            }
            lv.tile_bytes = lv.width > 8 ? at(mip, 8, 0, slice) - lv.base : 64ull * m.bytes;
            lv.row_stride = lv.height > 8 ? at(mip, 0, 8, slice) - lv.base : (pitch ? pitch : static_cast<uint64_t>((lv.width + 7) / 8) * lv.tile_bytes);
            m.levels.push_back(lv);
        }
    }
    if (static const bool probe = std::getenv("UTSURO_XGPROBE") != nullptr; probe && ok && !linear_layout && info.width >= 64 && info.height >= 16 && info.array_size == 1) {
        // Which slots are GetTexelElementOffsetBytes and CopyFromSubresource in this xg_x.dll: ask for a texel whose 1D
        // thin-tiled offset is known, and copy mip 0 out and compare with our own detiling.
        static std::once_flag once;
        std::call_once(once, [&] {
            const TileLevel& lv = m.levels[0];
            const uint32_t x = 9, y = 3;
            const uint64_t expect = lv.base + (y / 8) * lv.row_stride + (x / 8) * lv.tile_bytes + Morton8(x & 7, y & 7) * m.bytes;
            const uint64_t bit = Runtime::Get().AllocGuest(16, 16);
            for (int slot = 3; slot <= 6; ++slot) {
                const uint64_t r = xg::Call(*g_kernel, vtable[slot], {comp, 0, 0, x, y, 0, 0, bit});
                UT_INFO("xg probe: %ux%u fmt %u tile %u: slot %d (texel 9,3) -> %#llx, expected 1D offset %#llx", info.width, info.height, info.format, tile_mode, slot, (unsigned long long)r, (unsigned long long)expect);
            }
            const uint64_t pitch = static_cast<uint64_t>(lv.width) * m.bytes;
            const uint64_t dest = Runtime::Get().AllocGuest(pitch * lv.height + 64, 64);
            std::memset(reinterpret_cast<void*>(dest), 0xA5, pitch * lv.height);
            // The texture memory itself is not known here; the caller's placement is: look it up from the first placed resource with this model later.
            g_probe_dest = dest; g_probe_comp = comp; g_probe_pitch = pitch; g_probe_vtable7 = vtable[7]; g_probe_key = info.width * 1000003u + info.height * 1009u + info.format;
            return;   // keep the computer alive for the copy probe
        });
        if (g_probe_comp == comp) { m.ok = ok; return m; }
    }
    if (static const bool no_xg = std::getenv("UTSURO_NOXGCOPY") != nullptr; !no_xg && !linear_layout && tile_mode >= 4) {
        // 2D tilings (bank and pipe swizzled macro tiles): xg_x.dll copies them out itself. 1D thin tiling (mode 2) is
        // the Morton layout above, which the host does faster.
        m.computer = comp;
        m.copy_fn = vtable[wide ? 9 : 6];
    } else {
        xg::Call(*g_kernel, vtable[1], {comp});   // Release
    }
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
    if (r.placement == 0 || r.placement_bytes == 0 || r.placement_bytes > (4u << 20) || (r.bind & 0x40) != 0) return false;
    const uint64_t frame = g_frame.load();
    if (frame < r.next_check_frame) return false;
    {
        // Several deferred contexts draw with the same textures at once: one of them hashes per frame.
        std::atomic_ref<uint64_t> checked {r.checked_frame};
        uint64_t seen = checked.load(std::memory_order_relaxed);
        if (seen == frame || !checked.compare_exchange_strong(seen, frame)) return false;
    }
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
    if (h == r.content_hash) {
        // Unchanged: look again less often. Textures the CPU really rewrites (glyph caches) keep being checked every frame.
        r.check_interval = std::min<uint32_t>(r.check_interval * 2, 8);
        r.next_check_frame = frame + r.check_interval;
        return false;
    }
    r.check_interval = 1;
    r.next_check_frame = frame + 1;
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
    if (!r.texture || r.placement == 0 || (r.bind & 0x40) != 0) return;
    const auto& info = gpu::TextureInfo(*r.texture);
    if (info.depth > 1) {
        // A volume (Forza Horizon 2's colour grading LUTs). Tiled ones are copied out by xg_x.dll's 3D address computer; the
        // descriptor is D3D11_TEXTURE3D_DESC followed, as for 2D, by two ESRAM words and then the tile mode and pitch.
        std::vector<uint8_t> level;
        uint32_t ebytes, eblock;
        ElementOf(info.format, &ebytes, &eblock);
        if (r.tile_mode > 1 && g_kernel != nullptr && ebytes == 4) {
            // Tiled: xg_x.dll's 3D address computer, asked once per shape to copy out a source whose every element holds its own
            // index. That gives the top level's layout as a table, and each later upload (the title rewrites its LUTs) is a host
            // gather with no call into the guest.
            static std::mutex m;
            static std::map<std::string, std::vector<uint32_t>> layouts;   // shape -> tiled element index of each linear texel
            char key[96];
            std::snprintf(key, sizeof key, "%u %u %u %u %u %u %u", info.width, info.height, info.depth, info.mips, info.format, r.tile_mode, r.pitch);
            std::vector<uint32_t>* layout = nullptr;
            {
                std::scoped_lock lock {m};
                auto it = layouts.find(key);
                if (it == layouts.end()) {
                    std::vector<uint32_t> table;
                    if (const uint64_t create = xg::Export("XGCreateTexture3DComputer"); create != 0) {
                        const uint64_t desc = Runtime::Get().AllocGuest(0x40, 16), out = Runtime::Get().AllocGuest(0x10, 16);
                        auto* d = reinterpret_cast<uint32_t*>(desc);
                        std::memset(d, 0, 0x40);
                        d[0] = info.width; d[1] = info.height; d[2] = info.depth; d[3] = info.mips; d[4] = info.format; d[6] = 8; d[11] = r.tile_mode; d[12] = r.pitch;
                        *reinterpret_cast<uint64_t*>(out) = 0;
                        const uint64_t hr = xg::Call(*g_kernel, create, {desc, out});
                        if (hr == 0 && *reinterpret_cast<uint64_t*>(out) != 0) {
                            const uint64_t comp = *reinterpret_cast<uint64_t*>(out);
                            const uint64_t* vtable = *reinterpret_cast<uint64_t**>(comp);
                            const bool wide = xg::InImage(vtable[8]) && xg::InImage(vtable[11]);
                            const uint64_t texels = static_cast<uint64_t>(info.width) * info.height * info.depth;
                            const uint64_t src_words = texels * 4;   // room for tile padding
                            const uint64_t src = Runtime::Get().AllocGuest(src_words * 4, 256), dst = Runtime::Get().AllocGuest(texels * 4 + 64, 64);
                            for (uint64_t k = 0; k < src_words; ++k) reinterpret_cast<uint32_t*>(src)[k] = static_cast<uint32_t>(k);
                            const uint32_t row = info.width * 4;
                            if (xg::Call(*g_kernel, vtable[wide ? 9 : 6], {comp, dst, 0, 0, src, row, static_cast<uint64_t>(row) * info.height}) == 0) {
                                table.assign(reinterpret_cast<const uint32_t*>(dst), reinterpret_cast<const uint32_t*>(dst) + texels);
                                for (uint32_t v : table) if (v >= src_words) { table.clear(); break; }
                            }
                        }
                        UT_INFO("d3d: xg layout for a %s volume: %s", key, table.empty() ? "none" : "ok");
                    }
                    it = layouts.emplace(key, std::move(table)).first;
                }
                layout = &it->second;
            }
            if (!layout->empty()) {
                const size_t texels = layout->size();
                uint32_t max_index = 0;
                for (uint32_t v : *layout) max_index = std::max(max_index, v);
                std::vector<uint32_t> tiled(static_cast<size_t>(max_index) + 1);
                if (SafeRead(r.placement, tiled.data(), tiled.size() * 4)) {
                    std::vector<uint32_t> linear(texels);
                    for (size_t k = 0; k < texels; ++k) linear[k] = tiled[(*layout)[k]];
                    const uint32_t row = info.width * 4;
                    gpu::UploadTexture(*r.texture, 0, 0, nullptr, linear.data(), row, row * info.height);
                    static std::atomic<int> shown {0};
                    if (shown++ < 4) {
                        auto texel = [&](uint32_t x, uint32_t y, uint32_t z) { return linear[(static_cast<size_t>(z) * info.height + y) * info.width + x]; };
                        const uint32_t w = info.width - 1, h = info.height - 1, dd = info.depth - 1;
                        UT_INFO("d3d: volume %ux%ux%u fmt %u tile %u detiled: (0,0,0) %08x (max,0,0) %08x (0,max,0) %08x (0,0,max) %08x (max,max,max) %08x", info.width, info.height, info.depth, info.format,
                                r.tile_mode, texel(0, 0, 0), texel(w, 0, 0), texel(0, h, 0), texel(0, 0, dd), texel(w, h, dd));
                    }
                    return;
                }
            }
        }
        // Otherwise linear rows and slices; each mip follows the last.
        uint64_t at = r.placement;
        for (uint32_t mip = 0; mip < info.mips; ++mip) {
            const uint32_t w = std::max(1u, info.width >> mip), h = std::max(1u, info.height >> mip), dd = std::max(1u, info.depth >> mip);
            uint32_t row, rows;
            FormatShape(info.format, w, h, &row, &rows);
            const uint32_t pitch = mip == 0 ? std::max(r.pitch, row) : row;
            level.resize(static_cast<size_t>(pitch) * rows * dd);
            if (!SafeRead(at, level.data(), level.size())) break;
            gpu::UploadTexture(*r.texture, mip, 0, nullptr, level.data(), pitch, pitch * rows);
            at += level.size();
        }
        static std::atomic<int> shown {0};
        if (shown++ < 8) {
            size_t nz = 0;
            for (uint8_t b : level) nz += b != 0;
            UT_INFO("d3d: placement volume %ux%ux%u fmt %u tile %u pitch %u uploaded from %#llx (last level %zu non-zero bytes)", info.width, info.height, info.depth, info.format, r.tile_mode, r.pitch,
                    (unsigned long long)r.placement, nz);
        }
        return;
    }
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
    if (g_probe_comp != 0 && g_probe_key == info.width * 1000003u + info.height * 1009u + info.format && !model->linear) {
        static std::once_flag once;
        std::call_once(once, [&] {
            const TileLevel& lv = model->levels[0];
            const uint64_t* vt = *reinterpret_cast<uint64_t**>(g_probe_comp);
            const uint64_t slice = g_probe_pitch * lv.height;
            const std::vector<std::pair<const char*, std::vector<uint64_t>>> variants = {
                {"(dest,0,0,src,pitch,slice)", {g_probe_dest, 0, 0, r.placement, g_probe_pitch, slice}},
                {"(dest,pitch,slice,0,0,src)", {g_probe_dest, g_probe_pitch, slice, 0, 0, r.placement}},
                {"(dest,pitch,slice,0,src)", {g_probe_dest, g_probe_pitch, slice, 0, r.placement}},
                {"(dest,0,src,pitch,slice)", {g_probe_dest, 0, r.placement, g_probe_pitch, slice}},
            };
            for (int slot = 6; slot <= 7; ++slot) for (const auto& [name, a] : variants) {
                std::memset(reinterpret_cast<void*>(g_probe_dest), 0xA5, slice);
                std::vector<uint64_t> args {g_probe_comp}; args.insert(args.end(), a.begin(), a.end());
                const uint64_t hr = xg::Call(*g_kernel, vt[slot], args);
                size_t same = 0, total = 0, nonfill = 0;
                const auto* dst = reinterpret_cast<const uint8_t*>(g_probe_dest);
                for (uint32_t y = 0; y < std::min<uint32_t>(lv.height, 32); ++y) for (uint32_t x = 0; x < std::min<uint32_t>(lv.width, 64); ++x) {
                    const uint64_t src = lv.base + (y / 8) * lv.row_stride + (x / 8) * lv.tile_bytes + Morton8(x & 7, y & 7) * model->bytes;
                    const uint8_t* d = dst + y * g_probe_pitch + x * model->bytes;
                    ++total;
                    if (d[0] != 0xA5) ++nonfill;
                    if (src + model->bytes <= raw.size() && std::memcmp(raw.data() + src, d, model->bytes) == 0) ++same;
                }
                UT_INFO("xg probe: slot %d %s -> %#llx; %zu written, %zu of %zu match our 1D detile", slot, name, (unsigned long long)hr, nonfill, same, total);
            }
        });
    }
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
    bool stream_prefix = true;
    for (uint32_t slice = 0; slice < info.array_size; ++slice) {
        for (uint32_t mip = 0; mip < info.mips; ++mip) {
            const TileLevel& lv = model->levels[slice * model->levels_per_slice + mip];
            if (!level_present(lv)) { if (slice == 0 && stream_prefix) lowest_missing = mip + 1; continue; }
            const uint32_t row_pitch = lv.width * bytes;
            linear.resize(static_cast<size_t>(row_pitch) * lv.height);
            bool copied = false;
            if (model->copy_fn != 0) {
                // The console's own detiling, into a guest scratch buffer the helper thread can write.
                static uint64_t scratch = 0, scratch_size = 0;
                const uint64_t need = static_cast<uint64_t>(row_pitch) * lv.height + 64;
                if (need > scratch_size) { scratch = Runtime::Get().AllocGuest(std::max<uint64_t>(need, 1u << 20), 64); scratch_size = std::max<uint64_t>(need, 1u << 20); }
                const auto t0 = std::chrono::steady_clock::now();
                const uint64_t hr = xg::Call(*g_kernel, model->copy_fn, {model->computer, scratch, static_cast<uint64_t>(slice) * info.mips + mip, 0, r.placement, row_pitch, static_cast<uint64_t>(row_pitch) * lv.height});
                if (hr == 0) {
                    std::memcpy(linear.data(), reinterpret_cast<const void*>(scratch), linear.size());
                    copied = true;
                    static std::atomic<uint64_t> calls {0}, micros {0}, bytes_detiled {0};
                    static std::atomic<int64_t> last_report {0};
                    calls++; micros += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); bytes_detiled += static_cast<uint64_t>(row_pitch) * lv.height;
                    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now - last_report.load() >= 5) { last_report = now; UT_INFO("d3d: xg detile so far: %llu levels, %llu ms, %llu MB", (unsigned long long)calls.load(), (unsigned long long)(micros.load() / 1000), (unsigned long long)(bytes_detiled.load() >> 20)); }
                }
            }
            if (!copied) {
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
            }
            if (slice == 0 && stream_prefix) {
                // Committed but never filled (a level the streamer has not delivered): not a level the shader may sample yet.
                const uint64_t* w = reinterpret_cast<const uint64_t*>(linear.data());
                const size_t words = linear.size() / 8;
                size_t k = 0;
                while (k < words && w[k] == 0) ++k;
                if (k == words && linear.size() >= 64 && mip + 1 < info.mips) {
                    lowest_missing = mip + 1;
                    continue;
                }
                stream_prefix = false;
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
    if (static const bool stats = std::getenv("UTSURO_STREAMLOG2") != nullptr; stats && info.width >= 512) {
        static std::mutex m;
        static std::map<std::string, std::pair<uint32_t, uint32_t>> seen;   // texture -> (first mip sampled, mips)
        std::scoped_lock lock {m};
        char key[64];
        std::snprintf(key, sizeof key, "%#llx %ux%u f%u", (unsigned long long)r.placement, info.width, info.height, info.format);
        seen[key] = {lowest_missing, info.mips};
        static uint64_t last_frame = 0;
        if (g_frame.load() >= last_frame + 600) {
            last_frame = g_frame.load();
            size_t full = 0, partial = 0, none = 0;
            for (auto& [k, v] : seen) { if (v.first == 0) ++full; else if (v.first + 1 >= v.second) ++none; else ++partial; }
            UT_INFO("d3d: streamed textures >=512: %zu with every level, %zu with some levels, %zu with only the smallest level", full, partial, none);
        }
    }
}

// ---- the title's inline descriptor stream ----------------------------------------------------------
// Packets: a header (register | (dwords - 1) << 16) and the dwords. Registers are byte offsets of resource slots: textures (T#, 8 dwords)
// from 0x6b40 in 0x20 steps, samplers (S#, 4) from 0x7b40 in 0x10 steps, vertex streams (V#, 4) from 0x7d70, and constant buffers
// (V#, 4) from 0x1c40 (vertex stage) and from 0x6a00 (pixel stage).
constexpr uint32_t kRegTexture = 0x6b40, kRegSampler = 0x7b40, kRegVertex = 0x7d70, kRegCbA = 0x1c40, kRegCbB = 0x6a00;

/// Whether every page of [address, address + bytes) was readable the first time it was looked at: the mach probe that
/// SafeCopy makes costs more than the copy itself, and these descriptors point into the title's constant and vertex
/// memory, which stays mapped once it exists. A page that failed is probed again each time (it may be committed later).
bool PagesKnownReadable(uint64_t address, uint32_t bytes) {
    static thread_local std::vector<uint64_t> known;   // sorted page addresses, a few thousand at most
    const uint64_t first = address & ~4095ull, last = (address + bytes - 1) & ~4095ull;
    for (uint64_t page = first; page <= last; page += 4096) {
        auto it = std::lower_bound(known.begin(), known.end(), page);
        if (it != known.end() && *it == page) continue;
        uint64_t probe;
        if (!SafeRead(page, &probe, 8)) return false;
        known.insert(it, page);
    }
    return true;
}

std::shared_ptr<gpu::Buffer> CopyOfHost(uint64_t address, uint32_t bytes, bool persistent = false) {
    if (address == 0 || bytes == 0) return nullptr;
    static thread_local std::vector<uint8_t> data;
    const uint32_t target_size = std::max<uint32_t>(bytes, 16);
    data.resize(target_size);
    if (PagesKnownReadable(address, bytes)) {
        std::memcpy(data.data(), reinterpret_cast<const void*>(address), bytes);
        if (bytes < target_size) std::memset(data.data() + bytes, 0, target_size - bytes);
    } else if (!SafeCopy(address, data.data(), bytes)) {
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
        if (got < target_size) std::memset(data.data() + got, 0, target_size - got);
    } else if (bytes < target_size) {
        std::memset(data.data() + bytes, 0, target_size - bytes);
    }
    if (persistent) return gpu::CreateBuffer(static_cast<uint32_t>(data.size()), false, data.data());
    return gpu::CreateTransientBuffer(static_cast<uint32_t>(data.size()), data.data());
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
void BindStream(CtxState& s, gpu::DrawState& d, gpu::DrawArgs& args, bool deferred = false, RecordedDraw* rd = nullptr) {

    // The indices the draw reads, and with them the vertices. (An indirect draw's counts are on the GPU: whole streams.)
    const bool indirect = args.indirect != nullptr;
    int64_t first_vertex = args.indexed ? 0 : args.start;
    int64_t last_vertex = args.indexed ? 0 : static_cast<int64_t>(args.start) + args.count - 1;
    if (indirect) {
        first_vertex = 0;
        last_vertex = INT32_MAX;
        if (args.indexed && s.ib_address != 0) {
            static std::atomic<int> shown {0};
            if (shown++ < 4) UT_WARN("d3d: indirect indexed draw from a placement index buffer: its size is not known here");
        }
    } else if (args.indexed && s.ib_address != 0) {
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
            if (deferred)
                d.index_buffer = gpu::CreateBuffer(static_cast<uint32_t>(indices.size()), false, indices.data());
            else
                d.index_buffer = gpu::CreateTransientBuffer(static_cast<uint32_t>(indices.size()), indices.data());
            d.index_offset = 0;
            d.index_format = s.ib_format;
            args.start = 0;
            if (lo <= hi) {
                first_vertex = static_cast<int64_t>(lo) + args.base_vertex;
                last_vertex = static_cast<int64_t>(hi) + args.base_vertex;
                args.base_vertex = -static_cast<int32_t>(lo);
            }
            if (rd) {
                rd->ib_addr = at;
                rd->ib_size = static_cast<uint32_t>(args.count) * size;
            }
        }
    } else if (args.indexed) {
        // An index buffer that is a real GPU buffer (not placement memory): its indices are in shared memory too, so read
        // the range this draw uses. Without the range every vertex stream below is cut to a single vertex, and every
        // triangle collapses to the first vertex -- Forza Horizon 2's world renders nothing at all that way (the whole
        // 1920x1080 scene target stayed at the clear value while ~600 draws a frame went through).
        Res* ibr = ResAt(s.ib);
        if (ibr != nullptr && ibr->buffer != nullptr && args.count > 0) {
            const uint32_t size = s.ib_format == 42 ? 4 : 2;
            const uint32_t bytes = static_cast<uint32_t>(args.count) * size;
            const uint64_t at = static_cast<uint64_t>(s.ib_offset) + static_cast<uint64_t>(args.start) * size;
            const uint8_t* data = gpu::BufferData(*ibr->buffer);
            if (data != nullptr && at + bytes <= gpu::BufferSize(*ibr->buffer)) {
                uint32_t lo = 0xFFFFFFFFu, hi = 0;
                for (uint32_t i = 0; i < args.count; ++i) {
                    const uint32_t v = size == 4 ? reinterpret_cast<const uint32_t*>(data + at)[i] : reinterpret_cast<const uint16_t*>(data + at)[i];
                    if (v == 0xFFFF && size == 2) continue;   // restart index
                    lo = std::min(lo, v); hi = std::max(hi, v);
                }
                if (lo <= hi) {
                    first_vertex = static_cast<int64_t>(lo) + args.base_vertex;
                    last_vertex = static_cast<int64_t>(hi) + args.base_vertex;
                    args.base_vertex = -static_cast<int32_t>(lo);
                    static std::atomic<int> shown {0};
                    if (shown++ < 6) UT_INFO("d3d: indexed draw count %u: index range %u..%u from a GPU index buffer (streams were cut to one vertex before)", args.count, lo, hi);
                }
            }
        }
    } else {
        args.start = 0;
    }
    if (s.regs.empty()) return;
    auto get = [&](uint32_t reg) -> const CtxState::Reg* { auto it = s.regs.find(reg); return it == s.regs.end() ? nullptr : &it->second; };
    auto address = [](const CtxState::Reg& r) { return static_cast<uint64_t>(r.dw[0]) | (static_cast<uint64_t>(r.dw[1] & 0xFFFF) << 32); };
    Res* layout = ResAt(s.layout);
    for (uint32_t k = 0; k < 8; ++k) {
        const auto* v = get(kRegVertex + 0x10 * k);
        if (v == nullptr || v->n < 3) continue;
        const uint64_t base = address(*v);
        const uint32_t stride = (v->dw[1] >> 16) & 0x3FFF, size = v->dw[2];
        if (k == 0 && args.indexed && args.count == 6 && std::getenv("UTSURO_QUADLOG") != nullptr && g_frame.load() >= static_cast<uint64_t>(std::atoi(std::getenv("UTSURO_QUADLOG")))) {
            static std::atomic<int> shown {0};
            if (shown++ < 3000) UT_INFO("d3d: quad draw: stream0 base %#llx stride %u size %u dw %08x %08x %08x %08x; first_vertex %lld last %lld base_vertex %d start %u", (unsigned long long)base, stride, size, v->dw[0], v->dw[1], v->dw[2], v->dw[3], (long long)first_vertex, (long long)last_vertex, args.base_vertex, args.start);
        }
        if (stride == 0 || base == 0) continue;
        bool per_instance = false;
        if (layout != nullptr) for (const auto& e : layout->elements) if (e.slot == k && e.slot_class == 1) per_instance = true;
        uint64_t lo, hi;
        if (per_instance) { lo = 0; hi = indirect ? size : std::min<uint64_t>(size, (static_cast<uint64_t>(args.start_instance) + args.instances) * stride); }
        else { lo = static_cast<uint64_t>(std::max<int64_t>(first_vertex, 0)) * stride; hi = std::min<uint64_t>(size, static_cast<uint64_t>(std::max<int64_t>(last_vertex + 1, 0)) * stride); }
        if (hi <= lo) continue;
        d.vb[k].buffer = CopyOfHost(base + lo, static_cast<uint32_t>(hi - lo), deferred);
        d.vb[k].stride = stride;
        d.vb[k].offset = 0;
        if (rd) {
            rd->vb_addr[k] = base + lo;
            rd->vb_size[k] = static_cast<uint32_t>(hi - lo);
        }
    }
    for (int stage = 0; stage < 2; ++stage) {
        const auto& sh = stage == 0 ? d.vs : d.ps;
        if (!sh) continue;
        const auto& rf = gpu::Reflection(*sh);
        for (uint32_t k = 0; k < 14; ++k) {
            if (!(rf.constant_buffers & (1u << k))) continue;
            const auto* v = get((stage == 0 ? kRegCbA : kRegCbB) + 0x10 * k);
            if (v == nullptr || v->n < 3) {
                // A deferred draw whose declared constant buffers mostly have no packet: dump this context's whole
                // register map once -- Forza Horizon 2's world draws bind their vertex constants somewhere we do not
                // look, and this says where.
                static std::atomic<int> shown {0};
                uint32_t declared = 0, unbound = 0;
                for (uint32_t j = 0; j < 14; ++j) {
                    if (!(rf.constant_buffers & (1u << j))) continue;
                    ++declared;
                    const auto* w = get((stage == 0 ? kRegCbA : kRegCbB) + 0x10 * j);
                    if (w == nullptr || w->n < 3) ++unbound;
                }
                if (deferred && declared >= 3 && unbound >= 2 && g_frame.load() >= (std::getenv("UTSURO_CBFROM") != nullptr ? std::strtoull(std::getenv("UTSURO_CBFROM"), nullptr, 10) : ~0ull) && shown++ < 4) {
                    std::vector<uint32_t> keys;
                    for (const auto& [reg, r] : s.regs) keys.push_back(reg);
                    std::sort(keys.begin(), keys.end());
                    std::string list;
                    for (uint32_t reg : keys) { char t[24]; std::snprintf(t, sizeof t, " %#x(%u)", reg, s.regs[reg].n); list += t; }
                    UT_INFO("d3d: %s cb%u has no packet (deferred draw, %u of %u declared cbs unbound, %zu regs):%s", stage ? "ps" : "vs", k, unbound, declared, s.regs.size(), list.c_str());
                    int dumped = 0;
                    for (uint32_t reg : keys) {
                        const auto& r = s.regs[reg];
                        if (r.n < 3) continue;
                        UT_INFO("d3d:   reg %#x n %u: %08x %08x %08x %08x", reg, r.n, r.dw[0], r.dw[1], r.dw[2], r.dw[3]);
                        if (++dumped >= 48) break;
                    }
                }
                continue;
            }
            uint32_t size = std::min<uint32_t>(v->dw[2], 65536);
            if (rf.cb_size[k] != 0) size = std::min<uint32_t>(size, rf.cb_size[k] * 16);   // the shader reads no more than it declared
            d.cb[stage][k].buffer = CopyOfHost(address(*v), size, deferred);
            if (!d.cb[stage][k].buffer) {
                static std::atomic<int> shown {0};
                if (std::getenv("UTSURO_REGLOG") != nullptr && shown++ < 12)
                    UT_INFO("d3d: %s cb%u packet (n %u): %08x %08x %08x %08x gives no buffer (address %#llx size %u)", stage ? "ps" : "vs", k, v->n, v->dw[0], v->dw[1], v->dw[2], v->dw[3], (unsigned long long)address(*v), size);
            }
            d.cb[stage][k].offset = 0;
            d.cb[stage][k].size = size;
            if (rd) {
                rd->cb_addr[stage][k] = address(*v);
                rd->cb_size[stage][k] = size;
            }
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

static CtxState* GetImmCtx(Obj& ctx) {
    auto dit = ctx.u64.find("device");
    if (dit != ctx.u64.end()) {
        if (Obj* dev = Runtime::Get().FromPointer(dit->second)) {
            auto cit = dev->u64.find("context");
            if (cit != dev->u64.end()) {
                if (Obj* imm = Runtime::Get().FromPointer(cit->second)) {
                    return &StateOf(*imm);
                }
            }
        }
    }
    return nullptr;
}

gpu::DrawState Snapshot(Obj& ctx, CtxState& s, gpu::DrawArgs& args, RecordedDraw* rd = nullptr) {
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
    const bool deferred = ctx.u64["deferred"] != 0;
    for (int stage = 0; stage < 2; ++stage) {
        const int src = stage == 0 ? 0 : 4;
        const auto& sh = stage == 0 ? d.vs : d.ps;
        if (!sh) continue;
        const auto& rf = gpu::Reflection(*sh);
        for (int i = 0; i < 16; ++i) {
            if (!(rf.constant_buffers & (1u << i))) continue;
            int cb_st = src;
            int cb_sl = i;
            if (s.cb_remap[src][i].stage < 6) {
                cb_st = s.cb_remap[src][i].stage;
                cb_sl = s.cb_remap[src][i].slot;
            }
            uint64_t addr = s.cb_address[cb_st][cb_sl];
            uint64_t cb_res = s.cb[cb_st][cb_sl];
            uint32_t first = s.cb_first[cb_st][cb_sl];
            uint32_t count = s.cb_count[cb_st][cb_sl];
            // A deferred draw's unset slot stays empty here: ExecuteCommandList fills it from the executing context then.
            if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag && stage == 0 && i == 2) {
                static std::mutex m;
                static std::map<uint64_t, int> seen;
                std::scoped_lock lock {m};
                const uint64_t key = addr ? addr : cb_res;
                if (seen.size() < 80 && seen[key]++ == 0) {
                    Res* r = ResAt(cb_res);
                    const uint64_t from = addr ? addr + first * 16 : (r ? r->host_ptr + first * 16 : 0);
                    uint32_t w[16] {}; int nz = -1;
                    if (from && SafeRead(from, w, 64)) { nz = 0; for (uint32_t x : w) nz += x != 0; }
                    UT_INFO("d3d: cbdiag vs cb2 slot: addr %#llx res %#llx (buffer %d host %#llx %u B) first %u count %u src %#llx nonzero-of-16 %d, rf size %u, frame %llu ctx %llu deferred %d remap %d",
                            (unsigned long long)addr, (unsigned long long)cb_res, r && r->buffer ? 1 : 0, (unsigned long long)(r ? r->host_ptr : 0), r ? r->host_size : 0, first, count, (unsigned long long)from, nz,
                            rf.cb_size[i], (unsigned long long)g_frame.load(), (unsigned long long)ctx.id, deferred ? 1 : 0, s.cb_remap[src][i].stage < 6 ? 1 : 0);
                }
            }
            if (addr != 0) {
                uint32_t size = count ? count * 16 : 65536;
                if (rf.cb_size[i] != 0) size = std::min<uint32_t>(size, rf.cb_size[i] * 16);
                const uint64_t final_addr = addr + first * 16;
                d.cb[stage][i].buffer = CopyOfHost(final_addr, size, deferred);
                d.cb[stage][i].offset = 0;
                d.cb[stage][i].size = size;
                if (rd != nullptr && stage < 2 && i < 14) {
                    rd->cb_addr[stage][i] = final_addr;
                    rd->cb_size[stage][i] = size;
                }
            } else if (Res* r = ResAt(cb_res)) {
                if (r->buffer) {
                    d.cb[stage][i].buffer = r->buffer;
                    d.cb[stage][i].offset = first * 16;
                    d.cb[stage][i].size = count ? count * 16 : gpu::BufferSize(*r->buffer);
                    if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag && stage == 0 && i == 2) {
                        static std::mutex m;
                        static std::map<uint64_t, int> seen;
                        std::scoped_lock lock {m};
                        if (seen.size() < 60 && seen[cb_res]++ == 0) {
                            const uint32_t* w = reinterpret_cast<const uint32_t*>(gpu::BufferData(*r->buffer));
                            uint32_t nz = 0, n = gpu::BufferSize(*r->buffer) / 4;
                            for (uint32_t k = 0; w && k < n; ++k) nz += w[k] != 0;
                            UT_INFO("d3d: cbdiag vs cb2 buffer obj %#llx res %p: %u B usage %u bind %#x, %u of %u dwords non-zero now, first %u count %u, frame %llu ctx %llu deferred %d",
                                    (unsigned long long)cb_res, static_cast<void*>(r), gpu::BufferSize(*r->buffer), r->usage, r->bind, nz, n, first, count, (unsigned long long)g_frame.load(), (unsigned long long)ctx.id, deferred ? 1 : 0);
                        }
                    }
                } else if (r->host_ptr != 0) {
                    uint32_t size = count ? count * 16 : (r->host_size ? r->host_size : 65536);
                    if (rf.cb_size[i] != 0) size = std::min<uint32_t>(size, rf.cb_size[i] * 16);
                    const uint64_t final_addr = r->host_ptr + first * 16;
                    if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag) {
                        // Where the title's own buffer descriptor (object +0x20) points now, against the creation address.
                        static std::mutex m;
                        static std::map<uint64_t, int> seen;
                        std::scoped_lock lock {m};
                        if (seen.size() < 200 && seen[cb_res]++ == 0) {
                            uint64_t desc[2] {};
                            SafeRead(cb_res + 0x20, desc, 16);
                            auto zero = [](uint64_t a, uint32_t n) { uint32_t w[16] {}; if (!SafeRead(a, w, std::min<uint32_t>(n, 64))) return -1; for (uint32_t x : w) if (x) return 0; return 1; };
                            const uint64_t base = desc[0] & 0xFFFFFFFFFFFFull;
                            UT_INFO("d3d: cbdiag %s cb%d obj %#llx host %#llx (%u B, zero %d) desc base %#llx dw2 %#x (zero %d) first %u count %u frame %llu deferred %d",
                                    stage ? "ps" : "vs", i, (unsigned long long)cb_res, (unsigned long long)r->host_ptr, r->host_size, zero(r->host_ptr, size),
                                    (unsigned long long)base, (unsigned)(desc[1] & 0xFFFFFFFF), base ? zero(base, size) : -2, first, count, (unsigned long long)g_frame.load(), deferred ? 1 : 0);
                        }
                    }
                    d.cb[stage][i].buffer = CopyOfHost(final_addr, size, deferred);
                    d.cb[stage][i].offset = 0;
                    d.cb[stage][i].size = size;
                    if (rd != nullptr && stage < 2 && i < 14) {
                        rd->cb_addr[stage][i] = final_addr;
                        rd->cb_size[stage][i] = size;
                    }
                }
            }
        }
        for (int i = 0; i < 16; ++i) {
            int srv_st = src;
            int srv_sl = i;
            if (s.srv_remap[src][i].stage < 6) {
                srv_st = s.srv_remap[src][i].stage;
                srv_sl = s.srv_remap[src][i].slot;
            }
            uint64_t srv_ptr = s.srv[srv_st][srv_sl];
            if (!srv_ptr && deferred) {
                if (CtxState* imm_s = GetImmCtx(ctx)) srv_ptr = imm_s->srv[srv_st][srv_sl];
            }
            if (Res* r = ResAt(srv_ptr)) {
                Res* target = r->base ? r->base : r;
                if (target->placement != 0 && ((target->placement_dirty && g_frame.load() >= target->next_try_frame) || PlacementChanged(*target)))
                    UploadPlacement(*target);
                d.srv[stage][i] = r->view;
            }
            if (rf.samplers & (1u << i)) {
                int samp_st = src;
                int samp_sl = i;
                if (s.sampler_remap[src][i].stage < 6) {
                    samp_st = s.sampler_remap[src][i].stage;
                    samp_sl = s.sampler_remap[src][i].slot;
                }
                uint64_t samp_ptr = s.sampler[samp_st][samp_sl];
                if (!samp_ptr && deferred) {
                    if (CtxState* imm_s = GetImmCtx(ctx)) samp_ptr = imm_s->sampler[samp_st][samp_sl];
                }
                if (Res* r = ResAt(samp_ptr)) d.sampler[stage][i] = r->state;
            }
        }
        for (uint8_t slot : rf.high_slots) {
            int srv_st = src;
            int srv_sl = slot;
            if (s.srv_remap[src][slot].stage < 6) {
                srv_st = s.srv_remap[src][slot].stage;
                srv_sl = s.srv_remap[src][slot].slot;
            }
            uint64_t srv_ptr = s.srv[srv_st][srv_sl];
            if (!srv_ptr && deferred) {
                if (CtxState* imm_s = GetImmCtx(ctx)) srv_ptr = imm_s->srv[srv_st][srv_sl];
            }
            if (Res* r = ResAt(srv_ptr)) {
                Res* target = r->base ? r->base : r;
                if (target->placement != 0 && ((target->placement_dirty && g_frame.load() >= target->next_try_frame) || PlacementChanged(*target)))
                    UploadPlacement(*target);
                d.extra_srv[stage].emplace_back(slot, r->view);
            }
            if (static const bool diag = std::getenv("UTSURO_SRVDIAG") != nullptr; diag) {
                Res* r = ResAt(srv_ptr);
                if (!r || !r->view) {
                    static std::mutex m;
                    static std::map<std::pair<int, int>, int> seen;
                    std::scoped_lock lock {m};
                    if (seen[{stage, slot}]++ < 3) {
                        const auto reg = s.regs.find(kRegTexture + 0x20 * slot);
                        Obj* o = srv_ptr ? Runtime::Get().FromPointer(srv_ptr) : nullptr;
                        UT_INFO("d3d: srvdiag ctx %llu%s %s t%u: srv %#llx (res %p view %p obj %p) remap %u/%u, register %s, base %p buffer %p host %#llx",
                                (unsigned long long)ctx.id, deferred ? " (deferred)" : "", stage ? "ps" : "vs", slot, (unsigned long long)srv_ptr, static_cast<void*>(r),
                                r ? static_cast<void*>(r->view.get()) : nullptr, static_cast<void*>(o), s.srv_remap[src][slot].stage, s.srv_remap[src][slot].slot,
                                reg != s.regs.end() ? "set" : "none", r ? static_cast<void*>(r->base) : nullptr, r && r->base ? static_cast<void*>(r->base->buffer.get()) : nullptr,
                                r && r->base ? (unsigned long long)r->base->host_ptr : 0ull);
                    }
                }
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (s.vb_address[i] != 0 && s.stride[i] != 0) {
            const uint32_t size = 65536 * 16;
            d.vb[i].buffer = CopyOfHost(s.vb_address[i], size, deferred);
            d.vb[i].stride = s.stride[i];
            d.vb[i].offset = s.voffset[i];
            if (rd != nullptr && i < 8) {
                rd->vb_addr[i] = s.vb_address[i];
                rd->vb_size[i] = size;
            }
        } else if (Res* r = ResAt(s.vb[i])) {
            if (r->buffer) {
                d.vb[i].buffer = r->buffer;
            } else if (r->host_ptr != 0 && s.stride[i] != 0) {
                const uint32_t size = r->host_size ? r->host_size : 65536;
                d.vb[i].buffer = CopyOfHost(r->host_ptr, size, deferred);
            }
            d.vb[i].stride = s.stride[i];
            d.vb[i].offset = s.voffset[i];
        }
    }
    if (Res* r = ResAt(s.ib)) {
        if (r->buffer) {
            d.index_buffer = r->buffer;
        } else if (r->host_ptr != 0) {
            s.ib_address = r->host_ptr + s.ib_offset;
        }
    }
    d.index_format = s.ib_format;
    d.index_offset = s.ib_offset;
    d.topology = s.topology;
    if (d.ps) {
        const auto& prf = gpu::Reflection(*d.ps);
        for (int i = 0; i < 64; ++i) if ((prf.uavs & (1u << i)) && s.uav[i] != 0) if (Res* r = ResAt(s.uav[i])) d.ps_uav[i] = r->view;
    }
    for (int i = 0; i < 8; ++i) {
        if (Res* r = ResAt(s.rtv[i])) d.rtv[i] = r->view;
    }
    if (static const bool diag = std::getenv("UTSURO_RTDIAG") != nullptr; diag) {
        // Views of tiny textures drawn with a big viewport: what the title keeps in the view objects (it may write the real
        // surface's registers there itself).
        static std::mutex m;
        static std::map<uint64_t, int> seen;
        auto dump = [&](const char* what, uint64_t view) {
            Res* r = ResAt(view);
            if (r == nullptr || r->base == nullptr || !r->base->texture) return;
            const auto& info = gpu::TextureInfo(*r->base->texture);
            static int big = 0;
            const bool tiny = info.width <= 4;
            std::scoped_lock lock {m};
            if (!tiny && (info.width != 1920 || big >= 3)) return;
            if (seen.size() >= 16 || seen[view]++ != 0) return;
            if (!tiny) ++big;
            uint32_t w[64] {};
            SafeRead(view, w, sizeof w);
            Obj* o = Runtime::Get().FromPointer(view);
            UT_INFO("d3d: rtdiag %s %#llx (%s, texture %ux%u fmt %u native %p, obj blocks %zu) viewport %gx%g frame %llu:", what, (unsigned long long)view, o ? o->class_name.c_str() : "?", info.width, info.height, info.format,
                    gpu::NativeOf(*r->base->texture), o ? o->blocks.size() : 0, s.num_viewports ? s.viewports[0].w : 0.f, s.num_viewports ? s.viewports[0].h : 0.f, (unsigned long long)g_frame.load());
            for (int row = 0; row < 8; ++row)
                UT_INFO("d3d:   +%#04x: %08x %08x %08x %08x %08x %08x %08x %08x", row * 32, w[row * 8], w[row * 8 + 1], w[row * 8 + 2], w[row * 8 + 3], w[row * 8 + 4], w[row * 8 + 5], w[row * 8 + 6], w[row * 8 + 7]);
        };
        dump("rtv0", s.rtv[0]);
        dump("dsv", s.dsv);
    }
    if (s.rtv[0] != 0 && (!d.rtv[0] || !d.rtv[0]->texture)) {
            // A draw into a target the host could not resolve ("no pass" in the frame stats): say which.
            static std::atomic<int> shown {0};
            Res* r = ResAt(s.rtv[0]);
            Res* base = r ? r->base : nullptr;
            if (shown++ < 12) UT_INFO("d3d: draw into an unresolved render target %#llx: res %p view %p base %p %s", (unsigned long long)s.rtv[0], r, r ? r->view.get() : nullptr, base,
                                      base && base->texture ? "(base has a texture)" : base && base->buffer ? "(base is a buffer)" : "(no base resource)");
        }
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
    BindStream(s, d, args, deferred, rd);
    return d;
}

// At the traced frame: where in the title each of the first draws comes from (UTSURO_TRACEFRAME).
void TraceDrawCaller(Call& c) {
    static uint64_t traced_frame = ~0ull;
    static int shown = 0;
    const uint64_t f = g_frame.load();
    if (!gpu::IsTraceFrame(f)) return;
    if (traced_frame != f) { traced_frame = f; shown = 0; }
    if (shown++ >= 24) return;
    GuestCpuState st;
    if (c.thread.control == nullptr || !c.thread.control->CaptureCpuState(&st)) return;
    uint64_t regs[16];
    for (int k = 0; k < 16; ++k) regs[k] = st.gregs[k];
    const auto frames = c.kernel.Backtrace(st.rip, regs, 8);
    std::string chain;
    for (size_t k = 1; k < frames.size(); ++k) chain += (k > 1 ? " < " : "") + c.kernel.Describe(frames[k]);
    UT_INFO("gpu: draw #%d on context %llu%s from %s", shown, (unsigned long long)c.obj.id, c.obj.u64["deferred"] != 0 ? " (deferred)" : "", chain.c_str());
    const CtxState& cs = StateOf(c.obj);
    for (int stage : {0, 4}) for (int i = 0; i < 16; ++i) {   // VS, PS
        Res* v = ResAt(cs.srv[stage][i]);
        Res* r = v ? v->base : nullptr;
        if (r == nullptr || !r->texture) continue;
        const auto& info = gpu::TextureInfo(*r->texture);
        UT_INFO("gpu:   %s t%d: res %p %ux%u fmt %u mips %u bind %#x usage %u placement %#llx bytes %llu tile %u dirty %d gpu_written %d", stage ? "ps" : "vs", i, r, info.width, info.height, info.format, info.mips, r->bind, r->usage,
                (unsigned long long)r->placement, (unsigned long long)r->placement_bytes, r->tile_mode, r->placement_dirty ? 1 : 0, r->gpu_written ? 1 : 0);
    }
}

// In a traced frame: a copy-like call between two resources, by native texture (to follow the frame's data flow).
void TraceCopy(const char* what, Call& c, uint64_t dst_obj, uint64_t src_obj) {
    if (!gpu::IsTraceFrame(g_frame.load())) return;
    auto describe = [](uint64_t obj) -> std::string {
        Res* r = ResAt(obj);
        char t[96];
        if (r == nullptr) return "?";
        if (r->texture) { const auto& i = gpu::TextureInfo(*r->texture); std::snprintf(t, sizeof t, "%p %ux%u fmt %u s%u", gpu::NativeOf(*r->texture), i.width, i.height, i.format, i.samples); return t; }
        std::snprintf(t, sizeof t, "buffer %u B", r->buffer ? gpu::BufferSize(*r->buffer) : r->host_size); return t;
    };
    UT_INFO("d3d: %s on ctx %llu%s: %s <- %s", what, (unsigned long long)c.obj.id, c.obj.u64["deferred"] != 0 ? " (deferred)" : "", describe(dst_obj).c_str(), describe(src_obj).c_str());
}

void DoDraw(Obj& ctx, const gpu::DrawArgs& args_in) {
    if (!gpu::Available()) return;
    gpu::DrawArgs args = args_in;
    CtxState& state = StateOf(ctx);
    ParseStream(ctx, state);
    const bool deferred = ctx.u64["deferred"] != 0;
    RecordedDraw rd_storage;
    RecordedDraw* rd_ptr = deferred ? &rd_storage : nullptr;
    const gpu::DrawState d = Snapshot(ctx, state, args, rd_ptr);
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
    // The title's inline draw-state block also carries data beyond topology/layout/shaders: dump it at the race to
    // find where the vertex constant buffers hide (UTSURO_CBFROM=frame).
    if (true) {
        static const uint64_t ubfrom = std::getenv("UTSURO_CBFROM") != nullptr ? std::strtoull(std::getenv("UTSURO_CBFROM"), nullptr, 10) : ~0ull;
        static std::atomic<int> ubshown {0};
        if (g_frame.load() >= ubfrom && ubshown++ < 3 && !ctx.blocks.empty() && d.vs && d.ps) {
            const uint32_t* u = reinterpret_cast<const uint32_t*>(ctx.blocks[0] + kUserDataOffset);
            UT_INFO("d3d: userdata of ctx %llu (deferred %d), %u dwords from %#x:", (unsigned long long)ctx.id, deferred ? 1 : 0, 96u, kUserDataOffset);
            for (int row = 0; row < 24; ++row) {
                const uint32_t* w = u + row * 4;
                UT_INFO("d3d:   +%#05x: %08x %08x %08x %08x  | %g %g %g %g", kUserDataOffset + row * 16, w[0], w[1], w[2], w[3],
                        *reinterpret_cast<const float*>(w), *reinterpret_cast<const float*>(w + 1), *reinterpret_cast<const float*>(w + 2), *reinterpret_cast<const float*>(w + 3));
            }
        }
    }
    // The race probe: for world-shaped draws (a vertex shader with several constant buffers), print the whole
    // context state -- which context, what its cb slots hold, and the last 64 bind calls (UTSURO_CBFROM=frame).
    if (true) {
        static const uint64_t cbfrom3 = std::getenv("UTSURO_CBFROM") != nullptr ? std::strtoull(std::getenv("UTSURO_CBFROM"), nullptr, 10) : ~0ull;
        static std::atomic<int> probe {0};
        if (g_frame.load() >= cbfrom3 && d.vs) {
            const auto& rf = gpu::Reflection(*d.vs);
            uint32_t declared = 0, bound = 0;
            for (uint32_t k = 0; k < 14; ++k) if (rf.constant_buffers & (1u << k)) { ++declared; if (d.cb[0][k].buffer) ++bound; }
            if (declared >= 3 && probe++ < 8) {
                std::string cbs;
                for (uint32_t k = 0; k < 14; ++k) {
                    if (!(rf.constant_buffers & (1u << k))) continue;
                    char t[80];
                    if (Res* r = ResAt(state.cb[0][k])) std::snprintf(t, sizeof t, " cb%u=%#llx(buf %d)", k, (unsigned long long)state.cb[0][k], r->buffer != nullptr);
                    else std::snprintf(t, sizeof t, " cb%u=%#llx", k, (unsigned long long)state.cb[0][k]);
                    cbs += t;
                }
                UT_INFO("d3d: probe draw on ctx %llu (deferred %d, %zu regs, parsed %#llx): vs cbs:%s (%u/%u bound)", (unsigned long long)ctx.id, deferred ? 1 : 0, state.regs.size(), (unsigned long long)state.stream_parsed, cbs.c_str(), bound, declared);
                const uint32_t n = g_cb_calls_n.load();
                for (uint32_t i = 0; i < n && i < 64; ++i) {
                    const CbCallLog& e = g_cb_calls[(g_cb_calls_n.load() - 1 - i) % 64];
                    UT_INFO("d3d:   bind -%u: frame %llu stage %u ctx %llu start %llu count %llu p0 %#llx", i, (unsigned long long)e.frame, e.stage, (unsigned long long)e.ctx, (unsigned long long)e.start, (unsigned long long)e.count, (unsigned long long)e.p0);
                }
            }
        }
    }
    if (static_cast<bool>(d.ps) && std::getenv("UTSURO_SRVLOG") != nullptr) {
        static std::atomic<int> logged {0};
        const auto& rf = gpu::Reflection(*d.ps);
        if (rf.textures && !d.srv[1][0] && logged++ < 6) {
            Res* r = ResAt(state.srv[4][0]);
            Obj* o = Runtime::Get().FromPointer(state.srv[4][0]);
            UT_INFO("d3d: draw with an unbound t0: s.srv[PS][0]=%#llx res=%p object=%s view=%p base=%p texture=%p", (unsigned long long)state.srv[4][0], (void*)r, o ? o->class_name.c_str() : "?", r ? (void*)r->view.get() : nullptr,
                    r ? (void*)r->base : nullptr, r && r->view ? (void*)r->view->texture.get() : nullptr);
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
    for (int i = 0; i < 8; ++i) {
        if (Res* r = ResAt(state.rtv[i])) {
            Res* target = r->base ? r->base : r;
            target->gpu_written = true;
        }
    }
    if (ctx.u64["deferred"] != 0) {
        rd_storage.state = d;
        rd_storage.args = args;
        rd_storage.own_om = state.om_bound;
        rd_storage.own_vp = state.vp_bound;
        for (int stage = 0; stage < 2; ++stage) {
            const std::shared_ptr<gpu::Shader>& sh = stage == 0 ? d.vs : d.ps;
            if (!sh) continue;
            const auto& rf = gpu::Reflection(*sh);
            const int src = stage == 0 ? 0 : 4;
            auto note = [&](uint32_t slot) {
                const auto& m = state.srv_remap[src][slot];
                if (m.stage < 6) rd_storage.srv_remaps.push_back({static_cast<uint8_t>(stage), static_cast<uint8_t>(slot), m.stage, m.slot});
            };
            for (uint32_t i = 0; i < 16; ++i) if (rf.textures & (1u << i)) note(i);
            for (uint8_t slot : rf.high_slots) note(slot);
            for (uint32_t i = 0; i < 14; ++i) {
                const auto& m = state.cb_remap[src][i];
                if ((rf.constant_buffers & (1u << i)) && m.stage < 6) rd_storage.cb_remaps.push_back({static_cast<uint8_t>(stage), static_cast<uint8_t>(i), m.stage, m.slot});
            }
        }
        state.recorded.push_back(std::move(rd_storage));
        Tally("~draws recorded");
        if (state.recorded.size() == 2048) UT_WARN("d3d: deferred context %llu holds 2048 draws and was never finished", (unsigned long long)ctx.id);
        return;
    }
    Tally("~draws immediate");
    gpu::Draw(d, args);
}

void DoDispatch(Obj& ctx, uint32_t x, uint32_t y, uint32_t z, std::shared_ptr<gpu::Buffer> indirect = nullptr, uint32_t indirect_offset = 0) {
    if (!gpu::Available() || x == 0 || y == 0 || z == 0) return;
    CtxState& s = StateOf(ctx);
    ParseStream(ctx, s);
    Res* r = ResAt(s.shader[5]);
    static int disp_calls = 0;
    if (disp_calls++ < 50) {
        UT_INFO("d3d: DoDispatch call %d: x=%u y=%u z=%u cs_res=%p sh=%p", disp_calls, x, y, z, r, r ? r->shader.get() : nullptr);
    }
    if (!r || !r->shader) return;
    const auto& rf = gpu::Reflection(*r->shader);
    gpu::DispatchState d;
    d.cs = r->shader;
    d.indirect = std::move(indirect);
    d.indirect_offset = indirect_offset;

    auto get = [&](uint32_t reg) -> const CtxState::Reg* {
        auto it = s.regs.find(reg);
        return it != s.regs.end() ? &it->second : nullptr;
    };
    auto address = [](const CtxState::Reg& reg) -> uint64_t {
        return (static_cast<uint64_t>(reg.dw[1] & 0xFFFF) << 32) | reg.dw[0];
    };

    // Constant buffers: check s.cb_address[5] or s.cb[5] first, or fall back to regs
    for (int i = 0; i < 16; ++i) {
        if (!(rf.constant_buffers & (1u << i))) continue;
        if (s.cb_address[5][i] != 0) {
            uint32_t size = s.cb_count[5][i] ? s.cb_count[5][i] * 16 : 65536;
            if (rf.cb_size[i] != 0) size = std::min<uint32_t>(size, rf.cb_size[i] * 16);
            d.cb[i].buffer = CopyOfHost(s.cb_address[5][i] + s.cb_first[5][i] * 16, size);
            d.cb[i].offset = 0;
            d.cb[i].size = size;
        } else if (Res* cb = ResAt(s.cb[5][i])) {
            if (cb->buffer) {
                d.cb[i].buffer = cb->buffer;
                d.cb[i].offset = s.cb_first[5][i] * 16;
                d.cb[i].size = s.cb_count[5][i] ? s.cb_count[5][i] * 16 : gpu::BufferSize(*cb->buffer);
            } else if (cb->host_ptr != 0) {
                uint32_t size = s.cb_count[5][i] ? s.cb_count[5][i] * 16 : (cb->host_size ? cb->host_size : 65536);
                if (rf.cb_size[i] != 0) size = std::min<uint32_t>(size, rf.cb_size[i] * 16);
                d.cb[i].buffer = CopyOfHost(cb->host_ptr + s.cb_first[5][i] * 16, size);
                d.cb[i].offset = 0;
                d.cb[i].size = size;
            }
        } else if (const auto* v = get(kRegCbB + 0x10 * i)) {
            uint32_t size = std::min<uint32_t>(v->dw[2], 65536);
            if (rf.cb_size[i] != 0) size = std::min<uint32_t>(size, rf.cb_size[i] * 16);
            d.cb[i].buffer = CopyOfHost(address(*v), size);
            d.cb[i].offset = 0;
            d.cb[i].size = size;
        }
    }

    // SRVs: check s.srv[5] first (through the stage remap, as draws do), or fall back to descriptor packets
    auto cs_srv = [&](uint32_t slot) -> uint64_t {
        if (s.srv_remap[5][slot].stage < 6) return s.srv[s.srv_remap[5][slot].stage][s.srv_remap[5][slot].slot];
        return s.srv[5][slot];
    };
    for (int i = 0; i < 16; ++i) {
        if (rf.textures & (1u << i)) {
            if (Res* srv = ResAt(cs_srv(i))) {
                Res* target = srv->base ? srv->base : srv;
                if (target->placement != 0 && ((target->placement_dirty && g_frame.load() >= target->next_try_frame) || PlacementChanged(*target)))
                    UploadPlacement(*target);
                d.srv[i] = srv->view;
            } else if (const auto* t = get(kRegTexture + 0x20 * i)) {
                if (Res* view = ResFromDescriptor(t->dw)) {
                    if (view->base != nullptr && ((view->base->placement_dirty && g_frame.load() >= view->base->next_try_frame) || PlacementChanged(*view->base))) UploadPlacement(*view->base);
                    d.srv[i] = view->view;
                }
            }
        }
        if (rf.samplers & (1u << i)) {
            const uint64_t sp = s.sampler_remap[5][i].stage < 6 ? s.sampler[s.sampler_remap[5][i].stage][s.sampler_remap[5][i].slot] : s.sampler[5][i];
            if (Res* samp = ResAt(sp)) {
                d.sampler[i] = samp->state;
            } else if (const auto* t = get(kRegSampler + 0x10 * i)) {
                if (Res* sampler = ResFromDescriptor(t->dw)) d.sampler[i] = sampler->state;
            }
        }
    }
    if (gpu::IsTraceFrame(g_frame.load()))
        for (int i = 0; i < 16; ++i)
            if ((rf.textures & (1u << i)) && !d.srv[i]) {
                uint64_t q[6] {};
                const bool readable = s.srv[5][i] != 0 && SafeRead(s.srv[5][i], q, sizeof q);
                Obj* o = s.srv[5][i] ? Runtime::Get().FromPointer(s.srv[5][i]) : nullptr;
                UT_INFO("d3d: dispatch on ctx %llu: cs t%d unresolved: srv %#llx remap %u/%u, descriptor reg %s; obj %p user %d; words %s %#llx %#llx %#llx %#llx %#llx %#llx",
                        (unsigned long long)ctx.id, i, (unsigned long long)s.srv[5][i], s.srv_remap[5][i].stage, s.srv_remap[5][i].slot, get(kRegTexture + 0x20 * i) ? "set" : "none",
                        static_cast<void*>(o), o && o->user ? 1 : 0, readable ? "" : "(unreadable)", (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                        (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
            }
    for (uint8_t slot : rf.high_slots) {
        if (Res* srv = ResAt(cs_srv(slot))) {
            Res* target = srv->base ? srv->base : srv;
            if (target->placement != 0 && ((target->placement_dirty && g_frame.load() >= target->next_try_frame) || PlacementChanged(*target))) UploadPlacement(*target);
            d.extra_srv.emplace_back(slot, srv->view);
        }
    }

    // UAVs: check s.uav
    for (int i = 0; i < 64; ++i) {
        if (rf.uavs & (1u << i)) {
            if (Res* uav = ResAt(s.uav[i])) {
                d.uav[i] = uav->view;
            }
        }
    }

    gpu::Dispatch(d, x, y, z);
}

// ---- resource contents -----------------------------------------------------------------------------

/// Sets a UAV's append/consume counter (an initial count given when it is bound; -1 keeps it), in order on the GPU.
void SetUavCounter(Res* uav, uint32_t value) {
    if (value == 0xFFFFFFFFu || uav == nullptr || !uav->view || !uav->view->counter) return;
    gpu::TextureView word;
    word.buffer = uav->view->counter;
    const uint32_t v[4] = {value, 0, 0, 0};
    gpu::ClearUav(word, v, false);
}

void TextureShape(const gpu::TextureDesc& d, uint32_t sub, uint32_t* mip, uint32_t* slice, uint32_t* row, uint32_t* rows, uint32_t* depth) {
    *mip = sub % std::max(1u, d.mips);
    *slice = sub / std::max(1u, d.mips);
    uint32_t w = std::max(1u, d.width >> *mip), h = std::max(1u, d.height >> *mip);
    FormatShape(d.format, w, h, row, rows);
    *depth = std::max(1u, d.depth >> *mip);
}

uint32_t MapBuffer(Call& c, Res& r, uint32_t type, uint64_t* out) {
    if (type == 1 || type == 3) {   // a read: what the GPU wrote, back on the CPU
        Tally("~buffer read maps");
        static std::atomic<int> shown {0};
        if (shown++ < 12) UT_INFO("d3d: Map for reading: buffer %u bytes usage %u bind %#x gpu_written %d", gpu::BufferSize(*r.buffer), r.usage, r.bind, r.gpu_written ? 1 : 0);
        static std::atomic<uint64_t> reads {0};
        if (std::getenv("UTSURO_READBACKLOG") != nullptr && gpu::BufferSize(*r.buffer) >= 16 && reads++ % 120 == 0) {
            const float* v = reinterpret_cast<const float*>(gpu::BufferData(*r.buffer));
            const uint32_t* u = reinterpret_cast<const uint32_t*>(v);
            UT_INFO("d3d: readback (frame %llu): %g %g %g %g (%08x %08x %08x %08x)", (unsigned long long)g_frame.load(), v[0], v[1], v[2], v[3], u[0], u[1], u[2], u[3]);
        }
    }
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
    OverrideAll("GetType", [](Call& c) -> uint32_t {
        if (auto* out = c.Ptr<uint32_t>(0)) {
            if (c.obj.class_name.find("Texture3D") != std::string::npos) *out = 4;
            else if (c.obj.class_name.find("Texture2D") != std::string::npos) *out = 3;
            else if (c.obj.class_name.find("Texture1D") != std::string::npos) *out = 2;
            else if (c.obj.class_name.find("Buffer") != std::string::npos) *out = 1;
            else *out = 3;
        }
        return 0;
    });
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
        if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag && r && r->buffer && (r->bind & 4)) {
            static std::atomic<int> shown {0};
            if (shown++ < 80) UT_INFO("d3d: cbdiag Map cb obj %#llx %u B usage %u type %llu on ctx %llu (deferred %d) frame %llu", (unsigned long long)c.Arg(0), gpu::BufferSize(*r->buffer), r->usage, (unsigned long long)c.Arg(2), (unsigned long long)c.obj.id, c.obj.u64["deferred"] != 0 ? 1 : 0, (unsigned long long)g_frame.load());
        }
        if (r == nullptr || out == nullptr) { if (out) std::memset(out, 0, 16); return 0x80004005u; }
        const uint32_t type = static_cast<uint32_t>(c.Arg(2));
        if (r->buffer) return MapBuffer(c, *r, type, out);
        if (!r->texture && r->own_ptr != 0) {
            // A placement buffer: its memory is the title's own, and Map hands it straight back (draws copy it when they run).
            static std::atomic<int> shown {0};
            if (shown++ < 12) UT_INFO("d3d: Map of a placement buffer %#llx (%u bytes, type %u)", (unsigned long long)r->own_ptr, r->host_size, type);
            out[0] = r->own_ptr;
            reinterpret_cast<uint32_t*>(out)[2] = r->host_size;
            reinterpret_cast<uint32_t*>(out)[3] = r->host_size;
            return 0;
        }
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
                    r->check_interval = 1;
                    r->next_check_frame = 0;
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
        static std::atomic<int> failed {0};
        if (failed++ < 12) UT_WARN("d3d: Map of %#llx failed: no buffer, texture or placement memory (type %u)", (unsigned long long)c.Arg(0), type);
        std::memset(out, 0, 16);
        return 0x80004005u;
    });
    OverrideAll("Unmap", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->texture || r->staging.empty()) return 0;
        if (r->map_write) {
            uint32_t mip, slice, row, rows, depth;
            TextureShape(gpu::TextureInfo(*r->texture), r->map_subresource, &mip, &slice, &row, &rows, &depth);
            if (static const bool log = std::getenv("UTSURO_BINKLOG") != nullptr; log) {
                const auto& i = gpu::TextureInfo(*r->texture);
                if (i.format == 61 && i.width >= 960) {
                    static std::map<Res*, uint32_t> counts;
                    static std::mutex m;
                    uint32_t n;
                    { std::scoped_lock lock {m}; n = ++counts[r]; }
                    if (n % 50 == 1) {
                        size_t nonzero = 0;
                        for (size_t k = 0; k < r->staging.size(); k += 64) nonzero += r->staging[k] != 0;
                        UT_INFO("bink: frame %llu: upload %u of %ux%u R8 (res %p): %zu of %zu sampled bytes non-zero", (unsigned long long)g_frame.load(), n, i.width, i.height, r, nonzero, r->staging.size() / 64);
                    }
                }
            }
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
        if (!r->buffer && !r->texture && r->own_ptr != 0) {
            // A placement buffer: the title's own memory, which draws copy from when they run. Forza Horizon 2's camera
            // constants are DEFAULT-usage placement buffers filled this way; dropped, every world vertex went to (0,0,0,0).
            const uint32_t left = box ? std::min(box[0], r->host_size) : 0, right = box ? std::min(box[3], r->host_size) : r->host_size;
            if (right > left) std::memcpy(reinterpret_cast<void*>(r->own_ptr + left), c.Ptr<void>(3), right - left);
            Tally("~placement buffer updates");
            return 0;
        }
        if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag && r->buffer && (r->bind & 4)) {
            static std::atomic<int> shown {0};
            if (shown++ < 80) UT_INFO("d3d: cbdiag UpdateSubresource cb obj %#llx %u B usage %u box %d on ctx %llu (deferred %d) frame %llu", (unsigned long long)c.Arg(0), gpu::BufferSize(*r->buffer), r->usage, box ? 1 : 0, (unsigned long long)c.obj.id, c.obj.u64["deferred"] != 0 ? 1 : 0, (unsigned long long)g_frame.load());
        }
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
            if (static const bool log = std::getenv("UTSURO_BINKLOG") != nullptr; log) {
                const auto& i = gpu::TextureInfo(*r->texture);
                if (i.format == 61 && i.width >= 960) {
                    static std::map<Res*, uint32_t> counts;
                    static std::mutex m;
                    uint32_t n;
                    { std::scoped_lock lock {m}; n = ++counts[r]; }
                    if (n % 50 == 1) {
                        const auto* src = c.Ptr<uint8_t>(3);
                        const uint32_t pitch = static_cast<uint32_t>(c.Arg(4)) ? static_cast<uint32_t>(c.Arg(4)) : row;
                        size_t nonzero = 0, total = 0;
                        for (uint32_t y = 0; y < rows; y += 8) for (uint32_t x = 0; x < row; x += 64) { ++total; nonzero += src[static_cast<size_t>(y) * pitch + x] != 0; }
                        UT_INFO("bink: frame %llu: UpdateSubresource %u of %ux%u R8 (res %p, box %s, pitch %u): %zu of %zu sampled bytes non-zero", (unsigned long long)g_frame.load(), n, i.width, i.height, r, box ? "yes" : "no", pitch, nonzero, total);
                    }
                }
            }
            gpu::UploadTexture(*r->texture, mip, slice, box, c.Ptr<void>(3), static_cast<uint32_t>(c.Arg(4)) ? static_cast<uint32_t>(c.Arg(4)) : row,
                               static_cast<uint32_t>(c.Arg(5)));
        }
        return 0;
    });
    // Buffer copies where a side is a placement buffer (guest memory, no Metal storage): done on the CPU, in call order, which
    // is when draws read placement memory too.
    static auto CopyPlacementBytes = [](Res* dst, uint32_t dst_at, Res* src, uint32_t src_at, uint32_t bytes) -> bool {
        auto bytes_of = [](Res* r, uint32_t* size) -> uint8_t* {
            if (r->buffer) { *size = gpu::BufferSize(*r->buffer); return gpu::BufferData(*r->buffer); }
            if (!r->texture && r->own_ptr != 0) { *size = r->host_size; return reinterpret_cast<uint8_t*>(r->own_ptr); }
            return nullptr;
        };
        if ((dst->buffer && src->buffer) || dst->texture || src->texture) return false;
        uint32_t dsize = 0, ssize = 0;
        uint8_t* d = bytes_of(dst, &dsize);
        uint8_t* s = bytes_of(src, &ssize);
        if (d == nullptr || s == nullptr) return false;
        if (dst_at >= dsize || src_at >= ssize) return true;
        bytes = std::min({bytes, dsize - dst_at, ssize - src_at});
        std::memmove(d + dst_at, s + src_at, bytes);
        Tally("~placement buffer copies");
        return true;
    };
    OverrideAll("CopyResource", [](Call& c) -> uint32_t {
        TraceCopy("CopyResource", c, c.Arg(0), c.Arg(1));
        Res* dst = ResAt(c.Arg(0));
        Res* src = ResAt(c.Arg(1));
        if (dst == nullptr || src == nullptr) return 0;
        if (CopyPlacementBytes(dst, 0, src, 0, ~0u)) return 0;
        if (dst->buffer && src->buffer) {
            Tally("~buffer copies");
            static std::atomic<int> shown {0};
            if (shown++ < 12) UT_INFO("d3d: CopyResource buffer %u bytes (usage %u bind %#x) <- %u bytes (usage %u bind %#x)", gpu::BufferSize(*dst->buffer), dst->usage, dst->bind, gpu::BufferSize(*src->buffer), src->usage, src->bind);
            gpu::CopyBuffer(*dst->buffer, 0, *src->buffer, 0, std::min(gpu::BufferSize(*dst->buffer), gpu::BufferSize(*src->buffer)));
        } else if (dst->texture && src->texture) {
            const auto& d = gpu::TextureInfo(*src->texture);
            if (static std::atomic<int> shown {0}; (d.format == 19 || d.format == 20 || d.format == 40 || d.format == 41 || d.format == 44 || d.format == 45) && shown++ < 8)
                UT_INFO("d3d: CopyResource of a depth-format texture %ux%u fmt %u -> fmt %u", d.width, d.height, d.format, gpu::TextureInfo(*dst->texture).format);
            for (uint32_t a = 0; a < d.array_size; ++a)
                for (uint32_t m = 0; m < d.mips; ++m) gpu::CopyTexture(*dst->texture, m, a, 0, 0, 0, *src->texture, m, a, nullptr);
        }
        return 0;
    });
    // CopySubresourceRegion(dst, dst sub, x, y, z, src, src sub, box*)
    // DecompressResource(dst, dst sub, const D3D11X_POINT* dst point, src, src sub, const D3D11X_RECT* src rect, DXGI_FORMAT, flags): the
    // console decompresses depth (HTILE) or colour (CMASK/FMASK) into a surface shaders can read. Here surfaces are never compressed,
    // so it is a copy, converting depth into whatever the destination is. Rocket League reads its scene depth from such a copy
    // (fog, soft particles, shadow projection): without it that texture stays zero and dynamic shadows black out whole boxes.
    OverrideAll("DecompressResource", [](Call& c) -> uint32_t {
        TraceCopy("DecompressResource", c, c.Arg(0), c.Arg(3));
        Res* dst = ResAt(c.Arg(0));
        Res* src = ResAt(c.Arg(3));
        static std::atomic<int> shown {0};
        if (dst == nullptr || src == nullptr || !dst->texture || !src->texture) {
            if (shown++ < 6) UT_INFO("d3d: DecompressResource(%#llx <- %#llx): not two textures", (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(3));
            return 0;
        }
        uint32_t dm, ds, sm, ss, row, rows, depth;
        // UINT arguments: only the low 32 bits of the register are the value.
        const uint32_t dst_sub = static_cast<uint32_t>(c.Arg(1)), src_sub = static_cast<uint32_t>(c.Arg(4));
        {
            static std::mutex m; static std::set<std::pair<Res*, Res*>> seen;
            std::scoped_lock lock {m};
            if (seen.insert({dst, src}).second && seen.size() < 40)
                UT_INFO("d3d: DecompressResource pair: dst %p native %p fmt %u sub %u <- src %p native %p fmt %u sub %u, format %u flags %#x", dst, gpu::NativeOf(*dst->texture), gpu::TextureInfo(*dst->texture).format, dst_sub,
                        src, gpu::NativeOf(*src->texture), gpu::TextureInfo(*src->texture).format, src_sub, static_cast<uint32_t>(c.Arg(6)), static_cast<uint32_t>(c.Arg(7)));
        }
        if (dst == src && dst_sub == src_sub) return 0;   // in place: surfaces here are never compressed
        TextureShape(gpu::TextureInfo(*dst->texture), dst_sub, &dm, &ds, &row, &rows, &depth);
        TextureShape(gpu::TextureInfo(*src->texture), src_sub, &sm, &ss, &row, &rows, &depth);
        const auto* pt = c.Ptr<uint32_t>(2);
        const auto* rc = c.Ptr<uint32_t>(5);
        if (shown++ < 6) {
            const auto& di = gpu::TextureInfo(*dst->texture); const auto& si = gpu::TextureInfo(*src->texture);
            UT_INFO("d3d: DecompressResource %ux%u fmt %u (sub %u) <- %ux%u fmt %u (sub %u), point %s, rect %s, format %u flags %#x", di.width, di.height, di.format,
                    dst_sub, si.width, si.height, si.format, src_sub, pt ? "yes" : "no", rc ? "yes" : "no", static_cast<uint32_t>(c.Arg(6)), static_cast<uint32_t>(c.Arg(7)));
        }
        gpu::DecompressCopy(*dst->texture, dm, ds, pt ? pt[0] : 0, pt ? pt[1] : 0, *src->texture, sm, ss, rc);
        return 0;
    });
    // ResolveSubresource(dst, dst sub, src, src sub, DXGI_FORMAT): Forza Horizon 2 renders multisampled and resolves before its
    // post-processing samples the result; unanswered, that texture stayed empty and the picture black.
    OverrideAll("ResolveSubresource", [](Call& c) -> uint32_t {
        TraceCopy("ResolveSubresource", c, c.Arg(0), c.Arg(2));
        Res* dst = ResAt(c.Arg(0));
        Res* src = ResAt(c.Arg(2));
        if (dst == nullptr || src == nullptr || !dst->texture || !src->texture) return 0;
        uint32_t dm, ds, sm, ss, row, rows, depth;
        TextureShape(gpu::TextureInfo(*dst->texture), static_cast<uint32_t>(c.Arg(1)), &dm, &ds, &row, &rows, &depth);
        TextureShape(gpu::TextureInfo(*src->texture), static_cast<uint32_t>(c.Arg(3)), &sm, &ss, &row, &rows, &depth);
        static std::atomic<int> shown {0};
        if (shown++ < 4) {
            const auto& di = gpu::TextureInfo(*dst->texture); const auto& si = gpu::TextureInfo(*src->texture);
            UT_INFO("d3d: ResolveSubresource %ux%u fmt %u <- %ux%u fmt %u (format %u)", di.width, di.height, di.format, si.width, si.height, si.format, static_cast<uint32_t>(c.Arg(4)));
        }
        gpu::ResolveTexture(*dst->texture, dm, ds, *src->texture, sm, ss);
        return 0;
    });
    OverrideAll("CopySubresourceRegion", [](Call& c) -> uint32_t {
        TraceCopy("CopySubresourceRegion", c, c.Arg(0), c.Arg(5));
        Res* dst = ResAt(c.Arg(0));
        Res* src = ResAt(c.Arg(5));
        if (dst == nullptr || src == nullptr) return 0;
        const auto* box = c.Ptr<uint32_t>(7);
        const uint32_t dx = static_cast<uint32_t>(c.Arg(2)), dy = static_cast<uint32_t>(c.Arg(3)), dz = static_cast<uint32_t>(c.Arg(4));
        if (CopyPlacementBytes(dst, dx, src, box ? box[0] : 0, box ? (box[3] > box[0] ? box[3] - box[0] : 0) : ~0u)) return 0;
        if (dst->buffer && src->buffer) {
            const uint32_t left = box ? box[0] : 0, right = box ? box[3] : gpu::BufferSize(*src->buffer);
            if (right > left) gpu::CopyBuffer(*dst->buffer, dx, *src->buffer, left, right - left);
        } else if (dst->texture && src->texture) {
            if (static const bool skip = std::getenv("UTSURO_NOTEXCOPY") != nullptr; skip) return 0;   // A/B switch for what texture copies cost
            uint32_t sm, ss, sr, srs, sd, dm, ds, dr, drs, dd;
            TextureShape(gpu::TextureInfo(*src->texture), static_cast<uint32_t>(c.Arg(6)), &sm, &ss, &sr, &srs, &sd);
            TextureShape(gpu::TextureInfo(*dst->texture), static_cast<uint32_t>(c.Arg(1)), &dm, &ds, &dr, &drs, &dd);
            if (static const bool log = std::getenv("UTSURO_COPYLOG") != nullptr; log) {
                static std::atomic<int> shown {0};
                if (shown++ < 200) {
                    const auto& di = gpu::TextureInfo(*dst->texture); const auto& si = gpu::TextureInfo(*src->texture);
                    UT_INFO("d3d: CopySubresourceRegion tex %ux%u fmt %u mips %u placed %d mip %u slice %u at %u,%u <- %ux%u fmt %u mips %u placed %d mip %u slice %u box %s",
                            di.width, di.height, di.format, di.mips, dst->placement != 0, dm, ds, dx, dy, si.width, si.height, si.format, si.mips, src->placement != 0, sm, ss,
                            box ? (std::to_string(box[0]) + "," + std::to_string(box[1]) + "-" + std::to_string(box[3]) + "," + std::to_string(box[4])).c_str() : "none");
                }
            }
            gpu::CopyTexture(*dst->texture, dm, ds, dx, dy, dz, *src->texture, sm, ss, box);
        } else if (static const bool log = std::getenv("UTSURO_COPYLOG") != nullptr; log) {
            static std::atomic<int> shown {0};
            if (shown++ < 30) {
                auto describe = [](Res* r) -> std::string {
                    char b[96];
                    if (r->texture) { const auto& i = gpu::TextureInfo(*r->texture); std::snprintf(b, sizeof b, "texture %ux%u fmt %u placed %d", i.width, i.height, i.format, r->placement != 0); }
                    else std::snprintf(b, sizeof b, "buffer %u bytes host %#llx", r->host_size, (unsigned long long)r->host_ptr);
                    return b;
                };
                UT_INFO("d3d: CopySubresourceRegion %s <- %s (box %s, dst sub %llu, src sub %llu)", describe(dst).c_str(), describe(src).c_str(), box ? "yes" : "no", (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(6));
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
    // ClearUnorderedAccessViewUint/Float(uav, values[4]): Forza Horizon 2 resets its per-frame GPU counters with these (unhandled,
    // the 16-byte buffer it reads back each frame counted up for ever).
    OverrideAll("ClearUnorderedAccessViewUint", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->view || c.Arg(1) == 0) return 0;
        gpu::ClearUav(*r->view, c.Ptr<uint32_t>(1), false);
        return 0;
    });
    OverrideAll("ClearUnorderedAccessViewFloat", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->view || c.Arg(1) == 0) return 0;
        gpu::ClearUav(*r->view, c.Ptr<uint32_t>(1), true);
        return 0;
    });
    // ClearDepthStencilView(dsv, flags, depth (xmm3), stencil)
    OverrideAll("ClearDepthStencilView", [](Call& c) -> uint32_t {
        Res* r = ResAt(c.Arg(0));
        if (r == nullptr || !r->view) return 0;
        gpu::ClearDepthStencil(*r->view, static_cast<uint32_t>(c.Arg(1)), c.Float(2), static_cast<uint8_t>(c.Arg(3)));
        return 0;
    });
    // ClearState resets what is bound, not what a deferred context has recorded: Forza Horizon 2 records its video frame and then
    // clears state before its UI on the same context, which dropped the video (the screen stayed black while it played).
    OverrideAll("ClearState", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        std::vector<RecordedDraw> recorded = std::move(s.recorded);
        s = CtxState {};
        s.recorded = std::move(recorded);
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

    // GetImmCtx is defined statically above

    OverrideAll("RemapConstantBufferInheritance", [](Call& c) -> uint32_t {
        const uint32_t stage = static_cast<uint32_t>(c.Arg(0));
        const uint32_t slot = static_cast<uint32_t>(c.Arg(1));
        const uint32_t inherit_stage = static_cast<uint32_t>(c.Arg(2));
        const uint32_t inherit_slot = static_cast<uint32_t>(c.Arg(3));
        auto& s = StateOf(c.obj);
        if (stage < 6 && inherit_stage < 6 && slot < 16 && inherit_slot < 16) {
            s.cb_remap[stage][slot] = { static_cast<uint8_t>(inherit_stage), static_cast<uint8_t>(inherit_slot) };
            uint64_t cb = s.cb[inherit_stage][inherit_slot];
            uint64_t addr = s.cb_address[inherit_stage][inherit_slot];
            uint32_t first = s.cb_first[inherit_stage][inherit_slot];
            uint32_t count = s.cb_count[inherit_stage][inherit_slot];
            if (cb == 0 && addr == 0) {
                if (CtxState* imm_s = GetImmCtx(c.obj)) {
                    if (imm_s != &s) {
                        cb = imm_s->cb[inherit_stage][inherit_slot];
                        addr = imm_s->cb_address[inherit_stage][inherit_slot];
                        first = imm_s->cb_first[inherit_stage][inherit_slot];
                        count = imm_s->cb_count[inherit_stage][inherit_slot];
                    }
                }
            }
            s.cb[stage][slot] = cb;
            s.cb_address[stage][slot] = addr;
            s.cb_first[stage][slot] = first;
            s.cb_count[stage][slot] = count ? count : 4096;

            const uint32_t inherit_reg_base = (inherit_stage == 0 ? kRegCbA : ((inherit_stage == 4 || inherit_stage == 5) ? kRegCbB : 0));
            const uint32_t stage_reg_base = (stage == 0 ? kRegCbA : ((stage == 4 || stage == 5) ? kRegCbB : 0));
            if (inherit_reg_base != 0 && stage_reg_base != 0) {
                auto it = s.regs.find(inherit_reg_base + 0x10 * inherit_slot);
                if (it != s.regs.end()) {
                    s.regs[stage_reg_base + 0x10 * slot] = it->second;
                } else if (CtxState* imm_s = GetImmCtx(c.obj)) {
                    auto it2 = imm_s->regs.find(inherit_reg_base + 0x10 * inherit_slot);
                    if (it2 != imm_s->regs.end()) {
                        s.regs[stage_reg_base + 0x10 * slot] = it2->second;
                    }
                }
            }

            static std::atomic<int> shown {0};
            if (shown++ < 40) {
                UT_INFO("d3d: RemapConstantBufferInheritance(ctx %llu): %s cb%u <- %s cb%u (cb=%#llx addr=%#llx first=%u count=%u)",
                        (unsigned long long)c.obj.id, kStages[stage], slot, kStages[inherit_stage], inherit_slot,
                        (unsigned long long)cb, (unsigned long long)addr, first, count);
            }
        }
        return 0;
    });

    OverrideAll("RemapShaderResourceInheritance", [](Call& c) -> uint32_t {
        const uint32_t stage = static_cast<uint32_t>(c.Arg(0));
        const uint32_t slot = static_cast<uint32_t>(c.Arg(1));
        const uint32_t inherit_stage = static_cast<uint32_t>(c.Arg(2));
        const uint32_t inherit_slot = static_cast<uint32_t>(c.Arg(3));
        auto& s = StateOf(c.obj);
        if (stage < 6 && inherit_stage < 6 && slot < 128 && inherit_slot < 128) {
            s.srv_remap[stage][slot] = { static_cast<uint8_t>(inherit_stage), static_cast<uint8_t>(inherit_slot) };
            uint64_t srv = s.srv[inherit_stage][inherit_slot];
            if (srv == 0) {
                if (CtxState* imm_s = GetImmCtx(c.obj)) {
                    if (imm_s != &s) srv = imm_s->srv[inherit_stage][inherit_slot];
                }
            }
            s.srv[stage][slot] = srv;
            if (inherit_stage == 4 && stage == 4) {
                auto it = s.regs.find(kRegTexture + 0x20 * inherit_slot);
                if (it != s.regs.end()) s.regs[kRegTexture + 0x20 * slot] = it->second;
                else if (CtxState* imm_s = GetImmCtx(c.obj)) {
                    auto it2 = imm_s->regs.find(kRegTexture + 0x20 * inherit_slot);
                    if (it2 != imm_s->regs.end()) s.regs[kRegTexture + 0x20 * slot] = it2->second;
                }
            }
            static std::atomic<int> shown {0};
            if (shown++ < 30) {
                UT_INFO("d3d: RemapShaderResourceInheritance(ctx %llu): %s srv%u <- %s srv%u (srv=%#llx)",
                        (unsigned long long)c.obj.id, kStages[stage], slot, kStages[inherit_stage], inherit_slot, (unsigned long long)srv);
            }
        }
        return 0;
    });

    OverrideAll("RemapSamplerInheritance", [](Call& c) -> uint32_t {
        const uint32_t stage = static_cast<uint32_t>(c.Arg(0));
        const uint32_t slot = static_cast<uint32_t>(c.Arg(1));
        const uint32_t inherit_stage = static_cast<uint32_t>(c.Arg(2));
        const uint32_t inherit_slot = static_cast<uint32_t>(c.Arg(3));
        auto& s = StateOf(c.obj);
        if (stage < 6 && inherit_stage < 6 && slot < 16 && inherit_slot < 16) {
            s.sampler_remap[stage][slot] = { static_cast<uint8_t>(inherit_stage), static_cast<uint8_t>(inherit_slot) };
            uint64_t samp = s.sampler[inherit_stage][inherit_slot];
            if (samp == 0) {
                if (CtxState* imm_s = GetImmCtx(c.obj)) {
                    if (imm_s != &s) samp = imm_s->sampler[inherit_stage][inherit_slot];
                }
            }
            s.sampler[stage][slot] = samp;
        }
        return 0;
    });

    OverrideAll("RemapVertexBufferInheritance", [](Call& c) -> uint32_t {
        const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
        const uint32_t inherit_slot = static_cast<uint32_t>(c.Arg(1));
        auto& s = StateOf(c.obj);
        if (slot < 16 && inherit_slot < 16) {
            s.vb[slot] = s.vb[inherit_slot];
            s.vb_address[slot] = s.vb_address[inherit_slot];
            s.stride[slot] = s.stride[inherit_slot];
            s.voffset[slot] = s.voffset[inherit_slot];
        }
        return 0;
    });

    for (int st = 0; st < 6; ++st) {
        const std::string p = kStages[st];
        // Fast ConstantBuffer: (Slot, pBuffer)
        OverrideAll((p + "SetFastConstantBuffer").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
            if (slot < 16) {
                s.cb[st][slot] = c.Arg(1);
                s.cb_address[st][slot] = 0;
                s.cb_first[st][slot] = 0;
                s.cb_count[st][slot] = 4096;
            }
            return 0;
        });
        // Fast ShaderResource: (Slot, pSRV)
        OverrideAll((p + "SetFastShaderResource").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
            if (slot < 128) s.srv[st][slot] = c.Arg(1);
            if (c.Arg(1) != 0 && ResAt(c.Arg(1)) == nullptr) {
                static std::atomic<int> shown {0};
                if (shown++ < 16) UT_INFO("d3d: %sSetFastShaderResource(%llu, %#llx, %#llx, %#llx) is no view we made", kStages[st], (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2), (unsigned long long)c.Arg(3));
            }
            return 0;
        });
        // Fast Sampler: (Slot, pSampler)
        OverrideAll((p + "SetFastSampler").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
            if (slot < 16) s.sampler[st][slot] = c.Arg(1);
            return 0;
        });
        // Placement ConstantBuffer: (Slot, pBuffer, pBaseAddress)
        OverrideAll((p + "SetPlacementConstantBuffer").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
            if (slot < 16) {
                s.cb[st][slot] = c.Arg(1);
                s.cb_address[st][slot] = c.Arg(2);
                s.cb_first[st][slot] = 0;
                s.cb_count[st][slot] = 4096;
                if (c.Arg(2) != 0) {
                    if (Res* r = ResAt(c.Arg(1))) r->host_ptr = c.Arg(2);
                }
            }
            return 0;
        });
        // Placement ShaderResource: (Slot, pSRV, pBaseAddress)
        OverrideAll((p + "SetPlacementShaderResource").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
            if (slot < 128) {
                s.srv[st][slot] = c.Arg(1);
                if (c.Arg(1) != 0 && ResAt(c.Arg(1)) == nullptr) {
                    static std::atomic<int> shown {0};
                    if (shown++ < 16) UT_INFO("d3d: %sSetPlacementShaderResource(%llu, %#llx, %#llx, %#llx) is no view we made", kStages[st], (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2), (unsigned long long)c.Arg(3));
                }
                if (c.Arg(2) != 0) {
                    if (Res* r = ResAt(c.Arg(1))) {
                        if (r->base != nullptr) {
                            r->base->placement = c.Arg(2);
                            r->base->placement_dirty = true;
                        }
                    }
                }
            }
            return 0;
        });
        // GetConstantBuffers1
        OverrideAll((p + "GetConstantBuffers1").c_str(), [st](Call& c) -> uint32_t {
            auto& s = StateOf(c.obj);
            for (uint64_t i = 0; i < c.Arg(1) && c.Arg(0) + i < 16; ++i) {
                const uint64_t k = c.Arg(0) + i;
                if (c.Arg(2)) c.Ptr<uint64_t>(2)[i] = s.cb[st][k];
                if (c.Arg(3)) c.Ptr<uint32_t>(3)[i] = s.cb_first[st][k];
                if (c.Arg(4)) c.Ptr<uint32_t>(4)[i] = s.cb_count[st][k];
            }
            return 0;
        });
    }

    OverrideAll("IASetFastVertexBuffer", [](Call& c) -> uint32_t {
        const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
        if (slot < 16) {
            auto& s = StateOf(c.obj);
            s.vb[slot] = c.Arg(1);
            s.vb_address[slot] = 0;
            s.stride[slot] = static_cast<uint32_t>(c.Arg(2));
            s.voffset[slot] = 0;
        }
        return 0;
    });

    OverrideAll("IASetFastIndexBuffer", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s.ib_format = (c.Arg(0) & 0xFF) == 0 ? 57 : 42;
        s.ib = c.Arg(1);
        s.ib_address = 0;
        s.ib_offset = 0;
        return 0;
    });

    OverrideAll("IASetPlacementVertexBuffer", [](Call& c) -> uint32_t {
        const uint32_t slot = static_cast<uint32_t>(c.Arg(0));
        if (slot < 16) {
            auto& s = StateOf(c.obj);
            s.vb[slot] = c.Arg(1);
            s.vb_address[slot] = c.Arg(2);
            s.stride[slot] = static_cast<uint32_t>(c.Arg(3));
            s.voffset[slot] = 0;
            if (c.Arg(2) != 0) {
                if (Res* r = ResAt(c.Arg(1))) r->host_ptr = c.Arg(2);
            }
        }
        return 0;
    });

    OverrideAll("IASetPrimitiveResetIndex", [](Call&) -> uint32_t { return 0; });
    OverrideAll("SetShaderResourceViewMinLOD", [](Call&) -> uint32_t { return 0; });
    OverrideAll("SetResourceCompression", [](Call&) -> uint32_t { return 0; });
    OverrideAll("CSEnableAutomaticGpuFlush", [](Call&) -> uint32_t { return 0; });
    OverrideAll("GpuSendPipelinedEvent", [](Call&) -> uint32_t { return 0; });
    OverrideAll("InsertWaitUntilIdle", [](Call&) -> uint32_t { return 0; });
    OverrideAll("InsertWaitOnFence", [](Call&) -> uint32_t { return 0; });
    OverrideAll("FlushGpuCaches", [](Call&) -> uint32_t { return 0; });

    // FlushGpuCacheRange(flags, address, size): the title wrote this memory with the CPU; the GPU's copy of it is stale.
    OverrideAll("FlushGpuCacheRange", [](Call& c) -> uint32_t {
        const uint64_t from = c.Arg(1), to = c.Arg(1) + c.Arg(2);
        static std::atomic<int> shown {0};
        if (std::getenv("UTSURO_PLACE") != nullptr && shown++ < 12) UT_INFO("d3d: FlushGpuCacheRange(%#llx, %#llx, %#llx)", (unsigned long long)c.Arg(0), (unsigned long long)c.Arg(1), (unsigned long long)c.Arg(2));
        std::scoped_lock lock {g_res_mutex};
        int hit = 0;
        for (Res* r : g_placed)
            if (r->placement < to && r->placement + r->host_size > from) { r->placement_dirty = true; r->next_try_frame = 0; r->check_interval = 1; r->next_check_frame = 0; ++hit; }
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
        Tally("~lists executed");
        if (list != nullptr) Tally("~list draws replayed", list->recorded.size());
        static std::atomic<int> shown {0};
        if (shown++ < 24 || gpu::IsTraceFrame(g_frame.load())) UT_INFO("d3d: ExecuteCommandList(%#llx) on ctx %llu: res %p, %zu draws", (unsigned long long)c.Arg(0), (unsigned long long)c.obj.id, static_cast<void*>(list), list ? list->recorded.size() : 0);
        if (list == nullptr || !gpu::Available()) return 0;
        CtxState& imm = StateOf(c.obj);
        ParseStream(c.obj, imm);   // the immediate context's inline descriptors as of this execute
        auto imm_reg = [&imm](uint32_t reg) -> const CtxState::Reg* { auto it = imm.regs.find(reg); return it == imm.regs.end() ? nullptr : &it->second; };
        auto imm_address = [](const CtxState::Reg& r) { return static_cast<uint64_t>(r.dw[0]) | (static_cast<uint64_t>(r.dw[1] & 0xFFFF) << 32); };
        for (const RecordedDraw& rd : list->recorded) {
            gpu::DrawState d = rd.state;
            // Console command lists run against the executing context's output state: a draw recorded before its list bound
            // any render targets (or viewports) uses the ones bound when the list runs, and what a list binds stays bound
            // after it (see the end of this function). Forza Horizon 2 binds its scene target in one list and draws the
            // world from pre-recorded lists that bind none; their deferred contexts still held a 1x1 placeholder from
            // earlier use (UTSURO_RTINHERIT=0: replay what was recorded).
            static const std::string inherit_mode = std::getenv("UTSURO_RTINHERIT") ? std::getenv("UTSURO_RTINHERIT") : "tiny";
            if (inherit_mode == "list") {
                if (!rd.own_om) {
                    for (int i = 0; i < 8; ++i) { Res* r = ResAt(imm.rtv[i]); d.rtv[i] = r ? r->view : nullptr; }
                    Res* r = ResAt(imm.dsv);
                    d.dsv = r ? r->view : nullptr;
                }
                if (!rd.own_vp && imm.num_viewports != 0) {
                    d.num_viewports = imm.num_viewports;
                    std::memcpy(d.viewports, imm.viewports, sizeof d.viewports);
                }
            } else if (inherit_mode != "0" && d.rtv[0] && d.rtv[0]->texture) {
                // "tiny" (default): a list draw into a <= 4x4 placeholder while the executing context has a real target takes that.
                const auto& ti = gpu::TextureInfo(*d.rtv[0]->texture);
                Res* ir = ResAt(imm.rtv[0]);
                if (ti.width <= 4 && ti.height <= 4 && ir && ir->view && ir->view->texture && gpu::TextureInfo(*ir->view->texture).width > 4) {
                    for (int i = 0; i < 8; ++i) { Res* r = ResAt(imm.rtv[i]); d.rtv[i] = r ? r->view : nullptr; }
                    Res* r = ResAt(imm.dsv);
                    d.dsv = r ? r->view : nullptr;
                }
            }
            for (int i = 0; i < 8; ++i)
                if (!d.rtv[i]) if (Res* r = ResAt(imm.rtv[i])) d.rtv[i] = r->view;
            if (!d.dsv) if (Res* r = ResAt(imm.dsv)) d.dsv = r->view;
            if (d.num_viewports == 0) {
                d.num_viewports = imm.num_viewports;
                std::memcpy(d.viewports, imm.viewports, sizeof d.viewports);
            }
            // Fixed-function state the list never set comes from the executing context too: most of Forza Horizon 2's world
            // lists set none, so they drew with the D3D defaults (depth test LESS against its reversed-Z depth, back-face
            // culling, no blending) instead of the GREATER_EQUAL state the game binds before executing them.
            if (static const bool on = std::getenv("UTSURO_STATEINHERIT") == nullptr || std::getenv("UTSURO_STATEINHERIT")[0] != '0'; on) {
                if (d.depth_stencil.empty()) if (Res* r = ResAt(imm.depth_stencil)) { d.depth_stencil = r->state; d.stencil_ref = imm.stencil_ref; }
                if (d.raster.empty()) if (Res* r = ResAt(imm.raster)) d.raster = r->state;
                if (d.blend.empty()) if (Res* r = ResAt(imm.blend)) { d.blend = r->state; std::memcpy(d.blend_factor, imm.blend_factor, 16); d.sample_mask = imm.sample_mask; }
            }
            // Remapped slots read what the executing context has bound at the slot they inherit from (Forza Horizon 2's world lists
            // remap their vertex instance data and shadow maps this way; resolved at record time they found nothing).
            for (const auto& m : rd.srv_remaps) {
                Res* r = ResAt(imm.srv[m.from_stage][m.from_slot]);
                if (r == nullptr || !r->view) continue;
                Res* target = r->base ? r->base : r;
                if (target->placement != 0 && ((target->placement_dirty && g_frame.load() >= target->next_try_frame) || PlacementChanged(*target))) UploadPlacement(*target);
                if (m.slot < 16) { d.srv[m.stage][m.slot] = r->view; continue; }
                auto& extra = d.extra_srv[m.stage];
                extra.erase(std::remove_if(extra.begin(), extra.end(), [&](const auto& e) { return e.first == m.slot; }), extra.end());
                extra.emplace_back(m.slot, r->view);
            }
            for (const auto& m : rd.cb_remaps) {
                const uint64_t addr = imm.cb_address[m.from_stage][m.from_slot];
                const uint32_t first = imm.cb_first[m.from_stage][m.from_slot], count = imm.cb_count[m.from_stage][m.from_slot];
                const auto& rf = gpu::Reflection(*(m.stage == 0 ? d.vs : d.ps));
                uint32_t size = count ? count * 16 : 65536;
                if (rf.cb_size[m.slot] != 0) size = std::min<uint32_t>(size, rf.cb_size[m.slot] * 16);
                std::shared_ptr<gpu::Buffer> b;
                if (addr != 0) b = CopyOfHost(addr + first * 16, size, false);
                else if (Res* r = ResAt(imm.cb[m.from_stage][m.from_slot])) {
                    if (r->buffer) { d.cb[m.stage][m.slot].buffer = r->buffer; d.cb[m.stage][m.slot].offset = first * 16; d.cb[m.stage][m.slot].size = count ? count * 16 : gpu::BufferSize(*r->buffer); continue; }
                    if (r->host_ptr != 0) b = CopyOfHost(r->host_ptr + first * 16, size, false);
                }
                if (b) { d.cb[m.stage][m.slot].buffer = b; d.cb[m.stage][m.slot].offset = 0; d.cb[m.stage][m.slot].size = size; }
            }
            if (static const bool diag = std::getenv("UTSURO_SRVDIAG") != nullptr; diag && !rd.srv_remaps.empty()) {
                static std::atomic<int> shown {0};
                if (shown++ < 20) {
                    std::string t;
                    for (const auto& m : rd.srv_remaps) { char x[64]; std::snprintf(x, sizeof x, " %s t%u<-%s t%u%s", m.stage ? "ps" : "vs", m.slot, kStages[m.from_stage], m.from_slot, ResAt(imm.srv[m.from_stage][m.from_slot]) ? "" : "(unbound)"); t += x; }
                    UT_INFO("d3d: list draw remaps:%s", t.c_str());
                }
            }
            // Shader resources and samplers the list left unbound resolve from the executing context as well (Forza Horizon 2's
            // sky reads its scattering tables from vertex t0/t1 bound there; without them it read past a zero buffer).
            for (int stage = 0; stage < 2; ++stage) {
                const std::shared_ptr<gpu::Shader>& sh = stage == 0 ? d.vs : d.ps;
                if (!sh) continue;
                const auto& rf = gpu::Reflection(*sh);
                const int src = stage == 0 ? 0 : 4;
                auto imm_srv = [&](uint32_t slot) -> Res* {
                    int st = src, sl = static_cast<int>(slot);
                    if (imm.srv_remap[src][slot].stage < 6) { st = imm.srv_remap[src][slot].stage; sl = imm.srv_remap[src][slot].slot; }
                    return ResAt(imm.srv[st][sl]);
                };
                for (uint32_t i = 0; i < 16; ++i) {
                    if ((rf.textures & (1u << i)) && !d.srv[stage][i]) {
                        if (Res* r = imm_srv(i)) {
                            Res* target = r->base ? r->base : r;
                            if (target->placement != 0 && ((target->placement_dirty && g_frame.load() >= target->next_try_frame) || PlacementChanged(*target))) UploadPlacement(*target);
                            d.srv[stage][i] = r->view;
                        } else if (static std::atomic<int> shown {0}; shown++ < 16) {
                            UT_INFO("d3d: list draw: %s t%u unbound at execute too (shader %p, dimension %u)", stage ? "ps" : "vs", i, static_cast<void*>(sh.get()), rf.texture_dimension[i]);
                        }
                    }
                    if ((rf.samplers & (1u << i)) && d.sampler[stage][i].empty()) {
                        int st = src, sl = static_cast<int>(i);
                        if (imm.sampler_remap[src][i].stage < 6) { st = imm.sampler_remap[src][i].stage; sl = imm.sampler_remap[src][i].slot; }
                        if (Res* r = ResAt(imm.sampler[st][sl])) d.sampler[stage][i] = r->state;
                    }
                }
                for (uint8_t slot : rf.high_slots) {
                    const bool have = std::any_of(d.extra_srv[stage].begin(), d.extra_srv[stage].end(), [&](const auto& e) { return e.first == slot && e.second; });
                    if (!have) if (Res* r = imm_srv(slot)) d.extra_srv[stage].emplace_back(slot, r->view);
                }
            }
            // Dynamic re-snapshot of constant buffers from guest memory for the current frame
            for (int stage = 0; stage < 2; ++stage) {
                for (int k = 0; k < 14; ++k) {
                    if (std::any_of(rd.cb_remaps.begin(), rd.cb_remaps.end(), [&](const auto& m) { return m.stage == stage && m.slot == k; })) continue;   // resolved above
                    if (rd.cb_addr[stage][k] != 0 && rd.cb_size[stage][k] != 0) {
                        if (static const bool diag = std::getenv("UTSURO_CBDIAG") != nullptr; diag && stage == 0 && k == 2) {
                            static std::atomic<uint64_t> n {0};
                            if (n++ % 3000 == 0) {
                                const uint32_t* w = reinterpret_cast<const uint32_t*>(rd.cb_addr[0][2]);
                                uint32_t nz = 0;
                                for (uint32_t j = 0; j < rd.cb_size[0][2] / 4; ++j) nz += w[j] != 0;
                                UT_INFO("d3d: cbdiag replay vs cb2 %#llx %u B: %u dwords non-zero, frame %llu", (unsigned long long)rd.cb_addr[0][2], rd.cb_size[0][2], nz, (unsigned long long)g_frame.load());
                            }
                        }
                        if (auto b = CopyOfHost(rd.cb_addr[stage][k], rd.cb_size[stage][k])) {
                            d.cb[stage][k].buffer = b;
                            d.cb[stage][k].offset = 0;
                            d.cb[stage][k].size = rd.cb_size[stage][k];
                        }
                    }
                }
            }
            // On the console a command list runs against the immediate context's state: the title binds its
            // per-frame vertex constants and textures there and only records per-draw state in the list. The
            // deferred context's own stream never carries them, so a recorded draw's declared slots that stayed
            // unbound resolve from the immediate context now -- Forza Horizon 2's world draws kept every vertex
            // constant buffer unbound otherwise, collapsing the whole scene to (0,0,0,0) and an empty frame.
            for (int stage = 0; stage < 2; ++stage) {
                const std::shared_ptr<gpu::Shader>& sh = stage == 0 ? d.vs : d.ps;
                if (!sh) continue;
                const auto& rf = gpu::Reflection(*sh);
                const int src = stage == 0 ? 0 : 4;
                for (uint32_t k = 0; k < 14; ++k) {
                    if (!(rf.constant_buffers & (1u << k)) || d.cb[stage][k].buffer != nullptr) continue;
                    if (const auto* v = imm_reg((stage == 0 ? kRegCbA : kRegCbB) + 0x10 * k); v != nullptr && v->n >= 3) {
                        uint32_t size = std::min<uint32_t>(v->dw[2], 65536);
                        if (rf.cb_size[k] != 0) size = std::min<uint32_t>(size, rf.cb_size[k] * 16);
                        if (auto b = CopyOfHost(imm_address(*v), size)) {
                            d.cb[stage][k].buffer = b;
                            d.cb[stage][k].offset = 0;
                            d.cb[stage][k].size = size;
                        }
                    }
                    if (!d.cb[stage][k].buffer && imm.cb_address[src][k] != 0) {
                        uint32_t size = imm.cb_count[src][k] ? imm.cb_count[src][k] * 16 : 65536;
                        if (rf.cb_size[k] != 0) size = std::min<uint32_t>(size, rf.cb_size[k] * 16);
                        const uint64_t addr = imm.cb_address[src][k] + imm.cb_first[src][k] * 16;
                        d.cb[stage][k].buffer = CopyOfHost(addr, size, false);
                        d.cb[stage][k].offset = 0;
                        d.cb[stage][k].size = size;
                    }
                    if (!d.cb[stage][k].buffer) {
                        if (Res* r = ResAt(imm.cb[src][k])) {
                            if (r->buffer) {
                                d.cb[stage][k].buffer = r->buffer;
                                d.cb[stage][k].offset = imm.cb_first[src][k] * 16;
                                d.cb[stage][k].size = imm.cb_count[src][k] ? imm.cb_count[src][k] * 16 : gpu::BufferSize(*r->buffer);
                            } else if (r->host_ptr != 0) {
                                uint32_t size = imm.cb_count[src][k] ? imm.cb_count[src][k] * 16 : (r->host_size ? r->host_size : 65536);
                                if (rf.cb_size[k] != 0) size = std::min<uint32_t>(size, rf.cb_size[k] * 16);
                                const uint64_t addr = r->host_ptr + imm.cb_first[src][k] * 16;
                                d.cb[stage][k].buffer = CopyOfHost(addr, size, false);
                                d.cb[stage][k].offset = 0;
                                d.cb[stage][k].size = size;
                            }
                        }
                    }
                    static const uint64_t cbfrom2 = std::getenv("UTSURO_CBFROM") != nullptr ? std::strtoull(std::getenv("UTSURO_CBFROM"), nullptr, 10) : ~0ull;
                    static std::atomic<int> filled {0}, empty {0};
                    if (g_frame.load() < cbfrom2) continue;
                    if (d.cb[stage][k].buffer != nullptr) {
                        if (filled++ < 12) {
                            size_t nz = 0;
                            const uint32_t* dw = reinterpret_cast<const uint32_t*>(gpu::BufferData(*d.cb[stage][k].buffer) + d.cb[stage][k].offset);
                            const size_t words = std::min<size_t>(d.cb[stage][k].size != 0 ? d.cb[stage][k].size : 65536, gpu::BufferSize(*d.cb[stage][k].buffer)) / 4;
                            for (size_t w = 0; w < words; ++w) nz += dw[w] != 0;
                            UT_INFO("d3d: list draw: %s cb%u filled from the immediate context (%u bytes, %zu non-zero dwords)", stage ? "ps" : "vs", k, d.cb[stage][k].size, nz);
                        }
                    } else if (empty++ < 6) {
                        UT_INFO("d3d: list draw: %s cb%u still unbound at execute (%zu immediate registers)", stage ? "ps" : "vs", k, imm.regs.size());
                    }
                }
            }
            // Dynamic re-snapshot of vertex buffers from guest memory
            for (int k = 0; k < 8; ++k) {
                if (rd.vb_addr[k] != 0 && rd.vb_size[k] != 0) {
                    if (auto b = CopyOfHost(rd.vb_addr[k], rd.vb_size[k])) {
                        d.vb[k].buffer = b;
                        d.vb[k].offset = 0;
                    }
                }
            }
            gpu::Draw(d, rd.args);
        }
        // What the list bound stays bound on the executing context (the console's lists leave their output state behind).
        if (static const bool carry = std::getenv("UTSURO_RTINHERIT") != nullptr && std::string(std::getenv("UTSURO_RTINHERIT")) == "list"; carry) {
            if (list->final_om) {
                std::memcpy(imm.rtv, list->final_rtv, sizeof imm.rtv);
                imm.dsv = list->final_dsv;
            }
            if (list->final_vp) {
                imm.num_viewports = list->final_nvp;
                std::memcpy(imm.viewports, list->final_vps, sizeof imm.viewports);
            }
        }
        // A list runs as often as the title executes it: Forza Horizon 2 records its scene into lists once and executes ~850 a frame
        // (dropping the recording after the first run left everything but per-frame work undrawn). The recording goes when the title
        // releases the list (command lists are recycled objects).
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

    // GPU-written labels. The command processor would write these when the pipe drains; every call here has already
    // finished when it returns, so the label is written at once. Forza Horizon 2's render thread spins on one
    // (`WriteValueEndOfPipe(address, value)`) until it reaches the fence it expects.
    auto write_label = [](const char* name, bool wide) {
        return [name, wide](Call& c) -> uint32_t {
            uint64_t* at = c.Ptr<uint64_t>(0);
            uint64_t probe = 0;
            if (at == nullptr || !SafeRead(reinterpret_cast<uint64_t>(at), &probe, 8)) return 0;
            if (wide) *at = c.Arg(1); else *reinterpret_cast<uint32_t*>(at) = static_cast<uint32_t>(c.Arg(1));
            static std::atomic<int> shown {0};
            if (shown++ < 6) UT_INFO("d3d: %s(%p, %#llx)", name, static_cast<void*>(at), (unsigned long long)c.Arg(1));
            return 0;
        };
    };
    OverrideAll("WriteValueEndOfPipe", write_label("WriteValueEndOfPipe", false));
    OverrideAll("WriteValueBottomOfPipe", write_label("WriteValueBottomOfPipe", false));
    OverrideAll("WriteValueEndOfPipe64", write_label("WriteValueEndOfPipe64", true));
    OverrideAll("WriteTimestampToMemory", [](Call& c) -> uint32_t {
        uint64_t* at = c.Ptr<uint64_t>(0);
        uint64_t probe = 0;
        if (at != nullptr && SafeRead(reinterpret_cast<uint64_t>(at), &probe, 8)) *at = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10);   // 100 MHz GPU ticks
        return 0;
    });

    // ---- draws ----
    OverrideAll("CSSetShader", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        s.shader[5] = c.Arg(0);
        Res* r = ResAt(c.Arg(0));
        static std::atomic<int> cs_shown {0};
        if (cs_shown++ < 20) UT_INFO("d3d: CSSetShader cs=%#llx res=%p shader=%p", (unsigned long long)c.Arg(0), r, r ? r->shader.get() : nullptr);
        return 0;
    });
    // CSSetUnorderedAccessViews(start, count, uavs, initial counts): a count other than -1 sets the view's append/consume counter.
    OverrideAll("CSSetUnorderedAccessViews", [](Call& c) -> uint32_t {
        auto& s = StateOf(c.obj);
        uint32_t start = static_cast<uint32_t>(c.Arg(0));
        uint32_t count = static_cast<uint32_t>(c.Arg(1));
        const auto* uavs = c.Ptr<uint64_t>(2);
        const auto* initial = c.Arg(3) != 0 ? c.Ptr<uint32_t>(3) : nullptr;
        for (uint32_t i = 0; uavs && i < count && (start + i) < 64; ++i) {
            s.uav[start + i] = uavs[i];
            if (initial != nullptr) SetUavCounter(ResAt(uavs[i]), initial[i]);
        }
        return 0;
    });
    // CopyStructureCount(dst buffer, byte offset, uav): the view's counter, copied on the GPU in order with the work around it.
    OverrideAll("CopyStructureCount", [](Call& c) -> uint32_t {
        Res* dst = ResAt(c.Arg(0));
        Res* uav = ResAt(c.Arg(2));
        if (dst == nullptr || !dst->buffer || uav == nullptr || !uav->view || !uav->view->counter) return 0;
        gpu::CopyBuffer(*dst->buffer, static_cast<uint32_t>(c.Arg(1)), *uav->view->counter, 0, 4);
        return 0;
    });
    OverrideAll("Dispatch", [](Call& c) -> uint32_t {
        uint32_t x = static_cast<uint32_t>(c.Arg(0));
        uint32_t y = static_cast<uint32_t>(c.Arg(1));
        uint32_t z = static_cast<uint32_t>(c.Arg(2));
        DoDispatch(c.obj, x, y, z);
        return 0;
    });

    // Draw(Indexed)InstancedIndirect(args buffer, byte offset) and DispatchIndirect: the counts a GPU pass wrote (Forza Horizon 2's
    // GPU-culled draws), read by Metal from the buffer in order with the work that wrote them.
    OverrideAll("DrawInstancedIndirect", [](Call& c) -> uint32_t {
        Res* args = ResAt(c.Arg(0));
        if (args == nullptr || !args->buffer) return 0;
        gpu::DrawArgs a;
        a.indirect = args->buffer;
        a.indirect_offset = static_cast<uint32_t>(c.Arg(1));
        TraceDrawCaller(c);
        DoDraw(c.obj, a);
        return 0;
    });
    OverrideAll("DrawIndexedInstancedIndirect", [](Call& c) -> uint32_t {
        Res* args = ResAt(c.Arg(0));
        if (args == nullptr || !args->buffer) return 0;
        gpu::DrawArgs a;
        a.indexed = true;
        a.indirect = args->buffer;
        a.indirect_offset = static_cast<uint32_t>(c.Arg(1));
        TraceDrawCaller(c);
        DoDraw(c.obj, a);
        return 0;
    });
    OverrideAll("DispatchIndirect", [](Call& c) -> uint32_t {
        Res* args = ResAt(c.Arg(0));
        if (args == nullptr || !args->buffer) return 0;
        DoDispatch(c.obj, 1, 1, 1, args->buffer, static_cast<uint32_t>(c.Arg(1)));
        return 0;
    });
    OverrideAll("Draw", [](Call& c) -> uint32_t {
        gpu::DrawArgs a;
        a.count = static_cast<uint32_t>(c.Arg(0));
        a.start = static_cast<uint32_t>(c.Arg(1));
        TraceDrawCaller(c);
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
        TraceDrawCaller(c);
        DoDraw(c.obj, a);
        return 0;
    });
    OverrideAll("DrawInstanced", [](Call& c) -> uint32_t {
        gpu::DrawArgs a;
        a.count = static_cast<uint32_t>(c.Arg(0));
        a.start = static_cast<uint32_t>(c.Arg(1));
        a.start_instance = static_cast<uint32_t>(c.Arg(1) >> 32);
        a.instances = static_cast<uint32_t>(c.Arg(2));
        TraceDrawCaller(c);
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
        TraceDrawCaller(c);
        DoDraw(c.obj, a);
        return 0;
    });

    // ---- constant buffer ranges (D3D11.1) ----
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
    // Present1(sync interval, flags, params): the same.
    OverrideAll("Present1", [](Call& c) -> uint32_t {
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
