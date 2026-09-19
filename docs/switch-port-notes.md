# Dawn (WebGPU) → Nintendo Switch Port Notes

Working notes for getting Dawn/Tint building for aarch64-none-elf (devkitA64/
libnx), targeting NVK (Mesa's Vulkan driver, ported to the Switch's Tegra X1
GPU via the separate `nxvk` fork at `/home/proot-dev/switch/nxvk`) as the
Vulkan backend. This is the graphics side of the WiiCompiled Switch port
(`/home/proot-dev/switch/wiicompiled` - see that repo's own
`docs/switch-port-notes.md` for the runtime/emulator side); Aurora (the GX→
modern-API renderer WiiCompiled links against) targets Dawn's WebGPU C API,
which is why this needs to exist at all rather than going straight to Vulkan.

## Prior state found at session start

- **NVK is already built and installed**: `/opt/devkitpro/portlibs/switch/lib/
  libnvk.a` + `libnvk_support.a`, plus full Vulkan headers
  (`/opt/devkitpro/portlibs/switch/include/vulkan/`). Built via `nxvk`'s own
  Docker/podman-based cross toolchain (not reproducible in this session -
  Termux/proot has no container runtime - but the *output* is already
  installed and usable as an ordinary devkitPro portlib).
- **A real, substantial Dawn-for-Switch CMake build already exists and was
  mid-flight**: `/home/proot-dev/switch/dawn/build-switch`, configured via
  `/home/proot-dev/switch/dawn/build-switch-toolchain.cmake`
  (`-DDAWN_ENABLE_BACKEND_VULKAN`, devkitA64 cross-compiler,
  `-D__SWITCH__`). 709 objects / 91 archives already built as of this
  session's start (`.ninja_log` timestamp: several hours earlier - the ninja
  process itself was not still running, just stalled on the next compile
  error).
- **`src/utils/platform.h` already has a deliberate, well-reasoned
  `DAWN_PLATFORM_IS_HORIZON` platform category**, distinct from POSIX on
  purpose: "newlib/libnx provides pthreads and a POSIX-ish libc subset, but
  no dlopen and no real mmap... letting `DAWN_PLATFORM_IS(POSIX)` code paths
  run here would pull those in silently."
- **`DynamicLib.cpp` already has complete Horizon stubs for all 5 methods**,
  with a comment explicitly describing the intended fix for Vulkan
  specifically: "The Vulkan backend's Switch path bypasses this file
  entirely (static-linked against libnvk.a, driven through
  `vk_icdGetInstanceProcAddr` directly)" - i.e. someone had already fully
  designed the fix, just hadn't written the `BackendVk.cpp` patch yet.

None of this was done in this session - it's all prior work (likely the
earlier, pre-context-compaction portion of this same overall effort).
Tonight's work picks up from the first actual compile error and continues
resuming the ninja build, fixing one platform gap at a time, matching the
same iterative pattern used all session for the WiiCompiled runtime itself.

## Fixes this session

Build resumed at `-j3` (see the WiiCompiled notes for why - Termux/proot on a
phone, higher parallelism overloads the device).

### `dawn/common/SystemHandle.cpp` - no Horizon branch

