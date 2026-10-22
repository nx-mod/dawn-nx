// Copyright 2017 The Dawn & Tint Authors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef SRC_DAWN_NATIVE_DEKO3D_SHADERMODULEDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_SHADERMODULEDEKO3D_H_

#include <map>
#include <string>
#include <vector>

#include "src/dawn/native/IntegerTypes.h"
#include "src/dawn/native/Serializable.h"
#include "src/dawn/native/ShaderModule.h"

namespace dawn::native {
struct ProgrammableStage;
}  // namespace dawn::native

namespace dawn::native::deko3d {

class Device;
class PipelineLayout;

#define COMBINED_SAMPLER_ELEMENT_MEMBERS(X) \
    X(BindGroupIndex, group)                \
    X(BindingIndex, index)                  \
    X(BindingIndex, shaderArraySize, 1u)
DAWN_SERIALIZABLE(struct, CombinedSamplerElement, COMBINED_SAMPLER_ELEMENT_MEMBERS){};
#undef COMBINED_SAMPLER_ELEMENT_MEMBERS

bool operator<(const CombinedSamplerElement& a, const CombinedSamplerElement& b);

#define COMBINED_SAMPLER_MEMBERS(X)                               \
    /* nullopt: the texture is used without a sampler (textureLoad) and Dawn binds a placeholder. */ \
    X(std::optional<CombinedSamplerElement>, samplerLocation)     \
    X(CombinedSamplerElement, textureLocation)

DAWN_SERIALIZABLE(struct, CombinedSampler, COMBINED_SAMPLER_MEMBERS) {
    std::string GetName() const;
};
#undef COMBINED_SAMPLER_MEMBERS

bool operator<(const CombinedSampler& a, const CombinedSampler& b);

// First texture unit of each combined texture/sampler across all stages of a pipeline.
using TextureUnitMap = std::map<CombinedSampler, uint32_t>;

class ShaderModule final : public ShaderModuleBase {
  public:
    static ResultOrError<Ref<ShaderModule>> Create(
        Device* device,
        const UnpackedPtr<ShaderModuleDescriptor>& descriptor,
        const std::vector<tint::wgsl::Extension>& internalExtensions);

    // Combined texture/samplers the entry point uses, computed from reflection only.
    std::vector<CombinedSampler> GetCombinedSamplers(const ProgrammableStage& programmableStage,
                                                     const PipelineLayout* layout);

    // WGSL -> GLSL 4.6 (Tint, deko3d conventions) -> DKSH (uam), cached through the blob cache.
    ResultOrError<std::string> CompileShader(const ProgrammableStage& programmableStage,
                                             SingleShaderStage stage,
                                             const PipelineLayout* layout,
                                             const TextureUnitMap& textureUnits,
                                             Extent3D* workgroupSize);

  private:
    ShaderModule(Device* device,
                 const UnpackedPtr<ShaderModuleDescriptor>& descriptor,
                 std::vector<tint::wgsl::Extension> internalExtensions);
    ~ShaderModule() override = default;
};

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_SHADERMODULEDEKO3D_H_
