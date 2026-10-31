// SPDX-License-Identifier: GPL-2.0-or-later
// Build once as the fake loader DLL, and once with native.cpp as the test executable.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef FSR412_TEST_LOADER
#include "../gpu/third_party/fsr412/sdk/upscalers/include/ffx_upscale.h"
#include "../gpu/third_party/fsr412/sdk/api/include/vk/ffx_api_vk.h"

static int created = 0, destroyed = 0;
static ffxCreateContextDescUpscale* saved = nullptr;
static bool Case(const char* name) { return std::strcmp(std::getenv("FSR412_TEST_CASE"), name) == 0; }

extern "C" __declspec(dllexport) int ffxTestCount(bool destroys) { return destroys ? destroyed : created; }

ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* header,
                               const ffxAllocationCallbacks*) {
    ++created;
    saved = reinterpret_cast<ffxCreateContextDescUpscale*>(header);
    assert(header->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
    auto* backend = reinterpret_cast<ffxCreateBackendVKDesc*>(header->pNext);
    assert(backend->header.type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK);
    auto* version = reinterpret_cast<ffxOverrideVersion*>(backend->header.pNext);
    assert(version->header.type == FFX_API_DESC_TYPE_OVERRIDE_VERSION && version->versionId == 42);
    assert(saved->maxUpscaleSize.width == 1920 && saved->maxUpscaleSize.height == 1080);
    assert(saved->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE);
    if (Case("create-failure")) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    *context = reinterpret_cast<void*>(1);
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*) {
    assert(*context == reinterpret_cast<void*>(1));
    ++destroyed;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxQuery(ffxContext*, ffxQueryDescHeader* header) {
    if (Case("query-failure")) return FFX_API_RETURN_ERROR;
    if (header->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* versions = reinterpret_cast<ffxQueryDescGetVersions*>(header);
        *versions->outputCount = Case("unsupported") ? 1 : 2;
        if (versions->versionIds) {
            versions->versionIds[0] = 3;
            versions->versionNames[0] = "3.1.6";
            if (!Case("unsupported")) {
                versions->versionIds[1] = 42;
                versions->versionNames[1] = "4.1.2";
            }
        }
    } else {
        assert(header->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION);
        // The API requires creation descriptors to remain alive for the whole context lifetime.
        assert(saved->header.pNext->pNext->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION);
        auto* actual = reinterpret_cast<ffxQueryGetProviderVersion*>(header);
        actual->versionId = Case("mismatch") ? 3 : 42;
        actual->versionName = Case("mismatch") ? "3.1.6" : "4.1.2";
    }
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDispatch(ffxContext*, const ffxDispatchDescHeader* header) {
    auto* frame = reinterpret_cast<const ffxDispatchDescUpscale*>(header);
    assert(frame->depth.description.usage & FFX_API_RESOURCE_USAGE_STENCILTARGET);
    assert(frame->depth.description.format == FFX_API_SURFACE_FORMAT_R32_FLOAT);
    assert(frame->depth.state == FFX_API_RESOURCE_STATE_COMPUTE_READ);
    assert(frame->output.state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    return FFX_API_RETURN_OK;
}
#else
#include "../gpu/shadps4/video_core/renderer_vulkan/fsr411/native.h"

static int barriers_recorded = 0;
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags,
    VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
    const VkBufferMemoryBarrier*, uint32_t count, const VkImageMemoryBarrier* barriers) {
    assert(count == 3);
    assert(barriers[1].subresourceRange.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    assert(barriers[1].oldLayout == (barriers_recorded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL));
    assert(barriers[1].newLayout == (barriers_recorded ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    ++barriers_recorded;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    _putenv_s("FSR412_TEST_CASE", argv[1]);
    const bool supported = std::strcmp(argv[1], "supported") == 0;
    const bool no_create = std::strcmp(argv[1], "unsupported") == 0 ||
                           std::strcmp(argv[1], "query-failure") == 0;
    HMODULE module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
    assert(module);
    auto count = reinterpret_cast<int (*)(bool)>(GetProcAddress(module, "ffxTestCount"));
    assert(count);
    {
        Fsr411::Frame frame;
        frame.output.width = 1920;
        frame.output.height = 1080;
        frame.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        Fsr411::NativeUpscaler native(VK_NULL_HANDLE, VK_NULL_HANDLE, frame);
        assert(native.Ready() == supported);
        assert(count(false) == (no_create ? 0 : 1));
        if (supported) {
            std::string error;
            assert(native.Record(frame, error) && error.empty());
            assert(barriers_recorded == 2);
        }
    }
    assert(count(true) == (supported || std::strcmp(argv[1], "mismatch") == 0 ? 1 : 0));
    FreeLibrary(module);
    std::printf("PASS provider selection: %s\n", argv[1]);
}
#endif
