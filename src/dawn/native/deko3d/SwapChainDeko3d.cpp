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

#include "src/dawn/native/deko3d/SwapChainDeko3d.h"

#include <switch.h>

#include "src/dawn/native/Surface.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/QueueDeko3d.h"
#include "src/dawn/native/deko3d/TextureDeko3d.h"
#include "src/dawn/native/deko3d/UtilsDeko3d.h"

namespace dawn::native::deko3d {

// static
ResultOrError<Ref<SwapChain>> SwapChain::Create(Device* device,
                                                Surface* surface,
                                                SwapChainBase* previousSwapChain,
                                                const SurfaceConfiguration* config) {
    Ref<SwapChain> swapchain = AcquireRef(new SwapChain(device, surface, config));
    DAWN_TRY(swapchain->Initialize(previousSwapChain));
    return swapchain;
}

MaybeError SwapChain::Initialize(SwapChainBase* previousSwapChain) {
    if (previousSwapChain != nullptr) {
        DAWN_INVALID_IF(previousSwapChain->GetBackendType() != wgpu::BackendType::Deko3d,
                        "deko3d::SwapChain cannot switch between APIs");
        // A libnx window accepts one swapchain at a time.
        previousSwapChain->DetachFromSurface();
    }

    Device* device = ToBackend(GetDevice());
    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, device->GetDkDevice());
    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layoutMaker.format = ToDkImageFormat(GetFormat());
    layoutMaker.dimensions[0] = GetWidth();
    layoutMaker.dimensions[1] = GetHeight();
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layoutMaker);
    const uint32_t align = dkImageLayoutGetAlignment(&layout);
    const uint32_t imageSize = (dkImageLayoutGetSize(&layout) + align - 1) & ~(align - 1);

    DkMemBlockMaker memMaker;
    dkMemBlockMakerDefaults(&memMaker, device->GetDkDevice(), kFramebufferCount * imageSize);
    memMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    mFramebufferMemory = dkMemBlockCreate(&memMaker);
    DAWN_INVALID_IF(mFramebufferMemory == nullptr, "Allocating swapchain framebuffers failed");

    DkImage const* images[kFramebufferCount];
    for (uint32_t i = 0; i < kFramebufferCount; ++i) {
        dkImageInitialize(&mFramebuffers[i], &layout, mFramebufferMemory, i * imageSize);
        images[i] = &mFramebuffers[i];
    }

    DkSwapchainMaker swapchainMaker;
    dkSwapchainMakerDefaults(&swapchainMaker, device->GetDkDevice(),
                             static_cast<NWindow*>(GetSurface()->GetAndroidNativeWindow()), images,
                             kFramebufferCount);
    mDkSwapchain = dkSwapchainCreate(&swapchainMaker);
    DAWN_INVALID_IF(mDkSwapchain == nullptr, "dkSwapchainCreate failed");
    dkSwapchainSetSwapInterval(mDkSwapchain,
                               GetPresentMode() == wgpu::PresentMode::Immediate ? 0 : 1);
    return {};
}

SwapChain::~SwapChain() {
    DetachFromSurfaceImpl();
}

ResultOrError<SwapChainTextureInfo> SwapChain::GetCurrentTextureImpl() {
    Device* device = ToBackend(GetDevice());
    mAcquiredSlot = dkQueueAcquireImage(device->GetDkQueue(), mDkSwapchain);
    DAWN_INVALID_IF(mAcquiredSlot < 0, "dkQueueAcquireImage failed");

    TextureDescriptor textureDesc = GetSwapChainBaseTextureDescriptor(this);
    mTexture = Texture::CreateWrapping(device, Unpack(&textureDesc), &mFramebuffers[mAcquiredSlot]);

    SwapChainTextureInfo info;
    info.texture = mTexture;
    info.status = wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal;
    return info;
}

MaybeError SwapChain::PresentImpl() {
    Device* device = ToBackend(GetDevice());
    // Everything rendered into the image must be on the queue before it is presented.
    if (device->HasPendingCommands()) {
        DAWN_TRY(device->GetQueue()->SubmitPendingCommands());
    }
    static uint32_t sFrame = 0;
    ++sFrame;
    if (device->mDebugDumpFrame && mAcquiredSlot >= 0) {
        DkImageView view;
        dkImageViewDefaults(&view, &mFramebuffers[mAcquiredSlot]);
        device->DebugDumpImage(view, GetWidth(), GetHeight(), GetFormat(), "swapchain");
    }
    device->mDebugDumpFrame = kDeko3dDiag && sFrame >= 600 && sFrame % 300 == 0;
    if (kDeko3dDiag && sFrame % 300 == 0) {
        device->ReportMemory("periodic");
    }
    if (mAcquiredSlot >= 0) {
        dkQueuePresentImage(device->GetDkQueue(), mDkSwapchain, mAcquiredSlot);
        mAcquiredSlot = -1;
    }
    if (mTexture != nullptr) {
        mTexture->APIDestroy();
        mTexture = nullptr;
    }
    return {};
}

void SwapChain::DetachFromSurfaceImpl() {
    if (mTexture != nullptr) {
        mTexture->APIDestroy();
        mTexture = nullptr;
    }
    Device* device = ToBackend(GetDevice());
    if (mDkSwapchain != nullptr) {
        dkQueueWaitIdle(device->GetDkQueue());
        dkSwapchainDestroy(mDkSwapchain);
        mDkSwapchain = nullptr;
    }
    if (mFramebufferMemory != nullptr) {
        dkMemBlockDestroy(mFramebufferMemory);
        mFramebufferMemory = nullptr;
    }
}

}  // namespace dawn::native::deko3d
