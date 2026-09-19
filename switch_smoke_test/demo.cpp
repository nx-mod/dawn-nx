// dawn-nx demo: WebGPU on Nintendo Switch through Dawn's C++ API, end to end.
//
//   1. instance, a Vulkan adapter (NVK) and a device
//   2. buffers: WriteBuffer, CopyBufferToBuffer, MapAsync readback
//   3. compute: a WGSL shader over a storage buffer
//   4. render to a texture: vertex and uniform buffers, a sampled texture and a
//      sampler; the pixels are read back and checked
//   5. present: a surface on the Switch's default window, 180 frames
//
// Every check is written to sdmc:/switch/dawn-nx-demo.log (and to nxlink when
// one is listening). The screen flashes green (all passed) or red, Dawn is torn
// down in order, and the report is shown on a text console until + is pressed.
#include <switch.h>

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Run as an application (full memory), with the whole heap available to NVK.
extern "C" {
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

namespace {

FILE* g_log = nullptr;
std::string g_report;   // Everything reported, shown on the console at the end
int g_passed = 0;
int g_failed = 0;
int g_deviceErrors = 0;
wgpu::Instance g_instance;

void Report(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_report += line;
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
    fputs(line, stdout);
    fflush(stdout);
}

void Check(const char* what, bool ok) {
    (ok ? g_passed : g_failed)++;
    Report("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

std::string Str(wgpu::StringView view) {
    if (view.data == nullptr) {
        return {};
    }
    return view.length == WGPU_STRLEN ? std::string(view.data) : std::string(view.data, view.length);
}

bool Wait(const wgpu::Future& future) {
    return g_instance.WaitAny(future, 5'000'000'000ull) == wgpu::WaitStatus::Success;
}

bool ReadBack(const wgpu::Buffer& buffer, size_t size, void* out) {
    wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::CallbackCancelled;
    const wgpu::Future future = buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
                                                [&status](wgpu::MapAsyncStatus s, wgpu::StringView) { status = s; });
    if (!Wait(future) || status != wgpu::MapAsyncStatus::Success) {
        return false;
    }
    std::memcpy(out, buffer.GetConstMappedRange(0, size), size);
    buffer.Unmap();
    return true;
}

wgpu::Buffer MakeBuffer(const wgpu::Device& device, uint64_t size, wgpu::BufferUsage usage) {
    wgpu::BufferDescriptor desc;
    desc.size = size;
    desc.usage = usage;
    return device.CreateBuffer(&desc);
}

wgpu::ShaderModule MakeShader(const wgpu::Device& device, const char* wgsl) {
    wgpu::ShaderSourceWGSL source;
    source.code = wgsl;
    wgpu::ShaderModuleDescriptor desc;
    desc.nextInChain = &source;
    return device.CreateShaderModule(&desc);
}

void Submit(const wgpu::Device& device, const wgpu::CommandEncoder& encoder) {
    const wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
}

// --- 1. instance, adapter, device ---------------------------------------------

wgpu::Adapter RequestAdapter() {
    wgpu::RequestAdapterOptions options;
    options.backendType = wgpu::BackendType::Vulkan;
    wgpu::Adapter result;
    const wgpu::Future future = g_instance.RequestAdapter(
        &options, wgpu::CallbackMode::WaitAnyOnly,
        [&result](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                result = std::move(adapter);
            } else {
                Report("  adapter request failed: %s\n", Str(message).c_str());
            }
        });
    return Wait(future) ? result : nullptr;
}

wgpu::Device RequestDevice(const wgpu::Adapter& adapter) {
    wgpu::DeviceDescriptor desc;
    desc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        g_deviceErrors++;
        Report("  device error %d: %s\n", static_cast<int>(type), Str(message).c_str());
    });
    desc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
                               [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                                   if (reason != wgpu::DeviceLostReason::Destroyed) {
                                       Report("  device lost: %s\n", Str(message).c_str());
                                   }
                               });
    wgpu::Device result;
    const wgpu::Future future = adapter.RequestDevice(
        &desc, wgpu::CallbackMode::WaitAnyOnly,
        [&result](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                result = std::move(device);
            } else {
                Report("  device request failed: %s\n", Str(message).c_str());
            }
        });
    return Wait(future) ? result : nullptr;
}

// --- 2. buffers ---------------------------------------------------------------

