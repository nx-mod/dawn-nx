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

#include "src/dawn/native/deko3d/CommandBufferDeko3d.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "src/dawn/common/MatchVariant.h"
#include "src/dawn/common/Range.h"
#include "src/dawn/native/BindGroup.h"
#include "src/dawn/native/BindGroupTracker.h"
#include "src/dawn/native/CommandEncoder.h"
#include "src/dawn/native/Commands.h"
#include "src/dawn/native/ExternalTexture.h"
#include "src/dawn/native/RenderBundle.h"
#include "src/dawn/native/deko3d/BindGroupDeko3d.h"
#include "src/dawn/native/deko3d/BufferDeko3d.h"
#include "src/dawn/native/deko3d/ComputePipelineDeko3d.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/PipelineLayoutDeko3d.h"
#include "src/dawn/native/deko3d/RenderPipelineDeko3d.h"
#include "src/dawn/native/deko3d/SamplerDeko3d.h"
#include "src/dawn/native/deko3d/TextureDeko3d.h"

namespace dawn::native::deko3d {

namespace {

constexpr uint32_t kMaxTextureUnits = 128;

TexelExtent3D ComputeTextureCopyExtent(const TextureCopy& textureCopy, const TexelExtent3D& copySize) {
    TexelExtent3D valid = copySize;
    const TextureBase* texture = textureCopy.texture.Get();
    TexelExtent3D virtualSize =
        texture->GetMipLevelSingleSubresourceVirtualSize(textureCopy.mipLevel, textureCopy.aspect);
    if (copySize.width > virtualSize.width - textureCopy.origin.x) {
        valid.width = virtualSize.width - textureCopy.origin.x;
    }
    if (copySize.height > virtualSize.height - textureCopy.origin.y) {
        valid.height = virtualSize.height - textureCopy.origin.y;
    }
    return valid;
}

DkImageView SingleSubresourceView(Texture* texture, uint32_t mipLevel, uint32_t layer, Aspect aspect) {
    DkImageView view;
    dkImageViewDefaults(&view, texture->GetDkImage());
    view.mipLevelOffset = static_cast<uint8_t>(mipLevel);
    view.mipLevelCount = 1;
    if (texture->GetDimension() != wgpu::TextureDimension::e3D) {
        view.layerOffset = static_cast<uint16_t>(layer);
        view.layerCount = 1;
    }
    if (aspect == Aspect::Stencil) {
        view.dsSource = DkDsSource_Stencil;
    }
    return view;
}

// Mirrors the frontend's bind groups into deko3d uniform/storage/texture/image slots. Texture
// units combine an image descriptor with a sampler descriptor, so both halves are tracked per unit
// and the unit's handle is rebuilt whenever either changes.
class BindGroupTracker : public BindGroupTrackerBase<false> {
  public:
    explicit BindGroupTracker(DkCmdBuf cmdBuf) : mCmdBuf(cmdBuf) {}

    void OnSetPipeline(RenderPipeline* pipeline) {
        BindGroupTrackerBase::OnSetPipeline(pipeline);
        mPipeline = pipeline;
        mStageMask = pipeline->GetDkStageMask();
        ResetUnits();
    }

    void OnSetPipeline(ComputePipeline* pipeline) {
        BindGroupTrackerBase::OnSetPipeline(pipeline);
        mPipeline = pipeline;
        mStageMask = pipeline->GetDkStageMask();
        ResetUnits();
    }

    MaybeError Apply() {
        BeforeApply();
        for (BindGroupIndex index : mDirtyBindGroupsObjectChangedOrIsDynamic) {
            DAWN_TRY(ApplyBindGroup(index, mBindGroups[index], GetDynamicOffsets(index)));
        }
        if (mPlaceholderSamplerPending) {
            if (Sampler* sampler = mPipeline->GetPlaceholderSampler()) {
                for (TextureUnit unit : mPipeline->GetPlaceholderSamplerUnits()) {
                    SetUnitSampler(uint32_t(unit), sampler->GetDescriptorId());
                }
            }
            mPlaceholderSamplerPending = false;
        }
        FlushUnits();
        AfterApply();
        return {};
    }

  private:
    void ResetUnits() {
        mPlaceholderSamplerPending = true;
        mDirtyUnitsBegin = kMaxTextureUnits;
        mDirtyUnitsEnd = 0;
    }

    void MarkUnitDirty(uint32_t unit) {
        mDirtyUnitsBegin = std::min(mDirtyUnitsBegin, unit);
        mDirtyUnitsEnd = std::max(mDirtyUnitsEnd, unit + 1);
    }

    void SetUnitImage(uint32_t unit, uint32_t imageId) {
        if (unit < kMaxTextureUnits) {
            mUnitImage[unit] = imageId;
            MarkUnitDirty(unit);
        }
    }

    void SetUnitSampler(uint32_t unit, uint32_t samplerId) {
        if (unit < kMaxTextureUnits) {
            mUnitSampler[unit] = samplerId;
            MarkUnitDirty(unit);
        }
    }

    void ForEachStage(wgpu::ShaderStage visibility, auto&& fn) {
        if ((visibility & wgpu::ShaderStage::Vertex) && (mStageMask & DkStageFlag_Vertex)) {
            fn(DkStage_Vertex);
        }
        if ((visibility & wgpu::ShaderStage::Fragment) && (mStageMask & DkStageFlag_Fragment)) {
            fn(DkStage_Fragment);
        }
        if ((visibility & wgpu::ShaderStage::Compute) && (mStageMask & DkStageFlag_Compute)) {
            fn(DkStage_Compute);
        }
    }

