// Minimal Dawn-for-Switch smoke test.
//
// Creates a WGPUInstance, requests an adapter forcing the Vulkan backend
// (so it must go through NVK's statically-linked ICD entry point via
// vk_icdGetInstanceProcAddr - see BackendVk.cpp), and if that succeeds,
// requests a device from it. Every step is logged to sdmc: since stdout/
// stderr capture is not reliable on Switch (see switch-port-notes.md).
//
// This exercises the exact path WiiCompiled's Aurora integration will need
// later, without pulling in any WiiCompiled code.

#include <switch.h>

#include <vulkan/vulkan.h>
#include <webgpu/webgpu.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

namespace {

// Writes to both the sdmc: log file (durable, FTP-fetchable fallback) and
// stdout (live-streamed to whoever ran `nxlink -s`, once nxlinkStdio() has
// redirected it).
void DualLog(FILE* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    if (file) {
        vfprintf(file, fmt, args);
        fflush(file);
    }
    vfprintf(stdout, fmt, args2);
    fflush(stdout);
    va_end(args2);
    va_end(args);
}

FILE* g_vkDebugLog = nullptr;

VKAPI_ATTR VkBool32 VKAPI_CALL RawVkDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                   void* /*userdata*/) {
    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? "ERR"
                        : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                        : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? "INFO"
                                                                                       : "DBG";
    DualLog(g_vkDebugLog, "[vk/%s] %s: %s\n", level, data->pMessageIdName ? data->pMessageIdName : "?",
            data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

void RawVulkanProbe(FILE* log) {
    g_vkDebugLog = log;
    auto getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr(nullptr, "vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr) {
        DualLog(log, "raw vk probe: vk_icdGetInstanceProcAddr(null, \"vkGetInstanceProcAddr\") returned null\n");
        return;
    }

    auto vkCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vkCreateInstance) {
        DualLog(log, "raw vk probe: could not resolve vkCreateInstance\n");
        return;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dawn_switch_smoke_test";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT debugCi{};
    debugCi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCi.pfnUserCallback = RawVkDebugCallback;

    const char* extensions[] = {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pNext = &debugCi; // catches messages raised during instance creation itself
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 3;
    createInfo.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    DualLog(log, "raw vk probe: vkCreateInstance -> VkResult=%d\n", static_cast<int>(result));
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        return;
    }

    auto vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        getInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (vkCreateDebugUtilsMessengerEXT) {
        vkCreateDebugUtilsMessengerEXT(instance, &debugCi, nullptr, &messenger);
    }

    auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    if (!vkEnumeratePhysicalDevices) {
        DualLog(log, "raw vk probe: could not resolve vkEnumeratePhysicalDevices\n");
        return;
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    DualLog(log, "raw vk probe: vkEnumeratePhysicalDevices(count query) -> VkResult=%d, count=%u\n",
            static_cast<int>(result), deviceCount);

    if (deviceCount > 0) {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        DualLog(log, "raw vk probe: vkEnumeratePhysicalDevices(fetch) -> VkResult=%d, count=%u\n",
                static_cast<int>(result), deviceCount);

        auto vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
        for (uint32_t i = 0; i < deviceCount && vkGetPhysicalDeviceProperties; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devices[i], &props);
            DualLog(log, "raw vk probe: device[%u] = %s (apiVersion=%u.%u.%u, driverVersion=%u)\n", i,
                    props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
        }
    }

    auto vkDestroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(instance, "vkDestroyInstance"));
    if (vkDestroyInstance) {
        vkDestroyInstance(instance, nullptr);
    }
}

// Actually puts pixels on screen: brings up its own instance/device (VK_KHR_surface +
// VK_NN_vi_surface + VK_KHR_swapchain), creates a swapchain over libnx's default NWindow, and
// cycles the clear color for a few seconds. Independent of RawVulkanProbe/Dawn above - a plain,
// separate raw-Vulkan path, since Dawn's public API has no Horizon/VI surface source type yet.
void PresentColorCycleDemo(FILE* log) {
    auto getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr(nullptr, "vkGetInstanceProcAddr"));
    auto vkCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vkCreateInstance) {
        DualLog(log, "present demo: no vkCreateInstance\n");
        return;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dawn_switch_smoke_test";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    const char* instExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instCi{};
    instCi.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCi.pApplicationInfo = &appInfo;
    instCi.enabledExtensionCount = 2;
    instCi.ppEnabledExtensionNames = instExts;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instCi, nullptr, &instance);
    DualLog(log, "present demo: vkCreateInstance -> %d\n", static_cast<int>(result));
    if (result != VK_SUCCESS) {
        return;
    }

#define RESOLVE(name) \
    auto name = reinterpret_cast<PFN_vk##name>(getInstanceProcAddr(instance, "vk" #name))
    RESOLVE(EnumeratePhysicalDevices);
    RESOLVE(GetPhysicalDeviceQueueFamilyProperties);
    RESOLVE(CreateDevice);
    RESOLVE(GetDeviceProcAddr);
    RESOLVE(DestroyInstance);
    RESOLVE(CreateViSurfaceNN);
    RESOLVE(DestroySurfaceKHR);
    RESOLVE(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    RESOLVE(GetPhysicalDeviceSurfaceFormatsKHR);
#undef RESOLVE

    uint32_t deviceCount = 1;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    EnumeratePhysicalDevices(instance, &deviceCount, &phys);
    if (phys == VK_NULL_HANDLE) {
        DualLog(log, "present demo: no physical device\n");
        DestroyInstance(instance, nullptr);
        return;
    }

    uint32_t qfCount = 0;
    GetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    GetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfProps.data());
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qfi = i;
            break;
        }
    }
    if (qfi == UINT32_MAX) {
        DualLog(log, "present demo: no graphics queue family\n");
        DestroyInstance(instance, nullptr);
        return;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCi{};
    queueCi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCi.queueFamilyIndex = qfi;
    queueCi.queueCount = 1;
    queueCi.pQueuePriorities = &queuePriority;

    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo devCi{};
    devCi.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCi.queueCreateInfoCount = 1;
    devCi.pQueueCreateInfos = &queueCi;
    devCi.enabledExtensionCount = 1;
    devCi.ppEnabledExtensionNames = devExts;

    VkDevice device = VK_NULL_HANDLE;
    result = CreateDevice(phys, &devCi, nullptr, &device);
    DualLog(log, "present demo: vkCreateDevice -> %d\n", static_cast<int>(result));
    if (result != VK_SUCCESS) {
        DestroyInstance(instance, nullptr);
        return;
    }

#define RESOLVED(name) auto name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name))
    RESOLVED(GetDeviceQueue);
    RESOLVED(CreateSwapchainKHR);
    RESOLVED(DestroySwapchainKHR);
    RESOLVED(GetSwapchainImagesKHR);
    RESOLVED(AcquireNextImageKHR);
    RESOLVED(QueuePresentKHR);
    RESOLVED(CreateCommandPool);
    RESOLVED(DestroyCommandPool);
    RESOLVED(AllocateCommandBuffers);
    RESOLVED(BeginCommandBuffer);
    RESOLVED(EndCommandBuffer);
    RESOLVED(ResetCommandBuffer);
    RESOLVED(CmdClearColorImage);
    RESOLVED(CmdPipelineBarrier);
    RESOLVED(CreateSemaphore);
    RESOLVED(DestroySemaphore);
    RESOLVED(CreateFence);
    RESOLVED(DestroyFence);
    RESOLVED(WaitForFences);
    RESOLVED(ResetFences);
    RESOLVED(QueueSubmit);
    RESOLVED(QueueWaitIdle);
    RESOLVED(DestroyDevice);
#undef RESOLVED

    if (!CreateViSurfaceNN) {
        DualLog(log, "present demo: no vkCreateViSurfaceNN\n");
        DestroyDevice(device, nullptr);
        DestroyInstance(instance, nullptr);
        return;
    }

    VkQueue queue = VK_NULL_HANDLE;
    GetDeviceQueue(device, qfi, 0, &queue);

    VkViSurfaceCreateInfoNN surfaceCi{};
    surfaceCi.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    surfaceCi.window = nwindowGetDefault();

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    result = CreateViSurfaceNN(instance, &surfaceCi, nullptr, &surface);
    DualLog(log, "present demo: vkCreateViSurfaceNN -> %d\n", static_cast<int>(result));
    if (result != VK_SUCCESS) {
        DestroyDevice(device, nullptr);
        DestroyInstance(instance, nullptr);
        return;
    }

    VkSurfaceCapabilitiesKHR caps{};
    GetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = 1280;
        extent.height = 720;
    }

    uint32_t formatCount = 0;
    GetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    GetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &formatCount, formats.data());
    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosenFormat = f;
            break;
        }
    }
    DualLog(log, "present demo: surface %ux%u format=%d\n", extent.width, extent.height,
            static_cast<int>(chosenFormat.format));

    VkSwapchainCreateInfoKHR swapchainCi{};
    swapchainCi.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCi.surface = surface;
    swapchainCi.minImageCount = caps.minImageCount;
    swapchainCi.imageFormat = chosenFormat.format;
    swapchainCi.imageColorSpace = chosenFormat.colorSpace;
    swapchainCi.imageExtent = extent;
    swapchainCi.imageArrayLayers = 1;
    swapchainCi.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCi.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCi.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCi.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainCi.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainCi.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = CreateSwapchainKHR(device, &swapchainCi, nullptr, &swapchain);
    DualLog(log, "present demo: vkCreateSwapchainKHR -> %d\n", static_cast<int>(result));
    if (result != VK_SUCCESS) {
        DestroySurfaceKHR(instance, surface, nullptr);
        DestroyDevice(device, nullptr);
        DestroyInstance(instance, nullptr);
        return;
    }

    uint32_t imageCount = 0;
    GetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    GetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    DualLog(log, "present demo: swapchain images=%u\n", imageCount);

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = qfi;
    VkCommandPool pool = VK_NULL_HANDLE;
    CreateCommandPool(device, &poolCi, nullptr, &pool);

    constexpr int kInFlight = 3;
    VkSemaphore acquireSem[kInFlight]{};
    VkSemaphore presentSem[kInFlight]{};
    VkFence inflightFence[kInFlight]{};
    VkCommandBuffer cmd[kInFlight]{};
    for (int i = 0; i < kInFlight; ++i) {
        VkSemaphoreCreateInfo semCi{};
        semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        CreateSemaphore(device, &semCi, nullptr, &acquireSem[i]);
        CreateSemaphore(device, &semCi, nullptr, &presentSem[i]);
        VkFenceCreateInfo fenceCi{};
        fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CreateFence(device, &fenceCi, nullptr, &inflightFence[i]);
        VkCommandBufferAllocateInfo cmdAi{};
        cmdAi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAi.commandPool = pool;
        cmdAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAi.commandBufferCount = 1;
        AllocateCommandBuffers(device, &cmdAi, &cmd[i]);
    }

    constexpr int kFrames = 180; // ~3s at 60fps
    int presented = 0;
    for (int frame = 0; frame < kFrames && appletMainLoop(); ++frame) {
        const int slot = frame % kInFlight;
        WaitForFences(device, 1, &inflightFence[slot], VK_TRUE, UINT64_MAX);

        uint32_t idx = 0;
        result = AcquireNextImageKHR(device, swapchain, UINT64_MAX, acquireSem[slot], VK_NULL_HANDLE, &idx);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            if (result == VK_ERROR_OUT_OF_DATE_KHR) break;
            continue;
        }
        ResetFences(device, 1, &inflightFence[slot]);
        ResetCommandBuffer(cmd[slot], 0);

        const float t = static_cast<float>(frame) / 60.0f;
        VkClearColorValue clearColor{};
        clearColor.float32[0] = 0.5f + 0.5f * sinf(t);
        clearColor.float32[1] = 0.5f + 0.5f * sinf(t + 2.094f);
        clearColor.float32[2] = 0.5f + 0.5f * sinf(t + 4.188f);
        clearColor.float32[3] = 1.0f;

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        BeginCommandBuffer(cmd[slot], &beginInfo);

        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = images[idx];
        toDst.subresourceRange = range;
        CmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &toDst);

        CmdClearColorImage(cmd[slot], images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = images[idx];
        toPresent.subresourceRange = range;
        CmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &toPresent);
        EndCommandBuffer(cmd[slot]);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquireSem[slot];
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd[slot];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &presentSem[slot];
        if (QueueSubmit(queue, 1, &submit, inflightFence[slot]) != VK_SUCCESS) {
            break;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &presentSem[slot];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &idx;
        result = QueuePresentKHR(queue, &presentInfo);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            ++presented;
        } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            break;
        }
    }
    DualLog(log, "present demo: presented %d/%d frames\n", presented, kFrames);

    QueueWaitIdle(queue);
    for (int i = 0; i < kInFlight; ++i) {
        DestroySemaphore(device, acquireSem[i], nullptr);
        DestroySemaphore(device, presentSem[i], nullptr);
        DestroyFence(device, inflightFence[i], nullptr);
    }
    DestroyCommandPool(device, pool, nullptr);
    DestroySwapchainKHR(device, swapchain, nullptr);
    DestroySurfaceKHR(instance, surface, nullptr);
    DestroyDevice(device, nullptr);
    DestroyInstance(instance, nullptr);
}
} // namespace

