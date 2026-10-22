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

#include "src/dawn/native/deko3d/DeviceDeko3d.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "src/dawn/native/BackendConnection.h"
#include "src/dawn/native/ChainUtils.h"
#include "src/dawn/native/Commands.h"
#include "src/dawn/native/ErrorData.h"
#include "src/dawn/native/Instance.h"
#include "src/dawn/native/Surface.h"
#include "src/dawn/native/deko3d/BindGroupDeko3d.h"
#include "src/dawn/native/deko3d/BindGroupLayoutDeko3d.h"
#include "src/dawn/native/deko3d/BufferDeko3d.h"
#include "src/dawn/native/deko3d/CommandBufferDeko3d.h"
#include "src/dawn/native/deko3d/ComputePipelineDeko3d.h"
#include "src/dawn/native/deko3d/PipelineLayoutDeko3d.h"
#include "src/dawn/native/deko3d/QuerySetDeko3d.h"
#include "src/dawn/native/deko3d/QueueDeko3d.h"
#include "src/dawn/native/deko3d/RenderPipelineDeko3d.h"
#include "src/dawn/native/deko3d/SamplerDeko3d.h"
#include "src/dawn/native/deko3d/ShaderModuleDeko3d.h"
#include "src/dawn/native/deko3d/SwapChainDeko3d.h"
#include "src/dawn/native/deko3d/TextureDeko3d.h"

namespace dawn::native::deko3d {

namespace {

constexpr uint32_t kCommandMemoryBlockSize = 256 * 1024;
constexpr uint32_t kUploadMemoryBlockSize = 4 * 1024 * 1024;
constexpr uint32_t kCodeMemoryBlockSize = 1024 * 1024;

void DebugCallback(void* userData, const char* context, DkResult result, const char* message) {
    std::fprintf(stderr, "[deko3d] %s: result=%d %s\n", context ? context : "", int(result),
                 message ? message : "");
}

uint32_t AlignUp(uint64_t value, uint32_t alignment) {
    return static_cast<uint32_t>((value + alignment - 1) & ~uint64_t(alignment - 1));
}

}  // namespace

// static
ResultOrError<Ref<Device>> Device::Create(AdapterBase* adapter,
                                          const UnpackedPtr<DeviceDescriptor>& descriptor,
                                          const TogglesState& deviceToggles,
                                          Ref<DeviceBase::DeviceLostEvent>&& lostEvent) {
    Ref<Device> device =
        AcquireRef(new Device(adapter, descriptor, deviceToggles, std::move(lostEvent)));
    DAWN_TRY(device->Initialize(descriptor));
    return device;
}

Device::~Device() {
    Destroy(DestroyReason::CppDestructor);
}

MaybeError Device::Initialize(const UnpackedPtr<DeviceDescriptor>& descriptor) {
    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);  // depth [0,1], upper-left origin: WebGPU conventions
    deviceMaker.cbDebug = DebugCallback;
    mDkDevice = dkDeviceCreate(&deviceMaker);
    DAWN_INVALID_IF(mDkDevice == nullptr, "dkDeviceCreate failed");

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, mDkDevice);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_HighPrio;
    mDkQueue = dkQueueCreate(&queueMaker);
    DAWN_INVALID_IF(mDkQueue == nullptr, "dkQueueCreate failed");

    DAWN_TRY(AcquireCommandBuffer());

    DkMemBlockMaker memMaker;
    dkMemBlockMakerDefaults(&memMaker, mDkDevice,
                            AlignUp(kMaxImageDescriptors * sizeof(DkImageDescriptor),
                                    DK_MEMBLOCK_ALIGNMENT));
    memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    mImageDescriptorMemory = dkMemBlockCreate(&memMaker);
    dkMemBlockMakerDefaults(&memMaker, mDkDevice,
                            AlignUp(kMaxSamplerDescriptors * sizeof(DkSamplerDescriptor),
                                    DK_MEMBLOCK_ALIGNMENT));
    memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    mSamplerDescriptorMemory = dkMemBlockCreate(&memMaker);
    DAWN_INVALID_IF(mImageDescriptorMemory == nullptr || mSamplerDescriptorMemory == nullptr,
                    "Allocating deko3d descriptor sets failed");
    for (uint32_t i = kMaxImageDescriptors; i > 0; --i) {
        mFreeImageDescriptors.push_back(i - 1);
    }
    for (uint32_t i = kMaxSamplerDescriptors; i > 0; --i) {
        mFreeSamplerDescriptors.push_back(i - 1);
    }

    Ref<Queue> queue;
    DAWN_TRY_ASSIGN(queue, Queue::Create(this, &descriptor->defaultQueue));
    return DeviceBase::Initialize(descriptor, std::move(queue));
}

