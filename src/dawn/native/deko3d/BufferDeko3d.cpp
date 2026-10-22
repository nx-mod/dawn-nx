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

#include "src/dawn/native/deko3d/BufferDeko3d.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "src/dawn/native/CommandBuffer.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"

namespace dawn::native::deko3d {

// static
ResultOrError<Ref<Buffer>> Buffer::CreateInternalBuffer(Device* device,
                                                        const BufferDescriptor* descriptor,
                                                        bool shouldLazyClear) {
    Ref<Buffer> buffer;
    DAWN_TRY_ASSIGN(buffer, Buffer::Create(device, Unpack(descriptor)));
    if (!shouldLazyClear) {
        buffer->SetInitialized(true);
    }
    if (descriptor->mappedAtCreation) {
        [[maybe_unused]] bool usingStagingBuffer;
        DAWN_TRY_ASSIGN(usingStagingBuffer, buffer->MapAtCreationInternal());
    }
    return std::move(buffer);
}

// static
ResultOrError<Ref<Buffer>> Buffer::Create(Device* device,
                                          const UnpackedPtr<BufferDescriptor>& descriptor) {
    Ref<Buffer> buffer = AcquireRef(new Buffer(device, descriptor));
    const bool clearNonzero =
        device->IsToggleEnabled(Toggle::NonzeroClearResourcesOnCreationForTesting) &&
        !descriptor->mappedAtCreation;
    DAWN_TRY(buffer->Initialize(clearNonzero));
    return std::move(buffer);
}

Buffer::Buffer(Device* device, const UnpackedPtr<BufferDescriptor>& descriptor)
    : BufferBase(device, descriptor) {
    // Keep at least 4 bytes so clamped shader accesses stay in bounds, and round uniform buffers
    // to 16 bytes since Tint may polyfill them as array<vec4u, N>.
    const uint64_t alignment = (GetUsage() & wgpu::BufferUsage::Uniform) ? 16u : 4u;
    mAllocatedSize = Align(std::max(GetSize(), uint64_t(4u)), alignment);
}

Buffer::~Buffer() = default;

MaybeError Buffer::Initialize(bool clearNonzero) {
    Device* device = ToBackend(GetDevice());
    const uint64_t blockSize = Align(GetAllocatedSize(), uint64_t(DK_MEMBLOCK_ALIGNMENT));
    DAWN_INVALID_IF(blockSize > UINT32_MAX, "Buffer size %u is too large for deko3d.", GetSize());

    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device->GetDkDevice(), static_cast<uint32_t>(blockSize));
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    mMemBlock = dkMemBlockCreate(&maker);
    if (mMemBlock == nullptr) {
        ToBackend(GetDevice())->ReportMemory("buffer alloc failed");
        std::fprintf(stderr, "[deko3d-mem] failed buffer size=%llu\n", (unsigned long long)blockSize);
        return DAWN_OUT_OF_MEMORY_ERROR("dkMemBlockCreate failed for a buffer.");
    }
    GetMemStats().bufferBytes += blockSize;
    GetMemStats().bufferCount++;
    if (clearNonzero) {
        std::memset(GetCpuAddr(), 1, blockSize);
    }
    return {};
}

MaybeError Buffer::EnsureDataInitialized(bool* outDidDataInitialization) {
    const bool needed = NeedsInitialization();
    if (needed) {
        InitializeToZero();
    }
    if (outDidDataInitialization) {
        *outDidDataInitialization = needed;
    }
    return {};
}

MaybeError Buffer::EnsureDataInitializedAsDestination(uint64_t offset,
                                                      uint64_t size,
                                                      bool* outDidDataInitialization) {
    if (outDidDataInitialization) {
        *outDidDataInitialization = false;
    }
    if (!NeedsInitialization()) {
        return {};
    }
    if (IsFullBufferRange(offset, size)) {
        SetInitialized(true);
        return {};
    }
    InitializeToZero();
    if (outDidDataInitialization) {
        *outDidDataInitialization = true;
    }
    return {};
}

MaybeError Buffer::EnsureDataInitializedAsDestination(const CopyTextureToBufferCmd* copy,
                                                      bool* outDidDataInitialization) {
    if (outDidDataInitialization) {
        *outDidDataInitialization = false;
    }
    if (!NeedsInitialization()) {
        return {};
    }
    if (IsFullBufferOverwrittenInTextureToBufferCopy(copy)) {
        SetInitialized(true);
        return {};
    }
    InitializeToZero();
    if (outDidDataInitialization) {
        *outDidDataInitialization = true;
    }
    return {};
}

void Buffer::InitializeToZero() {
    DAWN_ASSERT(NeedsInitialization());
    std::memset(GetCpuAddr(), 0, GetAllocatedSize());
    GetDevice()->IncrementLazyClearCountForTesting();
    SetInitialized(true);
}

bool Buffer::IsCPUWritableAtCreation() const {
    return true;
}

MaybeError Buffer::MapAtCreationImpl() {
    return {};
}

MaybeError Buffer::MapAsyncImpl(wgpu::MapMode mode, size_t offset, size_t size) {
    // The frontend resolves the map once the queue has finished every command submitted so far,
    // after which the unified-memory pointer is safe to read or write.
    DAWN_TRY(EnsureDataInitialized());
    return {};
}

MaybeError Buffer::FinalizeMapImpl(BufferState newState) {
    return {};
}

void* Buffer::GetMappedPointerImpl() {
    return GetCpuAddr();
}

void Buffer::UnmapImpl(BufferState oldState, BufferState newState) {}

void Buffer::DestroyImpl(DestroyReason reason) {
    BufferBase::DestroyImpl(reason);
    if (mMemBlock != nullptr) {
        GetMemStats().bufferBytes -= dkMemBlockGetSize(mMemBlock);
        GetMemStats().bufferCount--;
        // Commands already submitted may still reference the memory; free it once they finish.
        ToBackend(GetDevice())->DestroyMemBlockWhenIdle(mMemBlock);
        mMemBlock = nullptr;
    }
}

}  // namespace dawn::native::deko3d
