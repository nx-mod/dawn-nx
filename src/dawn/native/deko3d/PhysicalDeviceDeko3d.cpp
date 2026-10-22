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

#include "src/dawn/native/deko3d/PhysicalDeviceDeko3d.h"

#include <utility>
#include <vector>

#include "src/dawn/native/BackendConnection.h"
#include "src/dawn/native/Instance.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"

namespace dawn::native::deko3d {

// static
ResultOrError<Ref<PhysicalDevice>> PhysicalDevice::Create() {
    Ref<PhysicalDevice> physicalDevice = AcquireRef(new PhysicalDevice());
    DAWN_TRY(physicalDevice->Initialize());
    return physicalDevice;
}

PhysicalDevice::PhysicalDevice() : PhysicalDeviceBase(wgpu::BackendType::Deko3d) {
    mVendorId = 0x10DE;  // NVIDIA
    mDeviceId = 0;
    mName = "NVIDIA Tegra X1 (deko3d)";
    mDriverDescription = "deko3d";
    mAdapterType = wgpu::AdapterType::IntegratedGPU;
}

bool PhysicalDevice::SupportsExternalImages() const {
    return false;
}

bool PhysicalDevice::SupportsFeatureLevel(wgpu::FeatureLevel, InstanceBase*) const {
    return true;
}

ResultOrError<PhysicalDeviceSurfaceCapabilities> PhysicalDevice::GetSurfaceCapabilities(
    InstanceBase*,
    const Surface*) const {
    PhysicalDeviceSurfaceCapabilities capabilities;
    capabilities.usages = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                          wgpu::TextureUsage::TextureBinding;
    capabilities.formats = {wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureFormat::RGBA8UnormSrgb};
    capabilities.presentModes = {wgpu::PresentMode::Fifo, wgpu::PresentMode::Immediate};
    capabilities.alphaModes = {wgpu::CompositeAlphaMode::Opaque};
    return capabilities;
}

MaybeError PhysicalDevice::InitializeImpl() {
    return {};
}

void PhysicalDevice::InitializeSupportedFeaturesImpl() {
    EnableFeature(Feature::TextureCompressionBC);
    EnableFeature(Feature::ImplicitDeviceSynchronization);
    EnableFeature(Feature::Float32Filterable);
    EnableFeature(Feature::DepthClipControl);
    EnableFeature(Feature::IndirectFirstInstance);
    EnableFeature(Feature::RG11B10UfloatRenderable);
}

MaybeError PhysicalDevice::InitializeSupportedLimitsImpl(CombinedLimits* limits) {
    GetDefaultLimitsForSupportedFeatureLevel(limits);
    limits->v1.maxTextureDimension1D = 16384;
    limits->v1.maxTextureDimension2D = 16384;
    limits->v1.maxTextureDimension3D = 2048;
    limits->v1.maxTextureArrayLayers = 2048;
    return {};
}

FeatureValidationResult PhysicalDevice::ValidateFeatureSupportedWithTogglesImpl(
    wgpu::FeatureName,
    const TogglesState&) const {
    return {};
}

void PhysicalDevice::SetupBackendAdapterToggles(dawn::platform::Platform*,
                                                TogglesState*) const {}

void PhysicalDevice::SetupBackendDeviceToggles(dawn::platform::Platform*,
                                               TogglesState* deviceToggles) const {
    // deko3d's GLSL comes from Tint's desktop GLSL writer, like the GL backend.
    deviceToggles->Default(Toggle::DisableBaseVertex, false);
}

ResultOrError<Ref<DeviceBase>> PhysicalDevice::CreateDeviceImpl(
    AdapterBase* adapter,
    const UnpackedPtr<DeviceDescriptor>& descriptor,
    const TogglesState& deviceToggles,
    Ref<DeviceBase::DeviceLostEvent>&& lostEvent) {
    return Device::Create(adapter, descriptor, deviceToggles, std::move(lostEvent));
}

void PhysicalDevice::PopulateBackendProperties(UnpackedPtr<AdapterInfo>&, const TogglesState&) const {}

namespace {

class Backend : public BackendConnection {
  public:
    explicit Backend(InstanceBase* instance)
        : BackendConnection(instance, wgpu::BackendType::Deko3d) {}

    std::vector<Ref<PhysicalDeviceBase>> DiscoverPhysicalDevices(
        const UnpackedPtr<RequestAdapterOptions>& options) override {
        if (options->forceFallbackAdapter) {
            return {};
        }
        if (mPhysicalDevice == nullptr) {
            ResultOrError<Ref<PhysicalDevice>> created = PhysicalDevice::Create();
            if (created.IsError()) {
                GetInstance()->ConsumedErrorAndWarnOnce(created.AcquireError());
                return {};
            }
            mPhysicalDevice = created.AcquireSuccess();
        }
        return {mPhysicalDevice};
    }

  private:
    Ref<PhysicalDevice> mPhysicalDevice;
};

}  // namespace

BackendConnection* Connect(InstanceBase* instance) {
    return new Backend(instance);
}

}  // namespace dawn::native::deko3d
