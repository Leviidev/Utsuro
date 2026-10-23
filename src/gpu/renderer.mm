// renderer.mm -- Metal implementation of gpu/renderer.h.
//
// Buffers are shared MTLBuffers the guest reads and writes directly. A buffer whose contents are
// replaced wholesale (WRITE_DISCARD, UpdateSubresource) moves to a fresh region of a per-frame
// ring, so commands already recorded keep the data they were recorded with; ring-resident
// buffers that outlive their frame are copied out before the region is reused.
#include "gpu/renderer.h"

#include "engine/log.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace utsuro::gpu {

bool WritePngRGBA(const uint8_t* rgba, size_t w, size_t h, const char* path);

// ---- objects -------------------------------------------------------------------------------

struct Buffer {
    id<MTLBuffer> mtl {nil};
    uint32_t offset {0};
    uint32_t size {0};
    bool dynamic {false};
    int ring_frame {-1};     ///< >= 0: the storage lives in that ring slot
};

struct Texture {
    id<MTLTexture> mtl {nil};
    TextureDesc desc;
    MTLPixelFormat pixel_format {MTLPixelFormatInvalid};
    std::map<std::string, id<MTLTexture>> views;
    uint64_t depth_gen {0}, copy_gen {~0ull};   ///< a depth texture sampled as float: bumped when it is written, and when its R32Float copy was last made
    id<MTLTexture> depth_copy {nil};
    uint32_t min_mip {0};   ///< sampled views start here (levels above it were never written)
    uint32_t uploads {0};   ///< UploadTexture calls (debugging: a texture nothing was ever written to)
};

struct Shader {
    ShaderStage stage {ShaderStage::Unknown};
    TranslatedShader translated;          ///< the default (all-float inputs) translation
    std::vector<uint8_t> dxbc;
    id<MTLFunction> function {nil};
    id<MTLLibrary> library {nil};
    bool failed {false};
    int serial {0};
    /// Vertex shaders are specialised by how their layout feeds integer inputs.
    std::map<std::string, id<MTLFunction>> variants;
};

struct InputLayout {
    std::vector<InputElement> elements;
    std::vector<SignatureElement> vs_inputs;   ///< the signature the layout was created against
};

namespace {

constexpr int kRingFrames = 3;
constexpr size_t kRingBytes = 48u << 20;

struct Ring {
    id<MTLBuffer> buffer {nil};
    size_t capacity {0};
    size_t head {0};
    std::vector<std::weak_ptr<Buffer>> live;
};

struct Globals {
    std::recursive_mutex mutex;
    std::mutex ring_mutex;   ///< the ring (g->frame, ring heads): taken alone, or inside `mutex`, never around it
    id<MTLDevice> device {nil};
    id<MTLCommandQueue> queue {nil};
    bool ready {false};

    // Current command buffer / encoder.
    id<MTLCommandBuffer> command {nil};
    id<MTLRenderCommandEncoder> encoder {nil};
    std::string pass_key;

    Ring ring[kRingFrames];
    int frame {0};
    dispatch_semaphore_t frames_in_flight {nullptr};

    std::unordered_map<std::string, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<std::string, id<MTLComputePipelineState>> compute_pipelines;
    std::unordered_map<std::string, id<MTLDepthStencilState>> depth_states;
    std::unordered_map<std::string, id<MTLSamplerState>> samplers;
    std::map<uint32_t, id<MTLTexture>> dummy_textures;
    id<MTLSamplerState> default_sampler {nil};
    id<MTLBuffer> null_buffer {nil};
    id<MTLBuffer> zero_cb {nil};

    id<MTLRenderPipelineState> present_pipeline {nil};
    id<MTLComputePipelineState> fill_pipeline {nil};   ///< writes one 32-bit value over a buffer (UAV clears)
    CAMetalLayer* layer {nil};

    uint64_t frame_count {0};
    int trace_draws {0};
    uint64_t trace_frame {0};
    uint32_t trace_cap {1500};   ///< draws logged in the traced frame (UTSURO_TRACECAP)
    id<MTLRenderPipelineState> depth_copy_pipeline {nil};
    int step_dumps {0}, srv_dumps {0};
    int draws_in_command {0};   ///< draws encoded into the current command buffer (it is committed every so many)
    const char* dir_or_empty() const { return dump_dir ? dump_dir : "."; }
    std::vector<std::shared_ptr<Texture>> trace_rts;   // render targets drawn to during the traced frame, dumped at its Present
    std::vector<Texture*> trace_depths;               // depth textures sampled during the traced frame (their float copies are summarised at Present)
    std::string snapshot_path;
    std::string debug;   // UTSURO_GPUDEBUG: nodepth, nocull, noblend
    struct Stats { uint64_t draws {0}, no_vs {0}, bad_vs {0}, bad_ps {0}, no_pass {0}, no_pso {0}, bad_topology {0}, clears {0}, no_ib {0}; } stats;
    std::map<std::string, uint64_t> rt_census;   ///< UTSURO_RTCENSUS: draws per render target since the last stats line
    std::chrono::steady_clock::time_point last_present;
    const char* dump_dir {nullptr};
    uint32_t dump_every {1};
};

Globals* g = nullptr;
bool g_png_alpha = false;   // debugging: RGBA16F targets are saved as their alpha channel

// ---- format tables -------------------------------------------------------------------------

MTLPixelFormat PixelFormat(uint32_t f) {
    switch (f) {
    case 1: case 2: return MTLPixelFormatRGBA32Float;
    case 3: return MTLPixelFormatRGBA32Uint;
    case 4: return MTLPixelFormatRGBA32Sint;
    case 9: case 10: return MTLPixelFormatRGBA16Float;
    case 11: return MTLPixelFormatRGBA16Unorm;
    case 12: return MTLPixelFormatRGBA16Uint;
    case 13: return MTLPixelFormatRGBA16Snorm;
    case 14: return MTLPixelFormatRGBA16Sint;
    case 15: case 16: return MTLPixelFormatRG32Float;
    case 17: return MTLPixelFormatRG32Uint;
    case 18: return MTLPixelFormatRG32Sint;
    case 19: case 20: case 44: case 45: return MTLPixelFormatDepth32Float_Stencil8;
    case 21: case 46: return MTLPixelFormatDepth32Float_Stencil8;
    case 23: case 24: return MTLPixelFormatRGB10A2Unorm;
    case 25: return MTLPixelFormatRGB10A2Uint;
    case 26: return MTLPixelFormatRG11B10Float;
    case 27: case 28: return MTLPixelFormatRGBA8Unorm;
    case 29: return MTLPixelFormatRGBA8Unorm_sRGB;
    case 30: return MTLPixelFormatRGBA8Uint;
    case 31: return MTLPixelFormatRGBA8Snorm;
    case 32: return MTLPixelFormatRGBA8Sint;
    case 33: case 34: return MTLPixelFormatRG16Float;
    case 35: return MTLPixelFormatRG16Unorm;
    case 36: return MTLPixelFormatRG16Uint;
    case 37: return MTLPixelFormatRG16Snorm;
    case 38: return MTLPixelFormatRG16Sint;
    case 39: case 41: return MTLPixelFormatR32Float;
    case 40: return MTLPixelFormatDepth32Float;
    case 42: return MTLPixelFormatR32Uint;
    case 43: return MTLPixelFormatR32Sint;
    case 48: case 49: return MTLPixelFormatRG8Unorm;
    case 50: return MTLPixelFormatRG8Uint;
    case 51: return MTLPixelFormatRG8Snorm;
    case 52: return MTLPixelFormatRG8Sint;
    case 53: case 56: return MTLPixelFormatR16Unorm;
    case 54: return MTLPixelFormatR16Float;
    case 55: return MTLPixelFormatDepth16Unorm;
    case 57: return MTLPixelFormatR16Uint;
    case 58: return MTLPixelFormatR16Snorm;
    case 59: return MTLPixelFormatR16Sint;
    case 60: case 61: return MTLPixelFormatR8Unorm;
    case 62: return MTLPixelFormatR8Uint;
    case 63: return MTLPixelFormatR8Snorm;
    case 64: return MTLPixelFormatR8Sint;
    case 65: return MTLPixelFormatA8Unorm;
    case 67: return MTLPixelFormatRGB9E5Float;
    case 70: case 71: return MTLPixelFormatBC1_RGBA;
    case 72: return MTLPixelFormatBC1_RGBA_sRGB;
    case 73: case 74: return MTLPixelFormatBC2_RGBA;
    case 75: return MTLPixelFormatBC2_RGBA_sRGB;
    case 76: case 77: return MTLPixelFormatBC3_RGBA;
    case 78: return MTLPixelFormatBC3_RGBA_sRGB;
    case 79: case 80: return MTLPixelFormatBC4_RUnorm;
    case 81: return MTLPixelFormatBC4_RSnorm;
    case 82: case 83: return MTLPixelFormatBC5_RGUnorm;
    case 84: return MTLPixelFormatBC5_RGSnorm;
    case 85: return MTLPixelFormatB5G6R5Unorm;
    case 86: return MTLPixelFormatA1BGR5Unorm;
    case 87: case 88: case 90: case 92: return MTLPixelFormatBGRA8Unorm;
    case 91: case 93: return MTLPixelFormatBGRA8Unorm_sRGB;
    case 94: case 95: return MTLPixelFormatBC6H_RGBUfloat;
    case 96: return MTLPixelFormatBC6H_RGBFloat;
    case 97: case 98: return MTLPixelFormatBC7_RGBAUnorm;
    case 99: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    default: return MTLPixelFormatInvalid;
    }
}

bool IsDepth(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8;
}
bool HasStencil(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8; }

static size_t PixelFormatBytes(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatA8Unorm:
    case MTLPixelFormatR8Unorm:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatR8Sint:
        return 1;
    case MTLPixelFormatR16Unorm:
    case MTLPixelFormatR16Snorm:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint:
    case MTLPixelFormatR16Float:
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRG8Sint:
    case MTLPixelFormatDepth16Unorm:
        return 2;
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatR32Sint:
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatRG11B10Float:
    case MTLPixelFormatRGB9E5Float:
    case MTLPixelFormatDepth32Float:
#if !TARGET_OS_IPHONE
    case MTLPixelFormatDepth24Unorm_Stencil8:
#endif
    case MTLPixelFormatDepth32Float_Stencil8:
        return 4;
    case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatBC1_RGBA:
    case MTLPixelFormatBC1_RGBA_sRGB:
    case MTLPixelFormatBC4_RUnorm:
    case MTLPixelFormatBC4_RSnorm:
        return 8;
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGBA32Sint:
    case MTLPixelFormatRGBA32Float:
    case MTLPixelFormatBC2_RGBA:
    case MTLPixelFormatBC2_RGBA_sRGB:
    case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm:
    case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC6H_RGBFloat:
    case MTLPixelFormatBC6H_RGBUfloat:
    case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        return 16;
    default:
        return 0;
    }
}

MTLVertexFormat VertexFormat(uint32_t f) {
    switch (f) {
    case 2: return MTLVertexFormatFloat4;
    case 3: return MTLVertexFormatUInt4;
    case 4: return MTLVertexFormatInt4;
    case 6: return MTLVertexFormatFloat3;
    case 7: return MTLVertexFormatUInt3;
    case 8: return MTLVertexFormatInt3;
    case 10: return MTLVertexFormatHalf4;
    case 11: return MTLVertexFormatUShort4Normalized;
    case 12: return MTLVertexFormatUShort4;
    case 13: return MTLVertexFormatShort4Normalized;
    case 14: return MTLVertexFormatShort4;
    case 16: return MTLVertexFormatFloat2;
    case 17: return MTLVertexFormatUInt2;
    case 18: return MTLVertexFormatInt2;
    case 24: return MTLVertexFormatUInt1010102Normalized;
    case 28: return MTLVertexFormatUChar4Normalized;
    case 30: return MTLVertexFormatUChar4;
    case 31: return MTLVertexFormatChar4Normalized;
    case 32: return MTLVertexFormatChar4;
    case 34: return MTLVertexFormatHalf2;
    case 35: return MTLVertexFormatUShort2Normalized;
    case 36: return MTLVertexFormatUShort2;
    case 37: return MTLVertexFormatShort2Normalized;
    case 38: return MTLVertexFormatShort2;
    case 41: return MTLVertexFormatFloat;
    case 42: return MTLVertexFormatUInt;
    case 43: return MTLVertexFormatInt;
    case 49: return MTLVertexFormatUChar2Normalized;
    case 50: return MTLVertexFormatUChar2;
    case 51: return MTLVertexFormatChar2Normalized;
    case 52: return MTLVertexFormatChar2;
    case 54: return MTLVertexFormatHalf;
    case 56: return MTLVertexFormatUShortNormalized;
    case 57: return MTLVertexFormatUShort;
    case 58: return MTLVertexFormatShortNormalized;
    case 59: return MTLVertexFormatShort;
    case 61: return MTLVertexFormatUCharNormalized;
    case 62: return MTLVertexFormatUChar;
    case 63: return MTLVertexFormatCharNormalized;
    case 64: return MTLVertexFormatChar;
    case 87: return MTLVertexFormatUChar4Normalized_BGRA;
    default: return MTLVertexFormatFloat4;
    }
}

// ---- state decoding -------------------------------------------------------------------------

template <typename T>
T Rd(const std::string& s, size_t off, T fallback = T {}) {
    if (off + sizeof(T) > s.size()) return fallback;
    T v;
    std::memcpy(&v, s.data() + off, sizeof(T));
    return v;
}

MTLBlendFactor BlendFactor(uint32_t b) {
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    case 16: return MTLBlendFactorSource1Color;
    case 17: return MTLBlendFactorOneMinusSource1Color;
    case 18: return MTLBlendFactorSource1Alpha;
    case 19: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}