namespace {

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    if (g_log) {
        vfprintf(g_log, fmt, args);
        fflush(g_log);
    }
    vfprintf(stdout, fmt, args2);
    fflush(stdout);
    va_end(args2);
    va_end(args);
}

void PrintStringView(const char* label, WGPUStringView sv) {
    if (sv.data && sv.length > 0) {
        Log("  %s: %.*s\n", label, static_cast<int>(sv.length), sv.data);
    } else {
        Log("  %s: (empty)\n", label);
    }
}

struct AdapterResult {
    WGPUAdapter adapter = nullptr;
    bool done = false;
    bool success = false;
};

struct DeviceResult {
    WGPUDevice device = nullptr;
    bool done = false;
    bool success = false;
};

void OnAdapterRequestEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                            void* userdata1, void* /*userdata2*/) {
    auto* result = static_cast<AdapterResult*>(userdata1);
    result->done = true;
    result->success = (status == WGPURequestAdapterStatus_Success);
    Log("aurora_smoke: adapter request status=%d\n", static_cast<int>(status));
    PrintStringView("message", message);
    if (result->success) {
        result->adapter = adapter;
    }
}

void OnDeviceRequestEnded(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                           void* userdata1, void* /*userdata2*/) {
    auto* result = static_cast<DeviceResult*>(userdata1);
    result->done = true;
    result->success = (status == WGPURequestDeviceStatus_Success);
    Log("aurora_smoke: device request status=%d\n", static_cast<int>(status));
    PrintStringView("message", message);
    if (result->success) {
        result->device = device;
    }
}

