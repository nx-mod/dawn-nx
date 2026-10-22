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

#include "src/dawn/native/deko3d/SamplerDeko3d.h"

#include <algorithm>

#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/UtilsDeko3d.h"

namespace dawn::native::deko3d {

// static
ResultOrError<Ref<Sampler>> Sampler::Create(Device* device, const SamplerDescriptor* descriptor) {
    Ref<Sampler> sampler = AcquireRef(new Sampler(device, descriptor));
    DAWN_TRY(sampler->Initialize(descriptor));
    return sampler;
}

Sampler::Sampler(Device* device, const SamplerDescriptor* descriptor)
    : SamplerBase(device, descriptor) {}

Sampler::~Sampler() = default;

MaybeError Sampler::Initialize(const SamplerDescriptor* descriptor) {
    DkSampler dkSampler;
    dkSamplerDefaults(&dkSampler);
    dkSampler.minFilter = ToDkFilter(descriptor->minFilter);
    dkSampler.magFilter = ToDkFilter(descriptor->magFilter);
    dkSampler.mipFilter = ToDkMipFilter(descriptor->mipmapFilter);
    dkSampler.wrapMode[0] = ToDkWrapMode(descriptor->addressModeU);
    dkSampler.wrapMode[1] = ToDkWrapMode(descriptor->addressModeV);
    dkSampler.wrapMode[2] = ToDkWrapMode(descriptor->addressModeW);
    dkSampler.lodClampMin = descriptor->lodMinClamp;
    dkSampler.lodClampMax = descriptor->lodMaxClamp;
    if (descriptor->compare != wgpu::CompareFunction::Undefined) {
        dkSampler.compareEnable = true;
        dkSampler.compareOp = ToDkCompareOp(descriptor->compare);
    }
    dkSampler.maxAnisotropy = static_cast<float>(std::max<uint16_t>(GetMaxAnisotropy(), 1));

    DAWN_TRY_ASSIGN(mDescriptorId, ToBackend(GetDevice())->AllocateSamplerDescriptor(dkSampler));
    return {};
}

void Sampler::DestroyImpl(DestroyReason reason) {
    if (mDescriptorId != UINT32_MAX) {
        ToBackend(GetDevice())->ReleaseSamplerDescriptor(mDescriptorId);
        mDescriptorId = UINT32_MAX;
    }
    SamplerBase::DestroyImpl(reason);
}

}  // namespace dawn::native::deko3d