MTLBlendOperation BlendOp(uint32_t o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
MTLCompareFunction CompareFunc(uint32_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
MTLStencilOperation StencilOp(uint32_t o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}
MTLSamplerAddressMode Address(uint32_t a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

// ---- helpers --------------------------------------------------------------------------------

std::string Key(const void* p, size_t n) { return std::string(static_cast<const char*>(p), n); }

id<MTLBuffer> NewBuffer(size_t size) {
    id<MTLBuffer> b = [g->device newBufferWithLength:std::max<size_t>(size, 16) options:MTLResourceStorageModeShared];
    return b;
}

void EndEncoder() {
    if (g->encoder != nil) { [g->encoder endEncoding]; g->encoder = nil; }
    g->pass_key.clear();
}

id<MTLCommandBuffer> Command() {
    if (g->command == nil) g->command = [g->queue commandBuffer];
    return g->command;
}

/// A counter word for append/consume UAVs bound without one (their counts go nowhere).
id<MTLBuffer> ScratchCounter() {
    static id<MTLBuffer> b = nil;
    if (b == nil) { b = NewBuffer(256); std::memset(b.contents, 0, 256); }
    return b;
}

id<MTLTexture> ViewOf(Texture& t, const ViewDesc& v, bool as_render_target) {

    MTLPixelFormat fmt = v.format != 0 ? PixelFormat(v.format) : t.pixel_format;
    if (fmt == MTLPixelFormatInvalid) fmt = t.pixel_format;
    // A view must match the parent's bytes-per-pixel and depth classification.
    if (PixelFormatBytes(fmt) != PixelFormatBytes(t.mtl.pixelFormat) || IsDepth(t.mtl.pixelFormat) || IsDepth(fmt)) {
        fmt = t.mtl.pixelFormat;
    }
    MTLTextureType type = t.mtl.textureType;
    uint32_t dim = v.dimension;
    if (dim != 0) {
        switch (dim) {
        case 2: type = MTLTextureType1D; break;
        case 3: type = MTLTextureType1DArray; break;
        case 4: type = MTLTextureType2D; break;
        case 5: type = MTLTextureType2DArray; break;
        case 6: type = MTLTextureType2DMultisample; break;
        case 7: type = MTLTextureType2DMultisampleArray; break;
        case 8: type = MTLTextureType3D; break;
        case 9: type = MTLTextureTypeCube; break;
        case 10: type = MTLTextureTypeCubeArray; break;
        }
        // Render-target views use their own numbering (same values for the dimensions that matter).
    }
    {
        // Keep to view types Metal allows for this texture's type.
        const MTLTextureType base = t.mtl.textureType;
        bool ok = type == base;
        if (base == MTLTextureType2D) ok = type == MTLTextureType2D || type == MTLTextureType2DArray;
        else if (base == MTLTextureType2DArray) ok = type == MTLTextureType2D || type == MTLTextureType2DArray || type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray;
        else if (base == MTLTextureTypeCube) ok = type == MTLTextureTypeCube || type == MTLTextureType2D || type == MTLTextureType2DArray;
        else if (base == MTLTextureTypeCubeArray) ok = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray || type == MTLTextureType2D || type == MTLTextureType2DArray;
        else if (base == MTLTextureType1D) ok = type == MTLTextureType1D || type == MTLTextureType1DArray;
        else if (base == MTLTextureType1DArray) ok = type == MTLTextureType1D || type == MTLTextureType1DArray;
        if (!ok) type = base;
    }
    const uint32_t total_mips = static_cast<uint32_t>(t.mtl.mipmapLevelCount);
    const uint32_t total_slices = static_cast<uint32_t>(t.mtl.arrayLength);
    uint32_t first_mip = std::min(std::max(v.first_mip, as_render_target ? 0u : t.min_mip), total_mips - 1);
    uint32_t mip_count = v.mip_count == 0 ? total_mips - first_mip : std::min(v.mip_count, total_mips - first_mip);
    uint32_t first_slice = std::min(v.first_slice, total_slices > 0 ? total_slices - 1 : 0);
    uint32_t slice_count = v.slice_count == 0 ? (total_slices - first_slice) : std::min(v.slice_count, total_slices - first_slice);
    if (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) {
        if (t.mtl.textureType == MTLTextureTypeCube) { first_slice = 0; slice_count = 6; }
        if (type == MTLTextureTypeCube) slice_count = 6;
    }
    if (t.mtl.textureType == MTLTextureType3D) return t.mtl;
    if (type == MTLTextureType2D || type == MTLTextureType1D || type == MTLTextureType3D || type == MTLTextureType2DMultisample) slice_count = 1;
    const bool whole = fmt == t.mtl.pixelFormat && type == t.mtl.textureType && first_mip == 0 && mip_count == total_mips && first_slice == 0 && slice_count == total_slices;
    if (whole) return t.mtl;
    char key[96];
    std::snprintf(key, sizeof key, "%lu/%lu/%u/%u/%u/%u%s", (unsigned long)fmt, (unsigned long)type, first_mip, mip_count, first_slice, slice_count, as_render_target ? "r" : "s");
    auto it = t.views.find(key);
    if (it != t.views.end()) return it->second;
        id<MTLTexture> view = [t.mtl newTextureViewWithPixelFormat:fmt textureType:type levels:NSMakeRange(first_mip, mip_count) slices:NSMakeRange(first_slice, slice_count)];
    t.views[key] = view;
    return view;
}

id<MTLTexture> DummyTexture(uint32_t dim) {
    auto it = g->dummy_textures.find(dim);
    if (it != g->dummy_textures.end()) return it->second;
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.width = d.height = 1;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModeShared;
    switch (dim) {
    case 2: d.textureType = MTLTextureType1D; break;
    case 5: d.textureType = MTLTextureType3D; d.depth = 1; break;
    case 6: d.textureType = MTLTextureTypeCube; break;
    case 7: d.textureType = MTLTextureType1DArray; d.arrayLength = 1; break;
    case 8: d.textureType = MTLTextureType2DArray; d.arrayLength = 1; break;
    default: d.textureType = MTLTextureType2D; break;
    }
    id<MTLTexture> t = [g->device newTextureWithDescriptor:d];
    const uint32_t zero[1] = {0};
    if (d.textureType == MTLTextureTypeCube) {
        for (int s = 0; s < 6; ++s) [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:s withBytes:zero bytesPerRow:4 bytesPerImage:4];
    } else if (d.textureType == MTLTextureType3D) {
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:zero bytesPerRow:4 bytesPerImage:4];
    } else if (d.textureType != MTLTextureType1D) {
        [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:0 withBytes:zero bytesPerRow:4 bytesPerImage:4];
    } else {
        [t replaceRegion:MTLRegionMake1D(0, 1) mipmapLevel:0 slice:0 withBytes:zero bytesPerRow:4 bytesPerImage:4];
    }
    g->dummy_textures[dim] = t;
    return t;
}

id<MTLSamplerState> SamplerFor(const StateBlob& blob) {
    if (blob.empty()) return g->default_sampler;
    auto it = g->samplers.find(blob.bytes);
    if (it != g->samplers.end()) return it->second;
    const uint32_t filter = Rd<uint32_t>(blob.bytes, 0);
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    const bool aniso = (filter & 0x7F) == 0x55;
    d.minFilter = (filter & 0x10) || aniso ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = (filter & 0x04) || aniso ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = (filter & 0x01) || aniso ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    d.sAddressMode = Address(Rd<uint32_t>(blob.bytes, 4));
    d.tAddressMode = Address(Rd<uint32_t>(blob.bytes, 8));
    d.rAddressMode = Address(Rd<uint32_t>(blob.bytes, 12));
    d.maxAnisotropy = aniso ? std::max<uint32_t>(1, std::min<uint32_t>(16, Rd<uint32_t>(blob.bytes, 20))) : 1;
    // In Metal, samplers with compareFunction != Never cannot be sampled via texture2d.sample().
    // dxbc translates comparison ops (and shaders like UE3 shadow sampling) to regular texture2d.sample(),
    // so compareFunction must remain Never for Apple Silicon.
    (void)filter;
    const float border[4] = {Rd<float>(blob.bytes, 28), Rd<float>(blob.bytes, 32), Rd<float>(blob.bytes, 36), Rd<float>(blob.bytes, 40)};
    d.borderColor = border[3] == 0 ? MTLSamplerBorderColorTransparentBlack : (border[0] > 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack);
    d.lodMinClamp = Rd<float>(blob.bytes, 44);
    d.lodMaxClamp = std::min(Rd<float>(blob.bytes, 48, 1000.0f), 1000.0f);
    d.supportArgumentBuffers = NO;
    id<MTLSamplerState> s = [g->device newSamplerStateWithDescriptor:d];
    g->samplers[blob.bytes] = s;
    return s;
}

id<MTLDepthStencilState> DepthStateFor(const StateBlob& blob, bool has_depth, bool has_stencil) {
    std::string key = blob.bytes + (has_depth ? "D" : "d") + (has_stencil ? "S" : "s");
    auto it = g->depth_states.find(key);
    if (it != g->depth_states.end()) return it->second;
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    const bool depth_enable = blob.empty() ? true : Rd<uint32_t>(blob.bytes, 0) != 0;
    d.depthCompareFunction = has_depth && depth_enable ? CompareFunc(blob.empty() ? 2 : Rd<uint32_t>(blob.bytes, 8)) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = has_depth && depth_enable && (blob.empty() ? true : Rd<uint32_t>(blob.bytes, 4) != 0);
    if (has_stencil && !blob.empty() && Rd<uint32_t>(blob.bytes, 12) != 0) {
        auto face = [&](size_t base) {
            MTLStencilDescriptor* s = [MTLStencilDescriptor new];
            s.stencilFailureOperation = StencilOp(Rd<uint32_t>(blob.bytes, base));
            s.depthFailureOperation = StencilOp(Rd<uint32_t>(blob.bytes, base + 4));
            s.depthStencilPassOperation = StencilOp(Rd<uint32_t>(blob.bytes, base + 8));
            s.stencilCompareFunction = CompareFunc(Rd<uint32_t>(blob.bytes, base + 12));
            s.readMask = Rd<uint8_t>(blob.bytes, 16, 0xFF);
            s.writeMask = Rd<uint8_t>(blob.bytes, 17, 0xFF);
            return s;
        };
        d.frontFaceStencil = face(20);
        d.backFaceStencil = face(36);
    }
    id<MTLDepthStencilState> s = [g->device newDepthStencilStateWithDescriptor:d];
    g->depth_states[key] = s;
    return s;
}

bool CompileShader(Shader& s) {
    if (s.function != nil || s.failed) return s.function != nil;
    NSString* src = [NSString stringWithUTF8String:s.translated.msl.c_str()];
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.fastMathEnabled = NO;
    opts.preserveInvariance = YES;   // vertex positions are [[invariant]] (see the translator)
    NSError* err = nil;
    s.library = [g->device newLibraryWithSource:src options:opts error:&err];
    if (s.library == nil) {
        UT_ERROR("gpu: Metal rejected a shader: %s", [[err localizedDescription] UTF8String]);
        if (std::getenv("UTSURO_DUMPMSL")) std::fprintf(stderr, "%s\n", s.translated.msl.c_str());
        s.failed = true;
        return false;
    }
    s.function = [s.library newFunctionWithName:@"main0"];
    static int counter = 0;
    s.serial = ++counter;
    if (const char* dir = std::getenv("UTSURO_DUMPMSL")) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/shader_%03d_%s.metal", dir, s.serial, s.stage == ShaderStage::Vertex ? "vs" : "ps");
        if (FILE* f = std::fopen(path, "w")) { std::fputs(s.translated.msl.c_str(), f); std::fclose(f); }
    }
    return s.function != nil;
}

/// The vertex function for `classes` (one byte per input register; see TranslateDxbc).
id<MTLFunction> VariantFor(Shader& s, const std::vector<uint8_t>& classes) {
    bool any = false;
    for (uint8_t c : classes) any |= c != 0;
    if (!any) return CompileShader(s) ? s.function : nil;
    std::string key(classes.begin(), classes.end());
    auto it = s.variants.find(key);
    if (it != s.variants.end()) return it->second;
    TranslatedShader t = TranslateDxbc(s.dxbc.data(), s.dxbc.size(), &classes);
    id<MTLFunction> fn = nil;
    if (t.ok) {
        NSError* err = nil;
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.fastMathEnabled = NO;
        opts.preserveInvariance = YES;
        id<MTLLibrary> lib = [g->device newLibraryWithSource:[NSString stringWithUTF8String:t.msl.c_str()] options:opts error:&err];
        if (lib == nil) UT_ERROR("gpu: Metal rejected a shader variant: %s", [[err localizedDescription] UTF8String]);
        else fn = [lib newFunctionWithName:@"main0"];
    }
    s.variants[key] = fn;
    return fn;
}

struct PassTargets {
    std::shared_ptr<TextureView> rtv[8];
    std::shared_ptr<TextureView> dsv;
};

/// Ends the current encoder if the render targets differ and opens one on these.
bool BeginPass(const DrawState& st, int* width, int* height) {
    char key[512];
    int n = 0;
    id<MTLTexture> color[8] = {};
    id<MTLTexture> depth = nil;
    for (int i = 0; i < 8; ++i)
        if (st.rtv[i] && st.rtv[i]->texture) color[i] = ViewOf(*st.rtv[i]->texture, st.rtv[i]->desc, true);
    if (st.dsv && st.dsv->texture) depth = ViewOf(*st.dsv->texture, st.dsv->desc, true);
    for (int i = 0; i < 8; ++i) n += std::snprintf(key + n, sizeof key - n, "%p,", (__bridge void*)color[i]);
    n += std::snprintf(key + n, sizeof key - n, "%p", (__bridge void*)depth);
    if (g->encoder != nil && g->pass_key == key) {
        id<MTLTexture> ref = color[0] != nil ? color[0] : depth;
        if (ref) { *width = (int)ref.width; *height = (int)ref.height; }
        return true;
    }
    EndEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    bool any = false;
    for (int i = 0; i < 8; ++i) {
        if (color[i] == nil) continue;
        pass.colorAttachments[i].texture = color[i];
        pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (!any) { *width = (int)color[i].width; *height = (int)color[i].height; }
        any = true;
    }
    if (depth != nil) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (HasStencil(depth.pixelFormat)) {
            pass.stencilAttachment.texture = depth;
            pass.stencilAttachment.loadAction = MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!any) { *width = (int)depth.width; *height = (int)depth.height; }
        any = true;
    }
    if (!any) {
        // No attachments: a pass whose pixel shader writes through UAVs only. Metal wants the raster size then.
        for (int i = 0; i < 64 && !any; ++i) {
            if (st.ps_uav[i] && st.ps_uav[i]->texture) {
                pass.renderTargetWidth = st.ps_uav[i]->texture->mtl.width;
                pass.renderTargetHeight = st.ps_uav[i]->texture->mtl.height;
                pass.defaultRasterSampleCount = 1;
                *width = (int)pass.renderTargetWidth; *height = (int)pass.renderTargetHeight;
                any = true;
            }
        }
        if (!any && st.num_viewports > 0 && st.viewports[0].w > 0 && st.viewports[0].h > 0) {
            pass.renderTargetWidth = (NSUInteger)st.viewports[0].w; pass.renderTargetHeight = (NSUInteger)st.viewports[0].h;
            pass.defaultRasterSampleCount = 1;
            *width = (int)pass.renderTargetWidth; *height = (int)pass.renderTargetHeight;
            any = true;
        }
    }
    if (!any) return false;
    g->encoder = [Command() renderCommandEncoderWithDescriptor:pass];
    g->pass_key = key;
    return true;
}

/// The [[user(...)]] members of one struct in a translated shader (the vertex outputs, or the pixel inputs).
static std::set<std::string> UserNames(const std::string& msl, const char* struct_name) {
    std::set<std::string> names;
    const size_t a = msl.find(std::string("struct ") + struct_name + " {");
    if (a == std::string::npos) return names;
    const size_t b = msl.find("};", a);
    for (size_t p = msl.find("[[user(", a); p != std::string::npos && p < b; p = msl.find("[[user(", p)) {
        p += 7;
        const size_t q = msl.find(')', p);
        names.insert(msl.substr(p, q - p));
    }
    return names;
}