// TEMP bring-up: isolates which GPU data-transfer path hangs.
void Device::TraceSelfTest(const char* what, DkGpuAddr realDst) {
    static int sRuns = 0;
    if (sRuns++ >= 2 || dkQueueIsInErrorState(mDkQueue)) {
        return;
    }
    auto makeBlock = [&](uint32_t flags) {
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, mDkDevice, DK_MEMBLOCK_ALIGNMENT);
        m.flags = flags;
        return dkMemBlockCreate(&m);
    };
    auto step = [&](const char* name, auto&& record) {
        if (dkQueueIsInErrorState(mDkQueue)) {
            return;
        }
        DkCmdBuf tmp;
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, mDkDevice);
        tmp = dkCmdBufCreate(&cm);
        DkMemBlockMaker mm;
        dkMemBlockMakerDefaults(&mm, mDkDevice, 64 * 1024);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        DkMemBlock cmdMem = dkMemBlockCreate(&mm);
        dkCmdBufAddMemory(tmp, cmdMem, 0, 64 * 1024);
        record(tmp);
        std::fprintf(stderr, "[deko3d-selftest] %s/%s: submit\n", what, name);
        dkQueueSubmitCommands(mDkQueue, dkCmdBufFinishList(tmp));
        dkQueueWaitIdle(mDkQueue);
        std::fprintf(stderr, "[deko3d-selftest] %s/%s: ok=%d\n", what, name,
                     int(!dkQueueIsInErrorState(mDkQueue)));
    };
    const uint32_t kFlags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock a = makeBlock(kFlags);
    DkMemBlock b = makeBlock(kFlags);
    static const uint32_t kData[4] = {1, 2, 3, 4};
    std::fprintf(stderr, "[deko3d-selftest] %s: a=0x%llx b=0x%llx realDst=0x%llx\n", what,
                 (unsigned long long)dkMemBlockGetGpuAddr(a),
                 (unsigned long long)dkMemBlockGetGpuAddr(b), (unsigned long long)realDst);
    step("empty-own-cmdbuf-bind", [&](DkCmdBuf c) {
        dkCmdBufBindImageDescriptorSet(c, dkMemBlockGetGpuAddr(mImageDescriptorMemory), kMaxImageDescriptors);
    });
    step("pushdata-small", [&](DkCmdBuf c) { dkCmdBufPushData(c, dkMemBlockGetGpuAddr(a), kData, 16); });
    std::fprintf(stderr, "[deko3d-selftest] readback %u %u\n",
                 static_cast<uint32_t*>(dkMemBlockGetCpuAddr(a))[0],
                 static_cast<uint32_t*>(dkMemBlockGetCpuAddr(a))[3]);
    step("copybuffer-small", [&](DkCmdBuf c) {
        dkCmdBufCopyBuffer(c, dkMemBlockGetGpuAddr(a), dkMemBlockGetGpuAddr(b), 16);
    });
    if (realDst != 0) {
        step("pushdata-realdst", [&](DkCmdBuf c) { dkCmdBufPushData(c, realDst, kData, 16); });
    }
}

void Device::DestroyImpl(DestroyReason reason) {
    DAWN_ASSERT(GetState() == State::Disconnected);
    if (mDkQueue != nullptr) {
        dkQueueWaitIdle(mDkQueue);
    }
    ReleaseResourcesUpTo(ExecutionSerial(UINT64_MAX));
    for (DkMemBlock block : mPendingDestroyedMemory) {
        dkMemBlockDestroy(block);
    }
    mPendingDestroyedMemory.clear();
    for (const ImageAllocation& allocation : mPendingFreedImageMemory) {
        ReleaseImageMemory(allocation);
    }
    mPendingFreedImageMemory.clear();
    for (auto& chunk : mImageChunks) {
        dkMemBlockDestroy(chunk->block);
    }
    mImageChunks.clear();
    auto destroyEntry = [](std::unique_ptr<CmdBufEntry>& entry) {
        if (entry == nullptr) {
            return;
        }
        dkCmdBufDestroy(entry->cmdBuf);
        for (DkMemBlock block : entry->memory) {
            dkMemBlockDestroy(block);
        }
        entry.reset();
    };
    destroyEntry(mCmdBuf);
    for (auto& entry : mFreeCmdBufs) {
        destroyEntry(entry);
    }
    mFreeCmdBufs.clear();
    for (auto* list : {&mPendingUploadMemory, &mFreeCmdMemory, &mFreeUploadMemory, &mCodeMemory}) {
        for (DkMemBlock block : *list) {
            dkMemBlockDestroy(block);
        }
        list->clear();
    }
    for (DkMemBlock* block : {&mImageDescriptorMemory, &mSamplerDescriptorMemory}) {
        if (*block != nullptr) {
            dkMemBlockDestroy(*block);
            *block = nullptr;
        }
    }
    if (mDkQueue != nullptr) {
        dkQueueDestroy(mDkQueue);
        mDkQueue = nullptr;
    }
    if (mDkDevice != nullptr) {
        dkDeviceDestroy(mDkDevice);
        mDkDevice = nullptr;
    }
}

// Command recording

DkMemBlock Device::AcquireMemBlock(std::vector<DkMemBlock>* freeList, uint32_t size, uint32_t flags) {
    if (!freeList->empty() && size <= dkMemBlockGetSize(freeList->back())) {
        DkMemBlock block = freeList->back();
        freeList->pop_back();
        return block;
    }
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, mDkDevice, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}

MaybeError Device::AcquireCommandBuffer() {
    if (!mFreeCmdBufs.empty()) {
        mCmdBuf = std::move(mFreeCmdBufs.back());
        mFreeCmdBufs.pop_back();
        return {};
    }
    auto entry = std::make_unique<CmdBufEntry>();
    entry->device = mDkDevice;
    DkCmdBufMaker cmdBufMaker;
    dkCmdBufMakerDefaults(&cmdBufMaker, mDkDevice);
    cmdBufMaker.userData = entry.get();
    cmdBufMaker.cbAddMem = &Device::AddCommandMemory;
    entry->cmdBuf = dkCmdBufCreate(&cmdBufMaker);
    DAWN_INVALID_IF(entry->cmdBuf == nullptr, "dkCmdBufCreate failed");
    mCmdBuf = std::move(entry);
    return {};
}