void TestBuffers(const wgpu::Device& device) {
    std::array<uint32_t, 256> data;
    for (uint32_t i = 0; i < data.size(); i++) {
        data[i] = i * 2654435761u;
    }
    const uint64_t bytes = sizeof(data);
    const wgpu::Buffer source = MakeBuffer(device, bytes, wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst);
    const wgpu::Buffer readback = MakeBuffer(device, bytes, wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst);
    device.GetQueue().WriteBuffer(source, 0, data.data(), bytes);

    const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    encoder.CopyBufferToBuffer(source, 0, readback, 0, bytes);
    Submit(device, encoder);

    std::array<uint32_t, 256> out{};
    Check("buffer upload, GPU copy and readback", ReadBack(readback, bytes, out.data()) && out == data);
}

// --- 3. compute ---------------------------------------------------------------

constexpr const char* kComputeShader = R"(
@group(0) @binding(0) var<storage, read_write> values: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x < arrayLength(&values)) {
        values[id.x] = values[id.x] * values[id.x] + 7u;
    }
}
)";

void TestCompute(const wgpu::Device& device) {
    constexpr uint32_t kCount = 4096;
    std::array<uint32_t, kCount> data;
    for (uint32_t i = 0; i < kCount; i++) {
        data[i] = i;
    }
    const uint64_t bytes = sizeof(data);
    const wgpu::Buffer values = MakeBuffer(
        device, bytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst);
    const wgpu::Buffer readback = MakeBuffer(device, bytes, wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst);
    device.GetQueue().WriteBuffer(values, 0, data.data(), bytes);

    wgpu::ComputePipelineDescriptor pipelineDesc;
    pipelineDesc.compute.module = MakeShader(device, kComputeShader);
    pipelineDesc.compute.entryPoint = "main";
    const wgpu::ComputePipeline pipeline = device.CreateComputePipeline(&pipelineDesc);

    wgpu::BindGroupEntry entry;
    entry.binding = 0;
    entry.buffer = values;
    entry.size = bytes;
    wgpu::BindGroupDescriptor groupDesc;
    groupDesc.layout = pipeline.GetBindGroupLayout(0);
    groupDesc.entryCount = 1;
    groupDesc.entries = &entry;
    const wgpu::BindGroup group = device.CreateBindGroup(&groupDesc);

    const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    const wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, group);
    pass.DispatchWorkgroups((kCount + 63) / 64);
    pass.End();
    encoder.CopyBufferToBuffer(values, 0, readback, 0, bytes);
    Submit(device, encoder);

    std::array<uint32_t, kCount> out{};
    bool ok = ReadBack(readback, bytes, out.data());
    for (uint32_t i = 0; ok && i < kCount; i++) {
        ok = out[i] == i * i + 7u;
    }
    Check("compute shader over 4096 values", ok);
}

// --- 4. render to a texture -----------------------------------------------------

constexpr const char* kRenderShader = R"(
struct Uniforms { tint: vec4f }
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs(@location(0) position: vec2f, @location(1) uv: vec2f) -> VertexOut {
    var out: VertexOut;
    out.position = vec4f(position, 0.0, 1.0);
    out.uv = uv;
    return out;
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4f {
    return textureSample(tex, samp, in.uv) * u.tint;
}
)";

bool Near(uint8_t actual, int expected) {
    return std::abs(static_cast<int>(actual) - expected) <= 2;
}

