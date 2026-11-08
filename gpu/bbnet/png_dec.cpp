// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: libScePngDec on libpng. DriveClub decodes downloaded pictures with it (avatars of players
// and friends); without it the game retries the picture forever and shows a spinning ring.

#include <cstring>

#include <png.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::PngDec {

namespace {

constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_ADDR = 0x80690001;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_SIZE = 0x80690002;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_DATA = 0x80690010;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_HANDLE = 0x80690012;

enum ColorSpace : u16 { Grayscale = 2, Rgb = 3, Clut = 4, GrayscaleAlpha = 18, Rgba = 19 };
enum PixelFormat : u16 { R8G8B8A8 = 0, B8G8R8A8 = 1 };

struct ParseParam {
    const void* png_mem_addr;
    u32 png_mem_size;
    u32 reserved;
};

struct ImageInfo {
    u32 image_width;
    u32 image_height;
    u16 color_space;
    u16 bit_depth;
    u32 image_flag;
};

struct CreateParam {
    u32 this_size;
    u32 attribute;
    u32 max_image_width;
};

struct DecodeParam {
    const void* png_mem_addr;
    void* image_mem_addr;
    u32 png_mem_size;
    u32 image_mem_size;
    u16 pixel_format;
    u16 alpha_value;
    u32 image_pitch;
};

// The handle points into the memory the game hands to scePngDecCreate.
constexpr u32 HandleMagic = 0x504e4744; // "PNGD"
constexpr u32 HandleMemory = 64;

bool ReadHeader(const void* data, u32 size, png_image& image) {
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    return data && size && png_image_begin_read_from_memory(&image, data, size) != 0;
}

void FillInfo(const png_image& image, ImageInfo* info) {
    if (!info) {
        return;
    }
    const bool color = image.format & PNG_FORMAT_FLAG_COLOR;
    const bool alpha = image.format & PNG_FORMAT_FLAG_ALPHA;
    info->image_width = image.width;
    info->image_height = image.height;
    info->color_space = image.format & PNG_FORMAT_FLAG_COLORMAP ? Clut
                        : color ? (alpha ? Rgba : Rgb)
                                : (alpha ? GrayscaleAlpha : Grayscale);
    info->bit_depth = image.format & PNG_FORMAT_FLAG_LINEAR ? 16 : 8;
    info->image_flag = 0;
}

} // namespace

s32 PS4_SYSV_ABI scePngDecParseHeader(const ParseParam* param, ImageInfo* info) {
    if (!param || !info) {
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    png_image image;
    if (!ReadHeader(param->png_mem_addr, param->png_mem_size, image)) {
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }
    FillInfo(image, info);
    png_image_free(&image);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecQueryMemorySize(const CreateParam* param) {
    if (!param) {
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    return HandleMemory;
}

s32 PS4_SYSV_ABI scePngDecCreate(const CreateParam* param, void* memory, u32 memory_size,
                                 void** handle) {
    if (!param || !memory || !handle) {
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    if (memory_size < sizeof(u32)) {
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    }
    *static_cast<u32*>(memory) = HandleMagic;
    *handle = memory;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecDelete(void* handle) {
    if (!handle || *static_cast<u32*>(handle) != HandleMagic) {
        return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    }
    *static_cast<u32*>(handle) = 0;
    return ORBIS_OK;
}

// Decodes to 8-bit RGBA or BGRA rows of image_pitch bytes (0: width * 4). Pictures without alpha
// come out opaque. shortcut: alpha_value (the alpha for such pictures) is taken as 0xff.
s32 PS4_SYSV_ABI scePngDecDecode(void* handle, const DecodeParam* param, ImageInfo* info) {
    if (!handle || *static_cast<u32*>(handle) != HandleMagic) {
        return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    }
    if (!param || !param->image_mem_addr) {
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    }
    png_image image;
    if (!ReadHeader(param->png_mem_addr, param->png_mem_size, image)) {
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }
    FillInfo(image, info);
    const u32 pitch = param->image_pitch ? param->image_pitch : image.width * 4;
    if (u64(pitch) * image.height > param->image_mem_size) {
        png_image_free(&image);
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    }
    image.format = param->pixel_format == B8G8R8A8 ? PNG_FORMAT_BGRA : PNG_FORMAT_RGBA;
    if (!png_image_finish_read(&image, nullptr, param->image_mem_addr, int(pitch), nullptr)) {
        LOG_WARNING(Lib_Net, "PNG decode failed: {}", image.message);
        png_image_free(&image);
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("U6h4e5JRPaQ", "libScePngDec", 1, "libScePngDec", scePngDecParseHeader);
    LIB_FUNCTION("-6srIGbLTIU", "libScePngDec", 1, "libScePngDec", scePngDecQueryMemorySize);
    LIB_FUNCTION("m0uW+8pFyaw", "libScePngDec", 1, "libScePngDec", scePngDecCreate);
    LIB_FUNCTION("QbD+eENEwo8", "libScePngDec", 1, "libScePngDec", scePngDecDelete);
    LIB_FUNCTION("WC216DD3El4", "libScePngDec", 1, "libScePngDec", scePngDecDecode);
}

} // namespace Libraries::PngDec