// static
void Device::AddCommandMemory(void* userData, DkCmdBuf cmdBuf, size_t minReqSize) {
    CmdBufEntry* entry = static_cast<CmdBufEntry*>(userData);
    DkDevice device = entry->device;
    const uint32_t size = std::max<uint32_t>(kCommandMemoryBlockSize,
                                             AlignUp(minReqSize, DK_MEMBLOCK_ALIGNMENT));
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, size);
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock block = dkMemBlockCreate(&maker);
    if (block == nullptr) {
        return;  // deko3d reports the failure through the debug callback.
    }
    entry->memory.push_back(block);
    dkCmdBufAddMemory(cmdBuf, block, 0, size);
}

DkCmdBuf Device::GetPendingCommandBuffer() {
    DkCmdBuf cmdBuf = mCmdBuf->cmdBuf;
    if (!mHasPendingCommands) {
        mHasPendingCommands = true;
        dkCmdBufBindImageDescriptorSet(cmdBuf, dkMemBlockGetGpuAddr(mImageDescriptorMemory),
                                       kMaxImageDescriptors);
        dkCmdBufBindSamplerDescriptorSet(cmdBuf, dkMemBlockGetGpuAddr(mSamplerDescriptorMemory),
                                         kMaxSamplerDescriptors);
        if (kDeko3dTrace) TraceFlush("after descriptor set bind");
    }
    return cmdBuf;
}

MaybeError Device::SubmitPendingCommands(ExecutionSerial serial) {
    // TEMP bring-up tracing: synchronous submits so a GPU fault kills the app right after the
    // offending list, with its serial as the last line in the log.
    static uint32_t sTraceCount = 0;
    const bool trace = kDeko3dTrace && sTraceCount < 400;
    if (trace) {
        ++sTraceCount;
        if (kDeko3dTrace) std::fprintf(stderr, "[deko3d-trace] submit serial=%llu pending=%d\n",
                     (unsigned long long)uint64_t(serial), int(mHasPendingCommands));
    }
    auto inFlight = std::make_unique<InFlight>();
    inFlight->serial = serial;
    if (mHasPendingCommands) {
        DkCmdList list = dkCmdBufFinishList(mCmdBuf->cmdBuf);
        dkQueueSubmitCommands(mDkQueue, list);
        // The list lives in this command buffer's memory until the fence signals; record the next
        // one into a different command buffer.
        inFlight->cmdBuf = std::move(mCmdBuf);
        DAWN_TRY(AcquireCommandBuffer());
    }
    dkQueueSignalFence(mDkQueue, &inFlight->fence, true);
    // Submitted lists only reach the GPU on a queue flush; without this, work (and the fence that
    // completes MapAsync) waits for the next present.
    dkQueueFlush(mDkQueue);
    inFlight->uploadMemory = std::move(mPendingUploadMemory);
    inFlight->destroyedMemory = std::move(mPendingDestroyedMemory);
    inFlight->freedImageMemory = std::move(mPendingFreedImageMemory);
    mPendingFreedImageMemory.clear();
    mPendingUploadMemory.clear();
    mPendingDestroyedMemory.clear();
    mCurrentUploadBlock = nullptr;
    mCurrentUploadOffset = 0;
    mHasPendingCommands = false;
    mInFlight.push_back(std::move(inFlight));
    if (trace) {
        dkQueueWaitIdle(mDkQueue);
        if (kDeko3dTrace) std::fprintf(stderr, "[deko3d-trace] submit serial=%llu done error=%d\n",
                     (unsigned long long)uint64_t(serial), int(dkQueueIsInErrorState(mDkQueue)));
    }
    DAWN_INVALID_IF(dkQueueIsInErrorState(mDkQueue), "deko3d queue entered an error state");
    return {};
}

void Device::ReleaseResourcesUpTo(ExecutionSerial completed) {
    while (!mInFlight.empty() && mInFlight.front()->serial <= completed) {
        std::unique_ptr<InFlight> done = std::move(mInFlight.front());
        mInFlight.pop_front();
        if (done->cmdBuf != nullptr) {
            dkCmdBufClear(done->cmdBuf->cmdBuf);
            // Clear rewinds to the most recently added chunk; earlier chunks are no longer used.
            auto& memory = done->cmdBuf->memory;
            if (memory.size() > 1) {
                for (size_t i = 0; i + 1 < memory.size(); ++i) {
                    dkMemBlockDestroy(memory[i]);
                }
                memory.erase(memory.begin(), memory.end() - 1);
            }
            mFreeCmdBufs.push_back(std::move(done->cmdBuf));
        }
        for (DkMemBlock block : done->uploadMemory) {
            if (dkMemBlockGetSize(block) == kUploadMemoryBlockSize) {
                mFreeUploadMemory.push_back(block);
            } else {
                dkMemBlockDestroy(block);
            }
        }
        for (DkMemBlock block : done->destroyedMemory) {
            dkMemBlockDestroy(block);
        }
        for (const ImageAllocation& allocation : done->freedImageMemory) {
            ReleaseImageMemory(allocation);
        }
        mCompletedSerial = std::max(mCompletedSerial, done->serial);
    }
}

ExecutionSerial Device::PollCompletedSerial() {
    ExecutionSerial completed = mCompletedSerial;
    for (const auto& inFlight : mInFlight) {
        if (dkFenceWait(&inFlight->fence, 0) != DkResult_Success) {
            break;
        }
        completed = inFlight->serial;
    }
    ReleaseResourcesUpTo(completed);
    return mCompletedSerial;
}

ExecutionSerial Device::WaitForSerial(ExecutionSerial serial, Nanoseconds timeout) {
    for (const auto& inFlight : mInFlight) {
        if (inFlight->serial > serial) {
            break;
        }
        const int64_t ns = uint64_t(timeout) > uint64_t(INT64_MAX) ? -1 : int64_t(uint64_t(timeout));
        if (dkFenceWait(&inFlight->fence, ns) != DkResult_Success) {
            break;
        }
    }
    return PollCompletedSerial();
}

