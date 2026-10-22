// renderer.h -- the Metal side of the host D3D11.X device. The D3D layer (nt/d3d.cpp) owns the
// COM objects and the guest-visible state; this layer turns that state into Metal work.
#pragma once

#include "gpu/dxbc.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace utsuro::gpu {

struct Buffer;
struct Texture;
struct Shader;
struct InputLayout;

struct TextureDesc {
    uint32_t width {1}, height {1}, depth {1};   ///< depth > 1: a 3D texture
    uint32_t mips {1}, array_size {1};
    uint32_t format {0};                          ///< DXGI_FORMAT
    uint32_t samples {1};
    bool cube {false};
    bool render_target {false}, depth_stencil {false}, shader_resource {true};
};

/// How a view looks at a texture (SRV, RTV or DSV).
struct ViewDesc {
    uint32_t format {0};              ///< DXGI_FORMAT, 0: the texture's own
    uint32_t dimension {0};           ///< D3D11_SRV_DIMENSION / RTV / DSV values (1D 2, 1DARRAY 3, 2D 4, 2DARRAY 5, 2DMS 6, 2DMSARRAY 7, 3D 8, CUBE 9, CUBEARRAY 10)
    uint32_t first_mip {0}, mip_count {0};      ///< mip_count 0: all from first_mip
    uint32_t first_slice {0}, slice_count {0};  ///< array slices (or 3D depth slices for render targets)
};

struct TextureView {
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Buffer> buffer;
    ViewDesc desc;
};

struct BufferBinding {
    std::shared_ptr<Buffer> buffer;
    uint32_t offset {0};   ///< bytes
    uint32_t size {0};     ///< bytes; 0: to the end
};

struct VertexBufferBinding {
    std::shared_ptr<Buffer> buffer;
    uint32_t stride {0}, offset {0};
};

/// D3D11 state descriptors are passed through as the raw structs the title built.
struct StateBlob {
    std::string bytes;
    bool empty() const { return bytes.empty(); }
};

struct Viewport { float x, y, w, h, min_depth, max_depth; };
struct ScissorRect { int32_t left, top, right, bottom; };

struct InputElement {
    std::string semantic;
    uint32_t semantic_index {0};
    uint32_t format {0};
    uint32_t slot {0};
    uint32_t offset {0};       ///< byte offset, already resolved (no APPEND_ALIGNED)
    uint32_t slot_class {0};   ///< 0 per-vertex, 1 per-instance
    uint32_t step_rate {0};
};

struct DrawState {
    std::shared_ptr<Shader> vs, ps;
    std::shared_ptr<InputLayout> layout;
    BufferBinding cb[2][16];                    ///< [0] vertex stage, [1] pixel stage
    std::shared_ptr<TextureView> srv[2][16];
    std::vector<std::pair<uint8_t, std::shared_ptr<TextureView>>> extra_srv[2];   ///< SRVs in slots 16 and above (ShaderReflection::high_slots)
    std::shared_ptr<TextureView> ps_uav[64];    ///< pixel-stage UAVs (OMSetRenderTargetsAndUnorderedAccessViews)
    StateBlob sampler[2][16];
    VertexBufferBinding vb[16];
    std::shared_ptr<Buffer> index_buffer;
    uint32_t index_format {0}, index_offset {0};   ///< DXGI_FORMAT_R16_UINT (57) / R32_UINT (42)
    uint32_t topology {0};                          ///< D3D_PRIMITIVE_TOPOLOGY
    std::shared_ptr<TextureView> rtv[8];
    std::shared_ptr<TextureView> dsv;
    StateBlob blend, depth_stencil, raster;
    float blend_factor[4] {1, 1, 1, 1};
    uint32_t sample_mask {0xFFFFFFFF};
    uint32_t stencil_ref {0};
    Viewport viewports[16] {};
    uint32_t num_viewports {0};
    ScissorRect scissors[16] {};
    uint32_t num_scissors {0};
};

struct DrawArgs {
    bool indexed {false};
    uint32_t count {0};            ///< vertices or indices
    uint32_t instances {1};
    uint32_t start {0};            ///< first vertex or first index
    int32_t base_vertex {0};
    uint32_t start_instance {0};
};

struct DispatchState {
    std::shared_ptr<Shader> cs;
    BufferBinding cb[16];
    std::shared_ptr<TextureView> srv[16];
    std::vector<std::pair<uint8_t, std::shared_ptr<TextureView>>> extra_srv;   ///< SRVs in slots 16 and above
    StateBlob sampler[16];
    std::shared_ptr<TextureView> uav[64];
};

