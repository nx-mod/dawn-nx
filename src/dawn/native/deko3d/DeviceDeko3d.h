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

#ifndef SRC_DAWN_NATIVE_DEKO3D_DEVICEDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_DEVICEDEKO3D_H_

#include <atomic>
#include <map>
#include <deko3d.h>

#include "src/dawn/native/deko3d/ImageAllocationDeko3d.h"

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "src/dawn/native/Device.h"
#include "src/dawn/native/ExecutionQueue.h"
#include "src/dawn/native/deko3d/Forward.h"

namespace dawn::native::deko3d {

// TEMP bring-up tracing (synchronous flushes, per-command logs). Off for normal runs.
inline constexpr bool kDeko3dTrace = false;
// TEMP bring-up: periodic memory report and render-pass image dumps.
inline constexpr bool kDeko3dDiag = true;
struct MemStats {
    std::atomic<int64_t> bufferBytes{0}, bufferCount{0}, textureBytes{0}, textureCount{0};
};
MemStats& GetMemStats();

// Owns the deko3d device and queue plus the shared GPU-side state every backend object records
// into:
// - one pending DkCmdBuf that command buffers, lazy clears and uploads append to, submitted as a
//   single list per queue submit and tagged with a DkFence for completion tracking;
// - recycled command and upload memory, released back to the pool once their submit's fence has
//   signalled;
// - global image and sampler descriptor sets (texture binding handles are indices into them);
// - memory blocks whose owners were destroyed while the GPU might still read them.
class Device final : public DeviceBase {
  public:
    static ResultOrError<Ref<Device>> Create(AdapterBase* adapter,
                                             const UnpackedPtr<DeviceDescriptor>& descriptor,
                                             const TogglesState& deviceToggles,
                                             Ref<DeviceBase::DeviceLostEvent>&& lostEvent);
    ~Device() override;

    MaybeError Initialize(const UnpackedPtr<DeviceDescriptor>& descriptor);

    DkDevice GetDkDevice() const { return mDkDevice; }
    DkQueue GetDkQueue() const { return mDkQueue; }

    // Command recording. The pending command buffer always has the descriptor sets bound.
    DkCmdBuf GetPendingCommandBuffer();
    bool HasPendingCommands() const { return mHasPendingCommands; }
    // Finishes the pending list, submits it and signals a fence for `serial`.
    MaybeError SubmitPendingCommands(ExecutionSerial serial);
    // Completed serial according to signalled fences (non-blocking).
    ExecutionSerial PollCompletedSerial();
    // Blocks until `serial` is done or the timeout elapses; returns the completed serial.
    ExecutionSerial WaitForSerial(ExecutionSerial serial, Nanoseconds timeout);
    void WaitIdle();
    // TEMP bring-up: submits what is recorded so far and waits, logging `what` and queue state.
    void TraceFlush(const char* what);
    void TraceSelfTest(const char* what, DkGpuAddr realDst);
    // TEMP bring-up: flushes pending work, reads `view` back and writes it to the SD card as PPM.
    void DebugDumpImage(const DkImageView& view, uint32_t width, uint32_t height,
                        wgpu::TextureFormat format, const char* name);
    void ReportMemory(const char* why);
    bool mDebugDumpFrame = false;
    uint32_t mDebugDumpIndex = 0;

    // CPU-visible, GPU-readable scratch memory valid until the pending commands complete.
    MaybeError AllocateUploadMemory(uint64_t size, uint8_t** cpuAddr, DkGpuAddr* gpuAddr);

    void DestroyMemBlockWhenIdle(DkMemBlock memBlock);

    // Image memory. Textures are sub-allocated from large shared blocks: every DkMemBlock costs
    // an nvmap handle plus three GPU mappings, and hundreds of per-texture blocks exhaust the
    // driver (dkMemBlockCreate fails mid-race). Large images still get a dedicated block.
    MaybeError AllocateImageMemory(uint32_t size, uint32_t alignment, ImageAllocation* out);
    // Returns the range once the GPU has finished all work recorded so far.
    void FreeImageMemoryWhenIdle(const ImageAllocation& allocation);