void TestRender(const wgpu::Device& device) {
    constexpr uint32_t kSize = 64;  // 64 px * 4 bytes = 256, the required row alignment
    const wgpu::Queue queue = device.GetQueue();

    // A triangle covering the centre but not the top-left corner.
    const float vertices[] = {
        // x, y, u, v
        -0.9f, -0.9f, 0.0f, 1.0f,
        0.9f, -0.9f, 1.0f, 1.0f,
        0.0f, 0.9f, 0.5f, 0.0f,
    };
    const wgpu::Buffer vertexBuffer =
        MakeBuffer(device, sizeof(vertices), wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
    queue.WriteBuffer(vertexBuffer, 0, vertices, sizeof(vertices));

    const float tint[4] = {1.0f, 0.5f, 1.0f, 1.0f};
    const wgpu::Buffer uniformBuffer =
        MakeBuffer(device, sizeof(tint), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
    queue.WriteBuffer(uniformBuffer, 0, tint, sizeof(tint));

    // A 2x2 texture, every texel (200, 100, 50).
    wgpu::TextureDescriptor texDesc;
    texDesc.size = {2, 2, 1};
    texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    const wgpu::Texture texture = device.CreateTexture(&texDesc);
    const uint8_t texels[16] = {200, 100, 50, 255, 200, 100, 50, 255, 200, 100, 50, 255, 200, 100, 50, 255};
    wgpu::TexelCopyTextureInfo texDst;
    texDst.texture = texture;
    wgpu::TexelCopyBufferLayout texLayout;
    texLayout.bytesPerRow = 8;
    texLayout.rowsPerImage = 2;
    const wgpu::Extent3D texExtent = {2, 2, 1};
    queue.WriteTexture(&texDst, texels, sizeof(texels), &texLayout, &texExtent);
    const wgpu::Sampler sampler = device.CreateSampler();

    wgpu::TextureDescriptor targetDesc;
    targetDesc.size = {kSize, kSize, 1};
    targetDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    targetDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    const wgpu::Texture target = device.CreateTexture(&targetDesc);

    const wgpu::ShaderModule shader = MakeShader(device, kRenderShader);
    std::array<wgpu::VertexAttribute, 2> attributes;
    attributes[0].format = wgpu::VertexFormat::Float32x2;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = wgpu::VertexFormat::Float32x2;
    attributes[1].offset = 8;
    attributes[1].shaderLocation = 1;
    wgpu::VertexBufferLayout vertexLayout;
    vertexLayout.arrayStride = 16;
    vertexLayout.attributeCount = attributes.size();
    vertexLayout.attributes = attributes.data();
    wgpu::ColorTargetState colorTarget;
    colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
    wgpu::FragmentState fragment;
    fragment.module = shader;
    fragment.entryPoint = "fs";
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;
    wgpu::RenderPipelineDescriptor pipelineDesc;
    pipelineDesc.vertex.module = shader;
    pipelineDesc.vertex.entryPoint = "vs";
    pipelineDesc.vertex.bufferCount = 1;
    pipelineDesc.vertex.buffers = &vertexLayout;
    pipelineDesc.fragment = &fragment;
    const wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipelineDesc);

    std::array<wgpu::BindGroupEntry, 3> entries;
    entries[0].binding = 0;
    entries[0].buffer = uniformBuffer;
    entries[0].size = sizeof(tint);
    entries[1].binding = 1;
    entries[1].textureView = texture.CreateView();
    entries[2].binding = 2;
    entries[2].sampler = sampler;
    wgpu::BindGroupDescriptor groupDesc;
    groupDesc.layout = pipeline.GetBindGroupLayout(0);
    groupDesc.entryCount = entries.size();
    groupDesc.entries = entries.data();
    const wgpu::BindGroup group = device.CreateBindGroup(&groupDesc);

    wgpu::RenderPassColorAttachment attachment;
    attachment.view = target.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = {0.0, 0.0, 1.0, 1.0};
    wgpu::RenderPassDescriptor passDesc;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &attachment;

    const uint64_t bytes = kSize * kSize * 4;
    const wgpu::Buffer readback = MakeBuffer(device, bytes, wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst);

    const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    const wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, group);
    pass.SetVertexBuffer(0, vertexBuffer);
    pass.Draw(3);
    pass.End();
    wgpu::TexelCopyTextureInfo copySrc;
    copySrc.texture = target;
    wgpu::TexelCopyBufferInfo copyDst;
    copyDst.buffer = readback;
    copyDst.layout.bytesPerRow = kSize * 4;
    copyDst.layout.rowsPerImage = kSize;
    const wgpu::Extent3D copyExtent = {kSize, kSize, 1};
    encoder.CopyTextureToBuffer(&copySrc, &copyDst, &copyExtent);
    Submit(device, encoder);

    std::array<uint8_t, kSize * kSize * 4> pixels{};
    const bool read = ReadBack(readback, bytes, pixels.data());
    const uint8_t* centre = &pixels[((kSize / 2) * kSize + kSize / 2) * 4];
    const uint8_t* corner = &pixels[0];
    Report("         centre (%u, %u, %u, %u), corner (%u, %u, %u, %u)\n", centre[0], centre[1], centre[2], centre[3],
           corner[0], corner[1], corner[2], corner[3]);
    // texel * tint: (200, 100 * 0.5, 50)
    Check("render: vertex + uniform buffers, sampled texture",
          read && Near(centre[0], 200) && Near(centre[1], 50) && Near(centre[2], 50) && centre[3] == 255);
    Check("render: clear colour outside the triangle",
          read && corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255);
}

// --- 5. present -------------------------------------------------------------------

void ClearFrame(const wgpu::Device& device, const wgpu::Texture& texture, const wgpu::Color& color) {
    wgpu::RenderPassColorAttachment attachment;
    attachment.view = texture.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = color;
    wgpu::RenderPassDescriptor passDesc;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &attachment;
    const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    encoder.BeginRenderPass(&passDesc).End();
    Submit(device, encoder);
}

bool PresentFrame(const wgpu::Device& device, const wgpu::Surface& surface, const wgpu::Color& color) {
    wgpu::SurfaceTexture frame;
    surface.GetCurrentTexture(&frame);
    if (frame.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        frame.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        return false;
    }
    ClearFrame(device, frame.texture, color);
    return surface.Present() == wgpu::Status::Success;
}

