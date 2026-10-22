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

#include "src/dawn/native/deko3d/TextureDeko3d.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "src/dawn/native/Format.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/UtilsDeko3d.h"

namespace dawn::native::deko3d {

namespace {

DkImageType ImageTypeForTexture(const TextureBase* texture) {
    const bool multisampled = texture->GetSampleCount() > 1;
    switch (texture->GetDimension()) {
        case wgpu::TextureDimension::e1D:
            return texture->GetArrayLayers() > 1 ? DkImageType_1DArray : DkImageType_1D;
        case wgpu::TextureDimension::e3D:
            return DkImageType_3D;
        case wgpu::TextureDimension::e2D:
        case wgpu::TextureDimension::Undefined:
        default:
            if (texture->GetArrayLayers() > 1) {
                return multisampled ? DkImageType_2DMSArray : DkImageType_2DArray;
            }
            return multisampled ? DkImageType_2DMS : DkImageType_2D;
    }
}

DkImageType ImageTypeForViewDimension(wgpu::TextureViewDimension dimension, bool multisampled) {
    switch (dimension) {
        case wgpu::TextureViewDimension::e1D:
            return DkImageType_1D;
        case wgpu::TextureViewDimension::e2DArray:
            return multisampled ? DkImageType_2DMSArray : DkImageType_2DArray;
        case wgpu::TextureViewDimension::Cube:
            return DkImageType_Cubemap;
        case wgpu::TextureViewDimension::CubeArray:
            return DkImageType_CubemapArray;
        case wgpu::TextureViewDimension::e3D:
            return DkImageType_3D;
        case wgpu::TextureViewDimension::e2D:
        case wgpu::TextureViewDimension::Undefined:
        default:
            return multisampled ? DkImageType_2DMS : DkImageType_2D;
    }
}

DkMsMode ToDkMsMode(uint32_t sampleCount) {
    switch (sampleCount) {
        case 2:
            return DkMsMode_2x;
        case 4:
            return DkMsMode_4x;
        case 8:
            return DkMsMode_8x;
        default:
            return DkMsMode_1x;
    }
}

}  // namespace

// Texture

// static
ResultOrError<Ref<Texture>> Texture::Create(Device* device,
                                            const UnpackedPtr<TextureDescriptor>& descriptor) {
    Ref<Texture> texture = AcquireRef(new Texture(device, descriptor));
    DAWN_TRY(texture->Initialize());
    return std::move(texture);
}

// static
Ref<Texture> Texture::CreateWrapping(Device* device,
                                     const UnpackedPtr<TextureDescriptor>& descriptor,
                                     DkImage const* image) {
    Ref<Texture> texture = AcquireRef(new Texture(device, descriptor));
    texture->mImage = image;
    // Swapchain framebuffers are fully overwritten or cleared by the render pass using them.
    texture->SetIsSubresourceContentInitialized(true, texture->GetAllSubresources());
    return texture;
}

Texture::Texture(Device* device, const UnpackedPtr<TextureDescriptor>& descriptor)
    : TextureBase(device, descriptor) {}

Texture::~Texture() = default;

MaybeError Texture::Initialize() {
    Device* device = ToBackend(GetDevice());
    const DkImageFormat dkFormat = ToDkImageFormat(GetFormat().format);
    DAWN_INVALID_IF(dkFormat == DkImageFormat_None, "Texture format %s is not supported by deko3d.",
                    GetFormat().format);

    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, device->GetDkDevice());
    maker.type = ImageTypeForTexture(this);
    maker.format = dkFormat;
    maker.msMode = ToDkMsMode(GetSampleCount());
    maker.mipLevels = GetNumMipLevels();
    const Extent3D size = GetBaseSize();
    maker.dimensions[0] = size.width;
    maker.dimensions[1] = GetDimension() == wgpu::TextureDimension::e1D ? 1 : size.height;
    maker.dimensions[2] = size.depthOrArrayLayers;
    if (GetInternalUsage() & wgpu::TextureUsage::RenderAttachment) {
        maker.flags |= DkImageFlags_UsageRender;
    }
    if (GetInternalUsage() & wgpu::TextureUsage::StorageBinding) {
        maker.flags |= DkImageFlags_UsageLoadStore;
    }
    // Lazy clears of non-renderable textures go through a buffer copy, which needs no flag;
    // renderable formats get the render usage so zero-initialisation can use a fast clear.
    if (GetFormat().IsRenderable()) {
        maker.flags |= DkImageFlags_UsageRender;
    }
    maker.flags |= DkImageFlags_HwCompression;

    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    const uint64_t imageSize = dkImageLayoutGetSize(&layout);
    const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
    const uint64_t blockSize = Align(imageSize, uint64_t(std::max<uint32_t>(alignment, 1)));
    DAWN_INVALID_IF(blockSize > UINT32_MAX, "Texture is too large for deko3d.");