    // Records a GPU-timeline write of `size` bytes into `dst`. Goes through the 3D engine's inline
    // data path (dkCmdBufPushData): deko3d's dkCmdBufCopyBuffer drives the separate copy engine,
    // which hung the GPU (timeout) on hardware. `size` must be a multiple of 4, as WebGPU requires.
    void RecordBufferWrite(DkGpuAddr dst, const uint8_t* data, uint64_t size);

    ResultOrError<uint32_t> AllocateImageDescriptor(const DkImageView& view, bool loadStore);
    void ReleaseImageDescriptor(uint32_t id);
    ResultOrError<uint32_t> AllocateSamplerDescriptor(const DkSampler& sampler);
    void ReleaseSamplerDescriptor(uint32_t id);

    // Loads a compiled DKSH module into GPU code memory.
    MaybeError LoadShader(const std::vector<uint8_t>& dksh, DkShader* shader);

    ResultOrError<Ref<CommandBufferBase>> CreateCommandBuffer(
        CommandEncoder* encoder,
        const CommandBufferDescriptor* descriptor) override;

    MaybeError TickImpl() override;

    MaybeError CopyFromStagingToBuffer(BufferBase* source,
                                       uint64_t sourceOffset,
                                       BufferBase* destination,
                                       uint64_t destinationOffset,
                                       uint64_t size) override;
    MaybeError CopyFromStagingToTextureImpl(BufferBase* source,
                                            const TexelCopyBufferLayout& src,
                                            const TextureCopy& dst,
                                            const Extent3D& copySizePixels) override;

    uint32_t GetOptimalBytesPerRowAlignment() const override;
    uint64_t GetOptimalBufferToTextureCopyOffsetAlignment() const override;
    float GetTimestampPeriodInNS() const override;
    bool CanTextureLoadResolveTargetInTheSameRenderpass() const override;

    // Records a copy of `size` bytes of CPU data into `texture` at `dst`.
    MaybeError RecordTextureUpload(const TextureCopy& dst,
                                   const uint8_t* data,
                                   const TexelCopyBufferLayout& layout,
                                   const Extent3D& copySizePixels);

  private:
    using DeviceBase::DeviceBase;

    ResultOrError<Ref<BindGroupBase>> CreateBindGroupImpl(
        const UnpackedPtr<BindGroupDescriptor>& descriptor) override;
    ResultOrError<Ref<BindGroupLayoutInternalBase>> CreateBindGroupLayoutImpl(
        const UnpackedPtr<BindGroupLayoutDescriptor>& descriptor) override;
    ResultOrError<Ref<BufferBase>> CreateBufferImpl(
        const UnpackedPtr<BufferDescriptor>& descriptor) override;
    Ref<ComputePipelineBase> CreateUninitializedComputePipelineImpl(
        const UnpackedPtr<ComputePipelineDescriptor>& descriptor) override;
    ResultOrError<Ref<PipelineLayoutBase>> CreatePipelineLayoutImpl(
        const UnpackedPtr<PipelineLayoutDescriptor>& descriptor) override;
    ResultOrError<Ref<QuerySetBase>> CreateQuerySetImpl(
        const QuerySetDescriptor* descriptor) override;
    Ref<RenderPipelineBase> CreateUninitializedRenderPipelineImpl(
        const UnpackedPtr<RenderPipelineDescriptor>& descriptor) override;
    ResultOrError<Ref<ResourceTableBase>> CreateResourceTableImpl(
        const ResourceTableDescriptor* descriptor) override;
    ResultOrError<Ref<SamplerBase>> CreateSamplerImpl(const SamplerDescriptor* descriptor) override;
    ResultOrError<Ref<ShaderModuleBase>> CreateShaderModuleImpl(
        const UnpackedPtr<ShaderModuleDescriptor>& descriptor,
        const std::vector<tint::wgsl::Extension>& internalExtensions) override;
    ResultOrError<Ref<SwapChainBase>> CreateSwapChainImpl(
        Surface* surface,
        SwapChainBase* previousSwapChain,
        const SurfaceConfiguration* config) override;
    ResultOrError<Ref<TextureBase>> CreateTextureImpl(
        const UnpackedPtr<TextureDescriptor>& descriptor) override;
    ResultOrError<Ref<TextureViewBase>> CreateTextureViewImpl(
        TextureBase* texture,
        const UnpackedPtr<TextureViewDescriptor>& descriptor) override;