/// The pixel function to pair with `vs`. D3D links a pixel input the vertex shader does not write and the shader reads 0 there;
/// Metal refuses the pipeline ("not written by vertex shader"). Such pairs get a variant of the pixel shader without those inputs.
static id<MTLFunction> FragmentFor(Shader& vs, Shader& ps) {
    if (ps.function == nil) return nil;
    const std::set<std::string> have = UserNames(vs.translated.msl, "VSOut"), want = UserNames(ps.translated.msl, "PSIn");
    std::vector<std::string> missing;
    for (const std::string& n : want) if (have.count(n) == 0) missing.push_back(n);
    if (missing.empty()) return ps.function;
    std::string key = "unwritten";
    for (const std::string& n : missing) key += ":" + n;
    if (auto it = ps.variants.find(key); it != ps.variants.end()) return it->second;
    std::string msl = ps.translated.msl;
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (const std::string& n : missing) {
        if (const size_t at = msl.find("[[user(" + n + ")]]"); at != std::string::npos) {
            const size_t ls = msl.rfind('\n', at), le = msl.find('\n', at);
            if (ls != std::string::npos && le != std::string::npos) msl.erase(ls, le - ls);
        }
        const std::string use = "in." + n;
        for (size_t p = msl.find(use); p != std::string::npos; p = msl.find(use, p)) {
            const size_t end = p + use.size();
            if ((end < msl.size() && ident(msl[end])) || (p > 0 && ident(msl[p - 1]))) { p = end; continue; }
            msl.replace(p, use.size(), "float4(0.0f)");
            p += 12;
        }
    }
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.fastMathEnabled = NO;
    opts.preserveInvariance = YES;
    NSError* err = nil;
    id<MTLLibrary> lib = [g->device newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()] options:opts error:&err];
    id<MTLFunction> fn = lib != nil ? [lib newFunctionWithName:@"main0"] : nil;
    if (fn == nil) UT_ERROR("gpu: pixel shader #%d without %s: %s", ps.serial, key.c_str() + 10, err ? [[err localizedDescription] UTF8String] : "?");
    else {
        static std::atomic<int> shown {0};
        if (shown++ < 12) UT_INFO("gpu: pixel shader #%d with vertex shader #%d: inputs the vertex shader does not write read 0 (%s)", ps.serial, vs.serial, key.c_str() + 10);
    }
    ps.variants[key] = fn;
    return fn;
}

id<MTLRenderPipelineState> PipelineFor(const DrawState& st, Shader& vs, Shader* ps, InputLayout* layout, const uint32_t strides[16]) {
    std::string key;
    key.reserve(512);
    auto add = [&](const void* p, size_t n) { key.append(static_cast<const char*>(p), n); };
    // How the layout feeds integer inputs decides which translation of the vertex shader to use.
    std::vector<uint8_t> classes;
    if (layout != nullptr) {
        for (const SignatureElement& in : vs.translated.reflection.inputs) {
            if (in.system_value != 0) continue;
            for (const InputElement& e : layout->elements) {
                std::string name = e.semantic;
                for (auto& c : name) c = (char)std::toupper((unsigned char)c);
                if (name != in.semantic || e.semantic_index != in.index) continue;
                uint8_t cls = 0;
                switch (e.format) {
                case 3: case 7: case 12: case 17: case 25: case 30: case 36: case 42: case 50: case 57: case 62: cls = 1; break;
                case 4: case 8: case 14: case 18: case 32: case 38: case 43: case 52: case 59: case 64: cls = 2; break;
                }
                if (classes.size() <= in.reg) classes.resize(in.reg + 1, 0);
                classes[in.reg] = cls;
                break;
            }
        }
    }
    add(classes.data(), classes.size());
    const void* vsp = &vs; add(&vsp, 8);
    const void* psp = ps; add(&psp, 8);
    const void* lp = layout; add(&lp, 8);
    key += st.blend.bytes;
    for (int i = 0; i < 8; ++i) {
        MTLPixelFormat f = MTLPixelFormatInvalid;
        if (st.rtv[i] && st.rtv[i]->texture) { id<MTLTexture> t = ViewOf(*st.rtv[i]->texture, st.rtv[i]->desc, true); f = t.pixelFormat; }
        add(&f, sizeof f);
    }
    MTLPixelFormat df = MTLPixelFormatInvalid;
    uint32_t samples = 1;
    if (st.dsv && st.dsv->texture) { df = ViewOf(*st.dsv->texture, st.dsv->desc, true).pixelFormat; samples = (uint32_t)st.dsv->texture->mtl.sampleCount; }
    else if (st.rtv[0] && st.rtv[0]->texture) samples = (uint32_t)st.rtv[0]->texture->mtl.sampleCount;
    add(&df, sizeof df);
    add(&samples, 4);
    add(strides, 16 * 4);
    auto it = g->pipelines.find(key);
    if (it != g->pipelines.end()) return it->second;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = VariantFor(vs, classes);
    if (d.vertexFunction == nil) return nil;
    d.fragmentFunction = ps ? FragmentFor(vs, *ps) : nil;
    d.rasterSampleCount = samples;
    // Vertex layout.
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    std::vector<bool> described(32, false);
    const std::vector<SignatureElement>& vs_in = vs.translated.reflection.inputs;
    if (layout != nullptr) {
        for (const SignatureElement& in : vs_in) {
            if (in.system_value != 0) continue;
            const InputElement* match = nullptr;
            for (const InputElement& e : layout->elements) {
                std::string name = e.semantic;
                for (auto& c : name) c = (char)std::toupper((unsigned char)c);
                if (name == in.semantic && e.semantic_index == in.index) { match = &e; break; }
            }
            MTLVertexAttributeDescriptor* a = vd.attributes[in.reg];
            if (match != nullptr) {
                a.format = VertexFormat(match->format);
                a.offset = match->offset;
                a.bufferIndex = 15 + match->slot;
                MTLVertexBufferLayoutDescriptor* l = vd.layouts[15 + match->slot];
                l.stride = strides[match->slot] != 0 ? strides[match->slot] : 16;
                l.stepFunction = match->slot_class == 1 ? (match->step_rate == 0 ? MTLVertexStepFunctionConstant : MTLVertexStepFunctionPerInstance) : MTLVertexStepFunctionPerVertex;
                if (match->slot_class == 1 && match->step_rate > 1) l.stepRate = match->step_rate;
            } else {
                a.format = MTLVertexFormatFloat4;
                a.offset = 0;
                a.bufferIndex = 14;
                MTLVertexBufferLayoutDescriptor* l = vd.layouts[14];
                l.stride = 16;
                l.stepFunction = MTLVertexStepFunctionConstant;
            }
        }
    }
    d.vertexDescriptor = vd;
    // Colour attachments and blending.
    const bool independent = Rd<uint32_t>(st.blend.bytes, 4) != 0;
    for (int i = 0; i < 8; ++i) {
        if (!st.rtv[i] || !st.rtv[i]->texture) continue;
        id<MTLTexture> t = ViewOf(*st.rtv[i]->texture, st.rtv[i]->desc, true);
        MTLRenderPipelineColorAttachmentDescriptor* a = d.colorAttachments[i];
        a.pixelFormat = t.pixelFormat;
        if (st.blend.empty()) { a.writeMask = MTLColorWriteMaskAll; continue; }
        const size_t rt = 8 + 32 * (independent ? i : 0);
        a.blendingEnabled = Rd<uint32_t>(st.blend.bytes, rt) != 0 && g->debug.find("noblend") == std::string::npos;
        a.sourceRGBBlendFactor = BlendFactor(Rd<uint32_t>(st.blend.bytes, rt + 4));
        a.destinationRGBBlendFactor = BlendFactor(Rd<uint32_t>(st.blend.bytes, rt + 8));
        a.rgbBlendOperation = BlendOp(Rd<uint32_t>(st.blend.bytes, rt + 12));
        a.sourceAlphaBlendFactor = BlendFactor(Rd<uint32_t>(st.blend.bytes, rt + 16));
        a.destinationAlphaBlendFactor = BlendFactor(Rd<uint32_t>(st.blend.bytes, rt + 20));
        a.alphaBlendOperation = BlendOp(Rd<uint32_t>(st.blend.bytes, rt + 24));
        const uint8_t mask = Rd<uint8_t>(st.blend.bytes, rt + 28, 0xF);
        MTLColorWriteMask wm = 0;
        if (mask & 1) wm |= MTLColorWriteMaskRed;
        if (mask & 2) wm |= MTLColorWriteMaskGreen;
        if (mask & 4) wm |= MTLColorWriteMaskBlue;
        if (mask & 8) wm |= MTLColorWriteMaskAlpha;
        a.writeMask = wm;
    }
    if (df != MTLPixelFormatInvalid) {
        d.depthAttachmentPixelFormat = df;
        if (HasStencil(df)) d.stencilAttachmentPixelFormat = df;
    }
    if (!st.blend.empty() && Rd<uint32_t>(st.blend.bytes, 0) != 0 && g->debug.find("noa2c") == std::string::npos) d.alphaToCoverageEnabled = YES;
    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [g->device newRenderPipelineStateWithDescriptor:d error:&err];
    if (pso == nil) UT_ERROR("gpu: pipeline creation failed: %s", [[err localizedDescription] UTF8String]);
    g->pipelines[key] = pso;
    return pso;
}

MTLPrimitiveType Primitive(uint32_t topology, bool* ok) {
    *ok = true;
    switch (topology) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: *ok = false; return MTLPrimitiveTypeTriangle;
    }
}

// ---- the ring ---------------------------------------------------------------------------------

/// Gives `b` fresh storage in the current ring slot.
uint8_t* RingAlloc(const std::shared_ptr<Buffer>& b, uint32_t size) {
    std::scoped_lock lock {g->ring_mutex};
    Ring& r = g->ring[g->frame];
    const size_t aligned = (size + 255) & ~size_t(255);
    if (r.head + aligned > r.capacity) return nullptr;
    b->mtl = r.buffer;
    b->offset = (uint32_t)r.head;
    b->ring_frame = g->frame;
    r.head += aligned;
    r.live.push_back(b);
    return static_cast<uint8_t*>(r.buffer.contents) + b->offset;
}

/// Copies buffers still living in ring slot `f` out to storage of their own.
void EvictRing(int f) {
    Ring& r = g->ring[f];
    for (auto& w : r.live) {
        auto b = w.lock();
        if (!b || b->ring_frame != f) continue;
        id<MTLBuffer> own = NewBuffer(b->size);
        std::memcpy(own.contents, static_cast<uint8_t*>(b->mtl.contents) + b->offset, b->size);
        b->mtl = own;
        b->offset = 0;
        b->ring_frame = -1;
    }
    r.live.clear();
    r.head = 0;
}

} // namespace

// ================================================================================================
// Public API
// ================================================================================================

bool Init() {
    if (g != nullptr) return g->ready;
    g = new Globals();
    @autoreleasepool {
        g->device = MTLCreateSystemDefaultDevice();
        if (g->device == nil) { UT_ERROR("gpu: no Metal device"); return false; }
        g->queue = [g->device newCommandQueue];
        for (auto& r : g->ring) { r.buffer = NewBuffer(kRingBytes); r.capacity = kRingBytes; }
        g->frames_in_flight = dispatch_semaphore_create(kRingFrames);
        g->null_buffer = NewBuffer(256);
        std::memset(g->null_buffer.contents, 0, 256);
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = NO;
        g->default_sampler = [g->device newSamplerStateWithDescriptor:sd];
        g->last_present = std::chrono::steady_clock::now();
        g->dump_dir = std::getenv("UTSURO_DUMPFRAMES");
        if (const char* d = std::getenv("UTSURO_GPUDEBUG")) g->debug = d;
        if (const char* d = std::getenv("UTSURO_TRACEFRAME")) g->trace_frame = std::strtoull(d, nullptr, 10);
        if (const char* d = std::getenv("UTSURO_TRACECAP")) g->trace_cap = static_cast<uint32_t>(std::strtoul(d, nullptr, 10));
        if (const char* e = std::getenv("UTSURO_DUMPEVERY")) g->dump_every = (uint32_t)std::max(1, std::atoi(e));
    }
    g->ready = true;
    UT_INFO("gpu: Metal device %s", [[g->device name] UTF8String]);
    return true;
}

bool Available() { return g != nullptr && g->ready; }

std::shared_ptr<Buffer> CreateBuffer(uint32_t size, bool dynamic, const void* initial) {
    // No renderer lock: nothing shared is touched, and deferred contexts on other threads create buffers while draws are encoded.
    auto b = std::make_shared<Buffer>();
    b->size = size;
    b->dynamic = dynamic;
    b->mtl = NewBuffer(size);
    if (initial != nullptr) std::memcpy(b->mtl.contents, initial, size);
    else std::memset(b->mtl.contents, 0, size);
    return b;
}

/// A buffer that lives for this frame only (descriptor copies, per-draw constants): a slice of the frame's ring, or a buffer
/// of its own when the ring is full.
std::shared_ptr<Buffer> CreateTransientBuffer(uint32_t size, const void* initial) {
    auto b = std::make_shared<Buffer>();
    b->size = size;
    b->dynamic = false;
    if (uint8_t* at = RingAlloc(b, size)) {
        std::memcpy(at, initial, size);
        return b;
    }
    b->mtl = NewBuffer(size);
    std::memcpy(b->mtl.contents, initial, size);
    return b;
}

uint8_t* BufferData(Buffer& b) { return static_cast<uint8_t*>(b.mtl.contents) + b.offset; }
uint32_t BufferSize(Buffer& b) { return b.size; }

uint8_t* RenameBuffer(const std::shared_ptr<Buffer>& bp, bool keep) {
    std::scoped_lock lock {g->mutex};
    Buffer& b = *bp;
    uint8_t* old = BufferData(b);
    const uint32_t size = b.size;
    if (uint8_t* fresh = RingAlloc(bp, size)) {
        if (keep) std::memcpy(fresh, old, size);
        return fresh;
    }
    // Ring exhausted: storage of its own.
    id<MTLBuffer> own = NewBuffer(size);
    if (keep) std::memcpy(own.contents, old, size);
    b.mtl = own;
    b.offset = 0;
    b.ring_frame = -1;
    return static_cast<uint8_t*>(own.contents);
}

std::shared_ptr<Texture> CreateTexture(const TextureDesc& desc) {
    std::scoped_lock lock {g->mutex};
    auto t = std::make_shared<Texture>();
    t->desc = desc;
    t->pixel_format = PixelFormat(desc.format);
    if (t->pixel_format == MTLPixelFormatInvalid) {
        UT_WARN("gpu: unsupported texture format %u, using RGBA8", desc.format);
        t->pixel_format = MTLPixelFormatRGBA8Unorm;
    }
    // A typeless 32/16-bit surface bound as a depth buffer is a depth texture; its SRVs read the depth.
    if (desc.depth_stencil) {
        if (t->pixel_format == MTLPixelFormatR32Float) t->pixel_format = MTLPixelFormatDepth32Float;
        else if (t->pixel_format == MTLPixelFormatR16Unorm) t->pixel_format = MTLPixelFormatDepth16Unorm;
    }
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.pixelFormat = t->pixel_format;
    d.width = desc.width;
    d.height = desc.height;
    d.storageMode = MTLStorageModeShared;
    d.mipmapLevelCount = std::max<uint32_t>(1, desc.mips);
    d.sampleCount = std::max<uint32_t>(1, desc.samples);
    // Not every GPU does 8x (or any) multisampling: take the largest count the device has.
    while (d.sampleCount > 1 && ![g->device supportsTextureSampleCount:d.sampleCount]) d.sampleCount >>= 1;
    d.usage = MTLTextureUsagePixelFormatView | MTLTextureUsageShaderRead;
    if (desc.render_target || desc.depth_stencil) d.usage |= MTLTextureUsageRenderTarget;
    if (!desc.depth_stencil) d.usage |= MTLTextureUsageShaderWrite;
    if (desc.depth > 1) {
        d.textureType = MTLTextureType3D;
        d.depth = desc.depth;
    } else if (desc.cube) {
        d.textureType = desc.array_size > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
        if (desc.array_size > 6) d.arrayLength = desc.array_size / 6;
    } else if (desc.array_size > 1) {
        d.textureType = d.sampleCount > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        d.arrayLength = desc.array_size;
    } else {
        d.textureType = d.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    }
    if (d.sampleCount > 1) d.mipmapLevelCount = 1;
#if TARGET_OS_IPHONE
    // A multisample texture cannot be shared storage on an Apple GPU in a phone; nothing reads one from the CPU (a resolve target does).
    if (d.sampleCount > 1) d.storageMode = MTLStorageModePrivate;
#endif
    t->mtl = [g->device newTextureWithDescriptor:d];
    if (t->mtl == nil) UT_ERROR("gpu: cannot create a %ux%u texture of format %u", desc.width, desc.height, desc.format);
    return t;
}

