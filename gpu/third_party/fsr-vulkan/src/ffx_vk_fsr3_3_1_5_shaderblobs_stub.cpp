/*
 * Copyright (c) 2026 The DriveClub recompilation project contributors
 * SPDX-License-Identifier: MIT
 *
 * Supplies the embedded Vulkan permutation selected by the SDK host.
 */

#include "ffx_fsr3upscaler.h"
#include "ffx_fsr3upscaler_shaderblobs.h"
#include "ffx_vk_fsr3_3_1_5_bundle.h"

#include <cstring>

extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)
{
    if (!outBlob)
        return FFX_ERROR_INVALID_POINTER;
    std::memset(outBlob, 0, sizeof(*outBlob));
    const uint32_t permutation = 7u | (permutationOptions & 16u) |
        (passId == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN ? 32u : 0u);
    const uint32_t* words = nullptr;
    size_t wordCount = 0;
    if (ffxVkFsr3_3_1_5GetEmbeddedModule(uint32_t(passId), permutation, &words, &wordCount) !=
        FFX_VK_PORTABLE_OK)
        return FFX_ERROR_INVALID_ARGUMENT;
    outBlob->data = reinterpret_cast<const uint8_t*>(words);
    outBlob->size = uint32_t(wordCount * sizeof(uint32_t));
    return FFX_OK;
}

extern "C" FfxErrorCode fsr3UpscalerIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    (void)permutationOptions;
    isWave64 = false;
    return FFX_OK;
}