    void FlushUnits() {
        if (mDirtyUnitsBegin >= mDirtyUnitsEnd) {
            return;
        }
        std::array<DkResHandle, kMaxTextureUnits> handles;
        for (uint32_t unit = mDirtyUnitsBegin; unit < mDirtyUnitsEnd; ++unit) {
            handles[unit] = dkMakeTextureHandle(mUnitImage[unit], mUnitSampler[unit]);
        }
        const uint32_t count = mDirtyUnitsEnd - mDirtyUnitsBegin;
        ForEachStage(wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment | wgpu::ShaderStage::Compute,
                     [&](DkStage stage) {
                         dkCmdBufBindTextures(mCmdBuf, stage, mDirtyUnitsBegin,
                                              handles.data() + mDirtyUnitsBegin, count);
                     });
        mDirtyUnitsBegin = kMaxTextureUnits;
        mDirtyUnitsEnd = 0;
    }

    MaybeError ApplyBindGroup(BindGroupIndex groupIndex,
                              BindGroupBase* group,
                              const ityp::span<BindingIndex, uint32_t>& dynamicOffsets) {
        const auto& indices = ToBackend(mPipelineLayout)->GetBindingIndexInfo()[groupIndex];

        for (BindingIndex bindingIndex : Range(group->GetLayout()->GetBindingCount())) {
            const BindingInfo& bindingInfo = group->GetLayout()->GetBindingInfo(bindingIndex);
            if (bindingInfo.visibility == wgpu::ShaderStage::None) {
                continue;
            }
            FlatBindingIndex flatIndex = indices[bindingIndex];
            DAWN_TRY(MatchVariant(
                bindingInfo.bindingLayout,
                [&](const BufferBindingInfo& layout) -> MaybeError {
                    BufferBinding binding = group->GetBindingAsBufferBinding(bindingIndex);
                    Buffer* buffer = ToBackend(binding.buffer);
                    uint64_t offset = binding.offset;
                    if (layout.hasDynamicOffset) {
                        offset += uint64_t(dynamicOffsets[bindingIndex]);
                    }
                    DkBufExtents extents = {buffer->GetGpuAddr() + offset,
                                            static_cast<uint32_t>(binding.size)};
                    const bool uniform = layout.type == wgpu::BufferBindingType::Uniform;
                    if (uniform) {
                        extents.size = static_cast<uint32_t>(Align(binding.size, 16u));
                    }
                    ForEachStage(bindingInfo.visibility, [&](DkStage stage) {
                        if (uniform) {
                            dkCmdBufBindUniformBuffers(mCmdBuf, stage, uint32_t(flatIndex), &extents, 1);
                        } else {
                            dkCmdBufBindStorageBuffers(mCmdBuf, stage, uint32_t(flatIndex), &extents, 1);
                        }
                    });
                    buffer->TrackUsage();
                    return {};
                },
                [&](const StaticSamplerBindingInfo& layout) -> MaybeError {
                    Sampler* sampler = ToBackend(layout.sampler.Get());
                    for (TextureUnit unit : mPipeline->GetTextureUnitsForSampler(flatIndex)) {
                        SetUnitSampler(uint32_t(unit), sampler->GetDescriptorId());
                    }
                    return {};
                },
                [&](const SamplerBindingInfo&) -> MaybeError {
                    Sampler* sampler = ToBackend(group->GetBindingAsSampler(bindingIndex));
                    for (TextureUnit unit : mPipeline->GetTextureUnitsForSampler(flatIndex)) {
                        SetUnitSampler(uint32_t(unit), sampler->GetDescriptorId());
                    }
                    return {};
                },
                [&](const TextureBindingInfo&) -> MaybeError {
                    TextureView* view = ToBackend(group->GetBindingAsTextureView(bindingIndex));
                    uint32_t imageId;
                    DAWN_TRY_ASSIGN(imageId, view->GetImageDescriptorId());
                    for (TextureUnit unit : mPipeline->GetTextureUnitsForTextureView(flatIndex)) {
                        SetUnitImage(uint32_t(unit), imageId);
                    }
                    return {};
                },
                [&](const StorageTextureBindingInfo&) -> MaybeError {
                    TextureView* view = ToBackend(group->GetBindingAsTextureView(bindingIndex));
                    uint32_t imageId;
                    DAWN_TRY_ASSIGN(imageId, view->GetImageDescriptorId());
                    const DkResHandle handle = dkMakeImageHandle(imageId);
                    ForEachStage(bindingInfo.visibility, [&](DkStage stage) {
                        dkCmdBufBindImages(mCmdBuf, stage, uint32_t(flatIndex), &handle, 1);
                    });
                    return {};
                },
                [](const TexelBufferBindingInfo&) -> MaybeError {
                    return DAWN_UNIMPLEMENTED_ERROR("Texel buffers are not supported on deko3d.");
                },
                [](const InputAttachmentBindingInfo&) -> MaybeError { DAWN_UNREACHABLE(); },
                [](const ExternalTextureBindingInfo&) -> MaybeError {
                    return DAWN_UNIMPLEMENTED_ERROR("External textures are not supported on deko3d.");
                }));
        }
        return {};
    }