    void DestroyImpl(DestroyReason reason) override;

    static void AddCommandMemory(void* userData, DkCmdBuf cmdBuf, size_t minReqSize);
    DkMemBlock AcquireMemBlock(std::vector<DkMemBlock>* freeList,
                               uint32_t size,
                               uint32_t flags);
    void ReleaseResourcesUpTo(ExecutionSerial completed);

    DkDevice mDkDevice = nullptr;
    DkQueue mDkQueue = nullptr;
    bool mHasPendingCommands = false;

    // A DkCmdBuf with the memory it has been given. dkCmdBufClear rewinds into the last chunk, so a
    // command buffer may only be cleared and reused once the GPU has finished its submitted lists.
    struct CmdBufEntry {
        DkDevice device = nullptr;
        DkCmdBuf cmdBuf = nullptr;
        std::vector<DkMemBlock> memory;
    };
    MaybeError AcquireCommandBuffer();
    std::unique_ptr<CmdBufEntry> mCmdBuf;
    std::vector<std::unique_ptr<CmdBufEntry>> mFreeCmdBufs;

    // Upload allocations and destroyed memory referenced by the pending list, and the state of
    // submitted-but-unfinished lists (by the serial that completes them).
    struct InFlight {
        ExecutionSerial serial;
        std::unique_ptr<CmdBufEntry> cmdBuf;
        std::vector<DkMemBlock> uploadMemory;
        std::vector<DkMemBlock> destroyedMemory;
        std::vector<ImageAllocation> freedImageMemory;
        DkFence fence;
    };
    std::vector<DkMemBlock> mPendingUploadMemory;
    std::vector<DkMemBlock> mPendingDestroyedMemory;
    std::vector<ImageAllocation> mPendingFreedImageMemory;
    struct ImageChunk {
        DkMemBlock block = nullptr;
        std::map<uint32_t, uint32_t> freeRanges;  // offset -> size
        uint32_t used = 0;
    };
    std::vector<std::unique_ptr<ImageChunk>> mImageChunks;
    std::mutex mImageMemoryMutex;
    void ReleaseImageMemory(const ImageAllocation& allocation);
    std::deque<std::unique_ptr<InFlight>> mInFlight;
    std::vector<DkMemBlock> mFreeCmdMemory;
    std::vector<DkMemBlock> mFreeUploadMemory;
    DkMemBlock mCurrentUploadBlock = nullptr;
    uint32_t mCurrentUploadOffset = 0;
    ExecutionSerial mCompletedSerial{0u};

    // Descriptor sets.
    static constexpr uint32_t kMaxImageDescriptors = 4096;
    static constexpr uint32_t kMaxSamplerDescriptors = 1024;
    DkMemBlock mImageDescriptorMemory = nullptr;
    DkMemBlock mSamplerDescriptorMemory = nullptr;
    std::vector<uint32_t> mFreeImageDescriptors;
    std::vector<uint32_t> mFreeSamplerDescriptors;
    std::mutex mDescriptorMutex;

    // Shader code memory (bump allocated per block).
    std::vector<DkMemBlock> mCodeMemory;
    uint32_t mCodeOffset = 0;
    std::mutex mCodeMutex;
};

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_DEVICEDEKO3D_H_
