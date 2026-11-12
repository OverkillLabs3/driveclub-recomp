// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <vector>
#include <openssl/sha.h>
#include <png.h>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libpng/pngenc.h"
#include "core/libraries/libs.h"

#include "pngenc_error.h"

namespace Libraries::PngEnc {

static bool TraceBadgePixels() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BADGE_PIXELS");
        return value && value[0] == '1';
    }();
    return enabled;
}

static void LogBadgePixels(const OrbisPngEncEncodeParam& param, size_t stride) {
    const size_t row_bytes = size_t{param.image_width} * 4;
    if (param.bit_depth != 8 || stride < row_bytes ||
        row_bytes > param.image_mem_size ||
        param.image_height - 1 > (param.image_mem_size - row_bytes) / stride) {
        LOG_ERROR(Lib_Png, "BADGE_PIXELS source={} outside supported diagnostic layout",
                  static_cast<const void*>(param.image_mem_addr));
        return;
    }
    size_t nonzero_bytes = 0, nonzero_pixels = 0, alpha_nonzero = 0, alpha_opaque = 0;
    unsigned alpha_min = 255, alpha_max = 0;
    for (size_t y = 0; y < param.image_height; ++y) {
        const auto* row = param.image_mem_addr + y * stride;
        for (size_t x = 0; x < row_bytes; x += 4) {
            const unsigned alpha = row[x + 3];
            nonzero_bytes += (row[x] != 0) + (row[x + 1] != 0) + (row[x + 2] != 0) + (alpha != 0);
            nonzero_pixels += (row[x] | row[x + 1] | row[x + 2] | alpha) != 0;
            alpha_nonzero += alpha != 0;
            alpha_opaque += alpha == 255;
            alpha_min = std::min(alpha_min, alpha);
            alpha_max = std::max(alpha_max, alpha);
        }
    }
    LOG_ERROR(Lib_Png,
              "BADGE_PIXELS source={} destination={} {}x{} pitch={} stride={} format={} "
              "nonzeroBytes={} nonzeroPixels={} alphaNonzero={} alphaOpaque={} alphaMin={} alphaMax={}",
              static_cast<const void*>(param.image_mem_addr), static_cast<void*>(param.png_mem_addr),
              param.image_width, param.image_height, param.image_pitch, stride,
              static_cast<unsigned>(param.pixel_format), nonzero_bytes, nonzero_pixels, alpha_nonzero,
              alpha_opaque, alpha_min, alpha_max);
}

struct PngHandler {
    png_structp png_ptr;
    png_infop info_ptr;
};

struct PngWriter {
    u8* cursor;
    u8* start;
    size_t capacity;
    bool cancel_write;
};

static inline int MapPngFilter(u16 filter) {
    if (filter == (u16)OrbisPngEncFilterType::All) {
        return PNG_ALL_FILTERS;
    }

    int f = 0;

    if (filter & (u16)OrbisPngEncFilterType::None)
        f |= PNG_FILTER_NONE;
    if (filter & (u16)OrbisPngEncFilterType::Sub)
        f |= PNG_FILTER_SUB;
    if (filter & (u16)OrbisPngEncFilterType::Up)
        f |= PNG_FILTER_UP;
    if (filter & (u16)OrbisPngEncFilterType::Average)
        f |= PNG_FILTER_AVG;
    if (filter & (u16)OrbisPngEncFilterType::Paeth)
        f |= PNG_FILTER_PAETH;

    return f;
}

void PngWriteFn(png_structp png_ptr, png_bytep data, size_t length) {
    PngWriter* ctx = (PngWriter*)png_get_io_ptr(png_ptr);

    if ((size_t)(ctx->cursor - ctx->start) + length > ctx->capacity) {
        LOG_ERROR(Lib_Png, "PNG output buffer too small");
        ctx->cancel_write = true;
        return;
    }

    memcpy(ctx->cursor, data, length);
    ctx->cursor += length;
}

void PngFlushFn(png_structp png_ptr) {}

void PngEncError(png_structp png_ptr, png_const_charp error_message) {
    LOG_ERROR(Lib_Png, "PNG error {}", error_message);
}

void PngEncWarning(png_structp png_ptr, png_const_charp error_message) {
    LOG_ERROR(Lib_Png, "PNG warning {}", error_message);
}

