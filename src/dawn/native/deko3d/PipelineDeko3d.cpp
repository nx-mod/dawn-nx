// Copyright 2018 The Dawn & Tint Authors
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

#include "src/dawn/native/deko3d/PipelineDeko3d.h"

#include <set>
#include <string>

#include "src/dawn/common/Range.h"
#include "src/dawn/native/Device.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/PipelineLayoutDeko3d.h"
#include "src/dawn/native/deko3d/SamplerDeko3d.h"
#include "src/dawn/native/deko3d/ShaderModuleDeko3d.h"

namespace dawn::native::deko3d {

DkStage ToDkStage(SingleShaderStage stage) {
    switch (stage) {
        case SingleShaderStage::Vertex:
            return DkStage_Vertex;
        case SingleShaderStage::Fragment:
            return DkStage_Fragment;
        case SingleShaderStage::Compute:
            return DkStage_Compute;
    }
    DAWN_UNREACHABLE();
}

PipelineDeko3d::PipelineDeko3d() = default;
PipelineDeko3d::~PipelineDeko3d() = default;

MaybeError PipelineDeko3d::InitializeBase(const PipelineLayout* layout,
                                          const PerStage<ProgrammableStage>& stages,
                                          Extent3D* workgroupSize) {
    wgpu::ShaderStage activeStages = wgpu::ShaderStage::None;
    for (SingleShaderStage stage : IterateStages(kAllStages)) {
        if (stages[stage].module != nullptr) {
            activeStages |= StageBit(stage);
        }
    }

    // Texture units must be known before GLSL generation (uam needs explicit bindings), so they
    // are assigned from reflection first, then every stage is compiled against them.
    std::set<CombinedSampler> combinedSamplers;
    for (SingleShaderStage stage : IterateStages(activeStages)) {
        ShaderModule* module = ToBackend(stages[stage].module.Get());
        for (const CombinedSampler& combined : module->GetCombinedSamplers(stages[stage], layout)) {
            combinedSamplers.insert(combined);
        }
    }

    const auto& indices = layout->GetBindingIndexInfo();
    mUnitsForSamplers.resize(layout->GetNumSamplers());
    mUnitsForTextures.resize(layout->GetNumSampledTextures());
    TextureUnitMap textureUnits;
    TextureUnit textureUnit{0u};
    for (const CombinedSampler& combined : combinedSamplers) {
        textureUnits[combined] = uint32_t(textureUnit);
        BindingIndex textureArrayStart = combined.textureLocation.index;
        for (auto element : Range(combined.textureLocation.shaderArraySize)) {
            FlatBindingIndex textureIndex =
                indices[combined.textureLocation.group][textureArrayStart + element];
            mUnitsForTextures[textureIndex].push_back(textureUnit);
            if (!combined.samplerLocation) {
                mPlaceholderSamplerUnits.push_back(textureUnit);
            } else {
                FlatBindingIndex samplerIndex =
                    indices[combined.samplerLocation->group][combined.samplerLocation->index];
                mUnitsForSamplers[samplerIndex].push_back(textureUnit);
            }
            textureUnit++;
        }
    }

    Device* device = ToBackend(layout->GetDevice());
    for (SingleShaderStage stage : IterateStages(activeStages)) {
        ShaderModule* module = ToBackend(stages[stage].module.Get());
        std::string dksh;
        Extent3D localWorkgroupSize;
        DAWN_TRY_ASSIGN(dksh, module->CompileShader(stages[stage], stage, layout, textureUnits,
                                                    &localWorkgroupSize));
        if (stage == SingleShaderStage::Compute && workgroupSize != nullptr) {
            *workgroupSize = localWorkgroupSize;
        }
        DkShader shader;
        DAWN_TRY(device->LoadShader(std::vector<uint8_t>(dksh.begin(), dksh.end()), &shader));
        mShaders.push_back(shader);
        mStageMask |= 1u << ToDkStage(stage);
    }

    if (!mPlaceholderSamplerUnits.empty()) {
        Ref<SamplerBase> sampler;
        DAWN_TRY_ASSIGN(sampler, layout->GetDevice()->CreateSampler());
        mPlaceholderSampler = ToBackend(std::move(sampler));
    }
    return {};
}

const std::vector<TextureUnit>& PipelineDeko3d::GetTextureUnitsForSampler(
    FlatBindingIndex index) const {
    DAWN_ASSERT(index < mUnitsForSamplers.size());
    return mUnitsForSamplers[index];
}

const std::vector<TextureUnit>& PipelineDeko3d::GetTextureUnitsForTextureView(
    FlatBindingIndex index) const {
    DAWN_ASSERT(index < mUnitsForTextures.size());
    return mUnitsForTextures[index];
}

Sampler* PipelineDeko3d::GetPlaceholderSampler() const {
    return mPlaceholderSampler.Get();
}

}  // namespace dawn::native::deko3d
