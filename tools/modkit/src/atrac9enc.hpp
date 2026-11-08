// SPDX-License-Identifier: GPL-2.0-or-later
// An ATRAC9 encoder for the sound banks of the game. It writes what the console codec's decoder (LibAtrac9, MIT licence,
// third_party/LibAtrac9) reads: mono blocks only (a stereo stream is two mono blocks per frame), 48 kHz, scale factors
// with a fixed bit length, a constant bit allocation gradient and Huffman coded spectra, a frame budget taken from the
// stream's own configuration. The decoder's own tables and checks are used, so what is written decodes.
#pragma once

#include "common.hpp"

namespace dcmod {

struct At9Config {
    uint8_t data[4] = {0, 0, 0, 0};
    unsigned sample_rate_index = 0, channel_config = 0, frame_bytes = 0, superframe_index = 0;
    unsigned channels = 0, blocks = 0, frames_per_superframe = 0, superframe_bytes = 0, frame_samples = 0, sample_rate = 0;
    static At9Config Parse(const uint8_t* config4);  // throws Error for a configuration this encoder cannot write
};

// PCM channels (one vector of samples per channel at the stream's rate) to a sequence of superframes. The result is a whole
// number of superframes; `samples` receives the number of samples per channel the decoder will produce for it.
Bytes At9Encode(const At9Config& config, const std::vector<std::vector<float>>& pcm, size_t* samples);

// A sequence of superframes (the part of a stream that holds audio) to PCM channels of 16 bit range floats.
std::vector<std::vector<float>> At9Decode(const At9Config& config, const uint8_t* data, size_t size);

}  // namespace dcmod