    MaybeError allocated =
        device->AllocateImageMemory(static_cast<uint32_t>(blockSize), alignment, &mAllocation);
    if (allocated.IsError()) {
        device->ReportMemory("texture alloc failed");
        std::fprintf(stderr, "[deko3d-mem] failed texture size=%llu %ux%u\n",
                     (unsigned long long)blockSize, APIGetWidth(), APIGetHeight());
        return allocated;
    }
    GetMemStats().textureBytes += mAllocation.size;
    GetMemStats().textureCount++;
    dkImageInitialize(&mOwnedImage, &layout, mAllocation.block, mAllocation.offset);
    mImage = &mOwnedImage;

    if (device->IsToggleEnabled(Toggle::NonzeroClearResourcesOnCreationForTesting)) {
        DAWN_TRY(ClearTexture(GetAllSubresources(), TextureBase::ClearValue::NonZero));
    }
    return {};
}

void Texture::DestroyImpl(DestroyReason reason) {
    TextureBase::DestroyImpl(reason);
    if (mAllocation.block != nullptr) {
        GetMemStats().textureBytes -= mAllocation.size;
        GetMemStats().textureCount--;
        ToBackend(GetDevice())->FreeImageMemoryWhenIdle(mAllocation);
        mAllocation = {};
    }
    mImage = nullptr;
}

MaybeError Texture::ClearTexture(const SubresourceRange& range,
                                 TextureBase::ClearValue clearValue) {
    Device* device = ToBackend(GetDevice());
    DkCmdBuf cmdBuf = device->GetPendingCommandBuffer();
    const Format& format = GetFormat();
    const uint8_t fillByte = clearValue == TextureBase::ClearValue::Zero ? 0 : 1;
    const float fillFloat = clearValue == TextureBase::ClearValue::Zero ? 0.0f : 1.0f;

    for (uint32_t level = range.baseMipLevel; level < range.baseMipLevel + range.levelCount;
         ++level) {
        const Extent3D mipSize = GetMipLevelSingleSubresourcePhysicalSize(level, range.aspects);
        for (uint32_t layer = range.baseArrayLayer;
             layer < range.baseArrayLayer + range.layerCount; ++layer) {
            if (clearValue == TextureBase::ClearValue::Zero &&
                IsSubresourceContentInitialized(
                    SubresourceRange::SingleMipAndLayer(level, layer, range.aspects))) {
                continue;
            }
            DkImageView view;
            dkImageViewDefaults(&view, mImage);
            view.mipLevelOffset = static_cast<uint8_t>(level);
            view.mipLevelCount = 1;
            if (GetDimension() != wgpu::TextureDimension::e3D) {
                view.layerOffset = static_cast<uint16_t>(layer);
                view.layerCount = 1;
            }

            if (format.IsRenderable()) {
                DkScissor scissor = {0, 0, mipSize.width, mipSize.height};
                if (format.HasDepthOrStencil()) {
                    dkCmdBufBindRenderTargets(cmdBuf, nullptr, 0, &view);
                    dkCmdBufSetScissors(cmdBuf, 0, &scissor, 1);
                    dkCmdBufClearDepthStencil(cmdBuf, format.HasDepth(), fillFloat,
                                              format.HasStencil() ? 0xFF : 0, fillByte);
                } else {
                    DkImageView const* colorTargets[] = {&view};
                    dkCmdBufBindRenderTargets(cmdBuf, colorTargets, 1, nullptr);
                    dkCmdBufSetScissors(cmdBuf, 0, &scissor, 1);
                    dkCmdBufClearColorFloat(cmdBuf, 0, DkColorMask_RGBA, fillFloat, fillFloat,
                                            fillFloat, fillFloat);
                }
            } else {
                // Non-renderable (e.g. compressed) formats: copy a filled staging buffer.
                const TexelBlockInfo& block = format.GetAspectInfo(range.aspects).block;
                const uint64_t blocksX = (mipSize.width + block.width - 1) / block.width;
                const uint64_t blocksY = (mipSize.height + block.height - 1) / block.height;
                const uint64_t depth =
                    GetDimension() == wgpu::TextureDimension::e3D ? mipSize.depthOrArrayLayers : 1;
                const uint64_t byteSize = blocksX * blocksY * depth * block.byteSize;
                uint8_t* data = nullptr;
                DkGpuAddr gpuAddr = 0;
                DAWN_TRY(device->AllocateUploadMemory(byteSize, &data, &gpuAddr));
                std::memset(data, fillByte, byteSize);
                DkCopyBuf src = {gpuAddr, 0, 0};
                DkImageRect rect = {0, 0, 0, mipSize.width, mipSize.height,
                                    static_cast<uint32_t>(depth)};
                dkCmdBufCopyBufferToImage(cmdBuf, &src, &view, &rect, 0);
            }
            if (kDeko3dTrace) {
                char what[160];
                std::snprintf(what, sizeof(what),
                              "ClearTexture fmt=%d renderable=%d level=%u layer=%u size=%ux%ux%u",
                              int(format.format), int(format.IsRenderable()), level, layer,
                              mipSize.width, mipSize.height, mipSize.depthOrArrayLayers);
                device->TraceFlush(what);
            }
        }
    }

    if (clearValue == TextureBase::ClearValue::Zero) {
        SetIsSubresourceContentInitialized(true, range);
        device->IncrementLazyClearCountForTesting();
    }
    return {};
}

