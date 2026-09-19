// Copyright 2026 The Dawn & Tint Authors
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

#include "src/dawn/common/SystemHandle.h"

#include <utility>

#include "src/utils/assert.h"
#include "src/utils/log.h"

#if DAWN_PLATFORM_IS(WINDOWS)
#include "src/utils/windows_with_undefs.h"
#elif DAWN_PLATFORM_IS(FUCHSIA)
#include <zircon/syscalls.h>
#elif DAWN_PLATFORM_IS(HORIZON)
#include <switch.h>
#elif DAWN_PLATFORM_IS(POSIX)
#include <unistd.h>
#endif

namespace dawn {

namespace {

#if DAWN_PLATFORM_IS(WINDOWS)

constexpr inline HANDLE kInvalidHandle = nullptr;

inline bool IsHandleValid(HANDLE handle) {
    return handle != nullptr;
}

inline HANDLE DuplicateHandle(HANDLE handle) {
    HANDLE currentProcess = ::GetCurrentProcess();
    HANDLE outHandle;
    bool success = ::DuplicateHandle(currentProcess, handle, currentProcess, &outHandle, 0, FALSE,
                                     DUPLICATE_SAME_ACCESS);
    DAWN_CHECK(success);
    return outHandle;
}

inline void CloseHandle(HANDLE handle) {
    bool success = ::CloseHandle(handle);
    DAWN_CHECK(success);
}

#elif DAWN_PLATFORM_IS(FUCHSIA)

constexpr inline zx_handle_t kInvalidHandle = 0;

inline bool IsHandleValid(zx_handle_t handle) {
    return handle > 0;
}

inline zx_handle_t DuplicateHandle(zx_handle_t handle) {
    zx_handle_t outHandle = ZX_HANDLE_INVALID;
    auto status = zx_handle_duplicate(handle, ZX_RIGHT_SAME_RIGHTS, &outHandle);
    DAWN_CHECK(status == ZX_OK);
    return outHandle;
}

inline void CloseHandle(zx_handle_t handle) {
    auto status = zx_handle_close(handle);
    DAWN_CHECK(status == ZX_OK);
}

#elif DAWN_PLATFORM_IS(HORIZON)

// libnx's Handle is a u32 kernel object handle, same shape as Fuchsia's zx_handle_t (see
// SystemHandle.h), but Horizon has no user-mode SVC to duplicate an arbitrary handle the way
// zx_handle_duplicate does - specific object types have their own "another reference" mechanisms
// if needed, there's no blanket one. SystemHandle::Duplicate() is only reached by cross-API/
// cross-process resource sharing (SharedFence, SharedTextureMemory, Dawn Wire's out-of-process
// shared memory) - none of which apply to this single-backend (Vulkan/NVK), in-process game, so
// this fails fast instead of silently returning a wrong/invalid handle if that ever changes.

constexpr inline Handle kInvalidHandle = INVALID_HANDLE;

inline bool IsHandleValid(::Handle handle) {
    return handle != INVALID_HANDLE;
}

inline ::Handle DuplicateHandle(::Handle handle) {
    (void)handle;
    dawn::ErrorLog() << "SystemHandle::Duplicate() is not supported on Horizon (Switch).";
    DAWN_UNREACHABLE();
    return kInvalidHandle;
}

inline void CloseHandle(::Handle handle) {
    Result rc = svcCloseHandle(handle);
    DAWN_CHECK(R_SUCCEEDED(rc));
}

#elif DAWN_PLATFORM_IS(POSIX)

constexpr inline int kInvalidHandle = -1;

inline bool IsHandleValid(int handle) {
    return handle >= 0;
}

inline int DuplicateHandle(int handle) {
    int outHandle = dup(handle);
    DAWN_CHECK(outHandle >= 0);
    return outHandle;
}

inline void CloseHandle(int handle) {
    int status = close(handle);
    DAWN_CHECK(status >= 0);
}

#endif

}  // anonymous namespace

SystemHandle::SystemHandle() : mHandle(kInvalidHandle) {}
SystemHandle::SystemHandle(Handle handle) : mHandle(handle) {}

SystemHandle::SystemHandle(ErrorTag tag) : SystemHandle() {
    dawn::ErrorLog() << "SystemHandle constructed from incorrect handle type.";
    DAWN_UNREACHABLE();
}

bool SystemHandle::IsValid() const {
    return IsHandleValid(mHandle);
}

SystemHandle::SystemHandle(SystemHandle&& rhs) {
    mHandle = rhs.mHandle;
    rhs.mHandle = kInvalidHandle;
}

SystemHandle& SystemHandle::operator=(SystemHandle&& rhs) {
    if (this != &rhs) {
        if (IsValid()) {
            Close();
        }
        mHandle = rhs.mHandle;
        rhs.mHandle = kInvalidHandle;
    }
    return *this;
}

SystemHandle::Handle SystemHandle::Get() const {
    return mHandle;
}

SystemHandle::Handle* SystemHandle::GetMut() {
    if (IsValid()) {
        Close();
    }
    return &mHandle;
}

SystemHandle::Handle SystemHandle::Detach() {
    Handle handle = mHandle;
    mHandle = kInvalidHandle;
    return handle;
}

SystemHandle SystemHandle::Duplicate() const {
    DAWN_CHECK(IsValid());

    Handle handle = DuplicateHandle(mHandle);
    return SystemHandle(handle);
}

void SystemHandle::Close() {
    DAWN_ASSERT(IsValid());
    CloseHandle(mHandle);
    // Invalidate the handle after closing.
    mHandle = kInvalidHandle;
}

SystemHandle::~SystemHandle() {
    if (IsValid()) {
        Close();
    }
}

}  // namespace dawn