namespace {
// Mirrors deko3d's private dk_ctrlcmd.h layout (TEMP bring-up dump).
struct DumpCtrlHeader {
    uint64_t type : 8;
    uint64_t extra : 24;
    uint64_t arg : 32;
};
struct DumpGpfifoEntry {
    DkGpuAddr iova;
    uint32_t numCmds;
    uint32_t flags;
};
void DumpCmdList(DkCmdList list, const std::vector<DkMemBlock>& memory) {
    std::fprintf(stderr, "[deko3d-dump] list=%p\n", reinterpret_cast<void*>(list));
    const DumpCtrlHeader* cur = reinterpret_cast<const DumpCtrlHeader*>(list);
    int guard = 0;
    while (cur != nullptr && guard++ < 64) {
        std::fprintf(stderr, "[deko3d-dump]  ctrl type=%u arg=%u\n", unsigned(cur->type),
                     unsigned(cur->arg));
        switch (cur->type) {
            case 0:
                cur = nullptr;
                break;
            case 1:
            case 2: {
                const DumpCtrlHeader* ptr = *reinterpret_cast<const DumpCtrlHeader* const*>(cur + 1);
                cur = cur->type == 1 ? ptr : reinterpret_cast<const DumpCtrlHeader*>(
                                                 reinterpret_cast<const char*>(cur + 1) + sizeof(void*));
                break;
            }
            case 3: {
                auto* entries = reinterpret_cast<const DumpGpfifoEntry*>(cur + 1);
                for (uint32_t i = 0; i < cur->arg; ++i) {
                    const auto& e = entries[i];
                    std::fprintf(stderr, "[deko3d-dump]   gpfifo iova=0x%llx n=%u flags=%u:",
                                 (unsigned long long)e.iova, e.numCmds, e.flags);
                    const uint32_t* words = nullptr;
                    for (DkMemBlock b : memory) {
                        DkGpuAddr base = dkMemBlockGetGpuAddr(b);
                        if (e.iova >= base && e.iova < base + dkMemBlockGetSize(b)) {
                            words = static_cast<const uint32_t*>(dkMemBlockGetCpuAddr(b)) + (e.iova - base) / 4;
                        }
                    }
                    for (uint32_t w = 0; words && w < e.numCmds && w < 48; ++w) {
                        std::fprintf(stderr, " %08x", words[w]);
                    }
                    std::fprintf(stderr, "\n");
                }
                cur = reinterpret_cast<const DumpCtrlHeader*>(entries + cur->arg);
                break;
            }
            case 4:
            case 5:
                cur = reinterpret_cast<const DumpCtrlHeader*>(reinterpret_cast<const char*>(cur + 1) +
                                                              sizeof(void*));
                break;
            default:
                std::fprintf(stderr, "[deko3d-dump]  (stop at unknown ctrl type)\n");
                cur = nullptr;
                break;
        }
    }
}
}  // namespace

MemStats& GetMemStats() {
    static MemStats stats;
    return stats;
}

void Device::ReportMemory(const char* why) {
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    size_t freeUpload = 0, cmdMem = 0, cmdBufs = mFreeCmdBufs.size() + 1;
    for (DkMemBlock b : mFreeUploadMemory) freeUpload += dkMemBlockGetSize(b);
    auto countCmd = [&](const std::unique_ptr<CmdBufEntry>& e) {
        if (e) for (DkMemBlock b : e->memory) cmdMem += dkMemBlockGetSize(b);
    };
    countCmd(mCmdBuf);
    for (auto& e : mFreeCmdBufs) countCmd(e);
    size_t inflightUpload = 0, inflightDestroyed = 0;
    for (auto& f : mInFlight) {
        if (f->cmdBuf) { countCmd(f->cmdBuf); ++cmdBufs; }
        for (DkMemBlock b : f->uploadMemory) inflightUpload += dkMemBlockGetSize(b);
        for (DkMemBlock b : f->destroyedMemory) inflightDestroyed += dkMemBlockGetSize(b);
    }
    size_t pendingDestroyed = 0;
    for (DkMemBlock b : mPendingDestroyedMemory) pendingDestroyed += dkMemBlockGetSize(b);
    const MemStats& st = GetMemStats();
    std::fprintf(stderr,
                 "[deko3d-mem] %s: process used=%lluM total=%lluM | buffers %lld (%lldM) textures %lld (%lldM) | "
                 "cmdbufs %zu (%zuK) freeUpload %zuM inflight=%zu upload %zuM destroyed %zuM pendingDestroyed %zuM "
                 "code blocks %zu image chunks %zu\n",
                 why, (unsigned long long)(used >> 20), (unsigned long long)(total >> 20),
                 (long long)st.bufferCount.load(), (long long)(st.bufferBytes.load() >> 20),
                 (long long)st.textureCount.load(), (long long)(st.textureBytes.load() >> 20), cmdBufs,
                 cmdMem >> 10, freeUpload >> 20, mInFlight.size(), inflightUpload >> 20,
                 inflightDestroyed >> 20, pendingDestroyed >> 20, mCodeMemory.size(), mImageChunks.size());
}

