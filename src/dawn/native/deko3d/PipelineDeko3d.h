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

#ifndef SRC_DAWN_NATIVE_DEKO3D_PIPELINEDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_PIPELINEDEKO3D_H_

#include <deko3d.h>

#include <vector>

#include "src/dawn/common/ityp_vector.h"
#include "src/dawn/native/IntegerTypes.h"
#include "src/dawn/native/PerStage.h"
#include "src/dawn/native/Pipeline.h"
#include "src/dawn/native/deko3d/IntegerTypes.h"

namespace dawn::native {
struct ProgrammableStage;
}  // namespace dawn::native

namespace dawn::native::deko3d {

class PipelineLayout;
class Sampler;

// Shader + texture unit state shared by render and compute pipelines. Texture units are numbered
// across the whole pipeline and every combined texture/sampler is bound for each active stage.
class PipelineDeko3d {
  public:
    PipelineDeko3d();
    ~PipelineDeko3d();

    const std::vector<TextureUnit>& GetTextureUnitsForSampler(FlatBindingIndex index) const;
    const std::vector<TextureUnit>& GetTextureUnitsForTextureView(FlatBindingIndex index) const;
    const std::vector<TextureUnit>& GetPlaceholderSamplerUnits() const {
        return mPlaceholderSamplerUnits;
    }
    Sampler* GetPlaceholderSampler() const;

    // Stage flags (DkStageFlag_*) of the stages this pipeline has shaders for.
    uint32_t GetDkStageMask() const { return mStageMask; }
    const std::vector<DkShader>& GetShaders() const { return mShaders; }

  protected:
    MaybeError InitializeBase(const PipelineLayout* layout,
                              const PerStage<ProgrammableStage>& stages,
                              Extent3D* workgroupSize = nullptr);

  private:
    ityp::vector<FlatBindingIndex, std::vector<TextureUnit>> mUnitsForSamplers;
    ityp::vector<FlatBindingIndex, std::vector<TextureUnit>> mUnitsForTextures;
    std::vector<TextureUnit> mPlaceholderSamplerUnits;
    Ref<Sampler> mPlaceholderSampler;
    std::vector<DkShader> mShaders;
    uint32_t mStageMask = 0;
};

DkStage ToDkStage(SingleShaderStage stage);

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_PIPELINEDEKO3D_H_