namespace {
uint32_t BlockSize(MTLPixelFormat f, bool* compressed) {
    *compressed = true;
    switch (f) {
    case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC4_RUnorm: case MTLPixelFormatBC4_RSnorm: return 8;
    case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm: case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat:
    case MTLPixelFormatBC7_RGBAUnorm: case MTLPixelFormatBC7_RGBAUnorm_sRGB: return 16;
    default: *compressed = false; return 0;
    }
}
} // namespace

void SetMinMip(Texture& t, uint32_t mip) {
    std::scoped_lock lock {g->mutex};
    t.min_mip = mip;
}

void UploadTexture(Texture& t, uint32_t mip, uint32_t slice, const uint32_t box[6], const void* data, uint32_t row_pitch, uint32_t slice_pitch) {
    std::scoped_lock lock {g->mutex};
    if (t.mtl == nil || data == nullptr) return;
    ++t.uploads;
    if (IsDepth(t.pixel_format)) ++t.depth_gen;
    const uint32_t mw = std::max<uint32_t>(1, (uint32_t)t.mtl.width >> mip), mh = std::max<uint32_t>(1, (uint32_t)t.mtl.height >> mip);
    const uint32_t md = std::max<uint32_t>(1, (uint32_t)t.mtl.depth >> mip);
    uint32_t l = 0, tp = 0, f = 0, r = mw, b = mh, bk = md;
    if (box != nullptr) { l = box[0]; tp = box[1]; f = box[2]; r = box[3]; b = box[4]; bk = box[5]; }
    bool compressed;
    BlockSize(t.pixel_format, &compressed);
    // Compressed regions are block aligned in D3D; Metal wants the region in texels clamped to the mip size.
    MTLRegion region;
    if (t.mtl.textureType == MTLTextureType3D) region = MTLRegionMake3D(l, tp, f, r - l, b - tp, bk - f);
    else if (t.mtl.textureType == MTLTextureType1D || t.mtl.textureType == MTLTextureType1DArray) region = MTLRegionMake1D(l, r - l);
    else region = MTLRegionMake2D(l, tp, r - l, b - tp);
    if (compressed) {
        region.size.width = std::min<NSUInteger>(((region.size.width + 3) / 4) * 4, std::max<NSUInteger>(mw - l, 1));
        region.size.height = std::min<NSUInteger>(((region.size.height + 3) / 4) * 4, std::max<NSUInteger>(mh - tp, 1));
        if (mw < 4 || mh < 4) { region.size.width = mw; region.size.height = mh; }
    }
    NSUInteger arr_slice = slice;
    if (t.mtl.textureType != MTLTextureType3D && t.mtl.textureType != MTLTextureTypeCube && t.mtl.textureType != MTLTextureTypeCubeArray && t.mtl.textureType != MTLTextureType2DArray && t.mtl.textureType != MTLTextureType1DArray) arr_slice = 0;
    [t.mtl replaceRegion:region mipmapLevel:mip slice:arr_slice withBytes:data bytesPerRow:row_pitch bytesPerImage:slice_pitch];
}

bool ReadbackTexture(Texture& t, uint32_t mip, uint32_t slice, void* dst, uint32_t row_pitch) {
    std::scoped_lock lock {g->mutex};
    if (t.mtl == nil) return false;
    EndEncoder();
    if (g->command != nil) { [g->command commit]; [g->command waitUntilCompleted]; g->command = nil; }
    const uint32_t mw = std::max<uint32_t>(1, (uint32_t)t.mtl.width >> mip), mh = std::max<uint32_t>(1, (uint32_t)t.mtl.height >> mip);
    [t.mtl getBytes:dst bytesPerRow:row_pitch bytesPerImage:row_pitch * mh fromRegion:MTLRegionMake2D(0, 0, mw, mh) mipmapLevel:mip slice:slice];
    return true;
}

void CopyTexture(Texture& dst, uint32_t dmip, uint32_t dslice, uint32_t dx, uint32_t dy, uint32_t dz, Texture& src, uint32_t smip, uint32_t sslice, const uint32_t box[6]) {
    if (dst.mtl == nil || src.mtl == nil) return;
    if (smip >= src.mtl.mipmapLevelCount || dmip >= dst.mtl.mipmapLevelCount || src.mtl.sampleCount != dst.mtl.sampleCount) return;
    if (src.mtl.pixelFormat != dst.mtl.pixelFormat) {
        // A blit needs one format on both sides. Depth into a colour or another depth format goes through the shader copy.
        if (IsDepth(src.pixel_format) || IsDepth(dst.pixel_format)) {
            const uint32_t rect[4] = {box ? box[0] : 0, box ? box[1] : 0, box ? box[3] : ~0u, box ? box[4] : ~0u};
            DecompressCopy(dst, dmip, dslice, dx, dy, src, smip, sslice, rect);
        } else if (static std::atomic<int> shown {0}; shown++ < 8) {
            UT_INFO("gpu: texture copy between formats %lu -> %lu skipped", (unsigned long)src.mtl.pixelFormat, (unsigned long)dst.mtl.pixelFormat);
        }
        return;
    }
    const uint32_t mw = std::max<uint32_t>(1, (uint32_t)src.mtl.width >> smip), mh = std::max<uint32_t>(1, (uint32_t)src.mtl.height >> smip);
    const uint32_t md = src.mtl.textureType == MTLTextureType3D ? std::max<uint32_t>(1, (uint32_t)src.mtl.depth >> smip) : 1;
    const uint32_t dw = std::max<uint32_t>(1, (uint32_t)dst.mtl.width >> dmip), dh = std::max<uint32_t>(1, (uint32_t)dst.mtl.height >> dmip);
    const uint32_t ddp = dst.mtl.textureType == MTLTextureType3D ? std::max<uint32_t>(1, (uint32_t)dst.mtl.depth >> dmip) : 1;
    uint32_t l = 0, t = 0, f = 0, r = mw, b = mh, bk = md;
    if (box != nullptr) { l = box[0]; t = box[1]; f = box[2]; r = std::min(box[3], mw); b = std::min(box[4], mh); bk = std::min(box[5], md); }
    if (r <= l || b <= t || bk <= f || dx >= dw || dy >= dh || dz >= ddp) return;
    const uint32_t w = std::min(r - l, dw - dx), h = std::min(b - t, dh - dy), d = std::min(bk - f, ddp - dz);
    std::scoped_lock lock {g->mutex};
    EndEncoder();
    id<MTLBlitCommandEncoder> blit = [Command() blitCommandEncoder];
    [blit copyFromTexture:src.mtl sourceSlice:sslice sourceLevel:smip sourceOrigin:MTLOriginMake(l, t, f) sourceSize:MTLSizeMake(w, h, d)
                toTexture:dst.mtl destinationSlice:dslice destinationLevel:dmip destinationOrigin:MTLOriginMake(dx, dy, dz)];
    [blit endEncoding];
    if (IsDepth(dst.pixel_format)) ++dst.depth_gen;
}

void ResolveTexture(Texture& dst, uint32_t dmip, uint32_t dslice, Texture& src, uint32_t smip, uint32_t sslice) {
    if (dst.mtl == nil || src.mtl == nil) return;
    if (src.mtl.sampleCount <= 1) { CopyTexture(dst, dmip, dslice, 0, 0, 0, src, smip, sslice, nullptr); return; }
    if (src.mtl.pixelFormat != dst.mtl.pixelFormat || dst.mtl.sampleCount != 1) {
        static std::atomic<int> shown {0};
        if (shown++ < 4) UT_INFO("gpu: resolve %lu -> %lu (samples %lu -> %lu) not supported", (unsigned long)src.mtl.pixelFormat, (unsigned long)dst.mtl.pixelFormat,
                                 (unsigned long)src.mtl.sampleCount, (unsigned long)dst.mtl.sampleCount);
        return;
    }
    std::scoped_lock lock {g->mutex};
    EndEncoder();
    @autoreleasepool {
        // An empty render pass over the multisampled texture whose store action resolves it into the destination.
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        const bool depth = IsDepth(src.pixel_format);
        MTLRenderPassAttachmentDescriptor* a = depth ? static_cast<MTLRenderPassAttachmentDescriptor*>(pass.depthAttachment)
                                                     : static_cast<MTLRenderPassAttachmentDescriptor*>(pass.colorAttachments[0]);
        a.texture = src.mtl;
        a.level = smip;
        a.slice = sslice;
        a.loadAction = MTLLoadActionLoad;
        a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        a.resolveTexture = dst.mtl;
        a.resolveLevel = dmip;
        a.resolveSlice = dslice;
        if (depth) pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        if (depth) pass.renderTargetWidth = src.mtl.width, pass.renderTargetHeight = src.mtl.height;
        id<MTLRenderCommandEncoder> enc = [Command() renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];
    }
    if (IsDepth(dst.pixel_format)) ++dst.depth_gen;
}