MaybeError Texture::EnsureSubresourceContentInitialized(const SubresourceRange& range) {
    if (!GetDevice()->IsToggleEnabled(Toggle::LazyClearResourceOnFirstUse)) {
        return {};
    }
    if (!IsSubresourceContentInitialized(range)) {
        DAWN_TRY(ClearTexture(range, TextureBase::ClearValue::Zero));
    }
    return {};
}

// TextureView

// static
ResultOrError<Ref<TextureView>> TextureView::Create(
    TextureBase* texture,
    const UnpackedPtr<TextureViewDescriptor>& descriptor) {
    return AcquireRef(new TextureView(texture, descriptor));
}

TextureView::TextureView(TextureBase* texture, const UnpackedPtr<TextureViewDescriptor>& descriptor)
    : TextureViewBase(texture, descriptor) {
    Texture* backendTexture = ToBackend(texture);
    dkImageViewDefaults(&mView, backendTexture->GetDkImage());
    mView.type = ImageTypeForViewDimension(descriptor->dimension, texture->GetSampleCount() > 1);
    if (!GetFormat().HasDepthOrStencil() && GetFormat().format != texture->GetFormat().format) {
        mView.format = ToDkImageFormat(GetFormat().format);
    }
    if (GetAspects() == Aspect::Stencil) {
        mView.dsSource = DkDsSource_Stencil;
    }
    mView.mipLevelOffset = static_cast<uint8_t>(GetBaseMipLevel());
    mView.mipLevelCount = static_cast<uint8_t>(GetLevelCount());
    if (texture->GetDimension() != wgpu::TextureDimension::e3D) {
        mView.layerOffset = static_cast<uint16_t>(GetBaseArrayLayer());
        mView.layerCount = static_cast<uint16_t>(GetLayerCount());
    }
}

TextureView::~TextureView() = default;

DkImageView TextureView::GetDkImageViewForAttachment(uint32_t depthSlice) const {
    DkImageView view = mView;
    view.type = DkImageType_None;
    view.mipLevelCount = 1;
    if (GetTexture()->GetDimension() == wgpu::TextureDimension::e3D) {
        view.layerOffset = static_cast<uint16_t>(depthSlice);
        view.layerCount = 1;
    } else {
        view.layerCount = 1;
    }
    return view;
}

ResultOrError<uint32_t> TextureView::GetImageDescriptorId() {
    if (mImageDescriptorId == UINT32_MAX) {
        const bool storage = (GetUsage() & wgpu::TextureUsage::StorageBinding) != 0;
        DAWN_TRY_ASSIGN(mImageDescriptorId,
                        ToBackend(GetDevice())->AllocateImageDescriptor(mView, storage));
    }
    return mImageDescriptorId;
}

void TextureView::DestroyImpl(DestroyReason reason) {
    TextureViewBase::DestroyImpl(reason);
    if (mImageDescriptorId != UINT32_MAX) {
        ToBackend(GetDevice())->ReleaseImageDescriptor(mImageDescriptorId);
        mImageDescriptorId = UINT32_MAX;
    }
}

}  // namespace dawn::native::deko3d