s32 PS4_SYSV_ABI scePngEncCreate(const OrbisPngEncCreateParam* param, void* memoryAddress,
                                 u32 memorySize, OrbisPngEncHandle* handle) {
    LOG_ERROR(Lib_Png, "BADGE_TRACE Create: param={} mem={} memSize={} handleOut={}", (const void*)param, memoryAddress, memorySize, (void*)handle);
    if (param == nullptr || param->attribute != 0) {
        LOG_ERROR(Lib_Png, "Invalid param");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (memoryAddress == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid memory address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Png, "Invalid Size, width = {}", param->max_image_width);
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    auto pngh = (PngHandler*)memoryAddress;

    pngh->png_ptr =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, PngEncError, PngEncWarning);

    if (pngh->png_ptr == nullptr)
        return ORBIS_PNG_ENC_ERROR_FATAL;

    pngh->info_ptr = png_create_info_struct(pngh->png_ptr);
    if (pngh->info_ptr == nullptr) {
        png_destroy_write_struct(&pngh->png_ptr, nullptr);
        return false;
    }

    LOG_ERROR(Lib_Png, "BADGE_TRACE Create: success handle={}", (void*)pngh);
    *handle = pngh;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngEncDelete(OrbisPngEncHandle handle) {
    LOG_ERROR(Lib_Png, "BADGE_TRACE Delete: handle={}", (void*)handle);
    auto pngh = (PngHandler*)handle;
    png_destroy_write_struct(&pngh->png_ptr, &pngh->info_ptr);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngEncEncode(OrbisPngEncHandle handle, const OrbisPngEncEncodeParam* param,
                                 OrbisPngEncOutputInfo* outputInfo) {
    LOG_ERROR(Lib_Png, "BADGE_TRACE Encode: handle={} param={} outputInfo={}", (void*)handle, (const void*)param, (void*)outputInfo);

    if (handle == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid handle");
        return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    }

    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid param");
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    }

    LOG_ERROR(Lib_Png, "BADGE_TRACE Encode: image={}x{} pitch={} imageBytes={} pngCapacity={} format={} colorSpace={} bitDepth={}", param->image_width, param->image_height, param->image_pitch, param->image_mem_size, param->png_mem_size, (unsigned)param->pixel_format, (unsigned)param->color_space, param->bit_depth);
    if (param->image_mem_addr == nullptr || param->png_mem_addr == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid input or output address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (param->png_mem_size == 0 || param->image_mem_size == 0 || param->image_height == 0 ||
        param->image_width == 0) {
        LOG_ERROR(Lib_Png, "Invalid Size");
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    const bool trace_badge = TraceBadgePixels();
    if (trace_badge) {
        const size_t stride = param->color_space == OrbisPngEncColorSpace::RGBA
                                  ? size_t{param->image_width} * 4 : param->image_pitch;
        LogBadgePixels(*param, stride);
    }
    auto pngh = (PngHandler*)handle;

    if (setjmp(png_jmpbuf(pngh->png_ptr))) {
        LOG_ERROR(Lib_Png, "LibPNG aborted encode");
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }

    int png_color_type = PNG_COLOR_TYPE_RGB;

    if (param->color_space == OrbisPngEncColorSpace::RGBA) {
        png_color_type |= PNG_COLOR_MASK_ALPHA;
    }

    int png_interlace_type = PNG_INTERLACE_NONE;
    int png_compression_type = PNG_COMPRESSION_TYPE_DEFAULT;
    int png_filter_method = PNG_FILTER_TYPE_DEFAULT;

    PngWriter writer{};
    writer.cursor = param->png_mem_addr;
    writer.start = param->png_mem_addr;
    writer.capacity = param->png_mem_size;

    png_set_write_fn(pngh->png_ptr, &writer, PngWriteFn, PngFlushFn);

    png_set_IHDR(pngh->png_ptr, pngh->info_ptr, param->image_width, param->image_height,
                 param->bit_depth, png_color_type, png_interlace_type, png_compression_type,
                 png_filter_method);

    if (param->pixel_format == OrbisPngEncPixelFormat::B8G8R8A8) {
        png_set_bgr(pngh->png_ptr);
    }

    png_set_compression_level(pngh->png_ptr, std::clamp<u16>(param->compression_level, 0, 9));
    png_set_filter(pngh->png_ptr, 0, MapPngFilter(param->filter_type));

    png_write_info(pngh->png_ptr, pngh->info_ptr);

    int channels = 4;
    size_t row_stride = param->image_width * channels;

    uint32_t processed_height = 0;

    if (param->color_space == OrbisPngEncColorSpace::RGBA) {
        for (; processed_height < param->image_height; ++processed_height) {
            png_bytep row = (png_bytep)param->image_mem_addr + processed_height * row_stride;
            png_write_row(pngh->png_ptr, row);

            if (outputInfo != nullptr) {
                outputInfo->processed_height = processed_height;
            }

            if (writer.cancel_write) {
                LOG_ERROR(Lib_Png, "Ran out of room to write PNG");
                return ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
            }
        }
    } else {
        // our input data is always rgba but when outputting without an alpha channel, libpng
        // expects the input to not have alpha either, i couldn't find a way around this easily?
        // png_strip_alpha is for reading and set_background wasn't working, this seems fine...?
        std::vector<uint8_t> rgb_row(param->image_width * 3);

        for (; processed_height < param->image_height; ++processed_height) {
            const unsigned char* src =
                param->image_mem_addr + processed_height * param->image_pitch;

            uint8_t* dst = rgb_row.data();

            for (uint32_t x = 0; x < param->image_width; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                src += 4; // skip reading alpha channel
                dst += 3;
            }

            png_write_row(pngh->png_ptr, rgb_row.data());

            if (outputInfo != nullptr) {
                outputInfo->processed_height = processed_height;
            }

            if (writer.cancel_write) {
                LOG_ERROR(Lib_Png, "Ran out of room to write PNG");
                return ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
            }
        }
    }

    png_write_flush(pngh->png_ptr);

    png_write_end(pngh->png_ptr, pngh->info_ptr);
    if (writer.cancel_write) {
        return ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
    }
    if (trace_badge) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(writer.start, writer.cursor - writer.start, digest);
        std::string hash;
        for (const auto byte : digest) {
            hash += fmt::format("{:02x}", byte);
        }
        LOG_ERROR(Lib_Png, "BADGE_PIXELS encoded {}x{} sha256={}",
                  param->image_width, param->image_height, hash);
    }

    if (outputInfo != nullptr) {
        outputInfo->data_size = writer.cursor - writer.start;
        outputInfo->processed_height = processed_height;
    }

    LOG_ERROR(Lib_Png, "BADGE_TRACE Encode: success bytes={}", (size_t)(writer.cursor - writer.start));
    return writer.cursor - writer.start;
}

s32 PS4_SYSV_ABI scePngEncQueryMemorySize(const OrbisPngEncCreateParam* param) {
    LOG_ERROR(Lib_Png, "BADGE_TRACE QueryMemorySize: param={}", (const void*)param);
    if (param == nullptr) {
        LOG_ERROR(Lib_Png, "Invalid Address");
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    }

    if (param->attribute != 0 || param->max_filter_number > 5) {
        LOG_ERROR(Lib_Png, "Invalid Param, attribute = {}, max_filter_number = {}",
                  param->attribute, param->max_filter_number);
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    }

    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Png, "Invalid Size, width = {}", param->max_image_width);
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    }

    LOG_ERROR(Lib_Png, "BADGE_TRACE QueryMemorySize: structSize={} attribute={} width={} filters={} result={}", param->this_size, param->attribute, param->max_image_width, param->max_filter_number, (size_t)sizeof(PngHandler));
    return sizeof(PngHandler);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("7aGTPfrqT9s", "libScePngEnc", 1, "libScePngEnc", scePngEncCreate);
    LIB_FUNCTION("RUrWdwTWZy8", "libScePngEnc", 1, "libScePngEnc", scePngEncDelete);
    LIB_FUNCTION("xgDjJKpcyHo", "libScePngEnc", 1, "libScePngEnc", scePngEncEncode);
    LIB_FUNCTION("9030RnBDoh4", "libScePngEnc", 1, "libScePngEnc", scePngEncQueryMemorySize);
};

} // namespace Libraries::PngEnc