void DecompressCopy(Texture& dst, uint32_t dmip, uint32_t dslice, uint32_t dx, uint32_t dy, Texture& src, uint32_t smip, uint32_t sslice, const uint32_t rect[4]) {
    if (dst.mtl == nil || src.mtl == nil) return;
    const uint32_t sw = std::max<uint32_t>(1, (uint32_t)src.mtl.width >> smip), sh = std::max<uint32_t>(1, (uint32_t)src.mtl.height >> smip);
    uint32_t l = 0, t = 0, r = sw, b = sh;
    if (rect != nullptr) { l = rect[0]; t = rect[1]; r = std::min(rect[2], sw); b = std::min(rect[3], sh); }
    if (r <= l || b <= t) return;
    if (src.mtl == dst.mtl && smip == dmip && sslice == dslice) { if (IsDepth(dst.pixel_format)) ++dst.depth_gen; return; }   // in place: nothing to do here
    const bool sd = IsDepth(src.pixel_format), dd = IsDepth(dst.pixel_format);
    if (src.mtl.pixelFormat == dst.mtl.pixelFormat && src.mtl.sampleCount == dst.mtl.sampleCount) {
        const uint32_t box[6] = {l, t, 0, r, b, 1};
        CopyTexture(dst, dmip, dslice, dx, dy, 0, src, smip, sslice, box);
        return;
    }
    std::scoped_lock lock {g->mutex};
    EndEncoder();
    @autoreleasepool {
        // A full-screen triangle over the destination rectangle that reads the source texel for texel: depth -> depth writes
        // [[depth(any)]], anything else writes colour.
        static std::map<std::string, id<MTLRenderPipelineState>> pipelines;
        const std::string key = std::to_string((unsigned long)dst.mtl.pixelFormat) + (sd ? "d" : "c");
        id<MTLRenderPipelineState> pso = pipelines[key];
        if (pso == nil) {
            std::string fs = "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 pos [[position]]; };\n"
                             "vertex V vmain(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); return o; }\n"
                             "struct Off { int2 d; };\n";
            const std::string read = sd ? "depth2d<float> t [[texture(0)]]" : "texture2d<float> t [[texture(0)]]";
            const std::string val = sd ? "t.read(uint2(int2(in.pos.xy) + o.d), lod)" : "t.read(uint2(int2(in.pos.xy) + o.d), lod)";
            if (dd) fs += "struct F { float d [[depth(any)]]; };\nfragment F fmain(V in [[stage_in]], " + read + ", constant Off& o [[buffer(0)]], constant uint& lod [[buffer(1)]]) { F f; f.d = float(" + val + (sd ? "" : ".r") + "); return f; }\n";
            else fs += "fragment float4 fmain(V in [[stage_in]], " + read + ", constant Off& o [[buffer(0)]], constant uint& lod [[buffer(1)]]) { float4 v = float4(" + val + "); return " + (sd ? "float4(v.x, v.x, v.x, 1)" : "v") + "; }\n";
            NSError* err = nil;
            id<MTLLibrary> lib = [g->device newLibraryWithSource:[NSString stringWithUTF8String:fs.c_str()] options:nil error:&err];
            if (lib == nil) { UT_ERROR("gpu: decompress copy shader: %s", [[err localizedDescription] UTF8String]); return; }
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
            pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
            if (dd) { pd.depthAttachmentPixelFormat = dst.mtl.pixelFormat; if (HasStencil(dst.mtl.pixelFormat)) pd.stencilAttachmentPixelFormat = dst.mtl.pixelFormat; }
            else pd.colorAttachments[0].pixelFormat = dst.mtl.pixelFormat;
            pso = [g->device newRenderPipelineStateWithDescriptor:pd error:&err];
            if (pso == nil) { UT_ERROR("gpu: decompress copy pipeline: %s", [[err localizedDescription] UTF8String]); return; }
            pipelines[key] = pso;
        }
        MTLTextureDescriptor* vd = nil; (void)vd;
        id<MTLTexture> target = dst.mtl;
        if (dst.mtl.mipmapLevelCount > 1 || dst.mtl.arrayLength > 1)
            target = [dst.mtl newTextureViewWithPixelFormat:dst.mtl.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(dmip, 1) slices:NSMakeRange(dslice, 1)];
        id<MTLTexture> source = src.mtl;
        if (src.mtl.arrayLength > 1)
            source = [src.mtl newTextureViewWithPixelFormat:src.mtl.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, src.mtl.mipmapLevelCount) slices:NSMakeRange(sslice, 1)];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        if (dd) {
            pass.depthAttachment.texture = target;
            pass.depthAttachment.loadAction = MTLLoadActionLoad;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            if (HasStencil(target.pixelFormat)) { pass.stencilAttachment.texture = target; pass.stencilAttachment.loadAction = MTLLoadActionLoad; pass.stencilAttachment.storeAction = MTLStoreActionStore; }
        } else {
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> e = [Command() renderCommandEncoderWithDescriptor:pass];
        [e setRenderPipelineState:pso];
        if (dd) {
            static id<MTLDepthStencilState> always = nil;
            if (always == nil) { MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = YES; always = [g->device newDepthStencilStateWithDescriptor:d]; }
            [e setDepthStencilState:always];
        }
        MTLViewport vp {double(dx), double(dy), double(r - l), double(b - t), 0, 1};
        [e setViewport:vp];
        [e setScissorRect:MTLScissorRect {dx, dy, std::min<NSUInteger>(r - l, target.width - dx), std::min<NSUInteger>(b - t, target.height - dy)}];
        const int32_t off[2] = {int32_t(l) - int32_t(dx), int32_t(t) - int32_t(dy)};
        [e setFragmentBytes:off length:sizeof off atIndex:0];
        [e setFragmentBytes:&smip length:sizeof smip atIndex:1];
        [e setFragmentTexture:source atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    if (dd) ++dst.depth_gen;
}

const TextureDesc& TextureInfo(const Texture& t) { return t.desc; }
void* NativeOf(const Texture& t) { return (__bridge void*)t.mtl; }
const ShaderReflection& Reflection(const Shader& s) { return s.translated.reflection; }

void GenerateMips(Texture& t) {
    std::scoped_lock lock {g->mutex};
    if (t.mtl == nil || t.mtl.mipmapLevelCount < 2) return;
    EndEncoder();
    id<MTLBlitCommandEncoder> blit = [Command() blitCommandEncoder];
    [blit generateMipmapsForTexture:t.mtl];
    [blit endEncoding];
}

std::shared_ptr<Shader> CreateShader(const uint8_t* dxbc, size_t size, ShaderStage stage) {
    auto s = std::make_shared<Shader>();
    s->stage = stage;
    s->dxbc.assign(dxbc, dxbc + size);
    s->translated = TranslateDxbc(dxbc, size);
    if (!s->translated.ok) {
        UT_ERROR("gpu: cannot translate a shader: %s", s->translated.error.c_str());
        s->failed = true;
    }
    return s;
}

std::shared_ptr<InputLayout> CreateInputLayout(const std::vector<InputElement>& elements, const uint8_t* vs_dxbc, size_t size) {
    auto l = std::make_shared<InputLayout>();
    l->elements = elements;
    // Resolve APPEND_ALIGNED offsets.
    uint32_t next[16] = {};
    for (auto& e : l->elements) {
        if (e.slot < 16) {
            if (e.offset == 0xFFFFFFFFu) e.offset = next[e.slot];
            uint32_t bytes = 16;
            switch (e.format) {
            case 2: case 3: case 4: bytes = 16; break;
            case 6: case 7: case 8: bytes = 12; break;
            case 16: case 17: case 18: case 10: case 11: case 12: case 13: case 14: bytes = 8; break;
            case 41: case 42: case 43: case 28: case 30: case 31: case 32: case 34: case 35: case 36: case 37: case 38: case 24: case 87: bytes = 4; break;
            case 49: case 50: case 51: case 52: case 54: case 56: case 57: case 58: case 59: bytes = 2; break;
            case 61: case 62: case 63: case 64: bytes = 1; break;
            }
            next[e.slot] = e.offset + bytes;
        }
    }
    (void)vs_dxbc; (void)size;
    return l;
}

/// D3D shaders sample a depth buffer's SRV as plain floats; Metal only allows that through a depth2d. Keeps a float copy of `t`
/// (rebuilt whenever the depth texture was written since) and returns it.
id<MTLTexture> DepthAsFloat(Texture& t) {
    if (t.mtl == nil || t.mtl.textureType != MTLTextureType2D) return nil;
    if (t.depth_copy == nil || t.depth_copy.width != t.mtl.width || t.depth_copy.height != t.mtl.height) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:t.mtl.width height:t.mtl.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        t.depth_copy = [g->device newTextureWithDescriptor:d];
        t.copy_gen = ~0ull;
    }
    if (t.copy_gen == t.depth_gen) return t.depth_copy;
    {
        static std::mutex m; static std::vector<void*> seen;
        std::scoped_lock lock {m};
        if (std::find(seen.begin(), seen.end(), (__bridge void*)t.mtl) == seen.end() && seen.size() < 32) {
            seen.push_back((__bridge void*)t.mtl);
            UT_INFO("gpu: depth copy source %p: %lux%lu format %lu type %lu samples %lu usage %#lx storage %lu mips %lu", (__bridge void*)t.mtl, (unsigned long)t.mtl.width, (unsigned long)t.mtl.height,
                    (unsigned long)t.mtl.pixelFormat, (unsigned long)t.mtl.textureType, (unsigned long)t.mtl.sampleCount, (unsigned long)t.mtl.usage, (unsigned long)t.mtl.storageMode, (unsigned long)t.mtl.mipmapLevelCount);
        }
    }
    if (g->depth_copy_pipeline == nil) {
        NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 pos [[position]]; float2 uv; };\n"
            "vertex V vmain(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
            "fragment float4 fmain(V in [[stage_in]], depth2d<float> t [[texture(0)]]) { constexpr sampler s(filter::nearest, address::clamp_to_edge); float d = t.sample(s, in.uv); return float4(d, d, d, d); }\n";
        NSError* err = nil;
        id<MTLLibrary> lib = [g->device newLibraryWithSource:src options:nil error:&err];
        if (lib == nil) { UT_ERROR("gpu: depth copy shader: %s", [[err localizedDescription] UTF8String]); return nil; }
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
        g->depth_copy_pipeline = [g->device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (g->depth_copy_pipeline == nil) { UT_ERROR("gpu: depth copy pipeline: %s", [[err localizedDescription] UTF8String]); return nil; }
    }
    EndEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = t.depth_copy;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [Command() renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:g->depth_copy_pipeline];
    [e setFragmentTexture:t.mtl atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    t.copy_gen = t.depth_gen;
    return t.depth_copy;
}

void Draw(const DrawState& st, const DrawArgs& args) {
    std::scoped_lock lock {g->mutex};
    if (!st.vs) { ++g->stats.no_vs; return; }
    if (!st.vs->translated.ok || !CompileShader(*st.vs)) { ++g->stats.bad_vs; return; }
    Shader* ps = st.ps && st.ps->translated.ok ? st.ps.get() : nullptr;
    if (ps != nullptr && !CompileShader(*ps)) { ++g->stats.bad_ps; return; }
    @autoreleasepool {
        for (int stage = 0; stage < 2; ++stage) {
            for (int i = 0; i < 16; ++i)
                if (st.srv[stage][i] && st.srv[stage][i]->texture && IsDepth(st.srv[stage][i]->texture->pixel_format)) {
                    Texture* dt = st.srv[stage][i]->texture.get();
                    DepthAsFloat(*dt);
                    if (g->frame_count == g->trace_frame && std::find(g->trace_depths.begin(), g->trace_depths.end(), dt) == g->trace_depths.end()) g->trace_depths.push_back(dt);
                }
            for (const auto& [slot, view] : st.extra_srv[stage])
                if (view && view->texture && IsDepth(view->texture->pixel_format)) DepthAsFloat(*view->texture);
        }
        int width = 0, height = 0;
        if (!BeginPass(st, &width, &height)) { ++g->stats.no_pass; return; }
        if (static const bool census = std::getenv("UTSURO_RTCENSUS") != nullptr; census && st.rtv[0] && st.rtv[0]->texture) {
            char key[96];
            std::snprintf(key, sizeof key, "%p %lux%lu fmt %lu", (__bridge void*)st.rtv[0]->texture->mtl, (unsigned long)st.rtv[0]->texture->mtl.width, (unsigned long)st.rtv[0]->texture->mtl.height, (unsigned long)st.rtv[0]->texture->pixel_format);
            ++g->rt_census[key];
        }
        if (static const bool census = std::getenv("UTSURO_RTCENSUS") != nullptr; census && st.dsv && st.dsv->texture) {
            char key[96];
            std::snprintf(key, sizeof key, "dsv %p %lux%lu", (__bridge void*)st.dsv->texture->mtl, (unsigned long)st.dsv->texture->mtl.width, (unsigned long)st.dsv->texture->mtl.height);
            ++g->rt_census[key];
        }
        if (static const bool census = std::getenv("UTSURO_RTCENSUS") != nullptr; census && st.rtv[0] && st.rtv[0]->texture) {
            char key[96];
            std::snprintf(key, sizeof key, "%p %lux%lu fmt %lu", (__bridge void*)st.rtv[0]->texture->mtl, (unsigned long)st.rtv[0]->texture->mtl.width, (unsigned long)st.rtv[0]->texture->mtl.height, (unsigned long)st.rtv[0]->texture->pixel_format);
            ++g->rt_census[key];
        }
        uint32_t strides[16] = {};
        for (int i = 0; i < 16; ++i) strides[i] = st.vb[i].stride;
        id<MTLRenderPipelineState> pso = PipelineFor(st, *st.vs, ps, st.layout.get(), strides);
        if (pso == nil) { ++g->stats.no_pso; return; }
        id<MTLRenderCommandEncoder> e = g->encoder;
        [e setRenderPipelineState:pso];
        if (st.dsv && st.dsv->texture && (st.depth_stencil.bytes.size() < 8 || (Rd<uint32_t>(st.depth_stencil.bytes, 0) != 0 && Rd<uint32_t>(st.depth_stencil.bytes, 4) != 0))) ++st.dsv->texture->depth_gen;   // this draw may write depth

        // Fixed-function state.
        bool has_depth = st.dsv && st.dsv->texture;
        bool has_stencil = has_depth && HasStencil(st.dsv->texture->pixel_format);
        if (g->debug.find("nodepth") != std::string::npos) { StateBlob none; [e setDepthStencilState:DepthStateFor(none, false, false)]; }
        else [e setDepthStencilState:DepthStateFor(st.depth_stencil, has_depth, has_stencil)];
        [e setStencilReferenceValue:st.stencil_ref];
        const uint32_t cull = st.raster.empty() ? 3 : Rd<uint32_t>(st.raster.bytes, 4);
        [e setCullMode:(cull == 1 || g->debug.find("nocull") != std::string::npos) ? MTLCullModeNone : cull == 2 ? MTLCullModeFront : MTLCullModeBack];
        [e setFrontFacingWinding:(!st.raster.empty() && Rd<uint32_t>(st.raster.bytes, 8) != 0) ? MTLWindingCounterClockwise : MTLWindingClockwise];
        [e setTriangleFillMode:(!st.raster.empty() && Rd<uint32_t>(st.raster.bytes, 0) == 2) ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        if (!st.raster.empty()) {
            [e setDepthBias:(float)Rd<int32_t>(st.raster.bytes, 12) slopeScale:Rd<float>(st.raster.bytes, 20) clamp:Rd<float>(st.raster.bytes, 16)];
            [e setDepthClipMode:Rd<uint32_t>(st.raster.bytes, 24) != 0 ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
        } else {
            [e setDepthBias:0 slopeScale:0 clamp:0];
            [e setDepthClipMode:MTLDepthClipModeClip];
        }
        [e setBlendColorRed:st.blend_factor[0] green:st.blend_factor[1] blue:st.blend_factor[2] alpha:st.blend_factor[3]];
        MTLViewport vp;
        if (st.num_viewports > 0 && st.viewports[0].w > 0.0f && st.viewports[0].h > 0.0f) {
            vp = {st.viewports[0].x, st.viewports[0].y, st.viewports[0].w, st.viewports[0].h, st.viewports[0].min_depth, st.viewports[0].max_depth};
        } else {
            vp = {0, 0, (double)width, (double)height, 0, 1};
        }
        [e setViewport:vp];
        MTLScissorRect sc = {0, 0, (NSUInteger)width, (NSUInteger)height};
        const bool scissor_on = !st.raster.empty() && Rd<uint32_t>(st.raster.bytes, 28) != 0;
        if (scissor_on && st.num_scissors > 0) {
            const ScissorRect& r = st.scissors[0];
            const int l = std::max(0, std::min(r.left, width)), t = std::max(0, std::min(r.top, height));
            const int rr = std::max(l, std::min(r.right, width)), bb = std::max(t, std::min(r.bottom, height));
            sc = {(NSUInteger)l, (NSUInteger)t, (NSUInteger)(rr - l), (NSUInteger)(bb - t)};
        }
        [e setScissorRect:sc];

        // Resources: constant buffers at 0..13, textures and samplers at their register numbers.
        for (int stage = 0; stage < 2; ++stage) {
            Shader* sh = stage == 0 ? st.vs.get() : ps;
            if (sh == nullptr) continue;
            const ShaderReflection& rf = sh->translated.reflection;
            for (int i = 0; i < 16; ++i) {
                if (!(rf.constant_buffers & (1u << i))) continue;
                const BufferBinding& b = st.cb[stage][i];
                id<MTLBuffer> buf = b.buffer ? b.buffer->mtl : g->zero_cb;
                NSUInteger off = b.buffer ? b.buffer->offset + b.offset : 0;
                if (buf == nil) { if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); } buf = g->zero_cb; off = 0; }
                if (stage == 0) [e setVertexBuffer:buf offset:off atIndex:i];
                else [e setFragmentBuffer:buf offset:off atIndex:i];
            }
            if (stage == 1 && rf.uavs != 0) {
                // Pixel-shader UAVs: textures at 64 + slot, buffers at their pooled index.
                if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); }
                for (int i = 0; i < 64; ++i) {
                    if (!(rf.uavs & (1u << i))) continue;
                    const auto& v = st.ps_uav[i];
                    if (rf.uav_index[i] != 0) [e setFragmentBuffer:(v && v->buffer) ? v->buffer->mtl : g->zero_cb offset:(v && v->buffer) ? v->buffer->offset : 0 atIndex:rf.uav_index[i]];
                    else if (v && v->texture) [e setFragmentTexture:ViewOf(*v->texture, v->desc, false) atIndex:64 + i];
                    else [e setFragmentTexture:DummyTexture(3) atIndex:64 + i];
                    if (rf.counter_index[i] != 0) {
                        const bool have = v && v->counter && v->counter->mtl != nil;
                        [e setFragmentBuffer:have ? v->counter->mtl : ScratchCounter() offset:have ? v->counter->offset : 0 atIndex:rf.counter_index[i]];
                    }
                }
            }
            for (int i = 0; i < 16; ++i) {
                if (!(rf.textures & (1u << i))) continue;
                if (rf.texture_dimension[i] == 12) {
                    id<MTLBuffer> buf = (st.srv[stage][i] && st.srv[stage][i]->buffer) ? st.srv[stage][i]->buffer->mtl : g->zero_cb;
                    NSUInteger off = (st.srv[stage][i] && st.srv[stage][i]->buffer) ? st.srv[stage][i]->buffer->offset : 0;
                    if (buf == nil) { if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); } buf = g->zero_cb; off = 0; }
                    const uint32_t buf_idx = rf.buffer_index[i] != 0 ? rf.buffer_index[i] : ((stage == 0) ? (24 + i) : (14 + i));
                    if (stage == 0) [e setVertexBuffer:buf offset:off atIndex:buf_idx];
                    else [e setFragmentBuffer:buf offset:off atIndex:buf_idx];
                    continue;
                }
                id<MTLTexture> tex = nil;
                if (st.srv[stage][i] && st.srv[stage][i]->texture) {
                    Texture& tx = *st.srv[stage][i]->texture;
                    tex = IsDepth(tx.pixel_format) && tx.depth_copy != nil ? tx.depth_copy : ViewOf(tx, st.srv[stage][i]->desc, false);
                }
                if (tex == nil) tex = DummyTexture(rf.texture_dimension[i]);
                if (stage == 0) [e setVertexTexture:tex atIndex:i];
                else [e setFragmentTexture:tex atIndex:i];
            }
            for (const auto& [slot, view] : st.extra_srv[stage]) {
                if (rf.texture_dimension[slot] == 12) {
                    id<MTLBuffer> buf = (view && view->buffer) ? view->buffer->mtl : nil;
                    NSUInteger off = (view && view->buffer) ? view->buffer->offset : 0;
                    if (buf == nil) { if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); } buf = g->zero_cb; off = 0; }
                    if (stage == 0) [e setVertexBuffer:buf offset:off atIndex:rf.buffer_index[slot]];
                    else [e setFragmentBuffer:buf offset:off atIndex:rf.buffer_index[slot]];
                } else {
                    id<MTLTexture> tex = nil;
                    if (view && view->texture) { Texture& tx = *view->texture; tex = IsDepth(tx.pixel_format) && tx.depth_copy != nil ? tx.depth_copy : ViewOf(tx, view->desc, false); }
                    if (tex == nil) tex = DummyTexture(rf.texture_dimension[slot]);
                    if (stage == 0) [e setVertexTexture:tex atIndex:slot];
                    else [e setFragmentTexture:tex atIndex:slot];
                }
            }
            for (int i = 0; i < 16; ++i) {
                if (!(rf.samplers & (1u << i))) continue;
                id<MTLSamplerState> s = SamplerFor(st.sampler[stage][i]);
                if (stage == 0) [e setVertexSamplerState:s atIndex:i];
                else [e setFragmentSamplerState:s atIndex:i];
            }
        }
        [e setVertexBuffer:g->null_buffer offset:0 atIndex:14];
        for (int i = 0; i < 16; ++i) {
            if (!st.vb[i].buffer) continue;
            [e setVertexBuffer:st.vb[i].buffer->mtl offset:st.vb[i].buffer->offset + st.vb[i].offset atIndex:15 + i];
        }

        if (g->frame_count == g->trace_frame && g->trace_draws < g->trace_cap) {
            ++g->trace_draws;
            if (st.rtv[0] && st.rtv[0]->texture && std::find(g->trace_rts.begin(), g->trace_rts.end(), st.rtv[0]->texture) == g->trace_rts.end()) g->trace_rts.push_back(st.rtv[0]->texture);
            id<MTLTexture> rt0 = st.rtv[0] && st.rtv[0]->texture ? ViewOf(*st.rtv[0]->texture, st.rtv[0]->desc, true) : nil;
            if (st.dsv && st.dsv->texture) UT_INFO("gpu:   dsv [%p] %lux%lu fmt %lu depth_gen %llu", (__bridge void*)st.dsv->texture->mtl, (unsigned long)st.dsv->texture->mtl.width, (unsigned long)st.dsv->texture->mtl.height,
                                                    (unsigned long)st.dsv->texture->mtl.pixelFormat, (unsigned long long)st.dsv->texture->depth_gen);
            UT_INFO("gpu: draw %s count %u inst %u topo %u vs %p(#%d) ps %p(#%d) layout %p rt0 [%p] %ux%u fmt %lu depth %d vp %.0fx%.0f+%.0f,%.0f cb0 %p tex0 %p blend %zu", args.indexed ? "indexed" : "plain", args.count, args.instances, st.topology,
                    (void*)st.vs.get(), st.vs->serial, (void*)st.ps.get(), st.ps ? st.ps->serial : 0, (void*)st.layout.get(), st.rtv[0] && st.rtv[0]->texture ? (__bridge void*)st.rtv[0]->texture->mtl : nullptr, rt0 ? (unsigned)rt0.width : 0, rt0 ? (unsigned)rt0.height : 0, rt0 ? (unsigned long)rt0.pixelFormat : 0, has_depth,
                    st.num_viewports ? st.viewports[0].w : 0.f, st.num_viewports ? st.viewports[0].h : 0.f, st.num_viewports ? st.viewports[0].x : 0.f, st.num_viewports ? st.viewports[0].y : 0.f,
                    (void*)st.cb[0][0].buffer.get(), (void*)st.srv[1][0].get(), st.blend.bytes.size());
        }
        if (g->frame_count == g->trace_frame && g->trace_draws <= g->trace_cap && g->trace_draws > 0 && std::getenv("UTSURO_TRACEPIXELS")) {
            for (int stage = 0; stage < 2; ++stage) for (int slot = 0; slot < 14; ++slot) {
                const BufferBinding& b = st.cb[stage][slot];
                const Shader* shader = stage == 0 ? st.vs.get() : st.ps.get();
                if (shader == nullptr || !(shader->translated.reflection.constant_buffers & (1u << slot))) continue;
                if (!b.buffer) { UT_INFO("gpu:   %s cb%d: NOT BOUND", stage ? "ps" : "vs", slot); continue; }
                {
                    const uint32_t* dw = reinterpret_cast<const uint32_t*>(BufferData(*b.buffer) + b.offset);
                    size_t nz = 0;
                    const size_t count = std::min<size_t>(b.size ? b.size : b.buffer->size, b.buffer->size) / 4;
                    for (size_t k = 0; k < count; ++k) nz += dw[k] != 0;
                    UT_INFO("gpu:   %s cb%d has %zu non-zero dwords of %zu", stage ? "ps" : "vs", slot, nz, count);
                    if (count <= 80) {
                        std::string all;
                        for (size_t k = 0; k < count; ++k) { char t[32]; std::snprintf(t, sizeof t, "%g%s", reinterpret_cast<const float*>(dw)[k], (k % 4 == 3) ? " | " : " "); all += t; }
                        UT_INFO("gpu:   %s cb%d all: %s", stage ? "ps" : "vs", slot, all.c_str());
                    } else {
                        std::string regs;   // the registers that hold anything, by index
                        for (size_t k = 0; k + 3 < count; k += 4) {
                            if (!dw[k] && !dw[k + 1] && !dw[k + 2] && !dw[k + 3]) continue;
                            const float* fv = reinterpret_cast<const float*>(dw + k);
                            char t[96]; std::snprintf(t, sizeof t, " [%zu]=(%g %g %g %g)", k / 4, fv[0], fv[1], fv[2], fv[3]); regs += t;
                        }
                        UT_INFO("gpu:   %s cb%d non-zero registers:%s", stage ? "ps" : "vs", slot, regs.c_str());
                    }
                }
                const float* f = reinterpret_cast<const float*>(BufferData(*b.buffer) + b.offset);
                UT_INFO("gpu:   %s cb%d (%u bytes, off %u, ring %d): %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", stage ? "ps" : "vs", slot, b.buffer->size, b.offset, b.buffer->ring_frame,
                        f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15],
                        b.buffer->size >= 128 ? f[16] : 0, b.buffer->size >= 128 ? f[17] : 0, b.buffer->size >= 128 ? f[18] : 0, b.buffer->size >= 128 ? f[19] : 0,
                        b.buffer->size >= 128 ? f[20] : 0, b.buffer->size >= 128 ? f[21] : 0, b.buffer->size >= 128 ? f[22] : 0, b.buffer->size >= 128 ? f[23] : 0,
                        b.buffer->size >= 128 ? f[24] : 0, b.buffer->size >= 128 ? f[25] : 0, b.buffer->size >= 128 ? f[26] : 0, b.buffer->size >= 128 ? f[27] : 0,
                        b.buffer->size >= 128 ? f[28] : 0, b.buffer->size >= 128 ? f[29] : 0, b.buffer->size >= 128 ? f[30] : 0, b.buffer->size >= 128 ? f[31] : 0);
            }
            UT_INFO("gpu:   viewports %u: %g %g %g %g (%g..%g); scissors %u: %d %d %d %d", st.num_viewports, st.viewports[0].x, st.viewports[0].y, st.viewports[0].w, st.viewports[0].h, st.viewports[0].min_depth, st.viewports[0].max_depth,
                    st.num_scissors, st.scissors[0].left, st.scissors[0].top, st.scissors[0].right, st.scissors[0].bottom);
            {
                const auto& bb = st.blend.bytes;
                UT_INFO("gpu:   blend desc %zu bytes: a2c %u indep %u | en %u src %u dst %u op %u sa %u da %u oa %u mask %02x | depth desc %zu: en %u wr %u func %u | raster %zu: fill %u cull %u fccw %u clip %u scissor %u",
                        bb.size(), Rd<uint32_t>(bb, 0), Rd<uint32_t>(bb, 4), Rd<uint32_t>(bb, 8), Rd<uint32_t>(bb, 12), Rd<uint32_t>(bb, 16), Rd<uint32_t>(bb, 20), Rd<uint32_t>(bb, 24), Rd<uint32_t>(bb, 28), Rd<uint32_t>(bb, 32), Rd<uint8_t>(bb, 36),
                        st.depth_stencil.bytes.size(), Rd<uint32_t>(st.depth_stencil.bytes, 0), Rd<uint32_t>(st.depth_stencil.bytes, 4), Rd<uint32_t>(st.depth_stencil.bytes, 8),
                        st.raster.bytes.size(), Rd<uint32_t>(st.raster.bytes, 0), Rd<uint32_t>(st.raster.bytes, 4), Rd<uint32_t>(st.raster.bytes, 8), Rd<uint32_t>(st.raster.bytes, 24), Rd<uint32_t>(st.raster.bytes, 28));
            }
            if (st.ps) for (int i = 0; i < 16; ++i) {
                if (!(st.ps->translated.reflection.textures & (1u << i))) continue;
                if (!st.srv[1][i] || !st.srv[1][i]->texture) { UT_INFO("gpu:   ps t%d: NOT BOUND", i); continue; }
                Texture& tx = *st.srv[1][i]->texture;
                uint8_t px[16] = {};
                const NSUInteger mx = std::max<NSUInteger>(1, tx.mtl.width / 2), my = std::max<NSUInteger>(1, tx.mtl.height / 2);
                if (!IsDepth(tx.pixel_format) && tx.mtl.textureType == MTLTextureType2D && (tx.pixel_format == MTLPixelFormatRGBA8Unorm || tx.pixel_format == MTLPixelFormatBGRA8Unorm))
                    [tx.mtl getBytes:px bytesPerRow:64 fromRegion:MTLRegionMake2D(mx, my, 4, 1) mipmapLevel:0];
                UT_INFO("gpu:   ps t%d [%p]: %lux%lu fmt %lu mips %lu uploads %u centre %02x%02x%02x%02x %02x%02x%02x%02x", i, (__bridge void*)tx.mtl, (unsigned long)tx.mtl.width, (unsigned long)tx.mtl.height, (unsigned long)tx.pixel_format,
                        (unsigned long)tx.mtl.mipmapLevelCount, tx.uploads, px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
            }
            if (args.indexed && (args.count == 6 || args.count == 4) && st.index_buffer && st.vb[0].buffer && st.vb[0].stride >= 24) {
                const uint8_t* ib = BufferData(*st.index_buffer) + st.index_offset + args.start * (st.index_format == 42 ? 4 : 2);
                std::string idx;
                for (int k = 0; k < 6; ++k) { char t[16]; std::snprintf(t, sizeof t, "%u ", st.index_format == 42 ? reinterpret_cast<const uint32_t*>(ib)[k] : reinterpret_cast<const uint16_t*>(ib)[k]); idx += t; }
                std::string vtx;
                const uint32_t nv = std::min<uint32_t>(6, st.vb[0].buffer->size / st.vb[0].stride);
                for (uint32_t k = 0; k < nv; ++k) {
                    const float* v = reinterpret_cast<const float*>(BufferData(*st.vb[0].buffer) + st.vb[0].offset + k * st.vb[0].stride);
                    char t[160]; std::snprintf(t, sizeof t, "[%g %g %g %g | %g %g %g %g %g] ", v[0], v[1], v[2], v[3], v[4], v[5], st.vb[0].stride >= 32 ? v[6] : 0.f, st.vb[0].stride >= 36 ? v[7] : 0.f, st.vb[0].stride >= 36 ? v[8] : 0.f); vtx += t;
                }
                UT_INFO("gpu:   quad indices %s base %d; vertices %s", idx.c_str(), args.base_vertex, vtx.c_str());
            }
            if (st.vb[0].buffer) {
                const float* f = reinterpret_cast<const float*>(BufferData(*st.vb[0].buffer) + st.vb[0].offset);
                UT_INFO("gpu:   vb0 stride %u: %g %g %g %g | %g %g %g %g", st.vb[0].stride, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
        }
        bool ok;
        const MTLPrimitiveType prim = Primitive(st.topology, &ok);
        if (!ok) {
            static std::map<uint32_t, int> seen;
            if (seen[st.topology]++ == 0) UT_WARN("gpu: unsupported topology %#x", st.topology);
            ++g->stats.bad_topology;
            return;
        }
        ++g->stats.draws;
        if (args.indirect && args.indirect->mtl != nil) {
            // D3D's indirect argument records are laid out as Metal's MTLDraw(Indexed)PrimitivesIndirectArguments.
            if (args.indexed) {
                if (!st.index_buffer) { ++g->stats.no_ib; return; }
                const bool wide = st.index_format == 42;
                [e drawIndexedPrimitives:prim indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:st.index_buffer->mtl
                       indexBufferOffset:st.index_buffer->offset + st.index_offset indirectBuffer:args.indirect->mtl
                    indirectBufferOffset:args.indirect->offset + args.indirect_offset];
            } else {
                [e drawPrimitives:prim indirectBuffer:args.indirect->mtl indirectBufferOffset:args.indirect->offset + args.indirect_offset];
            }
        } else if (args.indexed) {
            if (!st.index_buffer) { ++g->stats.no_ib; return; }
            const bool wide = st.index_format == 42;
            const NSUInteger isize = wide ? 4 : 2;
            [e drawIndexedPrimitives:prim indexCount:args.count indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                         indexBuffer:st.index_buffer->mtl indexBufferOffset:st.index_buffer->offset + st.index_offset + args.start * isize
                       instanceCount:args.instances baseVertex:args.base_vertex baseInstance:args.start_instance];
        } else {
            [e drawPrimitives:prim vertexStart:args.start vertexCount:args.count instanceCount:args.instances baseInstance:args.start_instance];
        }
        if (++g->draws_in_command >= 1000) {
            // A 2D game issues thousands of sprite draws a frame: one command buffer that large cannot be grown any more.
            EndEncoder();
            [Command() commit];
            g->command = nil;
            g->draws_in_command = 0;
        }
        if (g->frame_count == g->trace_frame && g->dump_dir != nullptr && std::getenv("UTSURO_STEPDUMP") && g->trace_draws > 0 && ps != nullptr && g->srv_dumps < 3 && st.rtv[0] && st.rtv[0]->texture &&
            st.rtv[0]->texture->mtl.width >= 1900 && st.rtv[0]->texture->pixel_format == MTLPixelFormatRGBA8Unorm && std::popcount(ps->translated.reflection.textures) >= 4) {
            // Debug: the inputs of the final composite.
            EndEncoder();
            [Command() commit];
            [g->command waitUntilCompleted];
            g->command = nil;
            for (int i = 0; i < 16; ++i) {
                if (!(ps->translated.reflection.textures & (1u << i)) || !st.srv[1][i] || !st.srv[1][i]->texture) continue;
                char path[512];
                std::snprintf(path, sizeof path, "%s/comp%d_t%d_%lux%lu.png", g->dir_or_empty(), g->srv_dumps, i, (unsigned long)st.srv[1][i]->texture->mtl.width, (unsigned long)st.srv[1][i]->texture->mtl.height);
                SavePng(*st.srv[1][i]->texture, path);
                std::snprintf(path, sizeof path, "%s/comp%d_t%d_alpha.png", g->dir_or_empty(), g->srv_dumps, i);
                g_png_alpha = true; SavePng(*st.srv[1][i]->texture, path); g_png_alpha = false;
            }
            ++g->srv_dumps;
        }
        if (g->frame_count == g->trace_frame && g->dump_dir != nullptr && std::getenv("UTSURO_STEPDUMP") && g->trace_draws > 0 && g->trace_draws < g->trace_cap && st.rtv[0] && st.rtv[0]->texture && st.rtv[0]->texture->mtl.width <= (std::getenv("UTSURO_STEPDUMP")[0] == 'a' ? 4000 : 960) && st.rtv[0]->texture->mtl.width >= 400 && g->step_dumps < 400) {
            // Debug: the render target after this draw, one picture per draw.
            EndEncoder();
            [Command() commit];
            [g->command waitUntilCompleted];
            g->command = nil;
            char path[512];
            std::snprintf(path, sizeof path, "%s/step_%02d_draw%u_ps%d_%ux%u.png", g->dump_dir, g->step_dumps++, g->trace_draws, st.ps ? st.ps->serial : 0, (unsigned)st.rtv[0]->texture->mtl.width, (unsigned)st.rtv[0]->texture->mtl.height);
            SavePng(*st.rtv[0]->texture, path);
            std::snprintf(path, sizeof path, "%s/step_%02d_draw%u_ps%d_%ux%u_alpha.png", g->dump_dir, g->step_dumps - 1, g->trace_draws, st.ps ? st.ps->serial : 0, (unsigned)st.rtv[0]->texture->mtl.width, (unsigned)st.rtv[0]->texture->mtl.height);
            g_png_alpha = true;
            SavePng(*st.rtv[0]->texture, path);
            g_png_alpha = false;
        }
        if (g->frame_count == g->trace_frame && st.rtv[0] && st.rtv[0]->texture) {
            EndEncoder();
            [Command() commit];
            [g->command waitUntilCompleted];
            g->command = nil;
            id<MTLTexture> rt0 = ViewOf(*st.rtv[0]->texture, st.rtv[0]->desc, true);
            if (rt0 != nil && (rt0.pixelFormat == MTLPixelFormatRGBA8Unorm || rt0.pixelFormat == MTLPixelFormatBGRA8Unorm)) {
                std::vector<uint8_t> px(rt0.width * rt0.height * 4);
                [rt0 getBytes:px.data() bytesPerRow:rt0.width * 4 fromRegion:MTLRegionMake2D(0, 0, rt0.width, rt0.height) mipmapLevel:0];
                size_t lit = 0;
                for (size_t i = 0; i < px.size(); i += 4) lit += (px[i] | px[i + 1] | px[i + 2]) != 0;
                UT_INFO("gpu: after draw %d: rt0 [%p] %ux%u: %zu lit pixels", g->trace_draws, (__bridge void*)rt0, (unsigned)rt0.width, (unsigned)rt0.height, lit);
            }
            if (g->dump_dir != nullptr) {
                char dpath[512];
                std::snprintf(dpath, sizeof dpath, "%s/draw_%02u_rt0_%ux%u_fmt%lu.png", g->dump_dir, g->trace_draws, (unsigned)st.rtv[0]->texture->mtl.width, (unsigned)st.rtv[0]->texture->mtl.height, (unsigned long)st.rtv[0]->texture->mtl.pixelFormat);
                SavePng(*st.rtv[0]->texture, dpath);
            }
        }
    }
}