    DkCmdBuf mCmdBuf;
    PipelineDeko3d* mPipeline = nullptr;
    uint32_t mStageMask = 0;
    std::array<uint32_t, kMaxTextureUnits> mUnitImage{};
    std::array<uint32_t, kMaxTextureUnits> mUnitSampler{};
    uint32_t mDirtyUnitsBegin = kMaxTextureUnits;
    uint32_t mDirtyUnitsEnd = 0;
    bool mPlaceholderSamplerPending = true;
};

void ClearColorAttachment(DkCmdBuf cmdBuf,
                          uint32_t targetId,
                          const Format& format,
                          const dawn::native::Color& color) {
    switch (format.GetAspectInfo(Aspect::Color).baseType) {
        case TextureComponentType::Uint: {
            const std::array<uint32_t, 4> value = ConvertToUnsignedIntegerColor(color);
            dkCmdBufClearColor(cmdBuf, targetId, DkColorMask_RGBA, value.data());
            break;
        }
        case TextureComponentType::Sint: {
            const std::array<int32_t, 4> value = ConvertToSignedIntegerColor(color);
            dkCmdBufClearColor(cmdBuf, targetId, DkColorMask_RGBA, value.data());
            break;
        }
        case TextureComponentType::Float:
        default: {
            const std::array<float, 4> value = ConvertToFloatColor(color);
            dkCmdBufClearColor(cmdBuf, targetId, DkColorMask_RGBA, value.data());
            break;
        }
    }
}

}  // namespace

CommandBuffer::CommandBuffer(CommandEncoder* encoder, const CommandBufferDescriptor* descriptor)
    : CommandBufferBase(encoder, descriptor) {}

MaybeError CommandBuffer::RecordCommands(DkCmdBuf cmdBuf) {
    Device* device = ToBackend(GetDevice());
    auto LazyClearSyncScope = [](const SyncScopeResourceUsage& scope) -> MaybeError {
        for (size_t i = 0; i < scope.textures.size(); i++) {
            Texture* texture = ToBackend(scope.textures[i]);
            DAWN_TRY(scope.textureSyncInfos[i].Iterate(
                [&](const SubresourceRange& range, const TextureSyncInfo& syncInfo) -> MaybeError {
                    if (syncInfo.usage & ~wgpu::TextureUsage::RenderAttachment) {
                        DAWN_TRY(texture->EnsureSubresourceContentInitialized(range));
                    }
                    return {};
                }));
        }
        for (BufferBase* bufferBase : scope.buffers) {
            DAWN_TRY(ToBackend(bufferBase)->EnsureDataInitialized());
        }
        return {};
    };

    PassIndex nextComputePassNumber{0u};
    PassIndex nextRenderPassNumber{0u};

    static uint32_t sTraceCommands = 0;
    Command type;
    while (mCommands.NextCommandId(&type)) {
        if (sTraceCommands < 3000) {
            ++sTraceCommands;
            if (kDeko3dTrace) std::fprintf(stderr, "[deko3d-trace] cmd %d\n", int(type));
        }
        switch (type) {
            case Command::BeginComputePass: {
                mCommands.NextCommand<BeginComputePassCmd>();
                for (const SyncScopeResourceUsage& scope :
                     GetResourceUsages().computePasses[nextComputePassNumber].dispatchUsages) {
                    DAWN_TRY(LazyClearSyncScope(scope));
                }
                DAWN_TRY(RecordComputePass(cmdBuf));
                nextComputePassNumber++;
                break;
            }

            case Command::BeginRenderPass: {
                auto* cmd = mCommands.NextCommand<BeginRenderPassCmd>();
                DAWN_TRY(LazyClearSyncScope(GetResourceUsages().renderPasses[nextRenderPassNumber]));
                DAWN_TRY(LazyClearRenderPassAttachments(
                    GetDevice(), cmd, [&](TextureBase* texture, const SubresourceRange& range) {
                        return ToBackend(texture)->EnsureSubresourceContentInitialized(range);
                    }));
                DAWN_TRY(RecordRenderPass(cmdBuf, cmd, nextRenderPassNumber));
                nextRenderPassNumber++;
                break;
            }

            case Command::CopyBufferToBuffer: {
                CopyBufferToBufferCmd* copy = mCommands.NextCommand<CopyBufferToBufferCmd>();
                if (copy->size == 0) {
                    break;
                }
                Buffer* source = ToBackend(copy->source.Get());
                Buffer* destination = ToBackend(copy->destination.Get());
                DAWN_TRY(source->EnsureDataInitialized());
                DAWN_TRY(destination->EnsureDataInitializedAsDestination(copy->destinationOffset,
                                                                         copy->size));
                // The source's CPU view reflects every write recorded before this point only if it
                // is not written by GPU work in this same pass; Dawn's copy sources are mapped or
                // staging buffers, so snapshot them here.
                device->RecordBufferWrite(destination->GetGpuAddr() + copy->destinationOffset,
                                          source->GetCpuAddr() + copy->sourceOffset, copy->size);
                if (kDeko3dTrace) {
                    char what[160];
                    std::snprintf(what, sizeof(what), "CopyBufferToBuffer size=%llu srcOff=%llu dstOff=%llu",
                                  (unsigned long long)copy->size, (unsigned long long)copy->sourceOffset,
                                  (unsigned long long)copy->destinationOffset);
                    device->TraceFlush(what);
                }
                source->TrackUsage();
                destination->TrackUsage();
                break;
            }

            case Command::CopyBufferToTexture: {
                CopyBufferToTextureCmd* copy = mCommands.NextCommand<CopyBufferToTextureCmd>();
                if (copy->copySize.IsEmpty()) {
                    continue;
                }
                Buffer* buffer = ToBackend(copy->source.buffer.Get());
                DAWN_TRY(buffer->EnsureDataInitialized());
                const TypedTexelBlockInfo& blockInfo = GetBlockInfo(copy->destination);
                TexelCopyBufferLayout layout;
                layout.offset = copy->source.offset;
                layout.bytesPerRow = blockInfo.ToBytes(copy->source.blocksPerRow);
                layout.rowsPerImage = dchecked_cast<uint32_t>(copy->source.rowsPerImage);
                DAWN_TRY(device->RecordTextureUpload(copy->destination, buffer->GetCpuAddr(),
                                                     layout, copy->copySize.ToExtent3D()));
                buffer->TrackUsage();
                break;
            }

            case Command::CopyTextureToBuffer: {
                CopyTextureToBufferCmd* copy = mCommands.NextCommand<CopyTextureToBufferCmd>();
                if (copy->copySize.IsEmpty()) {
                    continue;
                }
                auto& src = copy->source;
                Texture* texture = ToBackend(src.texture.Get());
                Buffer* buffer = ToBackend(copy->destination.buffer.Get());
                DAWN_TRY(buffer->EnsureDataInitializedAsDestination(copy));
                const Extent3D size = copy->copySize.ToExtent3D();
                SubresourceRange range = GetSubresourcesAffectedByCopy(src, size);
                DAWN_TRY(texture->EnsureSubresourceContentInitialized(range));

                const TypedTexelBlockInfo& blockInfo = GetBlockInfo(src);
                const Origin3D origin = src.origin.ToOrigin3D();
                const bool is3D = texture->GetDimension() == wgpu::TextureDimension::e3D;
                const uint64_t bytesPerRow = blockInfo.ToBytes(copy->destination.blocksPerRow);
                const uint64_t bytesPerImage =
                    bytesPerRow * dchecked_cast<uint32_t>(copy->destination.rowsPerImage);
                const uint32_t layers = is3D ? 1 : size.depthOrArrayLayers;
                for (uint32_t layer = 0; layer < layers; ++layer) {
                    DkImageView view =
                        SingleSubresourceView(texture, src.mipLevel, origin.z + layer, src.aspect);
                    DkImageRect rect = {origin.x, origin.y, is3D ? origin.z : 0, size.width,
                                        size.height, is3D ? size.depthOrArrayLayers : 1};
                    DkCopyBuf dst = {
                        buffer->GetGpuAddr() + copy->destination.offset + bytesPerImage * layer,
                        // Strides in bytes: bytes per row, bytes per image.
                        static_cast<uint32_t>(bytesPerRow), static_cast<uint32_t>(bytesPerImage)};
                    dkCmdBufCopyImageToBuffer(cmdBuf, &view, &rect, &dst, 0);
                }
                buffer->TrackUsage();
                break;
            }

            case Command::CopyTextureToTexture: {
                CopyTextureToTextureCmd* copy = mCommands.NextCommand<CopyTextureToTextureCmd>();
                if (copy->copySize.IsEmpty()) {
                    continue;
                }
                auto& src = copy->source;
                auto& dst = copy->destination;
                Texture* srcTexture = ToBackend(src.texture.Get());
                Texture* dstTexture = ToBackend(dst.texture.Get());
                const Extent3D size = ComputeTextureCopyExtent(dst, copy->copySize).ToExtent3D();
                SubresourceRange srcRange = GetSubresourcesAffectedByCopy(src, size);
                SubresourceRange dstRange = GetSubresourcesAffectedByCopy(dst, size);
                DAWN_TRY(srcTexture->EnsureSubresourceContentInitialized(srcRange));
                if (IsCompleteSubresourceCopiedTo(dstTexture, size, dst.mipLevel, dst.aspect)) {
                    dstTexture->SetIsSubresourceContentInitialized(true, dstRange);
                } else {
                    DAWN_TRY(dstTexture->EnsureSubresourceContentInitialized(dstRange));
                }
                const Origin3D srcOrigin = src.origin.ToOrigin3D();
                const Origin3D dstOrigin = dst.origin.ToOrigin3D();
                const bool srcIs3D = srcTexture->GetDimension() == wgpu::TextureDimension::e3D;
                const bool dstIs3D = dstTexture->GetDimension() == wgpu::TextureDimension::e3D;
                const uint32_t layers = (srcIs3D && dstIs3D) ? 1 : size.depthOrArrayLayers;
                for (uint32_t layer = 0; layer < layers; ++layer) {
                    DkImageView srcView = SingleSubresourceView(
                        srcTexture, src.mipLevel, srcOrigin.z + layer, src.aspect);
                    DkImageView dstView = SingleSubresourceView(
                        dstTexture, dst.mipLevel, dstOrigin.z + layer, dst.aspect);
                    const uint32_t depth = (srcIs3D && dstIs3D) ? size.depthOrArrayLayers : 1;
                    DkImageRect srcRect = {srcOrigin.x, srcOrigin.y, srcIs3D ? srcOrigin.z : 0,
                                           size.width, size.height, depth};
                    DkImageRect dstRect = {dstOrigin.x, dstOrigin.y, dstIs3D ? dstOrigin.z : 0,
                                           size.width, size.height, depth};
                    dkCmdBufCopyImage(cmdBuf, &srcView, &srcRect, &dstView, &dstRect, 0);
                }
                break;
            }

            case Command::ClearBuffer: {
                ClearBufferCmd* cmd = mCommands.NextCommand<ClearBufferCmd>();
                if (cmd->size == 0) {
                    break;
                }
                Buffer* dstBuffer = ToBackend(cmd->buffer.Get());
                bool clearedToZero = false;
                DAWN_TRY(dstBuffer->EnsureDataInitializedAsDestination(cmd->offset, cmd->size,
                                                                       &clearedToZero));
                if (!clearedToZero) {
                    const std::vector<uint8_t> zeros(cmd->size, 0);
                    device->RecordBufferWrite(dstBuffer->GetGpuAddr() + cmd->offset, zeros.data(),
                                              cmd->size);
                }
                dstBuffer->TrackUsage();
                break;
            }

            case Command::WriteBuffer: {
                WriteBufferCmd* write = mCommands.NextCommand<WriteBufferCmd>();
                uint64_t size = write->size;
                uint8_t* data = mCommands.NextData<uint8_t>(size);
                if (size == 0) {
                    continue;
                }
                Buffer* dstBuffer = ToBackend(write->buffer.Get());
                DAWN_TRY(dstBuffer->EnsureDataInitializedAsDestination(write->offset, size));
                device->RecordBufferWrite(dstBuffer->GetGpuAddr() + write->offset, data, size);
                dstBuffer->TrackUsage();
                break;
            }

            case Command::ResolveQuerySet:
                return DAWN_UNIMPLEMENTED_ERROR("Query sets are not supported on deko3d yet.");
            case Command::WriteTimestamp:
                return DAWN_UNIMPLEMENTED_ERROR("WriteTimestamp unimplemented");

            case Command::InsertDebugMarker:
            case Command::PopDebugGroup:
            case Command::PushDebugGroup:
                SkipCommand(&mCommands, type);
                break;

            default:
                DAWN_UNREACHABLE();
        }
    }
    return {};
}

MaybeError CommandBuffer::RecordComputePass(DkCmdBuf cmdBuf) {
    ComputePipeline* lastPipeline = nullptr;
    BindGroupTracker bindGroupTracker(cmdBuf);

    Command type;
    while (mCommands.NextCommandId(&type)) {
        switch (type) {
            case Command::EndComputePass:
                mCommands.NextCommand<EndComputePassCmd>();
                dkCmdBufBarrier(cmdBuf, DkBarrier_Primitives, 0);
                return {};

            case Command::Dispatch: {
                DispatchCmd* dispatch = mCommands.NextCommand<DispatchCmd>();
                DAWN_TRY(bindGroupTracker.Apply());
                dkCmdBufDispatchCompute(cmdBuf, dispatch->x, dispatch->y, dispatch->z);
                break;
            }

            case Command::DispatchIndirect: {
                DispatchIndirectCmd* dispatch = mCommands.NextCommand<DispatchIndirectCmd>();
                DAWN_TRY(bindGroupTracker.Apply());
                Buffer* indirectBuffer = ToBackend(dispatch->indirectBuffer.Get());
                dkCmdBufDispatchComputeIndirect(cmdBuf,
                                                indirectBuffer->GetGpuAddr() + dispatch->indirectOffset);
                indirectBuffer->TrackUsage();
                break;
            }

            case Command::SetComputePipeline: {
                SetComputePipelineCmd* cmd = mCommands.NextCommand<SetComputePipelineCmd>();
                lastPipeline = ToBackend(cmd->pipeline).Get();
                lastPipeline->ApplyNow(cmdBuf);
                bindGroupTracker.OnSetPipeline(lastPipeline);
                break;
            }

            case Command::SetBindGroup: {
                SetBindGroupCmd* cmd = mCommands.NextCommand<SetBindGroupCmd>();
                uint32_t* dynamicOffsets = nullptr;
                if (cmd->dynamicOffsetCount > 0) {
                    dynamicOffsets = mCommands.NextData<uint32_t>(cmd->dynamicOffsetCount);
                }
                bindGroupTracker.OnSetBindGroup(cmd->index, cmd->group.Get(),
                                                cmd->dynamicOffsetCount, dynamicOffsets);
                break;
            }

            case Command::SetImmediates:
                return DAWN_UNIMPLEMENTED_ERROR("Immediates are not supported on deko3d yet.");

            case Command::InsertDebugMarker:
            case Command::PopDebugGroup:
            case Command::PushDebugGroup:
                SkipCommand(&mCommands, type);
                break;

            case Command::WriteTimestamp:
                return DAWN_UNIMPLEMENTED_ERROR("WriteTimestamp unimplemented");

            default:
                DAWN_UNREACHABLE();
        }
    }
    DAWN_UNREACHABLE();
}

MaybeError CommandBuffer::RecordRenderPass(DkCmdBuf cmdBuf,
                                           BeginRenderPassCmd* renderPass,
                                           PassIndex renderPassIndex) {
    const IndirectDrawMetadata& metadata = GetIndirectDrawMetadata()[renderPassIndex];
    IndirectDrawIndex indirectDrawIndex{0u};

    // Render targets.
    std::array<DkImageView, kMaxColorAttachments> colorViews;
    std::array<DkImageView const*, kMaxColorAttachments> colorTargets{};
    uint32_t colorCount = 0;
    for (auto i : renderPass->attachmentState->GetColorAttachmentsMask()) {
        const uint8_t index = static_cast<uint8_t>(i);
        TextureView* view = ToBackend(renderPass->colorAttachments[i].view.Get());
        colorViews[index] = view->GetDkImageViewForAttachment(renderPass->colorAttachments[i].depthSlice);
        colorTargets[index] = &colorViews[index];
        colorCount = std::max<uint32_t>(colorCount, index + 1);
    }
    for (uint32_t i = 0; i < colorCount; ++i) {
        if (colorTargets[i] == nullptr) {
            // deko3d needs a dense array; unused slots get a harmless alias of a used target and
            // a zero write mask from the pipeline.
            for (uint32_t j = 0; j < colorCount; ++j) {
                if (colorTargets[j] != nullptr) {
                    colorTargets[i] = colorTargets[j];
                    break;
                }
            }
        }
    }
    DkImageView depthView;
    DkImageView const* depthTarget = nullptr;
    if (renderPass->attachmentState->HasDepthStencilAttachment()) {
        TextureView* view = ToBackend(renderPass->depthStencilAttachment.view.Get());
        depthView = view->GetDkImageViewForAttachment(0);
        depthView.dsSource = DkDsSource_Depth;
        depthTarget = &depthView;
    }
    if (kDeko3dTrace) std::fprintf(stderr, "[deko3d-trace] renderpass %ux%u colors=%u depth=%d\n", renderPass->width,
                 renderPass->height, colorCount, int(depthTarget != nullptr));
    if (kDeko3dTrace) ToBackend(GetDevice())->TraceFlush("before render pass targets");
    dkCmdBufBindRenderTargets(cmdBuf, colorTargets.data(), colorCount, depthTarget);

    DkViewport viewport = {0.0f, 0.0f, float(renderPass->width), float(renderPass->height), 0.0f, 1.0f};
    DkScissor scissor = {0, 0, renderPass->width, renderPass->height};
    dkCmdBufSetViewports(cmdBuf, 0, &viewport, 1);
    dkCmdBufSetScissors(cmdBuf, 0, &scissor, 1);
    dkCmdBufSetBlendConst(cmdBuf, 0.0f, 0.0f, 0.0f, 0.0f);

    // Load ops. Clears honour the color write state, so reset it to write everything first.
    DkColorWriteState writeAll;
    dkColorWriteStateDefaults(&writeAll);
    dkCmdBufBindColorWriteState(cmdBuf, &writeAll);
    for (auto i : renderPass->attachmentState->GetColorAttachmentsMask()) {
        auto* attachment = &renderPass->colorAttachments[i];
        if (attachment->loadOp == wgpu::LoadOp::Clear) {
            ClearColorAttachment(cmdBuf, static_cast<uint8_t>(i), attachment->view->GetFormat(),
                                 attachment->clearColor);
        }
    }
    if (renderPass->attachmentState->HasDepthStencilAttachment()) {
        auto* attachment = &renderPass->depthStencilAttachment;
        const Format& format = attachment->view->GetTexture()->GetFormat();
        const bool clearDepth = format.HasDepth() && attachment->depthLoadOp == wgpu::LoadOp::Clear;
        const bool clearStencil =
            format.HasStencil() && attachment->stencilLoadOp == wgpu::LoadOp::Clear;
        if (clearDepth || clearStencil) {
            DkDepthStencilState ds;
            dkDepthStencilStateDefaults(&ds);
            ds.depthWriteEnable = clearDepth;
            dkCmdBufBindDepthStencilState(cmdBuf, &ds);
            dkCmdBufClearDepthStencil(cmdBuf, clearDepth, attachment->clearDepth,
                                      clearStencil ? 0xFF : 0,
                                      static_cast<uint8_t>(attachment->clearStencil));
        }
    }

    if (kDeko3dTrace) ToBackend(GetDevice())->TraceFlush("after render pass clears");
    RenderPipeline* lastPipeline = nullptr;
    uint32_t stencilReference = 0;
    Buffer* indexBuffer = nullptr;
    uint64_t indexBufferOffset = 0;
    DkIdxFormat indexFormat = DkIdxFormat_Uint16;
    uint32_t indexFormatSize = 2;
    BindGroupTracker bindGroupTracker(cmdBuf);
    uint32_t diagDraws = 0, diagPipelines = 0;

    auto BindIndexBuffer = [&](uint32_t firstIndex) {
        dkCmdBufBindIdxBuffer(cmdBuf, indexFormat,
                              indexBuffer->GetGpuAddr() + indexBufferOffset);
        (void)firstIndex;
    };

    auto SetPrimitiveRestart = [&]() {
        const DkPrimitive primitive = lastPipeline->GetDkPrimitive();
        const bool strip = primitive == DkPrimitive_LineStrip || primitive == DkPrimitive_TriangleStrip;
        dkCmdBufSetPrimitiveRestart(cmdBuf, strip,
                                    indexFormat == DkIdxFormat_Uint32 ? 0xFFFFFFFFu : 0xFFFFu);
    };

    auto DoRenderBundleCommand = [&](CommandIterator* iter, Command type) -> MaybeError {
        switch (type) {
            case Command::Draw: {
                DrawCmd* draw = iter->NextCommand<DrawCmd>();
                ++diagDraws;
                DAWN_TRY(bindGroupTracker.Apply());
                dkCmdBufDraw(cmdBuf, lastPipeline->GetDkPrimitive(), draw->vertexCount,
                             draw->instanceCount, draw->firstVertex, draw->firstInstance);
                if (kDeko3dTrace) {
                    char what[160];
                    std::snprintf(what, sizeof(what), "Draw prim=%d count=%u inst=%u first=%u",
                                  int(lastPipeline->GetDkPrimitive()), draw->vertexCount,
                                  draw->instanceCount, draw->firstVertex);
                    ToBackend(GetDevice())->TraceFlush(what);
                }
                break;
            }

            case Command::DrawIndexed: {
                DrawIndexedCmd* draw = iter->NextCommand<DrawIndexedCmd>();
                ++diagDraws;
                DAWN_TRY(bindGroupTracker.Apply());
                SetPrimitiveRestart();
                BindIndexBuffer(draw->firstIndex);
                dkCmdBufDrawIndexed(cmdBuf, lastPipeline->GetDkPrimitive(), draw->indexCount,
                                    draw->instanceCount, draw->firstIndex, draw->baseVertex,
                                    draw->firstInstance);
                if (kDeko3dTrace) {
                    char what[200];
                    std::snprintf(what, sizeof(what),
                                  "DrawIndexed prim=%d count=%u inst=%u firstIndex=%u base=%d fmt=%d ibOff=%llu",
                                  int(lastPipeline->GetDkPrimitive()), draw->indexCount,
                                  draw->instanceCount, draw->firstIndex, draw->baseVertex,
                                  int(indexFormat), (unsigned long long)indexBufferOffset);
                    ToBackend(GetDevice())->TraceFlush(what);
                }
                break;
            }

            case Command::DrawIndirect: {
                DrawIndirectCmd* draw = iter->NextCommand<DrawIndirectCmd>();
                DAWN_TRY(bindGroupTracker.Apply());
                IndirectDrawMetadata::ValidatedIndirectDraw validatedDraw =
                    metadata.GetValidatedIndirectDraw(draw, indirectDrawIndex++);
                Buffer* indirectBuffer = ToBackend(validatedDraw.indirectBuffer.Get());
                dkCmdBufDrawIndirect(cmdBuf, lastPipeline->GetDkPrimitive(),
                                     indirectBuffer->GetGpuAddr() + validatedDraw.indirectOffset);
                indirectBuffer->TrackUsage();
                break;
            }

            case Command::DrawIndexedIndirect: {
                DrawIndexedIndirectCmd* draw = iter->NextCommand<DrawIndexedIndirectCmd>();
                DAWN_TRY(bindGroupTracker.Apply());
                SetPrimitiveRestart();
                BindIndexBuffer(0);
                IndirectDrawMetadata::ValidatedIndirectDraw validatedDraw =
                    metadata.GetValidatedIndirectDraw(draw, indirectDrawIndex++);
                Buffer* indirectBuffer = ToBackend(validatedDraw.indirectBuffer.Get());
                dkCmdBufDrawIndexedIndirect(cmdBuf, lastPipeline->GetDkPrimitive(),
                                            indirectBuffer->GetGpuAddr() + validatedDraw.indirectOffset);
                indirectBuffer->TrackUsage();
                break;
            }

            case Command::InsertDebugMarker:
            case Command::PopDebugGroup:
            case Command::PushDebugGroup:
                SkipCommand(iter, type);
                break;

            case Command::SetRenderPipeline: {
                SetRenderPipelineCmd* cmd = iter->NextCommand<SetRenderPipelineCmd>();
                lastPipeline = ToBackend(cmd->pipeline).Get();
                ++diagPipelines;
                lastPipeline->ApplyNow(cmdBuf, stencilReference);
                bindGroupTracker.OnSetPipeline(lastPipeline);
                if (kDeko3dTrace) ToBackend(GetDevice())->TraceFlush("after SetRenderPipeline");
                break;
            }

            case Command::SetBindGroup: {
                SetBindGroupCmd* cmd = iter->NextCommand<SetBindGroupCmd>();
                uint32_t* dynamicOffsets = nullptr;
                if (cmd->dynamicOffsetCount > 0) {
                    dynamicOffsets = iter->NextData<uint32_t>(cmd->dynamicOffsetCount);
                }
                bindGroupTracker.OnSetBindGroup(cmd->index, cmd->group.Get(),
                                                cmd->dynamicOffsetCount, dynamicOffsets);
                break;
            }

            case Command::SetIndexBuffer: {
                SetIndexBufferCmd* cmd = iter->NextCommand<SetIndexBufferCmd>();
                indexBuffer = ToBackend(cmd->buffer.Get());
                indexBufferOffset = cmd->offset;
                indexFormat = cmd->format == wgpu::IndexFormat::Uint32 ? DkIdxFormat_Uint32
                                                                       : DkIdxFormat_Uint16;
                indexFormatSize = cmd->format == wgpu::IndexFormat::Uint32 ? 4 : 2;
                indexBuffer->TrackUsage();
                break;
            }

            case Command::SetVertexBuffer: {
                SetVertexBufferCmd* cmd = iter->NextCommand<SetVertexBufferCmd>();
                Buffer* buffer = ToBackend(cmd->buffer.Get());
                DkBufExtents extents = {buffer->GetGpuAddr() + cmd->offset,
                                        static_cast<uint32_t>(cmd->size)};
                dkCmdBufBindVtxBuffers(cmdBuf, static_cast<uint32_t>(cmd->slot), &extents, 1);
                buffer->TrackUsage();
                break;
            }

            case Command::SetImmediates:
                return DAWN_UNIMPLEMENTED_ERROR("Immediates are not supported on deko3d yet.");

            default:
                DAWN_UNREACHABLE();
        }
        return {};
    };

    Command type;
    while (mCommands.NextCommandId(&type)) {
        switch (type) {
            case Command::EndRenderPass: {
                mCommands.NextCommand<EndRenderPassCmd>();
                if (Device* dev = ToBackend(GetDevice()); dev->mDebugDumpFrame &&
                    renderPass->attachmentState->GetSampleCount() == 1) {
                    for (auto i : renderPass->attachmentState->GetColorAttachmentsMask()) {
                        TextureView* view = ToBackend(renderPass->colorAttachments[i].view.Get());
                        char name[64];
                        std::snprintf(name, sizeof(name), "pass%ux%u_c%u draws=%u pipelines=%u",
                                      renderPass->width, renderPass->height,
                                      unsigned(static_cast<uint8_t>(i)), diagDraws, diagPipelines);
                        dev->DebugDumpImage(colorViews[static_cast<uint8_t>(i)], renderPass->width,
                                            renderPass->height, view->GetFormat().format, name);
                    }
                }
                if (renderPass->attachmentState->GetSampleCount() > 1) {
                    for (auto i : renderPass->attachmentState->GetColorAttachmentsMask()) {
                        auto& attachment = renderPass->colorAttachments[i];
                        if (attachment.resolveTarget.Get() == nullptr) {
                            continue;
                        }
                        DkImageView src = ToBackend(attachment.view.Get())->GetDkImageViewForAttachment(0);
                        DkImageView dst =
                            ToBackend(attachment.resolveTarget.Get())->GetDkImageViewForAttachment(0);
                        dkCmdBufResolveImage(cmdBuf, &src, &dst);
                    }
                }
                (void)indexFormatSize;
                return {};
            }

            case Command::SetStencilReference: {
                SetStencilReferenceCmd* cmd = mCommands.NextCommand<SetStencilReferenceCmd>();
                stencilReference = cmd->reference;
                if (lastPipeline != nullptr && lastPipeline->UsesStencil()) {
                    const DepthStencilState* ds = lastPipeline->GetDepthStencilState();
                    dkCmdBufSetStencil(cmdBuf, DkFace_FrontAndBack,
                                       static_cast<uint8_t>(ds->stencilWriteMask),
                                       static_cast<uint8_t>(stencilReference),
                                       static_cast<uint8_t>(ds->stencilReadMask));
                }
                break;
            }

            case Command::SetViewport: {
                SetViewportCmd* cmd = mCommands.NextCommand<SetViewportCmd>();
                DkViewport vp = {cmd->x, cmd->y, cmd->width, cmd->height, cmd->minDepth, cmd->maxDepth};
                dkCmdBufSetViewports(cmdBuf, 0, &vp, 1);
                break;
            }

            case Command::SetScissorRect: {
                SetScissorRectCmd* cmd = mCommands.NextCommand<SetScissorRectCmd>();
                DkScissor rect = {cmd->x, cmd->y, cmd->width, cmd->height};
                dkCmdBufSetScissors(cmdBuf, 0, &rect, 1);
                break;
            }

            case Command::SetBlendConstant: {
                SetBlendConstantCmd* cmd = mCommands.NextCommand<SetBlendConstantCmd>();
                const std::array<float, 4> color = ConvertToFloatColor(cmd->color);
                dkCmdBufSetBlendConst(cmdBuf, color[0], color[1], color[2], color[3]);
                break;
            }

            case Command::ExecuteBundles: {
                ExecuteBundlesCmd* cmd = mCommands.NextCommand<ExecuteBundlesCmd>();
                auto bundles = mCommands.NextData<Ref<RenderBundleBase>>(cmd->count);
                for (uint32_t i = 0; i < cmd->count; ++i) {
                    CommandIterator* iter = DAWN_UNSAFE_TODO(bundles[i])->GetCommands();
                    iter->Reset();
                    while (iter->NextCommandId(&type)) {
                        DAWN_TRY(DoRenderBundleCommand(iter, type));
                    }
                }
                break;
            }

            case Command::BeginOcclusionQuery:
            case Command::EndOcclusionQuery:
                return DAWN_UNIMPLEMENTED_ERROR("Occlusion queries are not supported on deko3d yet.");

            case Command::WriteTimestamp:
                return DAWN_UNIMPLEMENTED_ERROR("WriteTimestamp unimplemented");

            default:
                DAWN_TRY(DoRenderBundleCommand(&mCommands, type));
                break;
        }
    }
    DAWN_UNREACHABLE();
}

}  // namespace dawn::native::deko3d
