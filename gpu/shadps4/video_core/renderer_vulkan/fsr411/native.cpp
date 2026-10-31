// SPDX-License-Identifier: GPL-2.0-or-later
#include "native.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "../../../../third_party/fsr412/sdk/upscalers/include/ffx_upscale.h"
#include "../../../../third_party/fsr412/sdk/api/include/vk/ffx_api_vk.h"
#endif

namespace Fsr411 {

struct NativeUpscaler::Impl {
#ifdef _WIN32
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext context = nullptr;
    ffxCreateContextDescUpscale desc{};
    ffxCreateBackendVKDesc backend{};
    ffxOverrideVersion version{};

    Impl(VkPhysicalDevice physical, VkDevice device, const Frame& frame) {
        wchar_t executable[32768];
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) return;
        const auto path = std::filesystem::path(executable).parent_path() /
                          "amd_fidelityfx_loader_vk.dll";
        module = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return;
        create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
        destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
        dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
        query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
        if (!create || !destroy || !dispatch || !query) return;

        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext = {device, physical, vkGetDeviceProcAddr};
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = &backend.deviceContext;
        versions.outputCount = &count;
        if (query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count) return;
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        versions.versionIds = ids.data();
        versions.versionNames = names.data();
        if (query(nullptr, &versions.header) != FFX_API_RETURN_OK) return;
        for (size_t i = 0; i < ids.size() && i < count; ++i) {
            if (names[i] && std::strcmp(names[i], "4.1.2") == 0) {
                version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                version.versionId = ids[i];
                break;
            }
        }
        if (!version.versionId) {
            std::puts("FSR 4.1.2: AMD runtime has no supported 4.1.2 provider; using portable INT8");
            return;
        }
        backend.header.pNext = &version.header;
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (frame.auto_exposure) desc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        desc.maxRenderSize = desc.maxUpscaleSize = {frame.output.width, frame.output.height};
        if (create(&context, &desc.header, nullptr) != FFX_API_RETURN_OK) {
            context = nullptr;
            std::puts("FSR 4.1.2: native context creation failed; using portable INT8");
            return;
        }
        ffxQueryGetProviderVersion actual{};
        actual.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (query(&context, &actual.header) != FFX_API_RETURN_OK || !actual.versionName ||
            std::strcmp(actual.versionName, "4.1.2") != 0 || actual.versionId != version.versionId) {
            destroy(&context, nullptr);
            context = nullptr;
            std::puts("FSR 4.1.2: native provider version mismatch; using portable INT8");
        }
    }

    ~Impl() {
        if (context) destroy(&context, nullptr);
        if (module) FreeLibrary(module);
    }

    bool Ready() const { return context != nullptr; }

    bool Record(const Frame& frame, std::string& error) {
        const auto resource = [](const Image& image, VkFormat format, uint32_t usage, uint32_t state) {
            FfxApiResourceDescription description{};
            description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            description.format = ffxApiGetSurfaceFormatVK(format);
            description.width = image.width;
            description.height = image.height;
            description.depth = description.mipCount = 1;
            description.usage = usage;
            if (ffxApiIsStencilFormat(format)) description.usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
            return ffxApiGetResourceVK(reinterpret_cast<void*>(image.image), description, state);
        };
        ffxDispatchDescUpscale dispatch_desc{};
        dispatch_desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch_desc.commandList = frame.cmdbuf;
        dispatch_desc.color = resource(frame.color, frame.color_format,
                                      FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch_desc.depth = resource(frame.depth, frame.depth_format,
                                      FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch_desc.motionVectors = resource(frame.motion, VK_FORMAT_R16G16_SFLOAT,
                                              FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch_desc.output = resource(frame.output, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch_desc.jitterOffset = {frame.jitter[0], frame.jitter[1]};
        dispatch_desc.motionVectorScale = {frame.motion_scale[0], frame.motion_scale[1]};
        dispatch_desc.renderSize = {frame.render_width, frame.render_height};
        dispatch_desc.upscaleSize = {frame.output.width, frame.output.height};
        dispatch_desc.enableSharpening = frame.sharpen;
        dispatch_desc.sharpness = frame.sharpness;
        dispatch_desc.frameTimeDelta = frame.frame_ms;
        dispatch_desc.preExposure = frame.pre_exposure;
        dispatch_desc.reset = frame.reset;
        dispatch_desc.cameraNear = frame.near_plane;
        dispatch_desc.cameraFar = frame.far_plane;
        dispatch_desc.cameraFovAngleVertical = frame.vertical_fov;
        dispatch_desc.viewSpaceToMetersFactor = 1.0f;

        VkImageMemoryBarrier barriers[3]{};
        const Image* images[] = {&frame.color, &frame.depth, &frame.motion};
        for (size_t i = 0; i < 3; ++i) {
            auto& barrier = barriers[i];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[i]->image;
            barrier.subresourceRange = {i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (i == 1 && ffxApiIsStencilFormat(frame.depth_format)) {
                barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }
        vkCmdPipelineBarrier(frame.cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);
        const auto result = dispatch(&context, &dispatch_desc.header);
        for (auto& barrier : barriers) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkMemoryBarrier output{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        output.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        output.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(frame.cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &output, 0, nullptr, 3, barriers);
        if (result != FFX_API_RETURN_OK) {
            error = "native AMD FSR 4.1.2 dispatch failed (" + std::to_string(result) + ")";
            return false;
        }
        return true;
    }
#else
    Impl(VkPhysicalDevice, VkDevice, const Frame&) {}
    bool Ready() const { return false; }
    bool Record(const Frame&, std::string&) { return false; }
#endif
};

NativeUpscaler::NativeUpscaler(VkPhysicalDevice physical, VkDevice device, const Frame& frame)
    : impl{std::make_unique<Impl>(physical, device, frame)} {}
NativeUpscaler::~NativeUpscaler() = default;
bool NativeUpscaler::Ready() const { return impl->Ready(); }
bool NativeUpscaler::Record(const Frame& frame, std::string& error) { return impl->Record(frame, error); }

} // namespace Fsr411