void Device::DebugDumpImage(const DkImageView& view, uint32_t width, uint32_t height,
                            wgpu::TextureFormat format, const char* name) {
    const bool rgba = format == wgpu::TextureFormat::RGBA8Unorm ||
                      format == wgpu::TextureFormat::RGBA8UnormSrgb;
    const bool bgra = format == wgpu::TextureFormat::BGRA8Unorm ||
                      format == wgpu::TextureFormat::BGRA8UnormSrgb;
    if (!(rgba || bgra)) std::fprintf(stderr, "[deko3d-frame] %s %ux%u unsupported format=%d\n", name, width, height,
                 int(format));
    if ((!rgba && !bgra) || width == 0 || height == 0 || dkQueueIsInErrorState(mDkQueue) ||
        mDebugDumpIndex >= 100000) {
        return;
    }
    if (mHasPendingCommands) {
        dkQueueSubmitCommands(mDkQueue, dkCmdBufFinishList(mCmdBuf->cmdBuf));
    }
    const uint32_t byteSize = width * height * 4;
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, mDkDevice, AlignUp(byteSize, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock out = dkMemBlockCreate(&mm);
    dkMemBlockMakerDefaults(&mm, mDkDevice, 64 * 1024);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock cmdMem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, mDkDevice);
    DkCmdBuf cb = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(cb, cmdMem, 0, 64 * 1024);
    DkImageRect rect = {0, 0, 0, width, height, 1};
    DkCopyBuf dst = {dkMemBlockGetGpuAddr(out), width * 4, byteSize};
    dkCmdBufCopyImageToBuffer(cb, &view, &rect, &dst, 0);
    dkQueueSubmitCommands(mDkQueue, dkCmdBufFinishList(cb));
    dkQueueWaitIdle(mDkQueue);
    if (!dkQueueIsInErrorState(mDkQueue)) {
        // Summary instead of a file: non-black pixel count, bounding box, average colour.
        const uint8_t* sp = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(out));
        uint64_t count = 0, sr = 0, sg = 0, sb = 0, sa = 0;
        uint32_t x0 = width, y0 = height, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* p = sp + (y * width + x) * 4;
                sa += p[3];
                if (p[0] | p[1] | p[2]) {
                    ++count;
                    sr += p[0]; sg += p[1]; sb += p[2];
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
            }
        }
        const uint64_t n = count ? count : 1;
        std::fprintf(stderr,
                     "[deko3d-frame] %s %ux%u lit=%llu/%u (%.1f%%) bbox=%u,%u-%u,%u avg=%02llx%02llx%02llx alpha=%llu\n",
                     name, width, height, (unsigned long long)count, width * height,
                     100.0 * double(count) / double(width * height), x0, y0, x1, y1,
                     (unsigned long long)(sr / n), (unsigned long long)(sg / n),
                     (unsigned long long)(sb / n), (unsigned long long)(sa / (uint64_t(width) * height)));
    }
    if (false) {
        char path[128];
        std::snprintf(path, sizeof(path), "sdmc:/switch/WiiCompiled/dump_%03u_%s.ppm",
                      mDebugDumpIndex++, name);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", width, height);
            const uint8_t* px = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(out));
            std::vector<uint8_t> row(width * 3);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t* p = px + (y * width + x) * 4;
                    row[x * 3] = bgra ? p[2] : p[0];
                    row[x * 3 + 1] = p[1];
                    row[x * 3 + 2] = bgra ? p[0] : p[2];
                }
                std::fwrite(row.data(), 1, row.size(), f);
            }
            std::fclose(f);
        }
    }
    dkCmdBufDestroy(cb);
    dkMemBlockDestroy(cmdMem);
    dkMemBlockDestroy(out);
    if (dkQueueIsInErrorState(mDkQueue)) {
        std::fprintf(stderr, "[deko3d-frame] %s queue error\n", name);
    }
}

void Device::TraceFlush(const char* what) {
    static uint32_t sCount = 0;
    if (sCount >= 600 || dkQueueIsInErrorState(mDkQueue)) {
        return;
    }
    ++sCount;
    if (kDeko3dTrace) std::fprintf(stderr, "[deko3d-trace] flush before: %s\n", what);
    DkCmdList list = dkCmdBufFinishList(mCmdBuf->cmdBuf);
    DumpCmdList(list, mCmdBuf->memory);
    dkQueueSubmitCommands(mDkQueue, list);
    dkQueueWaitIdle(mDkQueue);
    if (kDeko3dTrace) std::fprintf(stderr, "[deko3d-trace] flush ok=%d: %s\n", int(!dkQueueIsInErrorState(mDkQueue)),
                 what);
}

void Device::WaitIdle() {
    dkQueueWaitIdle(mDkQueue);
    PollCompletedSerial();
}

MaybeError Device::AllocateUploadMemory(uint64_t size, uint8_t** cpuAddr, DkGpuAddr* gpuAddr) {
    constexpr uint32_t kAlignment = 16;
    const uint32_t alignedSize = AlignUp(std::max<uint64_t>(size, 1), kAlignment);
    if (alignedSize > kUploadMemoryBlockSize / 2) {
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, mDkDevice, AlignUp(alignedSize, DK_MEMBLOCK_ALIGNMENT));
        maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        DkMemBlock block = dkMemBlockCreate(&maker);
        if (block == nullptr) {
            return DAWN_OUT_OF_MEMORY_ERROR("Allocating upload memory failed.");
        }
        mPendingUploadMemory.push_back(block);
        *cpuAddr = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block));
        *gpuAddr = dkMemBlockGetGpuAddr(block);
        return {};
    }
    if (mCurrentUploadBlock == nullptr || mCurrentUploadOffset + alignedSize > kUploadMemoryBlockSize) {
        mCurrentUploadBlock =
            AcquireMemBlock(&mFreeUploadMemory, kUploadMemoryBlockSize,
                            DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (mCurrentUploadBlock == nullptr) {
            return DAWN_OUT_OF_MEMORY_ERROR("Allocating upload memory failed.");
        }
        mPendingUploadMemory.push_back(mCurrentUploadBlock);
        mCurrentUploadOffset = 0;
    }
    *cpuAddr = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(mCurrentUploadBlock)) + mCurrentUploadOffset;
    *gpuAddr = dkMemBlockGetGpuAddr(mCurrentUploadBlock) + mCurrentUploadOffset;
    mCurrentUploadOffset += alignedSize;
    return {};
}