wgpu::Surface MakeSurface(const wgpu::Adapter& adapter, const wgpu::Device& device) {
    wgpu::SurfaceSourceViNN source;
    source.window = nwindowGetDefault();
    wgpu::SurfaceDescriptor desc;
    desc.nextInChain = &source;
    const wgpu::Surface surface = g_instance.CreateSurface(&desc);
    if (!surface) {
        return nullptr;
    }
    wgpu::SurfaceCapabilities caps;
    if (surface.GetCapabilities(adapter, &caps) != wgpu::Status::Success || caps.formatCount == 0) {
        return nullptr;
    }
    wgpu::SurfaceConfiguration config;
    config.device = device;
    config.format = caps.formats[0];
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = 1280;
    config.height = 720;
    config.presentMode = wgpu::PresentMode::Fifo;
    surface.Configure(&config);
    Report("         surface format %d, 1280x720, FIFO\n", static_cast<int>(config.format));
    return surface;
}

void TestPresent(const wgpu::Surface& surface, const wgpu::Device& device) {
    constexpr int kFrames = 180;
    int presented = 0;
    const u64 start = armGetSystemTick();
    for (int frame = 0; frame < kFrames && appletMainLoop(); frame++) {
        const double t = frame / 60.0;
        const wgpu::Color color = {0.5 + 0.5 * std::sin(t * 2.0), 0.5 + 0.5 * std::sin(t * 2.0 + 2.1),
                                   0.5 + 0.5 * std::sin(t * 2.0 + 4.2), 1.0};
        presented += PresentFrame(device, surface, color) ? 1 : 0;
    }
    const double seconds = armTicksToNs(armGetSystemTick() - start) / 1e9;
    Report("         %d/%d frames in %.2f s (%.1f fps)\n", presented, kFrames, seconds, presented / seconds);
    Check("present 180 frames to the screen", presented == kFrames);
}

}  // namespace

int main(int, char**) {
    // NVK is experimental on Switch and reports no devices unless asked to.
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
    socketInitializeDefault();
    nxlinkStdio();
    g_log = fopen("sdmc:/switch/dawn-nx-demo.log", "w");
    Report("dawn-nx demo\n\n");

    const wgpu::InstanceFeatureName features[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor instanceDesc;
    instanceDesc.requiredFeatureCount = 1;
    instanceDesc.requiredFeatures = features;
    g_instance = wgpu::CreateInstance(&instanceDesc);
    Check("create instance", g_instance != nullptr);

    wgpu::Adapter adapter = g_instance ? RequestAdapter() : nullptr;
    Check("Vulkan adapter (NVK)", adapter != nullptr);
    if (adapter) {
        wgpu::AdapterInfo info;
        adapter.GetInfo(&info);
        Report("         %s, %s\n", Str(info.device).c_str(), Str(info.description).c_str());
    }
    wgpu::Device device = adapter ? RequestDevice(adapter) : nullptr;
    Check("device", device != nullptr);

    wgpu::Surface surface;
    if (device) {
        TestBuffers(device);
        TestCompute(device);
        TestRender(device);
        surface = MakeSurface(adapter, device);
        Check("surface on the default window", surface != nullptr);
        if (surface) {
            TestPresent(surface, device);
        }
        Check("no device errors", g_deviceErrors == 0);
    }

    Report("\n%d passed, %d failed.\n", g_passed, g_failed);

    // Two seconds of green (all passed) or red on the GPU surface.
    const wgpu::Color result = g_failed == 0 ? wgpu::Color{0.0, 0.6, 0.0, 1.0} : wgpu::Color{0.7, 0.0, 0.0, 1.0};
    for (int frame = 0; surface && frame < 120 && appletMainLoop(); frame++) {
        PresentFrame(device, surface, result);
    }

    // Tear down in dependency order, logging each step, so a hang names itself.
    Report("\nshutting down: surface");
    if (surface) {
        surface.Unconfigure();
        surface = nullptr;
    }
    Report(", device");
    if (device) {
        device.Destroy();
        g_instance.ProcessEvents();
        device = nullptr;
    }
    Report(", adapter");
    adapter = nullptr;
    Report(", instance");
    g_instance = nullptr;
    Report(" - done.\n");

    // The window is free again (the swapchain released it): show the report
    // on a text console.
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    std::fputs(g_report.c_str(), stdout);
    std::printf("\nFinished. Report saved to sdmc:/switch/dawn-nx-demo.log\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);

    socketExit();
    return 0;
}
