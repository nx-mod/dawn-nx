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

#include "src/dawn/native/deko3d/QueueDeko3d.h"

#include <cstring>

#include "src/dawn/native/CommandValidation.h"
#include "src/dawn/native/Commands.h"
#include "src/dawn/native/deko3d/BufferDeko3d.h"
#include "src/dawn/native/deko3d/CommandBufferDeko3d.h"
#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/TextureDeko3d.h"

namespace dawn::native::deko3d {

// static
ResultOrError<Ref<Queue>> Queue::Create(Device* device, const QueueDescriptor* descriptor) {
    return AcquireRef(new Queue(device, descriptor));
}

Queue::Queue(Device* device, const QueueDescriptor* descriptor) : QueueBase(device, descriptor) {}

MaybeError Queue::SubmitImpl(uint32_t commandCount, CommandBufferBase* const* commands) {
    Device* device = ToBackend(GetDevice());
    for (uint32_t i = 0; i < commandCount; ++i) {
        DAWN_TRY(ToBackend(commands[i])->RecordCommands(device->GetPendingCommandBuffer()));
    }
    return SubmitPendingCommandsImpl();
}

MaybeError Queue::WriteBufferImpl(BufferBase* buffer,
                                  uint64_t bufferOffset,
                                  const void* data,
                                  size_t size) {
    Device* device = ToBackend(GetDevice());
    Buffer* dst = ToBackend(buffer);
    DAWN_TRY(dst->EnsureDataInitializedAsDestination(bufferOffset, size));
    // Unified memory: when no recorded or in-flight GPU work uses the buffer, write it directly.
    // Otherwise earlier commands must still see the old contents: large writes wait for the
    // in-flight work (if it is all submitted), small ones go through the command stream.
    constexpr size_t kInlineWriteLimit = 64 * 1024;
    ExecutionSerial lastUsage = dst->GetLastUsageSerial();
    bool inUse = lastUsage > device->PollCompletedSerial();
    if (inUse && size > kInlineWriteLimit && lastUsage <= GetLastSubmittedCommandSerial()) {
        device->WaitForSerial(lastUsage, Nanoseconds(uint64_t(-1)));
        inUse = lastUsage > device->PollCompletedSerial();
    }
    if (!inUse) {
        std::memcpy(dst->GetCpuAddr() + bufferOffset, data, size);
        return {};
    }
    device->RecordBufferWrite(dst->GetGpuAddr() + bufferOffset, static_cast<const uint8_t*>(data),
                              size);
    dst->TrackUsage();
    return {};
}

MaybeError Queue::WriteTextureImpl(const TexelCopyTextureInfo& destination,
                                   const void* data,
                                   size_t dataSize,
                                   const TexelCopyBufferLayout& dataLayout,
                                   const Extent3D& writeSizePixel) {
    TextureCopy textureCopy;
    textureCopy.texture = destination.texture;
    textureCopy.mipLevel = destination.mipLevel;
    textureCopy.origin = destination.origin;
    textureCopy.aspect = SelectFormatAspects(destination.texture->GetFormat(), destination.aspect);
    return ToBackend(GetDevice())
        ->RecordTextureUpload(textureCopy, static_cast<const uint8_t*>(data), dataLayout,
                              writeSizePixel);
}

bool Queue::HasPendingCommands() const {
    return ToBackend(GetDevice())->HasPendingCommands();
}

MaybeError Queue::SubmitPendingCommandsImpl() {
    Device* device = ToBackend(GetDevice());
    IncrementLastSubmittedCommandSerial();
    return device->SubmitPendingCommands(GetLastSubmittedCommandSerial());
}

ResultOrError<ExecutionSerial> Queue::CheckAndUpdateCompletedSerials() {
    return ToBackend(GetDevice())->PollCompletedSerial();
}

ResultOrError<ExecutionSerial> Queue::WaitForQueueSerialImpl(ExecutionSerial waitSerial,
                                                             Nanoseconds timeout) {
    ExecutionSerial completed = ToBackend(GetDevice())->WaitForSerial(waitSerial, timeout);
    return completed >= waitSerial ? completed : kWaitSerialTimeout;
}

void Queue::ForceEventualFlushOfCommands() {}

MaybeError Queue::WaitForIdleForDestructionImpl() {
    Device* device = ToBackend(GetDevice());
    if (device->HasPendingCommands()) {
        DAWN_TRY(SubmitPendingCommandsImpl());
    }
    device->WaitIdle();
    return {};
}

}  // namespace dawn::native::deko3d