void Dispatch(const DispatchState& st, uint32_t x, uint32_t y, uint32_t z) {
    std::scoped_lock lock {g->mutex};
    if (!st.cs || ((x == 0 || y == 0 || z == 0) && !st.indirect)) return;
    static int disp_count = 0;
    if (disp_count++ < 20 || g->frame_count == g->trace_frame) {
        UT_INFO("gpu: Dispatch call %d (frame %llu): x=%u y=%u z=%u cs=%p(#%d)", disp_count, (unsigned long long)g->frame_count, x, y, z, st.cs.get(), st.cs->serial);
        if (g->frame_count == g->trace_frame) {
            const ShaderReflection& rf = st.cs->translated.reflection;
            for (int i = 0; i < 16; ++i) {
                if (!(rf.textures & (1u << i))) continue;
                if (st.srv[i] && st.srv[i]->texture) { Texture& tx = *st.srv[i]->texture; UT_INFO("gpu:   cs t%d [%p]: %lux%lu fmt %lu uploads %u", i, (__bridge void*)tx.mtl, (unsigned long)tx.mtl.width, (unsigned long)tx.mtl.height, (unsigned long)tx.pixel_format, tx.uploads); }
                else if (st.srv[i] && st.srv[i]->buffer) UT_INFO("gpu:   cs t%d: buffer %u bytes", i, st.srv[i]->buffer->size);
                else UT_INFO("gpu:   cs t%d: NOT BOUND", i);
            }
            for (int i = 0; i < 64; ++i) {
                if (!(rf.uavs & (1u << i))) continue;
                if (st.uav[i] && st.uav[i]->texture) { Texture& tx = *st.uav[i]->texture; UT_INFO("gpu:   cs u%d [%p]: %lux%lu fmt %lu", i, (__bridge void*)tx.mtl, (unsigned long)tx.mtl.width, (unsigned long)tx.mtl.height, (unsigned long)tx.pixel_format); }
                else if (st.uav[i] && st.uav[i]->buffer) UT_INFO("gpu:   cs u%d: buffer %u bytes", i, st.uav[i]->buffer->size);
                else UT_INFO("gpu:   cs u%d: NOT BOUND", i);
            }
            for (int i = 0; i < 14; ++i) if (rf.constant_buffers & (1u << i)) UT_INFO("gpu:   cs cb%d: %s", i, st.cb[i].buffer ? "bound" : "NOT BOUND");
        }
    }
    @autoreleasepool {
        EndEncoder();
        if (!CompileShader(*st.cs)) return;
        id<MTLComputePipelineState> pso = nil;
        auto it = g->compute_pipelines.find(st.cs->translated.msl);
        if (it != g->compute_pipelines.end()) {
            pso = it->second;
        } else {
            NSError* err = nil;
            pso = [g->device newComputePipelineStateWithFunction:st.cs->function error:&err];
            if (pso == nil) {
                UT_ERROR("gpu: compute pipeline failed: %s", [[err localizedDescription] UTF8String]);
                return;
            }
            g->compute_pipelines[st.cs->translated.msl] = pso;
        }

        for (int i = 0; i < 16; ++i)
            if (st.srv[i] && st.srv[i]->texture && IsDepth(st.srv[i]->texture->pixel_format)) DepthAsFloat(*st.srv[i]->texture);
        for (const auto& [slot, view] : st.extra_srv)
            if (view && view->texture && IsDepth(view->texture->pixel_format)) DepthAsFloat(*view->texture);

        id<MTLComputeCommandEncoder> ce = [Command() computeCommandEncoder];
        if (ce == nil) return;
        [ce setComputePipelineState:pso];

        const ShaderReflection& rf = st.cs->translated.reflection;

        // Constant buffers 0..13
        for (int i = 0; i < 14; ++i) {
            if (!(rf.constant_buffers & (1u << i))) continue;
            const BufferBinding& b = st.cb[i];
            id<MTLBuffer> buf = b.buffer ? b.buffer->mtl : g->zero_cb;
            NSUInteger off = b.buffer ? b.buffer->offset + b.offset : 0;
            if (buf == nil) {
                if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); }
                buf = g->zero_cb; off = 0;
            }
            [ce setBuffer:buf offset:off atIndex:i];
        }

        // SRVs: textures at 0..15, structured buffers at 14..29
        for (int i = 0; i < 16; ++i) {
            if (!(rf.textures & (1u << i))) continue;
            if (rf.texture_dimension[i] == 12) {
                id<MTLBuffer> buf = (st.srv[i] && st.srv[i]->buffer) ? st.srv[i]->buffer->mtl : g->zero_cb;
                NSUInteger off = (st.srv[i] && st.srv[i]->buffer) ? st.srv[i]->buffer->offset : 0;
                if (buf == nil) {
                    if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); }
                    buf = g->zero_cb; off = 0;
                }
                [ce setBuffer:buf offset:off atIndex:(rf.buffer_index[i] != 0 ? rf.buffer_index[i] : 14 + i)];
                continue;
            }
            id<MTLTexture> tex = nil;
            if (st.srv[i] && st.srv[i]->texture) {
                Texture& tx = *st.srv[i]->texture;
                tex = IsDepth(tx.pixel_format) && tx.depth_copy != nil ? tx.depth_copy : ViewOf(tx, st.srv[i]->desc, false);
            }
            if (tex == nil) tex = DummyTexture(rf.texture_dimension[i]);
            [ce setTexture:tex atIndex:i];
        }

        for (const auto& [slot, view] : st.extra_srv) {
            if (rf.texture_dimension[slot] == 12) {
                id<MTLBuffer> buf = (view && view->buffer) ? view->buffer->mtl : nil;
                NSUInteger off = (view && view->buffer) ? view->buffer->offset : 0;
                if (buf == nil) { if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); } buf = g->zero_cb; off = 0; }
                [ce setBuffer:buf offset:off atIndex:rf.buffer_index[slot]];
            } else {
                id<MTLTexture> tex = nil;
                if (view && view->texture) { Texture& tx = *view->texture; tex = IsDepth(tx.pixel_format) && tx.depth_copy != nil ? tx.depth_copy : ViewOf(tx, view->desc, false); }
                if (tex == nil) tex = DummyTexture(rf.texture_dimension[slot]);
                [ce setTexture:tex atIndex:slot];
            }
        }
        // Samplers
        for (int i = 0; i < 16; ++i) {
            if (!(rf.samplers & (1u << i))) continue;
            id<MTLSamplerState> s = SamplerFor(st.sampler[i]);
            [ce setSamplerState:s atIndex:i];
        }

        // UAVs: buffers at 20..83, textures at 64..127
        for (int i = 0; i < 64; ++i) {
            if (!(rf.uavs & (1u << i))) continue;
            if (st.uav[i]) {
                if (st.uav[i]->buffer) {
                    id<MTLBuffer> buf = st.uav[i]->buffer->mtl;
                    NSUInteger off = st.uav[i]->buffer->offset;
                    [ce setBuffer:buf offset:off atIndex:(rf.uav_index[i] != 0 ? rf.uav_index[i] : 20 + i)];
                } else if (st.uav[i]->texture) {
                    Texture& tx = *st.uav[i]->texture;
                    id<MTLTexture> tex = ViewOf(tx, st.uav[i]->desc, false);
                    [ce setTexture:tex atIndex:64 + i];
                }
            } else {
                if (g->zero_cb == nil) { g->zero_cb = NewBuffer(65536); std::memset(g->zero_cb.contents, 0, 65536); }
                [ce setBuffer:g->zero_cb offset:0 atIndex:(rf.uav_index[i] != 0 ? rf.uav_index[i] : 20 + i)];
                [ce setTexture:DummyTexture(3) atIndex:64 + i];
            }
        }
        for (int i = 0; i < 64; ++i) {
            if (rf.counter_index[i] == 0) continue;
            const bool have = st.uav[i] && st.uav[i]->counter && st.uav[i]->counter->mtl != nil;
            [ce setBuffer:have ? st.uav[i]->counter->mtl : ScratchCounter() offset:have ? st.uav[i]->counter->offset : 0 atIndex:rf.counter_index[i]];
        }

        MTLSize threadgroups = MTLSizeMake(x, y, z);
        MTLSize threadsPerThreadgroup = MTLSizeMake(
            std::max(1u, rf.thread_group_size[0]),
            std::max(1u, rf.thread_group_size[1]),
            std::max(1u, rf.thread_group_size[2])
        );
        if (st.indirect && st.indirect->mtl != nil)
            [ce dispatchThreadgroupsWithIndirectBuffer:st.indirect->mtl indirectBufferOffset:st.indirect->offset + st.indirect_offset threadsPerThreadgroup:threadsPerThreadgroup];
        else
            [ce dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerThreadgroup];
        [ce endEncoding];
    }
}


