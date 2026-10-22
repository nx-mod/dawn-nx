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

#ifndef SRC_DAWN_NATIVE_DEKO3D_BUFFERDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_BUFFERDEKO3D_H_

#include <deko3d.h>

#include "src/dawn/native/Buffer.h"

namespace dawn::native::deko3d {

class Device;

// Switch memory is unified: every buffer lives in its own CPU-visible DkMemBlock, so mapping is a
// plain pointer and CPU writes need no staging copy.
class Buffer final : public BufferBase {
  public:
    static ResultOrError<Ref<Buffer>> CreateInternalBuffer(Device* device,
                                                           const BufferDescriptor* descriptor,
                                                           bool shouldLazyClear);
    static ResultOrError<Ref<Buffer>> Create(Device* device,
                                             const UnpackedPtr<BufferDescriptor>& descriptor);

    DkMemBlock GetMemBlock() const { return mMemBlock; }
    DkGpuAddr GetGpuAddr() const { return dkMemBlockGetGpuAddr(mMemBlock); }
    uint8_t* GetCpuAddr() const { return static_cast<uint8_t*>(dkMemBlockGetCpuAddr(mMemBlock)); }

    MaybeError EnsureDataInitialized(bool* outDidDataInitialization = nullptr);
    MaybeError EnsureDataInitializedAsDestination(uint64_t offset,
                                                  uint64_t size,
                                                  bool* outDidDataInitialization = nullptr);
    MaybeError EnsureDataInitializedAsDestination(const CopyTextureToBufferCmd* copy,
                                                  bool* outDidDataInitialization = nullptr);

    void TrackUsage() { MarkUsedInPendingCommands(); }

  private:
    Buffer(Device* device, const UnpackedPtr<BufferDescriptor>& descriptor);
    ~Buffer() override;
    MaybeError Initialize(bool clearNonzero);
    MaybeError MapAsyncImpl(wgpu::MapMode mode, size_t offset, size_t size) override;
    MaybeError FinalizeMapImpl(BufferState newState) override;
    void UnmapImpl(BufferState oldState, BufferState newState) override;
    void DestroyImpl(DestroyReason reason) override;
    bool IsCPUWritableAtCreation() const override;
    MaybeError MapAtCreationImpl() override;
    void* GetMappedPointerImpl() override;

    void InitializeToZero();

    DkMemBlock mMemBlock = nullptr;
};

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_BUFFERDEKO3D_H_