void Device::RecordBufferWrite(DkGpuAddr dst, const uint8_t* data, uint64_t size) {
    DAWN_ASSERT(size % 4 == 0);
    DkCmdBuf cmdBuf = GetPendingCommandBuffer();
    // Large writes would bloat command memory; stage them in upload memory and copy on the GPU.
    constexpr uint64_t kInlineLimit = 4096;
    if (size > kInlineLimit) {
        uint8_t* upload = nullptr;
        DkGpuAddr uploadAddr = 0;
        MaybeError allocated = AllocateUploadMemory(size, &upload, &uploadAddr);
        if (allocated.IsSuccess()) {
            std::memcpy(upload, data, size);
            dkCmdBufCopyBuffer(cmdBuf, uploadAddr, dst, static_cast<uint32_t>(size));
            return;
        }
        allocated.AcquireError();
    }
    constexpr uint64_t kMaxChunk = 0x7FFC;
    while (size != 0) {
        const uint32_t chunk = static_cast<uint32_t>(std::min(size, kMaxChunk));
        dkCmdBufPushData(cmdBuf, dst, data, chunk);
        dst += chunk;
        data += chunk;
        size -= chunk;
    }
}

namespace {
constexpr uint32_t kImageChunkSize = 64 * 1024 * 1024;
constexpr uint32_t kDedicatedImageThreshold = 16 * 1024 * 1024;
}  // namespace

MaybeError Device::AllocateImageMemory(uint32_t size, uint32_t alignment, ImageAllocation* out) {
    alignment = std::max<uint32_t>(alignment, 1);
    if (size > kDedicatedImageThreshold) {
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, mDkDevice, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
        maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
        DkMemBlock block = dkMemBlockCreate(&maker);
        if (block == nullptr) {
            return DAWN_OUT_OF_MEMORY_ERROR("dkMemBlockCreate failed for a large texture.");
        }
        *out = {block, 0, dkMemBlockGetSize(block), true};
        return {};
    }

    std::lock_guard<std::mutex> lock(mImageMemoryMutex);
    auto tryChunk = [&](ImageChunk& chunk) -> bool {
        for (auto it = chunk.freeRanges.begin(); it != chunk.freeRanges.end(); ++it) {
            const uint32_t rangeStart = it->first;
            const uint32_t rangeEnd = it->first + it->second;
            const uint32_t start = AlignUp(rangeStart, alignment);
            if (start < rangeStart || uint64_t(start) + size > rangeEnd) {
                continue;
            }
            chunk.freeRanges.erase(it);
            if (start > rangeStart) {
                chunk.freeRanges[rangeStart] = start - rangeStart;
            }
            if (start + size < rangeEnd) {
                chunk.freeRanges[start + size] = rangeEnd - (start + size);
            }
            chunk.used += size;
            *out = {chunk.block, start, size, false};
            return true;
        }
        return false;
    };
    for (auto& chunk : mImageChunks) {
        if (kImageChunkSize - chunk->used >= size && tryChunk(*chunk)) {
            return {};
        }
    }
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, mDkDevice, kImageChunkSize);
    maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock block = dkMemBlockCreate(&maker);
    if (block == nullptr) {
        return DAWN_OUT_OF_MEMORY_ERROR("dkMemBlockCreate failed for a texture memory chunk.");
    }
    auto chunk = std::make_unique<ImageChunk>();
    chunk->block = block;
    chunk->freeRanges[0] = kImageChunkSize;
    const bool ok = tryChunk(*chunk);
    DAWN_ASSERT(ok);
    mImageChunks.push_back(std::move(chunk));
    return {};
}

void Device::ReleaseImageMemory(const ImageAllocation& allocation) {
    if (allocation.dedicated) {
        dkMemBlockDestroy(allocation.block);
        return;
    }
    std::lock_guard<std::mutex> lock(mImageMemoryMutex);
    for (auto chunkIt = mImageChunks.begin(); chunkIt != mImageChunks.end(); ++chunkIt) {
        ImageChunk& chunk = **chunkIt;
        if (chunk.block != allocation.block) {
            continue;
        }
        uint32_t start = allocation.offset;
        uint32_t length = allocation.size;
        auto next = chunk.freeRanges.lower_bound(start);
        if (next != chunk.freeRanges.end() && start + length == next->first) {
            length += next->second;
            next = chunk.freeRanges.erase(next);
        }
        if (next != chunk.freeRanges.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) {
                start = prev->first;
                length += prev->second;
                chunk.freeRanges.erase(prev);
            }
        }
        chunk.freeRanges[start] = length;
        chunk.used -= allocation.size;
        // Keep one empty chunk around for reuse; return the rest to the system.
        if (chunk.used == 0) {
            size_t emptyChunks = 0;
            for (auto& c : mImageChunks) {
                emptyChunks += c->used == 0 ? 1 : 0;
            }
            if (emptyChunks > 1) {
                dkMemBlockDestroy(chunk.block);
                mImageChunks.erase(chunkIt);
            }
        }
        return;
    }
    DAWN_UNREACHABLE();
}

void Device::FreeImageMemoryWhenIdle(const ImageAllocation& allocation) {
    if (allocation.block == nullptr) {
        return;
    }
    if (!mHasPendingCommands && mInFlight.empty()) {
        ReleaseImageMemory(allocation);
        return;
    }
    mPendingFreedImageMemory.push_back(allocation);
}