void ClearRenderTarget(const TextureView& view, const float color[4]) {
    std::scoped_lock lock {g->mutex};
    if (!view.texture || view.texture->mtl == nil) return;
    ++g->stats.clears;
    @autoreleasepool {
        EndEncoder();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> t = ViewOf(*view.texture, view.desc, true);
        pass.colorAttachments[0].texture = t;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
        id<MTLRenderCommandEncoder> e = [Command() renderCommandEncoderWithDescriptor:pass];
        [e endEncoding];
    }
}

static bool IsIntegerFormat(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRG8Sint: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
        return true;
    default:
        return false;
    }
}

void ClearUav(const TextureView& view, const uint32_t value[4], bool as_float) {
    std::scoped_lock lock {g->mutex};
    @autoreleasepool {
        if (view.buffer && view.buffer->mtl != nil) {
            // Raw and structured views clear every 32-bit word to the first value (a typed buffer's elements are words here too).
            Buffer& b = *view.buffer;
            const uint32_t v = value[0];
            EndEncoder();
            if ((v & 0xFF) * 0x01010101u == v) {
                id<MTLBlitCommandEncoder> blit = [Command() blitCommandEncoder];
                [blit fillBuffer:b.mtl range:NSMakeRange(b.offset, b.size) value:static_cast<uint8_t>(v)];
                [blit endEncoding];
                return;
            }
            if (g->fill_pipeline == nil) {
                NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                    "kernel void fill(device uint* b [[buffer(0)]], constant uint2& a [[buffer(1)]], uint i [[thread_position_in_grid]]) { if (i < a.y) b[i] = a.x; }\n";
                NSError* err = nil;
                id<MTLLibrary> lib = [g->device newLibraryWithSource:src options:nil error:&err];
                if (lib != nil) g->fill_pipeline = [g->device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err];
                if (g->fill_pipeline == nil) { UT_ERROR("gpu: fill pipeline: %s", err ? [[err localizedDescription] UTF8String] : "?"); return; }
            }
            const uint32_t args[2] = {v, b.size / 4};
            id<MTLComputeCommandEncoder> ce = [Command() computeCommandEncoder];
            [ce setComputePipelineState:g->fill_pipeline];
            [ce setBuffer:b.mtl offset:b.offset atIndex:0];
            [ce setBytes:args length:sizeof args atIndex:1];
            [ce dispatchThreads:MTLSizeMake(args[1], 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(256, std::max<uint32_t>(1, args[1])), 1, 1)];
            [ce endEncoding];
            return;
        }
        if (!view.texture || view.texture->mtl == nil) return;
        Texture& t = *view.texture;
        id<MTLTexture> target = ViewOf(t, view.desc, true);
        if (target == nil) return;
        MTLClearColor color;
        if (as_float) {
            float f[4];
            std::memcpy(f, value, sizeof f);
            color = MTLClearColorMake(f[0], f[1], f[2], f[3]);
        } else if (IsIntegerFormat(target.pixelFormat)) {
            color = MTLClearColorMake(value[0], value[1], value[2], value[3]);
        } else {
            // A uint clear of a float or normalised view copies bits; the values titles use are 0 (and all ones).
            auto unit = [](uint32_t v) { return v == 0 ? 0.0 : 1.0; };
            color = MTLClearColorMake(unit(value[0]), unit(value[1]), unit(value[2]), unit(value[3]));
        }
        EndEncoder();
        ++g->stats.clears;
        const NSUInteger planes = target.textureType == MTLTextureType3D ? std::max<NSUInteger>(1, target.depth) : 1;
        const NSUInteger layers = target.textureType == MTLTextureType3D ? 1 : std::max<NSUInteger>(1, target.arrayLength);
        for (NSUInteger z = 0; z < planes; ++z) {
            for (NSUInteger layer = 0; layer < layers; ++layer) {
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].depthPlane = z;
                pass.colorAttachments[0].slice = layer;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = color;
                id<MTLRenderCommandEncoder> e = [Command() renderCommandEncoderWithDescriptor:pass];
                [e endEncoding];
            }
        }
    }
}

