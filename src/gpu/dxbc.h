// dxbc.h -- Shader Model 4/5 bytecode (DXBC) to Metal Shading Language.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace utsuro::gpu {

enum class ShaderStage { Vertex, Pixel, Compute, Geometry, Hull, Domain, Unknown };

/// One entry of an input/output signature.
struct SignatureElement {
    std::string semantic;     ///< Upper-case semantic name, e.g. "TEXCOORD".
    uint32_t index {0};       ///< Semantic index.
    uint32_t reg {0};         ///< Register number.
    uint32_t mask {0};        ///< Components used (bit 0 = x).
    uint32_t system_value {0};///< D3D_NAME_*: 0 none, 1 position, 2 clip distance, 3 cull distance, 4 render target array index, 5 viewport array index, 6 vertex id, 7 primitive id, 8 instance id, 9 is front face, 10 sample index.
    uint32_t component_type {0};
};

struct ShaderReflection {
    std::vector<SignatureElement> inputs, outputs;
    uint32_t constant_buffers {0};   ///< Bit i set: cb i is declared.
    uint32_t cb_size[16] {};         ///< Declared size of each constant buffer, in float4s.
    uint32_t textures {0};           ///< Bit i set: t i is declared (low 32 slots).
    uint32_t samplers {0};
    uint32_t texture_dimension[128] {};   ///< D3D10_SB_RESOURCE_DIMENSION per declared slot.
    bool writes_depth {false};
    uint32_t outputs_written {0};    ///< SV_Target mask for pixel shaders.
};

struct TranslatedShader {
    bool ok {false};
    std::string error;
    ShaderStage stage {ShaderStage::Unknown};
    std::string msl;
    std::string entry {"main0"};
    ShaderReflection reflection;
};

/// Translates a DXBC container (or a bare shader token stream) to MSL.
/// `input_classes[reg]` tells how the vertex layout supplies each vertex-shader input: 0 float
/// (the default), 1 unsigned integer, 2 signed integer. Integer inputs keep their bits in the register.
TranslatedShader TranslateDxbc(const uint8_t* data, size_t size, const std::vector<uint8_t>* input_classes = nullptr);

} // namespace utsuro::gpu