// ---- lifetime ----------------------------------------------------------------------------
bool Init();
bool Available();
/// The window's CAMetalLayer (an NSObject pointer); frames are presented into it.
void SetLayer(void* ca_metal_layer);

// ---- buffers ------------------------------------------------------------------------------
std::shared_ptr<Buffer> CreateBuffer(uint32_t size, bool dynamic, const void* initial);
/// A buffer for this frame only, from the frame ring when it has room.
std::shared_ptr<Buffer> CreateTransientBuffer(uint32_t size, const void* initial);
/// The CPU address of the buffer's current contents (shared memory: the guest reads and writes it).
uint8_t* BufferData(Buffer& buffer);
uint32_t BufferSize(Buffer& buffer);
/// Gives the buffer fresh storage so earlier draws keep the contents they were recorded with.
/// With `keep` the new storage starts as a copy of the old.
uint8_t* RenameBuffer(const std::shared_ptr<Buffer>& buffer, bool keep);

// ---- textures ------------------------------------------------------------------------------
std::shared_ptr<Texture> CreateTexture(const TextureDesc& desc);
void UploadTexture(Texture& texture, uint32_t mip, uint32_t slice, const uint32_t box[6] /* left top front right bottom back, or null */,
                   const void* data, uint32_t row_pitch, uint32_t slice_pitch);
bool ReadbackTexture(Texture& texture, uint32_t mip, uint32_t slice, void* dst, uint32_t row_pitch);
void CopyTexture(Texture& dst, uint32_t dst_mip, uint32_t dst_slice, uint32_t dx, uint32_t dy, uint32_t dz,
                 Texture& src, uint32_t src_mip, uint32_t src_slice, const uint32_t box[6]);
/// D3D11.X DecompressResource: copies a (compressed, on the console) surface into another, converting where the formats differ --
/// a depth buffer into a depth or R32 float texture a shader can sample. `rect` is {left, top, right, bottom} or null for all.
void DecompressCopy(Texture& dst, uint32_t dst_mip, uint32_t dst_slice, uint32_t dx, uint32_t dy, Texture& src, uint32_t src_mip, uint32_t src_slice, const uint32_t rect[4]);
/// ResolveSubresource: a multisampled texture averaged into one sample (Metal resolves it at the end of a render pass), or a copy
/// when the source has one sample.
void ResolveTexture(Texture& dst, uint32_t dst_mip, uint32_t dst_slice, Texture& src, uint32_t src_mip, uint32_t src_slice);
void GenerateMips(Texture& texture);
const TextureDesc& TextureInfo(const Texture& texture);
/// The Metal texture behind `texture`, for logs (matches the pointers frame traces print).
void* NativeOf(const Texture& texture);
/// Sampled views start at this mip: the levels above it have no texels yet (a streamed texture).
void SetMinMip(Texture& texture, uint32_t mip);

// ---- shaders and layouts ----------------------------------------------------------------------
std::shared_ptr<Shader> CreateShader(const uint8_t* dxbc, size_t size, ShaderStage stage);
const ShaderReflection& Reflection(const Shader& shader);
std::shared_ptr<InputLayout> CreateInputLayout(const std::vector<InputElement>& elements, const uint8_t* vs_dxbc, size_t size);

// ---- drawing ----------------------------------------------------------------------------------------
void Draw(const DrawState& state, const DrawArgs& args);
void Dispatch(const DispatchState& state, uint32_t x, uint32_t y, uint32_t z);
void ClearRenderTarget(const TextureView& view, const float color[4]);
void ClearDepthStencil(const TextureView& view, uint32_t flags, float depth, uint8_t stencil);
void CopyBuffer(Buffer& dst, uint32_t dst_offset, Buffer& src, uint32_t src_offset, uint32_t size);
/// Ends the frame: shows `back_buffer` in the window and waits for the display's pace.
void Present(const std::shared_ptr<Texture>& back_buffer, uint32_t sync_interval);
void Flush();
/// The next presented frame is written to `path` as a PNG (debugging).
void RequestSnapshot(const std::string& path);
/// Logs every draw of a frame a moment from now (needs UTSURO_TRACEPIXELS for the data dump).
void RequestTrace();
/// Whether `frame` is the frame being traced (UTSURO_TRACEFRAME / RequestTrace).
bool IsTraceFrame(uint64_t frame);

/// Writes the texture's top mip to a PNG (debugging).
bool SavePng(Texture& texture, const std::string& path);

} // namespace utsuro::gpu
