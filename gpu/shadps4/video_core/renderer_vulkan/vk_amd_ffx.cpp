// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_amd_ffx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "include/ffx_api.h"
#include "include/vk/ffx_api_vk.h"
#include "upscalers/ffx_upscale.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

constexpr const char* kLoader = "amd_fidelityfx_loader_vk.dll";

#ifdef _WIN32
std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), n);
    return wide;
}

bool Exists(const std::string& path) {
    return GetFileAttributesW(Widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}
#endif

std::string FindLoader() {
#ifdef _WIN32
    std::string candidates[3];
    if (const char* dir = std::getenv("BB_FFX_DIR"); dir && *dir) {
        candidates[0] = std::string{dir} + "/" + kLoader;
    }
    candidates[1] = std::string{"fsr/"} + kLoader;   // beside the game program (runtime/fsr)
    if (const char* user = std::getenv("BB_GPU_USER_DIR"); user && *user) {
        candidates[2] = std::string{user} + "/fsr/" + kLoader;
    }
    for (const auto& path : candidates) {
        if (!path.empty() && Exists(path)) {
            return path;
        }
    }
#endif
    return {};
}

void FfxMessage(uint32_t type, const wchar_t* message) {
    std::printf("AMD FSR: %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

} // namespace

struct AmdFfxUpscaler::Impl {
    Impl(const Instance& instance_, Scheduler& scheduler_) : instance{instance_}, scheduler{scheduler_} {}
    ~Impl() {
        scheduler.Finish();
        Destroy();
#ifdef _WIN32
        if (module) {
            FreeLibrary(static_cast<HMODULE>(module));
        }
#endif
    }

    const Instance& instance;
    Scheduler& scheduler;
    std::string problem;
    std::string version;
    bool fatal = false;
    void* module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext context = nullptr;
    u32 context_render_w = 0, context_render_h = 0, context_out_w = 0, context_out_h = 0;
    bool context_auto_exposure = false;

    void Fail(std::string reason, bool permanent) {
        if (problem != reason) {
            std::printf("Upscaler: AMD FSR 4.1 unavailable: %s\n", reason.c_str());
        }
        problem = std::move(reason);
        fatal |= permanent;
    }

    void Destroy() {
        if (context && destroy) {
            destroy(&context, nullptr);
        }
        context = nullptr;
    }

    bool Load() {
        if (module) {
            return true;
        }
#ifdef _WIN32
        const std::string path = FindLoader();
        if (path.empty()) {
            Fail(std::string{"AMD's "} + kLoader +
                     " and amd_fidelityfx_upscaler_vk.dll (FSR SDK 2.4) are not in BB_FFX_DIR or the fsr folder",
                 true);
            return false;
        }
        HMODULE handle = LoadLibraryW(Widen(path).c_str());
        if (!handle) {
            Fail("AMD's FidelityFX loader could not be loaded (" + path + ")", true);
            return false;
        }
        module = handle;
        create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(handle, "ffxCreateContext"));
        destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(handle, "ffxDestroyContext"));
        dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(handle, "ffxDispatch"));
        query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(handle, "ffxQuery"));
        if (!create || !destroy || !dispatch) {
            Fail("AMD's FidelityFX loader lacks the FidelityFX API entry points", true);
            return false;
        }
        std::printf("Upscaler: AMD FidelityFX loader: %s\n", path.c_str());
        return true;
#else
        Fail("AMD FSR 4.1 needs Windows", true);
        return false;