void Device::DestroyMemBlockWhenIdle(DkMemBlock memBlock) {
    if (!mHasPendingCommands && mInFlight.empty()) {
        dkMemBlockDestroy(memBlock);
        return;
    }
    mPendingDestroyedMemory.push_back(memBlock);
}

// Descriptors

ResultOrError<uint32_t> Device::AllocateImageDescriptor(const DkImageView& view, bool loadStore) {
    std::lock_guard<std::mutex> lock(mDescriptorMutex);
    if (mFreeImageDescriptors.empty()) {
        return DAWN_OUT_OF_MEMORY_ERROR("Out of deko3d image descriptors.");
    }
    const uint32_t id = mFreeImageDescriptors.back();
    mFreeImageDescriptors.pop_back();
    auto* descriptors = static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(mImageDescriptorMemory));
    dkImageDescriptorInitialize(&descriptors[id], &view, loadStore, false);
    return id;
}

void Device::ReleaseImageDescriptor(uint32_t id) {
    std::lock_guard<std::mutex> lock(mDescriptorMutex);
    mFreeImageDescriptors.push_back(id);
}

ResultOrError<uint32_t> Device::AllocateSamplerDescriptor(const DkSampler& sampler) {
    std::lock_guard<std::mutex> lock(mDescriptorMutex);
    if (mFreeSamplerDescriptors.empty()) {
        return DAWN_OUT_OF_MEMORY_ERROR("Out of deko3d sampler descriptors.");
    }
    const uint32_t id = mFreeSamplerDescriptors.back();
    mFreeSamplerDescriptors.pop_back();
    auto* descriptors =
        static_cast<DkSamplerDescriptor*>(dkMemBlockGetCpuAddr(mSamplerDescriptorMemory));
    dkSamplerDescriptorInitialize(&descriptors[id], &sampler);
    return id;
}

void Device::ReleaseSamplerDescriptor(uint32_t id) {
    std::lock_guard<std::mutex> lock(mDescriptorMutex);
    mFreeSamplerDescriptors.push_back(id);
}

MaybeError Device::LoadShader(const std::vector<uint8_t>& dksh, DkShader* shader) {
    std::lock_guard<std::mutex> lock(mCodeMutex);
    const uint32_t size = AlignUp(dksh.size(), DK_SHADER_CODE_ALIGNMENT);
    // deko3d forbids shader code in the last DK_SHADER_CODE_UNUSABLE_SIZE bytes of a block.
    if (mCodeMemory.empty() ||
        mCodeOffset + size + DK_SHADER_CODE_UNUSABLE_SIZE > dkMemBlockGetSize(mCodeMemory.back())) {
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(
            &maker, mDkDevice,
            std::max(kCodeMemoryBlockSize,
                     AlignUp(size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT)));
        maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        DkMemBlock block = dkMemBlockCreate(&maker);
        if (block == nullptr) {
            return DAWN_OUT_OF_MEMORY_ERROR("Allocating shader code memory failed.");
        }
        mCodeMemory.push_back(block);
        mCodeOffset = 0;
    }
    DkMemBlock block = mCodeMemory.back();
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block)) + mCodeOffset, dksh.data(),
                dksh.size());
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, block, mCodeOffset);
    dkShaderInitialize(shader, &maker);
    mCodeOffset += size;
    DAWN_INVALID_IF(!dkShaderIsValid(shader), "deko3d rejected a compiled shader module.");
    return {};
}

// Object creation

ResultOrError<Ref<BindGroupBase>> Device::CreateBindGroupImpl(
    const UnpackedPtr<BindGroupDescriptor>& descriptor) {
    return BindGroup::Create(this, descriptor);
}
ResultOrError<Ref<BindGroupLayoutInternalBase>> Device::CreateBindGroupLayoutImpl(
    const UnpackedPtr<BindGroupLayoutDescriptor>& descriptor) {
    return AcquireRef(new BindGroupLayout(this, descriptor));
}
ResultOrError<Ref<BufferBase>> Device::CreateBufferImpl(
    const UnpackedPtr<BufferDescriptor>& descriptor) {
    return Buffer::Create(this, descriptor);
}
ResultOrError<Ref<CommandBufferBase>> Device::CreateCommandBuffer(
    CommandEncoder* encoder,
    const CommandBufferDescriptor* descriptor) {
    return AcquireRef(new CommandBuffer(encoder, descriptor));
}
Ref<ComputePipelineBase> Device::CreateUninitializedComputePipelineImpl(
    const UnpackedPtr<ComputePipelineDescriptor>& descriptor) {
    return ComputePipeline::CreateUninitialized(this, descriptor);
}
ResultOrError<Ref<PipelineLayoutBase>> Device::CreatePipelineLayoutImpl(
    const UnpackedPtr<PipelineLayoutDescriptor>& descriptor) {
    return AcquireRef(new PipelineLayout(this, descriptor));
}
ResultOrError<Ref<QuerySetBase>> Device::CreateQuerySetImpl(const QuerySetDescriptor* descriptor) {
    return QuerySet::Create(this, descriptor);
}
Ref<RenderPipelineBase> Device::CreateUninitializedRenderPipelineImpl(
    const UnpackedPtr<RenderPipelineDescriptor>& descriptor) {
    return RenderPipeline::CreateUninitialized(this, descriptor);
}
ResultOrError<Ref<ResourceTableBase>> Device::CreateResourceTableImpl(
    const ResourceTableDescriptor* descriptor) {
    return DAWN_UNIMPLEMENTED_ERROR("Resource tables are not supported by the deko3d backend.");
}
ResultOrError<Ref<SamplerBase>> Device::CreateSamplerImpl(const SamplerDescriptor* descriptor) {
    return Sampler::Create(this, descriptor);
}
ResultOrError<Ref<ShaderModuleBase>> Device::CreateShaderModuleImpl(
    const UnpackedPtr<ShaderModuleDescriptor>& descriptor,
    const std::vector<tint::wgsl::Extension>& internalExtensions) {
    return ShaderModule::Create(this, descriptor, internalExtensions);
}
ResultOrError<Ref<SwapChainBase>> Device::CreateSwapChainImpl(Surface* surface,
                                                              SwapChainBase* previousSwapChain,
                                                              const SurfaceConfiguration* config) {
    return SwapChain::Create(this, surface, previousSwapChain, config);
}
ResultOrError<Ref<TextureBase>> Device::CreateTextureImpl(
    const UnpackedPtr<TextureDescriptor>& descriptor) {
    return Texture::Create(this, descriptor);
}
ResultOrError<Ref<TextureViewBase>> Device::CreateTextureViewImpl(
    TextureBase* texture,
    const UnpackedPtr<TextureViewDescriptor>& descriptor) {
    return TextureView::Create(texture, descriptor);
}