void OnDeviceError(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message, void* /*u1*/,
                    void* /*u2*/) {
    Log("aurora_smoke: DEVICE ERROR type=%d\n", static_cast<int>(type));
    PrintStringView("message", message);
}

} // namespace

// NVK's heap/memory management expects the app to hand over the whole
// available heap rather than libnx's small fixed default (see nxvk's
// switch/README.md "Calling" section).
extern "C" {
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

int main(int, char**) {
    // NVK is marked experimental/unsupported on Switch and reports zero
    // physical devices unless this is set - this is what our "count=0"
    // vkEnumeratePhysicalDevices result was actually telling us.
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);

    socketInitializeDefault();
    nxlinkStdio(); // redirects stdout/stderr to whichever host ran `nxlink -s`

    g_log = fopen("sdmc:/dawn_smoke_log.txt", "w");
    Log("=== Dawn-for-Switch smoke test starting ===\n");

    RawVulkanProbe(g_log);

    WGPUInstanceFeatureName requiredFeatures[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceDescriptor instanceDesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    instanceDesc.requiredFeatureCount = 1;
    instanceDesc.requiredFeatures = requiredFeatures;
    WGPUInstance instance = wgpuCreateInstance(&instanceDesc);
    if (!instance) {
        Log("FAIL: wgpuCreateInstance returned null\n");
        if (g_log) fclose(g_log);
        return 1;
    }
    Log("OK: wgpuCreateInstance succeeded\n");

    WGPURequestAdapterOptions adapterOpts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    adapterOpts.backendType = WGPUBackendType_Vulkan;

    AdapterResult adapterResult;
    WGPURequestAdapterCallbackInfo adapterCbInfo = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapterCbInfo.mode = WGPUCallbackMode_WaitAnyOnly;
    adapterCbInfo.callback = OnAdapterRequestEnded;
    adapterCbInfo.userdata1 = &adapterResult;

    WGPUFuture adapterFuture = wgpuInstanceRequestAdapter(instance, &adapterOpts, adapterCbInfo);

    WGPUFutureWaitInfo waitInfo = WGPU_FUTURE_WAIT_INFO_INIT;
    waitInfo.future = adapterFuture;
    WGPUWaitStatus waitStatus = wgpuInstanceWaitAny(instance, 1, &waitInfo, 5'000'000'000ull /* 5s */);
    Log("aurora_smoke: adapter wait status=%d\n", static_cast<int>(waitStatus));

    if (!adapterResult.done || !adapterResult.success || !adapterResult.adapter) {
        Log("FAIL: could not get a Vulkan adapter (NVK path did not come up)\n");
        wgpuInstanceRelease(instance);
        if (g_log) fclose(g_log);
        return 1;
    }

    WGPUAdapterInfo info = {};
    if (wgpuAdapterGetInfo(adapterResult.adapter, &info) == WGPUStatus_Success) {
        Log("OK: got adapter\n");
        PrintStringView("vendor", info.vendor);
        PrintStringView("architecture", info.architecture);
        PrintStringView("device", info.device);
        PrintStringView("description", info.description);
        Log("  backendType=%d adapterType=%d vendorID=0x%x deviceID=0x%x\n",
            static_cast<int>(info.backendType), static_cast<int>(info.adapterType), info.vendorID, info.deviceID);
        wgpuAdapterInfoFreeMembers(info);
    } else {
        Log("WARN: wgpuAdapterGetInfo failed\n");
    }

    DeviceResult deviceResult;
    WGPUDeviceDescriptor deviceDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    deviceDesc.uncapturedErrorCallbackInfo.callback = OnDeviceError;

    WGPURequestDeviceCallbackInfo deviceCbInfo = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    deviceCbInfo.mode = WGPUCallbackMode_WaitAnyOnly;
    deviceCbInfo.callback = OnDeviceRequestEnded;
    deviceCbInfo.userdata1 = &deviceResult;

    WGPUFuture deviceFuture = wgpuAdapterRequestDevice(adapterResult.adapter, &deviceDesc, deviceCbInfo);

    WGPUFutureWaitInfo deviceWaitInfo = WGPU_FUTURE_WAIT_INFO_INIT;
    deviceWaitInfo.future = deviceFuture;
    WGPUWaitStatus deviceWaitStatus = wgpuInstanceWaitAny(instance, 1, &deviceWaitInfo, 5'000'000'000ull);
    Log("aurora_smoke: device wait status=%d\n", static_cast<int>(deviceWaitStatus));

    bool overallSuccess = false;
    if (deviceResult.done && deviceResult.success && deviceResult.device) {
        Log("OK: got a WGPUDevice - NVK/Vulkan backend is alive end to end.\n");
        overallSuccess = true;
        wgpuDeviceRelease(deviceResult.device);
    } else {
        Log("FAIL: could not get a device from the adapter\n");
    }

    wgpuAdapterRelease(adapterResult.adapter);
    wgpuInstanceRelease(instance);

    Log("=== Dawn-for-Switch smoke test finished: %s ===\n", overallSuccess ? "SUCCESS" : "FAILURE");

    Log("--- present demo starting (separate raw-Vulkan instance) ---\n");
    PresentColorCycleDemo(g_log);
    if (g_log) fclose(g_log);

    socketExit();
    return overallSuccess ? 0 : 1;
}