#endif
    }

    bool CreateContext(const Frame& frame) {
        Destroy();
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext.vkDevice = instance.GetDevice();
        backend.deviceContext.vkPhysicalDevice = instance.GetPhysicalDevice();
        backend.deviceContext.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

        ffxCreateContextDescUpscaleVersion version_desc{};
        version_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version_desc.version = FFX_UPSCALER_VERSION;
        version_desc.header.pNext = &backend.header;

        ffxCreateContextDescUpscale upscale{};
        upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        upscale.header.pNext = &version_desc.header;
        // The game's colour is linear HDR; its depth is reversed (1 at the near plane) with an infinite far plane;
        // the motion vectors are in render pixels and carry no jitter.
        upscale.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                        FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                        (frame.auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u);
        upscale.maxRenderSize = {frame.render_width, frame.render_height};
        upscale.maxUpscaleSize = {frame.output.width, frame.output.height};
        upscale.fpMessage = &FfxMessage;

        const ffxReturnCode_t result = create(&context, &upscale.header, nullptr);
        if (result != FFX_API_RETURN_OK) {
            context = nullptr;
            std::string reason = "the upscale context could not be created (code " + std::to_string(result) + ")";
            if (result == FFX_API_RETURN_NO_PROVIDER || result == FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE) {
                reason += ": this graphics card or driver has no FSR 4.1 provider (needs a Radeon with Adrenalin "
                          "26.10.1 or newer)";
            }
            Fail(reason, true);
            return false;
        }
        context_render_w = frame.render_width;
        context_render_h = frame.render_height;
        context_out_w = frame.output.width;
        context_out_h = frame.output.height;
        context_auto_exposure = frame.auto_exposure;
        if (query) {
            ffxQueryGetProviderVersion provider{};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName) {
                version = provider.versionName;
            }
        }
        std::printf("Upscaler: AMD FSR 4.1 %s, %ux%u to %ux%u\n", version.empty() ? "(version unknown)" : version.c_str(),
                    frame.render_width, frame.render_height, frame.output.width, frame.output.height);
        return true;
    }

    static FfxApiResource Resource(const Image& image, bool uav, uint32_t state) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = static_cast<VkFormat>(image.format);
        info.extent = {image.width, image.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.usage = static_cast<VkImageUsageFlags>(image.usage);
        const FfxApiResourceDescription description =
            ffxApiGetImageResourceDescriptionVK(static_cast<VkImage>(image.image), info,
                                                uav ? FFX_API_RESOURCE_USAGE_UAV : 0u);
        return ffxApiGetResourceVK(reinterpret_cast<void*>(static_cast<VkImage>(image.image)), description, state);
    }

    bool Record(const Frame& frame) {
        if (fatal) {
            return false;
        }
        if (!Load()) {
            return false;
        }
        if (!context || context_render_w != frame.render_width || context_render_h != frame.render_height ||
            context_out_w != frame.output.width || context_out_h != frame.output.height ||
            context_auto_exposure != frame.auto_exposure) {
            scheduler.Finish();
            if (!CreateContext(frame)) {
                return false;
            }
        }
        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.commandList = reinterpret_cast<void*>(static_cast<VkCommandBuffer>(frame.cmdbuf));
        desc.color = Resource(frame.color, false, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = Resource(frame.depth, false, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = Resource(frame.motion, false, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.output = Resource(frame.output, true, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset = {frame.jitter[0], frame.jitter[1]};
        desc.motionVectorScale = {1.0f, 1.0f};
        desc.renderSize = {frame.render_width, frame.render_height};
        desc.upscaleSize = {frame.output.width, frame.output.height};
        desc.enableSharpening = frame.sharpen && frame.sharpness > 0.0f;
        desc.sharpness = frame.sharpness;
        desc.frameTimeDelta = frame.frame_ms;
        desc.preExposure = 1.0f;
        desc.reset = frame.reset;
        desc.cameraNear = frame.near_plane;
        desc.cameraFar = frame.far_plane;
        desc.cameraFovAngleVertical = frame.vertical_fov;
        desc.viewSpaceToMetersFactor = 1.0f;
        desc.flags = 0;
        const ffxReturnCode_t result = dispatch(&context, &desc.header);
        if (result != FFX_API_RETURN_OK) {
            Fail("the upscale dispatch failed (code " + std::to_string(result) + ")", true);
            return false;
        }
        problem.clear();
        return true;
    }
};

AmdFfxUpscaler::AmdFfxUpscaler(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

AmdFfxUpscaler::~AmdFfxUpscaler() = default;

bool AmdFfxUpscaler::Record(const Frame& frame) {
    return impl->Record(frame);
}

const char* AmdFfxUpscaler::Problem() const noexcept {
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool AmdFfxUpscaler::Fatal() const noexcept {
    return impl->fatal;
}

const char* AmdFfxUpscaler::Version() const noexcept {
    return impl->version.c_str();
}

} // namespace Vulkan
