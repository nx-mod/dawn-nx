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

#ifndef SRC_DAWN_NATIVE_DEKO3D_TEXTUREDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_TEXTUREDEKO3D_H_

#include <deko3d.h>

#include "src/dawn/native/Texture.h"
#include "src/dawn/native/deko3d/ImageAllocationDeko3d.h"

namespace dawn::native::deko3d {

class Device;

class Texture final : public TextureBase {
  public:
    static ResultOrError<Ref<Texture>> Create(Device* device,
                                              const UnpackedPtr<TextureDescriptor>& descriptor);
    // Wraps an image owned elsewhere (a swapchain framebuffer).
    static Ref<Texture> CreateWrapping(Device* device,
                                       const UnpackedPtr<TextureDescriptor>& descriptor,
                                       DkImage const* image);

    DkImage const* GetDkImage() const { return mImage; }

    // Records clears into the device's pending commands for uninitialized subresources.
    MaybeError EnsureSubresourceContentInitialized(const SubresourceRange& range);

  private:
    Texture(Device* device, const UnpackedPtr<TextureDescriptor>& descriptor);
    ~Texture() override;

    MaybeError Initialize();
    void DestroyImpl(DestroyReason reason) override;
    MaybeError ClearTexture(const SubresourceRange& range, TextureBase::ClearValue clearValue);

    DkImage mOwnedImage;
    DkImage const* mImage = nullptr;
    ImageAllocation mAllocation;
};

class TextureView final : public TextureViewBase {
  public:
    static ResultOrError<Ref<TextureView>> Create(
        TextureBase* texture,
        const UnpackedPtr<TextureViewDescriptor>& descriptor);

    // View over the whole view subresource range, for sampling and render targets.
    const DkImageView& GetDkImageView() const { return mView; }
    // View of a single mip level and layer (render attachment of a 3D slice or array layer).
    DkImageView GetDkImageViewForAttachment(uint32_t depthSlice) const;

    // Index in the device's image descriptor set; allocated on first binding.
    ResultOrError<uint32_t> GetImageDescriptorId();

  private:
    TextureView(TextureBase* texture, const UnpackedPtr<TextureViewDescriptor>& descriptor);
    ~TextureView() override;
    void DestroyImpl(DestroyReason reason) override;

    DkImageView mView;
    uint32_t mImageDescriptorId = UINT32_MAX;
};

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_TEXTUREDEKO3D_H_
