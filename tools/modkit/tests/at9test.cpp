// Test program for the ATRAC9 encoder (not part of the kit): encodes a test signal and measures what the decoder gives back.
#include "../src/atrac9enc.hpp"
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
using namespace dcmod;
int main() {
    const uint8_t cfg_bytes[4] = {0xFE, 0x72, 0x0D, 0xF0};
    At9Config cfg = At9Config::Parse(cfg_bytes);
    const size_t n = 48000 * 3;
    std::vector<std::vector<float>> pcm(2, std::vector<float>(n));
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / 48000;
        pcm[0][i] = float(8000 * std::sin(2 * M_PI * 440 * t) + 3000 * std::sin(2 * M_PI * 3000 * t) + 1000 * std::sin(2 * M_PI * 9000 * t));
        pcm[1][i] = float(6000 * std::sin(2 * M_PI * 660 * t) + 2000 * std::sin(2 * M_PI * 5000 * t * (1 + 0.1 * std::sin(t))));
    }
    size_t samples = 0;
    const Bytes enc = At9Encode(cfg, pcm, &samples);
    std::printf("encoded %zu bytes (%zu superframes) for %zu samples\n", enc.size(), enc.size() / cfg.superframe_bytes, samples);
    const auto dec = At9Decode(cfg, enc.data(), enc.size());
    for (int c = 0; c < 2; ++c) {
        double best = -1e9;
        int bestd = 0;
        for (int d = 0; d <= 1024; ++d) {
            double s = 0, e = 0;
            for (size_t i = 2000; i + 2000 < n && i + d < dec[c].size(); ++i) {
                const double a = pcm[c][i], b = dec[c][i + d];
                s += a * a, e += (a - b) * (a - b);
            }
            const double snr = 10 * std::log10(s / (e + 1e-9));
            if (snr > best) best = snr, bestd = d;
        }
        std::printf("channel %d: best delay %d samples, SNR %.1f dB\n", c, bestd, best);
    }
    std::puts("ok ATRAC9 encoder");
    return 0;
}