void ClearDepthStencil(const TextureView& view, uint32_t flags, float depth, uint8_t stencil) {
    std::scoped_lock lock {g->mutex};
    if (!view.texture || view.texture->mtl == nil) return;
    ++view.texture->depth_gen;
    @autoreleasepool {
        EndEncoder();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> t = ViewOf(*view.texture, view.desc, true);
        pass.depthAttachment.texture = t;
        pass.depthAttachment.loadAction = (flags & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = depth;
        if (HasStencil(t.pixelFormat)) {
            pass.stencilAttachment.texture = t;
            pass.stencilAttachment.loadAction = (flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = stencil;
        }
        id<MTLRenderCommandEncoder> e = [Command() renderCommandEncoderWithDescriptor:pass];
        [e endEncoding];
    }
}

void CopyBuffer(Buffer& dst, uint32_t dst_offset, Buffer& src, uint32_t src_offset, uint32_t size) {
    std::scoped_lock lock {g->mutex};
    EndEncoder();
    id<MTLBlitCommandEncoder> blit = [Command() blitCommandEncoder];
    [blit copyFromBuffer:src.mtl sourceOffset:src.offset + src_offset toBuffer:dst.mtl destinationOffset:dst.offset + dst_offset size:size];
    [blit endEncoding];
}

namespace {

void BuildPresentPipeline() {
    NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 pos [[position]]; float2 uv; };\n"
        "vertex V vmain(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
        "fragment float4 fmain(V in [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::linear, address::clamp_to_edge); return float4(t.sample(s, in.uv).rgb, 1.0); }\n";
    NSError* err = nil;
    id<MTLLibrary> lib = [g->device newLibraryWithSource:src options:nil error:&err];
    if (lib == nil) { UT_ERROR("gpu: present shader: %s", [[err localizedDescription] UTF8String]); return; }
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vmain"];
    d.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g->present_pipeline = [g->device newRenderPipelineStateWithDescriptor:d error:&err];
}

} // namespace

/// Writes 8-bit RGBA pixels (alpha ignored) as a PNG.
bool WritePngRGBA(const uint8_t* rgba, size_t w, size_t h, const char* path) {
    @autoreleasepool {
        std::vector<uint8_t> px(rgba, rgba + w * h * 4);
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // CoreGraphics wants BGRA
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
        bool ok = false;
        if (dest != nullptr) { CGImageDestinationAddImage(dest, img, nullptr); ok = CGImageDestinationFinalize(dest); CFRelease(dest); }
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        return ok;
    }
}

bool SavePng(Texture& texture, const std::string& path) {
    @autoreleasepool {
        id<MTLTexture> t = texture.mtl;
        if (t == nil) return false;
        const NSUInteger w = t.width, h = t.height;
        const bool half4 = t.pixelFormat == MTLPixelFormatRGBA16Float;
        const bool r11 = t.pixelFormat == MTLPixelFormatRG11B10Float;
        const bool rgb10 = t.pixelFormat == MTLPixelFormatRGB10A2Unorm;
        // always save (remove format check)
// if (!half4 && !r11 && !rgb10 && t.pixelFormat != MTLPixelFormatBGRA8Unorm && t.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB && t.pixelFormat != MTLPixelFormatRGBA8Unorm && t.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) return false;
        const NSUInteger bpp = half4 ? 8 : 4;
        std::vector<uint8_t> raw(w * h * bpp);
        std::vector<uint8_t> px(w * h * 4);
        if (t.storageMode == MTLStorageModePrivate) {
            // Private textures (iOS) have no CPU view: blit into a shared buffer first.
            id<MTLBuffer> staging = [g->device newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [g->queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                         toBuffer:staging destinationOffset:0 destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            std::memcpy(raw.data(), staging.contents, raw.size());
        } else {
            [t getBytes:raw.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        }
        if (rgb10) {
            const uint32_t* wp = reinterpret_cast<const uint32_t*>(raw.data());
            for (size_t i = 0; i < w * h; ++i) {
                px[i * 4 + 0] = static_cast<uint8_t>(((wp[i] >> 0) & 0x3FF) >> 2);
                px[i * 4 + 1] = static_cast<uint8_t>(((wp[i] >> 10) & 0x3FF) >> 2);
                px[i * 4 + 2] = static_cast<uint8_t>(((wp[i] >> 20) & 0x3FF) >> 2);
                px[i * 4 + 3] = 255;
            }
        } else if (r11) {
            // 5-bit exponent floats: 6 (or 5) mantissa bits, no sign.
            auto unpack = [](uint32_t bits, int mantissa) {
                const uint32_t e = bits >> mantissa, m = bits & ((1u << mantissa) - 1);
                if (e == 0) return static_cast<float>(m) / static_cast<float>(1u << mantissa) * (1.f / 16384.f);
                if (e == 31) return 65504.f;
                return (1.f + static_cast<float>(m) / static_cast<float>(1u << mantissa)) * std::ldexp(1.f, static_cast<int>(e) - 15);
            };
            const uint32_t* wp = reinterpret_cast<const uint32_t*>(raw.data());
            for (size_t i = 0; i < w * h; ++i) {
                const float c[3] = {unpack(wp[i] & 0x7FF, 6), unpack((wp[i] >> 11) & 0x7FF, 6), unpack(wp[i] >> 22, 5)};
                for (int k = 0; k < 3; ++k) {
                    const float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                    px[i * 4 + k] = static_cast<uint8_t>(v * 255.f + 0.5f);
                }
                px[i * 4 + 3] = 255;
            }
        } else if (half4) {
            const _Float16* hp = reinterpret_cast<const _Float16*>(raw.data());
            for (size_t i = 0; i < w * h; ++i) {
                for (int c = 0; c < 3; ++c) {
                    float v = static_cast<float>(hp[i * 4 + (g_png_alpha ? 3 : c)]);
                    v = v < 0 ? 0 : v > 1 ? 1 : v;
                    px[i * 4 + c] = static_cast<uint8_t>(v * 255.f + 0.5f);
                }
                px[i * 4 + 3] = 255;
            }
        } else {
            px = raw;
        }
        const bool bgra = !half4 && !r11 && !rgb10 && (t.pixelFormat == MTLPixelFormatBGRA8Unorm || t.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB);
        if (!bgra) for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // CoreGraphics below wants BGRA
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
        bool ok = false;
        if (dest != nullptr) {
            CGImageDestinationAddImage(dest, img, nullptr);
            ok = CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        return ok;
    }
}

void SetLayer(void* ca_metal_layer) {
    if (g == nullptr) return;
    std::scoped_lock lock {g->mutex};
    g->layer = (__bridge CAMetalLayer*)ca_metal_layer;
    g->layer.device = g->device;
    g->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g->layer.framebufferOnly = YES;
}

void Present(const std::shared_ptr<Texture>& back, uint32_t sync_interval) {
    std::scoped_lock lock {g->mutex};
    @autoreleasepool {
        EndEncoder();
        g->draws_in_command = 0;
        const uint64_t index = ++g->frame_count;
        // UTSURO_TRACEAFTER=<draws>:<frames>: trace the frame that many frames after the first one with more draws than that (a
        // title's heavy phase starts at a different frame from run to run).
        if (static const char* after = std::getenv("UTSURO_TRACEAFTER"); after != nullptr) {
            static uint64_t last_draws = 0, heavy = 0;
            static bool armed = false;
            const uint64_t frame_draws = g->stats.draws - last_draws;
            last_draws = g->stats.draws;
            const uint64_t threshold = std::strtoull(after, nullptr, 10);
            const char* colon = std::strchr(after, ':');
            heavy = frame_draws > threshold ? heavy + 1 : 0;
            if (!armed && heavy >= 30) {   // thirty heavy frames in a row: not a one-frame spike
                armed = true;
                g->trace_frame = index + (colon != nullptr ? std::strtoull(colon + 1, nullptr, 10) : 60);
                UT_INFO("gpu: frame %llu drew %llu: tracing frame %llu", (unsigned long long)index, (unsigned long long)frame_draws, (unsigned long long)g->trace_frame);
            }
        }
        if (index % 120 == 1) UT_INFO("gpu: Present back=%p mtl=%p", back.get(), back ? (__bridge void*)back->mtl : nullptr);
        if (index % 120 == 1) {
            static auto last_report = std::chrono::steady_clock::now();
            static uint64_t last_index = 0;
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - last_report).count();
            if (index > last_index && secs > 0.5) UT_INFO("gpu: %.1f frames per second", static_cast<double>(index - last_index) / secs);
            last_report = now;
            last_index = index;
            const auto& st = g->stats;
            UT_INFO("gpu: frame %llu: %llu draws, %llu clears; skipped: no vs %llu, bad vs %llu, bad ps %llu, no pass %llu, no pso %llu, topology %llu, no ib %llu", (unsigned long long)index,
                    (unsigned long long)st.draws, (unsigned long long)st.clears, (unsigned long long)st.no_vs, (unsigned long long)st.bad_vs, (unsigned long long)st.bad_ps,
                    (unsigned long long)st.no_pass, (unsigned long long)st.no_pso, (unsigned long long)st.bad_topology, (unsigned long long)st.no_ib);
            if (!g->rt_census.empty()) {
                std::string line;
                for (auto& [k, n] : g->rt_census) line += "  [" + k + "] x" + std::to_string(n);
                UT_INFO("gpu: render targets since the last line:%s", line.c_str());
                g->rt_census.clear();
            }
            if (!g->rt_census.empty()) {
                std::string line;
                for (auto& [k, n] : g->rt_census) line += "  [" + k + "] x" + std::to_string(n);
                UT_INFO("gpu: render targets since the last line:%s", line.c_str());
                g->rt_census.clear();
            }
        }
        // Show the back buffer in the window.
        if (g->layer != nil && back && back->mtl != nil) {
            if (g->present_pipeline == nil) BuildPresentPipeline();
            id<CAMetalDrawable> drawable = [g->layer nextDrawable];
            if (drawable != nil && g->present_pipeline != nil) {
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [Command() renderCommandEncoderWithDescriptor:pass];
                [e setRenderPipelineState:g->present_pipeline];
                [e setFragmentTexture:back->mtl atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
                [Command() presentDrawable:drawable];
            }
        }
        const bool snap = !g->snapshot_path.empty() && back && back->mtl != nil;
        const bool dump = snap || (g->dump_dir != nullptr && back && back->mtl != nil && (index % g->dump_every) == 0);
        id<MTLCommandBuffer> cmd = Command();
        // Frames in flight: wait for the oldest, then recycle its ring slot.
        dispatch_semaphore_t sem = g->frames_in_flight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(sem); }];
        [cmd commit];
        g->command = nil;
        if (dump) {
            [cmd waitUntilCompleted];
            char path[512];
            if (snap) { std::snprintf(path, sizeof path, "%s", g->snapshot_path.c_str()); g->snapshot_path.clear(); }
            else std::snprintf(path, sizeof path, "%s/frame_%06llu.png", g->dump_dir, (unsigned long long)index);
            SavePng(*back, path);
        }
        if (index == g->trace_frame + 1 && !g->trace_depths.empty()) {
            // The depth textures the traced frame sampled, as their float copies hold them now: range, how much is exactly 0 or 1
            // (a cleared or never-written depth map), and a grayscale picture of it.
            [cmd waitUntilCompleted];
            int n = 0;
            for (Texture* dt : g->trace_depths) {
                id<MTLTexture> c = dt->depth_copy;
                if (c == nil) continue;
                const NSUInteger w = c.width, h = c.height;
                id<MTLBuffer> staging = [g->device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [g->queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                [blit copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
                [blit endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const float* f = static_cast<const float*>(staging.contents);
                float lo = 1e30f, hi = -1e30f; size_t zeros = 0, ones = 0;
                for (size_t k = 0; k < w * h; ++k) { lo = std::min(lo, f[k]); hi = std::max(hi, f[k]); zeros += f[k] == 0.f; ones += f[k] == 1.f; }
                UT_INFO("gpu: traced depth texture %p %lux%lu: min %g max %g, %.1f%% exactly 0, %.1f%% exactly 1", (__bridge void*)dt->mtl, (unsigned long)w, (unsigned long)h, lo, hi,
                        100.0 * zeros / (w * h), 100.0 * ones / (w * h));
                if (g->dump_dir != nullptr) {
                    // Grayscale, stretched over the values that are not 1 (the far plane), so the content shows.
                    float vlo = 1e30f, vhi = -1e30f;
                    for (size_t k = 0; k < w * h; ++k) if (f[k] < 1.f) { vlo = std::min(vlo, f[k]); vhi = std::max(vhi, f[k]); }
                    std::vector<uint8_t> px(w * h * 4);
                    for (size_t k = 0; k < w * h; ++k) {
                        const float v = f[k] >= 1.f ? 1.f : (vhi > vlo ? (f[k] - vlo) / (vhi - vlo) : 0.f);
                        px[k * 4] = px[k * 4 + 1] = px[k * 4 + 2] = static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f);
                        px[k * 4 + 3] = 255;
                    }
                    char path[512];
                    std::snprintf(path, sizeof path, "%s/depth_%02d_%lux%lu.png", g->dump_dir, n++, (unsigned long)w, (unsigned long)h);
                    WritePngRGBA(px.data(), w, h, path);
                }
            }
            g->trace_depths.clear();
        }
        if (index == g->trace_frame + 1 && g->dump_dir != nullptr && !g->trace_rts.empty()) {
            [cmd waitUntilCompleted];
            int n = 0;
            for (const auto& rt : g->trace_rts) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/rt_%02d_%ux%u_f%lu.png", g->dump_dir, n++, (unsigned)(rt->mtl ? rt->mtl.width : 0), (unsigned)(rt->mtl ? rt->mtl.height : 0), (unsigned long)(rt->mtl ? rt->mtl.pixelFormat : 0));
                SavePng(*rt, path);
            }
            g->trace_rts.clear();
        }
        dispatch_semaphore_wait(g->frames_in_flight, DISPATCH_TIME_FOREVER);
        std::scoped_lock ring_lock {g->ring_mutex};
        g->frame = (g->frame + 1) % kRingFrames;
        EvictRing(g->frame);
    }
    // Pace to the display like a vsynced Present would (clamp sync_interval to 1 to unlock 60 FPS).
    const bool turbo = IsTurbo();
    const uint32_t interval = (sync_interval > 1) ? 1 : sync_interval;
    const auto period = std::chrono::microseconds(turbo || interval == 0 ? 0 : 16667 * interval);
    if (period.count() > 0) {
        const auto due = g->last_present + period;
        const auto now = std::chrono::steady_clock::now();
        if (due > now) std::this_thread::sleep_for(due - now);
        g->last_present = std::max(due, now - period);
    }
}

bool IsTurbo() {
    static const char* env = std::getenv("UTSURO_TURBO");
    if (env == nullptr) return false;
    if (env[0] == '1' && env[1] == '\0') return true;
    const uint64_t cap = std::strtoull(env, nullptr, 10);
    return g != nullptr && g->frame_count < cap;
}

void RequestSnapshot(const std::string& path) {
    if (g == nullptr) return;
    std::scoped_lock lock {g->mutex};
    g->snapshot_path = path;
}

bool IsTraceFrame(uint64_t frame) { return g != nullptr && frame == g->trace_frame; }

void RequestTrace() {
    if (g == nullptr) return;
    std::scoped_lock lock {g->mutex};
    g->trace_frame = g->frame_count + 3;
    g->trace_draws = 0;
    g->step_dumps = 0;
    g->srv_dumps = 0;
}

void Flush() {
    if (g == nullptr) return;
    std::scoped_lock lock {g->mutex};
    EndEncoder();
}

} // namespace utsuro::gpu
