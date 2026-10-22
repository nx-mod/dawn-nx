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

#ifndef SRC_DAWN_NATIVE_DEKO3D_SWAPCHAINDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_SWAPCHAINDEKO3D_H_

#include <deko3d.h>

#include "src/dawn/native/SwapChain.h"

namespace dawn::native::deko3d {

class Device;
class Texture;

// Renders straight into the DkSwapchain framebuffers: GetCurrentTexture acquires an image and
// wraps it, Present submits pending commands and queues the image for display.
class SwapChain final : public SwapChainBase {
  public:
    static ResultOrError<Ref<SwapChain>> Create(Device* device,
                                                Surface* surface,
                                                SwapChainBase* previousSwapChain,
                                                const SurfaceConfiguration* config);
    ~SwapChain() override;

  private:
    using SwapChainBase::SwapChainBase;
    MaybeError Initialize(SwapChainBase* previousSwapChain);

    MaybeError PresentImpl() override;
    ResultOrError<SwapChainTextureInfo> GetCurrentTextureImpl() override;
    void DetachFromSurfaceImpl() override;

    static constexpr uint32_t kFramebufferCount = 3;
    DkMemBlock mFramebufferMemory = nullptr;
    DkImage mFramebuffers[kFramebufferCount];
    DkSwapchain mDkSwapchain = nullptr;
    int mAcquiredSlot = -1;
    Ref<Texture> mTexture;
};

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_SWAPCHAINDEKO3D_H_
