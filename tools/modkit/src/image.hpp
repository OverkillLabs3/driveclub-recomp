// SPDX-License-Identifier: GPL-2.0-or-later
// PNG files in, block compressed textures out, so that a picture made in any editor can become a game texture.
#pragma once

#include "tex.hpp"

namespace dcmod {

struct Image {
    unsigned width = 0, height = 0;
    Bytes rgba;  // 4 bytes per pixel
};

bool IsPng(const Bytes& b);
Image LoadPng(const Bytes& file);  // 8 and 16 bit grey, RGB, palette and RGBA, not interlaced; throws Error
Image HalfSize(const Image& image);  // box filter

// A DDS with every mip level of `pb` made from the picture (which must have the texture's size). Supported formats:
// BC1 (DXT1), BC7, and plain 8 bit RGBA/BGRA.
Dds ImageToDds(const PixelBuffer& pb, const Image& image);

// A DDS file or a PNG picture, as a DDS that fits the texture.
Dds TextureSource(const PixelBuffer& pb, const Bytes& file);

}  // namespace dcmod
