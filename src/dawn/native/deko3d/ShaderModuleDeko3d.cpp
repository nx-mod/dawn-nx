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

#include "src/dawn/native/deko3d/ShaderModuleDeko3d.h"

#include <sstream>
#include <utility>

#include "dawn/platform/DawnPlatform.h"
#include "src/dawn/native/Adapter.h"
#include "src/dawn/native/BindGroupLayoutInternal.h"
#include "src/dawn/native/CacheRequest.h"
#include "src/dawn/native/Pipeline.h"
#include "src/dawn/native/TintUtils.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/PipelineLayoutDeko3d.h"
#include "src/dawn/native/deko3d/UamCompilerDeko3d.h"
#include "src/dawn/platform/tracing/TraceEvent.h"
#include "tint/tint.h"

namespace dawn::native::deko3d {
namespace {

#define DKSH_COMPILATION_REQUEST_MEMBERS(X)                                          \
    X(ShaderModuleBase::ShaderModuleHash, shaderModuleHash)                          \
    X(UnsafeUnserializedValue<ShaderModuleBase::ScopedUseTintProgram>, inputProgram) \
    X(SingleShaderStage, stage)                                                      \
    X(LimitsForCompilationRequest, limits)                                           \
    X(UnsafeUnserializedValue<LimitsForCompilationRequest>, adapterSupportedLimits)  \
    X(tint::glsl::writer::Options, tintOptions)                                      \
    X(UnsafeUnserializedValue<dawn::platform::Platform*>, platform)
DAWN_MAKE_CACHE_REQUEST(DkshCompilationRequest, DKSH_COMPILATION_REQUEST_MEMBERS);
#undef DKSH_COMPILATION_REQUEST_MEMBERS

#define DKSH_COMPILATION_MEMBERS(X) \
    X(std::string, dksh)            \
    X(Extent3D, workgroupSize)
DAWN_SERIALIZABLE(struct, DkshCompilation, DKSH_COMPILATION_MEMBERS) {
    static ResultOrError<DkshCompilation> FromValidatedBlob(Blob blob);
};
#undef DKSH_COMPILATION_MEMBERS

// static
ResultOrError<DkshCompilation> DkshCompilation::FromValidatedBlob(Blob blob) {
    DkshCompilation result;
    DAWN_TRY_ASSIGN(result, FromBlob(std::move(blob)));
    DAWN_INVALID_IF(result.dksh.size() < 4 || result.dksh.compare(0, 4, "DKSH") != 0,
                    "Cached DkshCompilation result is not a DKSH module");
    return result;
}

// Returns information about the texture/sampler pairs used by the entry point. This is necessary
// because GL uses combined texture/sampler bindings while WGSL allows mixing and matching textures
// and samplers in the shader. GL also uses a placeholder sampler to use with textures when they
// aren't combined with a sampler in the WGSL.
//
// Another subtlety is that Dawn uses pre-remapping BindingPoints when referring to bindings while
// the Tint GLSL writer uses post-remapping BindingPoints.
void GenerateCombinedSamplerInfo(
    const EntryPointMetadata& metadata,
    const tint::Bindings& bindings,
    const PipelineLayout* layout,
    std::vector<CombinedSampler>* combinedSamplers,
    tint::glsl::writer::CombinedTextureSamplerInfo* samplerTextureToName,
    tint::BindingPoint* placeholder_sampler_bind_point) {
    // Helper to avoid duplicated logic for when a CombinedSampler is determined. It takes a bunch
    // of information for both the texture and the sampler and translate to what Dawn/Tint need.
    struct CombinedBindingInfo {
        // Dawn takes BindGroupIndex + BindingIndex.
        BindGroupIndex group;
        BindingIndex index;
        BindingIndex shaderArraySize = BindingIndex(1u);

        // Tint takes the post-remapping binding point.
        tint::BindingPoint remappedBinding;
    };
    auto AddCombinedSampler = [&](CombinedBindingInfo texture,
                                  std::optional<CombinedBindingInfo> sampler,
                                  bool isPlane1 = false) {
        // Reflect to the pipeline the combination with BindGroupIndex + BindingIndex in that BGL.
        CombinedSampler combinedSampler = {{
            .samplerLocation = std::nullopt,
            .textureLocation = {{
                .group = texture.group,
                .index = texture.index,
                .shaderArraySize = texture.shaderArraySize,
            }},
        }};
        if (sampler.has_value()) {
            combinedSampler.samplerLocation = {{{
                .group = sampler->group,
                .index = sampler->index,
                .shaderArraySize = sampler->shaderArraySize,
            }}};
        }
        combinedSamplers->push_back(combinedSampler);

        // Let Tint know to generate a new GLSL sampler for this combination.
        tint::BindingPoint samplerRemapped = *placeholder_sampler_bind_point;
        if (sampler.has_value()) {
            samplerRemapped = {0, sampler->remappedBinding.binding};
        }
        samplerTextureToName->emplace(
            tint::glsl::writer::CombinedTextureSamplerPair{
                {0, texture.remappedBinding.binding}, samplerRemapped, isPlane1},
            combinedSampler.GetName());
    };

    for (const auto& use : metadata.samplerAndNonSamplerTexturePairs) {
        // Replace uses of the placeholder sampler with its actual binding point.
        std::optional<CombinedBindingInfo> sampler = std::nullopt;
        if (use.sampler != EntryPointMetadata::nonSamplerBindingPoint) {
            const BindGroupLayoutInternalBase* bgl = layout->GetBindGroupLayout(use.sampler.group);
            sampler = {
                .group = use.sampler.group,
                .index = bgl->AsBindingIndex(bgl->GetBindingMap().at(use.sampler.binding)),
                .remappedBinding = bindings.sampler.at(ToTint(use.sampler)),
            };
        }

        // Tint reflection returns information about uses of both regular textures and sampled
        // textures so we need to differentiate both cases here.
        const BindGroupLayoutInternalBase* bgl = layout->GetBindGroupLayout(use.texture.group);
        APIBindingIndex textureAPIIndex = bgl->GetBindingMap().at(use.texture.binding);
        const auto& bindingInfo = bgl->GetAPIBindingInfo(textureAPIIndex);

        // The easy case is when a regular texture is being handled.
        if (std::holds_alternative<TextureBindingInfo>(bindingInfo.bindingLayout)) {
            CombinedBindingInfo texture = {
                .group = use.texture.group,
                .index = bgl->AsBindingIndex(textureAPIIndex),
                .shaderArraySize =
                    metadata.bindings.at(use.texture.group).at(use.texture.binding).arraySize,
                .remappedBinding = bindings.texture.at(ToTint(use.texture)),
            };
            AddCombinedSampler(texture, sampler);
            continue;
        }

        // This is an external texture, add planes individually.
        const auto& bindingLayout = std::get<ExternalTextureBindingInfo>(bindingInfo.bindingLayout);

        auto& tint_data = bindings.external_texture.at(ToTint(use.texture));
        DAWN_ASSERT(std::holds_alternative<tint::ExternalMultiplanarTexture>(tint_data));

        tint::ExternalMultiplanarTexture mp_data =
            std::get<tint::ExternalMultiplanarTexture>(tint_data);

        CombinedBindingInfo plane0 = {
            .group = use.texture.group,
            .index = bindingLayout.plane0,
            .remappedBinding = mp_data.plane0,
        };
        AddCombinedSampler(plane0, sampler, false);

        CombinedBindingInfo plane1 = {
            .group = use.texture.group,
            .index = bindingLayout.plane1,
            .remappedBinding = mp_data.plane1,
        };
        AddCombinedSampler(plane1, sampler, true);
    }
}

tint::Bindings RemapBindings(const PipelineLayout* layout, SingleShaderStage stage) {
    return GenerateBindingRemapping(layout, stage, [&](BindGroupIndex group, BindingIndex index) {
        return tint::BindingPoint{
            .group = 0,
            .binding = uint32_t(layout->GetBindingIndexInfo()[group][index]),
        };
    });
}

constexpr tint::BindingPoint kPlaceholderSamplerBindPoint = {
    .group = static_cast<uint32_t>(kMaxBindGroups),
    .binding = 0,
};

}  // namespace

bool operator<(const CombinedSamplerElement& a, const CombinedSamplerElement& b) {
    return std::tie(a.group, a.index, a.shaderArraySize) <
           std::tie(b.group, b.index, b.shaderArraySize);
}

bool operator<(const CombinedSampler& a, const CombinedSampler& b) {
    return std::tie(a.samplerLocation, a.textureLocation) <
           std::tie(b.samplerLocation, b.textureLocation);
}

std::string CombinedSampler::GetName() const {
    std::ostringstream o;
    o << "dawn_combined";
    if (!samplerLocation) {
        o << "_placeholder_sampler";
    } else {
        o << "_" << static_cast<uint32_t>(samplerLocation->group) << "_"
          << static_cast<uint32_t>(samplerLocation->index);
    }
    o << "_with_" << static_cast<uint32_t>(textureLocation.group) << "_"
      << static_cast<uint32_t>(textureLocation.index);
    return o.str();
}

// static
ResultOrError<Ref<ShaderModule>> ShaderModule::Create(
    Device* device,
    const UnpackedPtr<ShaderModuleDescriptor>& descriptor,
    const std::vector<tint::wgsl::Extension>& internalExtensions) {
    Ref<ShaderModule> shader = AcquireRef(new ShaderModule(device, descriptor, internalExtensions));
    shader->Initialize();
    return shader;
}

ShaderModule::ShaderModule(Device* device,
                           const UnpackedPtr<ShaderModuleDescriptor>& descriptor,
                           std::vector<tint::wgsl::Extension> internalExtensions)
    : ShaderModuleBase(device, descriptor, std::move(internalExtensions)) {}

std::vector<CombinedSampler> ShaderModule::GetCombinedSamplers(
    const ProgrammableStage& programmableStage,
    const PipelineLayout* layout) {
    const EntryPointMetadata& metadata = GetEntryPoint(programmableStage.entryPoint);
    const SingleShaderStage stage = metadata.stage;
    tint::Bindings bindings = RemapBindings(layout, stage);
    std::vector<CombinedSampler> combinedSamplers;
    tint::glsl::writer::CombinedTextureSamplerInfo names;
    tint::BindingPoint placeholder = kPlaceholderSamplerBindPoint;
    GenerateCombinedSamplerInfo(metadata, bindings, layout, &combinedSamplers, &names, &placeholder);
    return combinedSamplers;
}

ResultOrError<std::string> ShaderModule::CompileShader(const ProgrammableStage& programmableStage,
                                                       SingleShaderStage stage,
                                                       const PipelineLayout* layout,
                                                       const TextureUnitMap& textureUnits,
                                                       Extent3D* workgroupSize) {
    TRACE_EVENT0(GetDevice()->GetPlatform(), General, "TranslateToDKSH");

    DkshCompilationRequest req = {};
    req.shaderModuleHash = GetHash();
    req.inputProgram = UnsafeUnserializedValue(UseTintProgram());

    const EntryPointMetadata& metadata = GetEntryPoint(programmableStage.entryPoint);
    tint::Bindings bindings = RemapBindings(layout, stage);

    req.tintOptions.placeholder_sampler_bind_point = kPlaceholderSamplerBindPoint;
    std::vector<CombinedSampler> combinedSamplers;
    GenerateCombinedSamplerInfo(metadata, bindings, layout, &combinedSamplers,
                                &req.tintOptions.sampler_texture_to_name,
                                &req.tintOptions.placeholder_sampler_bind_point);
    // Every pair gets the pipeline-wide texture unit the command recorder binds it to.
    for (const auto& [pair, name] : req.tintOptions.sampler_texture_to_name) {
        for (const CombinedSampler& combined : combinedSamplers) {
            if (combined.GetName() == name) {
                req.tintOptions.sampler_texture_to_binding[pair] = textureUnits.at(combined);
                break;
            }
        }
    }

    req.stage = stage;
    req.limits = LimitsForCompilationRequest::Create(GetDevice()->GetLimits().v1);
    req.adapterSupportedLimits = UnsafeUnserializedValue(
        LimitsForCompilationRequest::Create(GetDevice()->GetAdapter()->GetLimits().v1));
    req.platform = UnsafeUnserializedValue(GetDevice()->GetPlatform());

    req.tintOptions.entry_point_name = programmableStage.entryPoint;
    req.tintOptions.version =
        tint::glsl::writer::Version(tint::glsl::writer::Version::Standard::kDesktop, 4, 6);
    req.tintOptions.deko3d_conventions = true;
    req.tintOptions.substitute_overrides_config = {
        .map = BuildSubstituteOverridesTransformConfig(programmableStage),
    };
    req.tintOptions.disable_robustness = !GetDevice()->IsRobustnessEnabled();
    req.tintOptions.disable_workgroup_init =
        GetDevice()->IsToggleEnabled(Toggle::DisableWorkgroupInit);
    req.tintOptions.strip_all_names = !GetDevice()->IsToggleEnabled(Toggle::DisableSymbolRenaming);
    req.tintOptions.bindings = std::move(bindings);
    req.tintOptions.disable_polyfill_integer_div_mod =
        GetDevice()->IsToggleEnabled(Toggle::DisablePolyfillsOnIntegerDivisonAndModulo);
    req.tintOptions.disable_integer_range_analysis =
        !GetDevice()->IsToggleEnabled(Toggle::EnableIntegerRangeAnalysisInRobustness);
    req.tintOptions.use_uniform_buffers = true;

    CacheResult<DkshCompilation> compilationResult;
    DAWN_TRY_LOAD_OR_RUN(
        compilationResult, GetDevice(), std::move(req), DkshCompilation::FromValidatedBlob,
        [](DkshCompilationRequest r) -> ResultOrError<DkshCompilation> {
            auto shaderModule = r.inputProgram.UnsafeGetValue();
            auto inputProgram = shaderModule->GetTintProgram();
            auto device = shaderModule->GetDevice();
            tint::wgsl::reader::IROptions irOptions{
                .dump_ir_when_validating = device->IsToggleEnabled(Toggle::DumpTintIR),
                .enable_validation_asserts =
                    device->IsToggleEnabled(Toggle::EnableTintIRValidationAsserts),
            };
            auto ir = tint::wgsl::reader::ProgramToLoweredIR(inputProgram->program, irOptions);
            DAWN_INVALID_IF(ir != tint::Success, "An error occurred while generating Tint IR\n%s",
                            ir.Failure().reason);

            auto result = tint::glsl::writer::Generate(ir.Get(), r.tintOptions);
            DAWN_INVALID_IF(result != tint::Success,
                            "An error occurred while generating GLSL:\n%s",
                            result.Failure().reason);

            DkshCompilation compResult;
            std::string error;
            if (!CompileGlslToDksh(r.stage, result->glsl, &compResult.dksh, &error)) {
                return DAWN_VALIDATION_ERROR("uam failed to compile GLSL for deko3d:\n%s\n%s",
                                             error, result->glsl);
            }
            if (r.stage == SingleShaderStage::Compute) {
                DAWN_TRY_ASSIGN(compResult.workgroupSize,
                                ValidateComputeStageWorkgroupSize(
                                    result->workgroup_info, /*usesSubgroupMatrix=*/false,
                                    /*maxSubgroupSize=*/0, r.limits,
                                    r.adapterSupportedLimits.UnsafeGetValue()));
            }
            return compResult;
        },
        "Deko3d.CompileShaderToDKSH");

    if (workgroupSize != nullptr) {
        *workgroupSize = compilationResult->workgroupSize;
    }
    std::string dksh = compilationResult->dksh;
    GetDevice()->GetBlobCache()->EnsureStored(compilationResult);
    return dksh;
}

}  // namespace dawn::native::deko3d