MaybeError Device::TickImpl() {
    return {};
}

MaybeError Device::CopyFromStagingToBuffer(BufferBase* source,
                                           uint64_t sourceOffset,
                                           BufferBase* destination,
                                           uint64_t destinationOffset,
                                           uint64_t size) {
    Buffer* dst = ToBackend(destination);
    DAWN_TRY(dst->EnsureDataInitializedAsDestination(destinationOffset, size));
    RecordBufferWrite(dst->GetGpuAddr() + destinationOffset,
                      ToBackend(source)->GetCpuAddr() + sourceOffset, size);
    dst->TrackUsage();
    return {};
}

MaybeError Device::CopyFromStagingToTextureImpl(BufferBase* source,
                                                const TexelCopyBufferLayout& src,
                                                const TextureCopy& dst,
                                                const Extent3D& copySizePixels) {
    return RecordTextureUpload(dst, ToBackend(source)->GetCpuAddr(), src, copySizePixels);
}

MaybeError Device::RecordTextureUpload(const TextureCopy& dst,
                                       const uint8_t* data,
                                       const TexelCopyBufferLayout& layout,
                                       const Extent3D& copySizePixels) {
    Texture* texture = ToBackend(dst.texture.Get());
    SubresourceRange range = GetSubresourcesAffectedByCopy(dst, copySizePixels);
    if (IsCompleteSubresourceCopiedTo(texture, copySizePixels, dst.mipLevel, dst.aspect)) {
        texture->SetIsSubresourceContentInitialized(true, range);
    } else {
        DAWN_TRY(texture->EnsureSubresourceContentInitialized(range));
    }

    const TexelBlockInfo& block = texture->GetFormat().GetAspectInfo(dst.aspect).block;
    const bool is3D = texture->GetDimension() == wgpu::TextureDimension::e3D;
    const Origin3D origin = dst.origin.ToOrigin3D();
    const uint32_t imagesPerCopy = is3D ? copySizePixels.depthOrArrayLayers : 1;
    const uint32_t layerCount = is3D ? 1 : copySizePixels.depthOrArrayLayers;
    const uint64_t rowsPerImage = (copySizePixels.height + block.height - 1) / block.height;
    const uint64_t bytesPerImage = uint64_t(layout.bytesPerRow) * layout.rowsPerImage;
    const uint64_t blocksPerRow = layout.bytesPerRow / block.byteSize;

    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        const uint64_t layerOffset = layout.offset + bytesPerImage * layer;
        const uint64_t byteSize = is3D ? bytesPerImage * imagesPerCopy : bytesPerImage;
        uint8_t* upload = nullptr;
        DkGpuAddr uploadAddr = 0;
        DAWN_TRY(AllocateUploadMemory(byteSize, &upload, &uploadAddr));
        std::memcpy(upload, data + layerOffset, byteSize);

        DkImageView view;
        dkImageViewDefaults(&view, texture->GetDkImage());
        view.mipLevelOffset = static_cast<uint8_t>(dst.mipLevel);
        view.mipLevelCount = 1;
        if (!is3D) {
            view.layerOffset = static_cast<uint16_t>(origin.z + layer);
            view.layerCount = 1;
        }
        if (dst.aspect == Aspect::Stencil) {
            view.dsSource = DkDsSource_Stencil;
        }
        // DkCopyBuf strides are in bytes: bytes per row and bytes per image (layer stride).
        DkCopyBuf copyBuf = {uploadAddr, static_cast<uint32_t>(blocksPerRow * block.byteSize),
                             static_cast<uint32_t>(bytesPerImage)};
        DkImageRect rect = {origin.x, origin.y, is3D ? origin.z : 0,
                            copySizePixels.width, copySizePixels.height, imagesPerCopy};
        (void)rowsPerImage;
        dkCmdBufCopyBufferToImage(GetPendingCommandBuffer(), &copyBuf, &view, &rect, 0);
    }
    return {};
}

uint32_t Device::GetOptimalBytesPerRowAlignment() const {
    return 1;
}

uint64_t Device::GetOptimalBufferToTextureCopyOffsetAlignment() const {
    return 1;
}

float Device::GetTimestampPeriodInNS() const {
    return 1.0f;
}

bool Device::CanTextureLoadResolveTargetInTheSameRenderpass() const {
    return false;
}

}  // namespace dawn::native::deko3d
