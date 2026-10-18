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
    } else if (n == "CreateTexture1D" || n == "CreateTexture2D" || n == "CreateTexture3D") {
        gpu::TextureDesc d;
        uint32_t bind, misc, usage;
        if (n == "CreateTexture1D") {
            d.width = U32(desc, 0); d.height = 1; d.mips = MipCount(U32(desc, 1), d.width, 1); d.array_size = std::max(1u, U32(desc, 2)); d.format = U32(desc, 3);
            usage = U32(desc, 4); bind = U32(desc, 5); misc = U32(desc, 7);
        } else if (n == "CreateTexture2D") {
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
    } else if (n == "CreateBlendState" || n == "CreateDepthStencilState" || n == "CreateRasterizerState" || n == "CreateSamplerState" || n == "CreateDepthStencilStateX") {
        r.state.bytes = desc;
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

gpu::DrawState Snapshot(Obj& ctx, CtxState s) {
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
    return d;
}

void DoDraw(Obj& ctx, const gpu::DrawArgs& args) {
    if (!gpu::Available()) return;
    const gpu::DrawState d = Snapshot(ctx, StateOf(ctx));
    if (!d.vs) {
        static int shown = 0;
        const CtxState& s = StateOf(ctx);
        if (shown++ < 5) {
            Res* r = ResAt(s.shader[0]);
            UT_INFO("d3d: draw without a vertex shader: vs=%#llx res=%p shader=%p ps=%#llx", (unsigned long long)s.shader[0], (void*)r, r ? (void*)r->shader.get() : nullptr, (unsigned long long)s.shader[4]);
        }
    }
    if (args.indexed && !d.index_buffer) {
        static int shown = 0;
        const CtxState& s = StateOf(ctx);
        if (shown++ < 5) {
            Res* r = ResAt(s.ib);
            Obj* o = Runtime::Get().FromPointer(s.ib);
            UT_INFO("d3d: indexed draw without an index buffer: ib=%#llx res=%p buffer=%p obj=%s fmt=%u", (unsigned long long)s.ib, (void*)r, r ? (void*)r->buffer.get() : nullptr, o ? o->class_name.c_str() : "?", s.ib_format);
        }
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

void RegisterGpuOverrides() {
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
        Res* r = ResAt(c.Arg(0));
        auto* out = c.Ptr<uint64_t>(4);
        if (r == nullptr || out == nullptr) { if (out) std::memset(out, 0, 16); return 0x80004005u; }
        const uint32_t type = static_cast<uint32_t>(c.Arg(2));
        if (r->buffer) return MapBuffer(c, *r, type, out);
        if (r->texture) {
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