`SystemHandle` wraps a generic OS handle (used by `SystemEvent`/GPU fence
waiting, `SharedFence*`/cross-API interop, Wire's shared memory manager).
The header (`SystemHandle.h`) already had `Handle = uint32_t` for
`DAWN_PLATFORM_IS(HORIZON)`, grouped with Fuchsia ("libnx kernel object
Handle is a u32, same shape as Fuchsia's `zx_handle_t`") - only the `.cpp`
was missing the matching implementation branch.

Added `kInvalidHandle = INVALID_HANDLE`, `IsHandleValid()`, and
`CloseHandle()` via `svcCloseHandle` - real, working implementations, since
those matter even for ordinary single-process use. `DuplicateHandle()` fails
fast (`DAWN_UNREACHABLE()`) instead: Horizon has no user-mode SVC to
duplicate an arbitrary handle the way Fuchsia's `zx_handle_duplicate` does
(no `svcDuplicateHandle` in `libnx/kernel/svc.h`), and `Duplicate()` is only
ever reached by cross-API/cross-process resource sharing, none of which
applies to this single-backend (Vulkan/NVK), in-process game.

### `dawn/utils/SystemUtils.cpp` - `USleep` unimplemented

Trivial - `svcSleepThread(usecs * 1000)` (libnx takes nanoseconds).

### `dawn/native/vulkan/ExternalHandle.h` - `ExternalSemaphoreHandle` unimplemented

`ExternalMemoryHandle` right below it already had a generic `void*` fallback
for any unlisted platform ("so the rest of the Vulkan backend compiles") -
`ExternalSemaphoreHandle` didn't, and hard-`#error`'d instead. Both back
`VK_KHR_external_{memory,semaphore}_*` extensions for cross-process/cross-API
GPU resource sharing, which this game never uses. Added the same kind of
generic fallback (`void*`, matching `ExternalMemoryHandle`'s existing
pattern) rather than plumbing through a real Horizon-specific handle type
that nothing will ever construct.

### `dawn/native/vulkan/BackendVk.cpp` + `VulkanFunctions.{h,cpp}` - the actual "no Vulkan Loader on Switch" fix

This is the one `DynamicLib.cpp`'s comment predicted. Every other platform's
`VulkanInstance::Initialize()` calls `DynamicLib::Open()` on a Vulkan Loader
shared library (`libvulkan.so.1`, `vulkan-1.dll`, etc.) then resolves
`vkGetInstanceProcAddr` via `dlsym`/`GetProcAddress`. Switch has neither: no
dlopen, and no separate loader/ICD file to open in the first place - NVK is
linked directly as `libnvk.a`.

Fix, following the pattern the `DynamicLib.cpp` comment described:

- Declared `vk_icdGetInstanceProcAddr` as an `extern "C"` function in
  `BackendVk.cpp` under `DAWN_PLATFORM_IS(HORIZON)` - this is the standard
  Vulkan Loader↔ICD interface entry point every real ICD (including Mesa's
  NVK) exports, normally called by the *loader* after it `dlopen`s the
  driver; here it's just an ordinary linked symbol from `libnvk.a` instead.
- Added a new `VulkanFunctions::LoadGlobalProcs(PFN_vkGetInstanceProcAddr)`
  overload (`VulkanFunctions.h`/`.cpp`) that does the same global-proc
  loading the existing `LoadGlobalProcs(const DynamicLib&)` does, minus the
  `DynamicLib::GetProc` call - the two overloads now share one body, with
  the `DynamicLib` version calling the function-pointer version internally.
- In `VulkanInstance::Initialize()`: `ICD::None` skips
  `LoadVulkan(kVulkanLibName)` entirely on Horizon (nothing to open), and
  the final `mFunctions.LoadGlobalProcs(mVulkanLib)` call becomes
  `mFunctions.LoadGlobalProcs(vk_icdGetInstanceProcAddr)` on Horizon instead.
- `kVulkanLibName` (which previously hard-`#error`'d on any unlisted
  platform) gets an empty-string Horizon placeholder, since it's genuinely
  unused there now but the constant is still unconditionally declared.

Everything past this point in `VulkanInstance::Initialize()`
(`GatherGlobalInfo`, extension/layer enumeration, `vkCreateInstance`, device
enumeration) is unmodified and platform-generic - once `vkGetInstanceProcAddr`
is wired up, the rest of Dawn's Vulkan backend doesn't care how it got there.

## Status as of this note

Both fixes verified with a targeted `ninja <object>.o` build before
resuming the full build each time (same discipline as the WiiCompiled side:
never resume a multi-hundred-target build blind). Full `-j3` build resumed
and running past `dawn_native_objects`/`BackendVk.o`/`VulkanFunctions.o` as
of this note - next failure (if any) not yet seen.

## Full build succeeded; smoke test built to exercise it on hardware

`ninja dawn_native` then `ninja webgpu_dawn` both completed clean (only
harmless GCC ABI notes about C++17-vs-C++14 parameter passing for
`tint::core::Number<T>`) - `libdawn_native.a` and `libwebgpu_dawn.a` both
exist. This is the first time this session Dawn-for-Switch built completely
end to end.

Added `switch_smoke_test/` (guarded `add_subdirectory` under
`CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch"` in the top-level
`CMakeLists.txt`, matching the RenderDoc-exclusion guard style already used
elsewhere): a standalone executable that creates a `WGPUInstance`, requests
a Vulkan adapter/device, and - separately, since it turned out to be more
diagnostic - calls `vk_icdGetInstanceProcAddr` directly to drive raw
`vkCreateInstance`/`vkEnumeratePhysicalDevices` without Dawn in the loop at
all. Links `webgpu_dawn` plus `-lnvk -lnvk_support -lz` directly (NVK isn't
wired into Dawn's own CMake at all yet - this smoke test adds it standalone
for now).

### NVK link-time gaps (`nvk_switch_stubs.cpp`)

`libnvk.a`/`libnvk_support.a` (Mesa `util` code) reference POSIX APIs
devkitA64's newlib doesn't implement, plus expat (no `libexpat` portlib
built for Switch). Real implementations only where behavior actually
matters; everything else in `nvk_switch_stubs.cpp` is a safe "not
available" stub:

- `posix_memalign` - real, via newlib's `memalign()`.
- `sysconf(_SC_PAGESIZE)` / `sysconf(_SC_PHYS_PAGES)` - **real**, via
  `svcGetInfo(InfoType_TotalMemorySize, CUR_PROCESS_HANDLE)`. Originally
  stubbed to always return -1 like the WiiCompiled-side CryptoPP/imgui
  `sysconf` stub; that was wrong here; `nvk_physical_device.c:1593`'s
  `os_get_total_physical_memory()` hard-fails
  `VK_ERROR_INITIALIZATION_FAILED` on a negative result instead of falling
  back, unlike CryptoPP's callers.
- `geteuid`/`getuid`/`getegid`/`getgid`, `getpwuid_r`, `flock`, `dirfd`,
  `fstatat`, `regcomp`/`regexec`/`regfree`, `pthread_sigmask` - safe no-op/
  failure stubs; all live in Mesa's optional disk-cache/driconf machinery.
- **expat (`XML_ParserCreate` etc.)** - first attempt returned `nullptr`
  from `XML_ParserCreate`, which crashed: `nvk_CreateInstance` unconditionally
  calls `nvk_parse_dri_options` → `driParseConfigFiles` →
  `parseOneConfigFile` (Mesa's `xmlconfig.c`), which does **not** null-check
  the parser handle. Fixed by returning a real (if content-empty)
  `malloc(1)` handle and reporting success from every other call instead -
  none of our stub bodies dereference the handle, so this is safe.
- **`NVK_I_WANT_A_BROKEN_VULKAN_DRIVER` environment variable** - NVK is
  marked experimental/unsupported on Switch and silently reports zero
  physical devices (`vkEnumeratePhysicalDevices` succeeds with `count=0`)
  unless the app explicitly opts in via `setenv()` before any Vulkan call.
  Documented in `nxvk/switch/README.md`'s own example `main()` - found by
  reading that file, not guessed. `u32 __nx_applet_type =
  AppletType_Application;` / `size_t __nx_heap_size = 0;` globals are the
  other two things that README example does that we were missing.

### `nxlink` workflow (much faster than the FTP round-trip)

Discovered partway through this debugging chain: `nxlink -a <ip> -s
<nro>` pushes an NRO over the network, launches it, and - if the app calls
`nxlinkStdio()` early - streams its stdout/stderr live back into the
`nxlink` process itself. This replaced the WiiCompiled-style "upload NRO,
wait for the human to launch it, poll FTP for a written log file" loop with
direct, live console output, for this smoke test at least.

Caveat: the push step only works while **hbmenu itself is the foreground
app** on the console (that's when its netloader listens for incoming
pushes) - tested and confirmed sphaira (the FTP-serving homebrew app used
for the WiiCompiled side of this session) does *not* answer the same
protocol. Needs the human to switch back to hbmenu before each nxlink push.

### Current blocker: `wsi_device_init` null function pointer

With the above three fixes, `vkCreateInstance` succeeds cleanly
(`VkResult=0`) and NVK's own init banner/shader-cache-open messages print
correctly. The very next call - `vkEnumeratePhysicalDevices` - crashes
(Instruction Abort, PC=0) inside `wsi_device_init`
(`src/vulkan/wsi/wsi_common.c:112`, `nxvk` checkout), which NVK calls
unconditionally per physical device via `nvk_init_wsi`
(`nvk_physical_device.c:1697`) regardless of which instance extensions were
requested.

Root cause, traced as far as possible without rebuilding: `wsi_common.c`
resolves `GetPhysicalDeviceProperties2` via `nvk_wsi_proc_addr` →
`vk_instance_get_proc_addr_unchecked` (`vulkan/runtime/vk_instance.c:369`),
then calls it with **no null check** at line 112. That resolver checks the
instance's own dispatch table first, then falls back to a static, compile-
time-generated `vk_physical_device_trampolines` table - the latter isn't
runtime/instance-state-dependent, so a miss there points at a build/codegen
gap in this specific nxvk fork's compiled `libnvk.a`, not at anything our
calling code does. Ruled out by direct test: explicitly requesting
`VK_KHR_get_physical_device_properties2`/`VK_KHR_surface` as instance
extensions made no difference (expected, since "unchecked" resolution by
definition skips the enabled-extensions check per Mesa's own docs).

Can't rebuild `libnvk.a` to test a real fix or confirm the exact codegen
gap: `nxvk`'s build requires Docker/podman (see its README), neither of
which exists in this Termux/proot environment. Checked `gh api
repos/PalindromicBreadLoaf/nxvk/issues` - empty (no existing issue to
cross-reference).

### Resolution: it wasn't an nxvk bug at all - missing `--whole-archive`

User pushback ("it wouldn't be a lib at all if it doesn't init... there's
an easier way") was correct. Checked `nxvk`'s own reference linker
(`switch/build/build-nro.sh`) and its installed `nxvk.pc`: **`libnvk.a`
must be linked with `-Wl,--whole-archive ... -Wl,--no-whole-archive`**.
Without it, the linker only pulls object files it can see are directly
needed by name - and nothing calls into Mesa's generated dispatch/
trampoline tables by name at link time, only via runtime string lookup
(`vk_instance_get_proc_addr_unchecked` → `vk_physical_device_trampolines`),
so whole sections of the archive were silently dropped. Exact same failure
class as WiiCompiled's `libtranslated.a`/`libruntime.a` archive-extraction
bug from earlier in this project (self-registering globals invisible to
archive resolution) - should have recognized the pattern immediately.
`-lnvk_support` does NOT need `--whole-archive` (ordinary selective
linking is correct there, same as WiiCompiled's `libswitchext.a`).

Fixed in `switch_smoke_test/CMakeLists.txt`:
`-Wl,-u,vk_icdGetInstanceProcAddr -Wl,--whole-archive -lnvk
-Wl,--no-whole-archive -lnvk_support -lz`. Immediately after this fix,
`vkEnumeratePhysicalDevices` returned `count=1`, device "NVIDIA Tegra X1
(NVK gm20b), apiVersion=1.3.354" - the WSI crash was gone entirely.

While there, adopted two more corrections from nxvk's own proven-working
`switch/smoke/nvk_compat.c` (found via reading the actual reference
implementation rather than guessing): `getrandom()` (NAK's Rust code backs
its RNG with it - added proactively before `--whole-archive` could newly
expose it as undefined) via `randomGet()`, and `regcomp`/`regexec` set to
"compiles fine, matches nothing" (`return 0` / `REG_NOMATCH`) rather than
my own guess of "always fails to compile" (`return 1`).

### Then: Dawn's own adapter request still failed - `vkEnumerateInstanceLayerProperties`

With NVK itself fully working (confirmed via raw `vkCreateInstance` +
`vkEnumeratePhysicalDevices` in the smoke test), Dawn's own
`wgpuInstanceRequestAdapter` still reported "No supported adapters". Ruled
out interference from the raw probe's own instance create/destroy cycle
(disabled it - Dawn failed identically on a clean first call). Traced to
`VulkanFunctions.cpp`'s `GET_GLOBAL_PROC` macro: it hard-fails
(`DAWN_INTERNAL_ERROR`, aborting the entire Vulkan backend) if any global
proc resolves to null, *except* `vkEnumerateInstanceVersion` which is
explicitly allowed to be null a few lines below (Vulkan 1.0 doesn't have
it). NVK's global (instance=NULL) `vk_icdGetInstanceProcAddr` doesn't
resolve `vkEnumerateInstanceLayerProperties` - a real driver quirk, but a
harmless one, since Switch has zero Vulkan layers installed anywhere
regardless.

Fixed the same way Dawn already handles the version proc: load
`EnumerateInstanceLayerProperties` without the fatal null-check in
`LoadGlobalProcs`, and guard its one call site in `GatherGlobalInfo`
(`VulkanInfo.cpp`) with a null check - skipping it leaves `info.layers`
at its default-empty state, which is the factually correct answer here.

### Result: full instance → adapter → device chain confirmed on real hardware

```
OK: got adapter
  device: NVIDIA Tegra X1 (NVK gm20b)
  description: NVK: Mesa 26.2.2 (git-238e06f921) 26.0.128.2
OK: got a WGPUDevice - NVK/Vulkan backend is alive end to end.
=== Dawn-for-Switch smoke test finished: SUCCESS ===
```

Dawn-for-Switch is confirmed alive end-to-end on the actual Tegra X1 GPU
via NVK.

### Bonus: actual pixels on screen (raw Vulkan WSI, separate from Dawn)

Dawn's public `webgpu.h` has no Horizon/VI surface source type upstream
(nobody's added one), so getting real presentation working meant going
around Dawn, straight to the same raw-Vulkan approach as the adapter/
device probe. Adapted (not copied - own implementation, informed by
reading it) nxvk's own `switch/smoke/nvk_vi_swapchain.c` reference example:
`VK_KHR_surface` + `VK_NN_vi_surface` + `VK_KHR_swapchain`, `vkCreateViSurfaceNN`
over `nwindowGetDefault()`, triple-buffered swapchain, a 3-in-flight
present loop clearing to a sinusoidally-cycling color for 180 frames
(~3s at 60 Hz). Needed `-DVK_USE_PLATFORM_VI_NN` added to the smoke test's
compile definitions (matches nxvk.pc's own required Cflags) for the VI
surface types to even be declared.

**Ran clean on real hardware, first try after the fixes above**:
`vkCreateViSurfaceNN -> 0`, `vkCreateSwapchainKHR -> 0`, swapchain images=3,
`MESA: info: nvk wsi: zero-copy ENABLED`, presented 180/180 frames. Visually
confirmed on the console: cycling colors on screen for ~3 seconds.

This is the actual end-to-end proof the whole Dawn/NVK track was after:
real Vulkan draw commands landing on the real screen via NVK, zero-copy,
on real Tegra X1 hardware.

### Next steps for this track

1. Wire the same `--whole-archive` treatment into WiiCompiled's own
   eventual Aurora+Dawn link recipe (not just this standalone smoke test) -
   this exact bug would silently reappear there too.
2. Start replacing `switch_stubs_gx.cpp`'s no-ops with real calls into
   `aurora-main`'s GX backend now that the renderer underneath it is
   proven to work end-to-end, including presentation.
3. Dawn itself still has no public Horizon/VI surface type - if/when
   Aurora needs Dawn (not raw Vulkan) to present, that gap needs
   addressing upstream-style in Dawn's own surface backend code, following
   the pattern of the `DAWN_PLATFORM_IS_HORIZON` work already done
   elsewhere in this fork.
