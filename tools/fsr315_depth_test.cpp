#define fsr3UpscalerGetPermutationBlobByIndex ffxVk315Fsr3UpscalerGetPermutationBlobByIndex
#include "ffx_fsr3upscaler.h"
#include "ffx_fsr3upscaler_shaderblobs.h"
#include <spirv/unified1/spirv.hpp>
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
  for (unsigned inverted : {0u, 16u}) {
    for (unsigned pass = 0; pass <= 10; ++pass) {
      FfxShaderBlob blob{};
      assert(fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass(pass), inverted, &blob) == FFX_OK);
      assert(blob.data && blob.size % 4 == 0);
      const auto* words = reinterpret_cast<const uint32_t*>(blob.data);
      assert(words[0] == spv::MagicNumber);
      if (pass != FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS) continue;
      unsigned minimums = 0, maximums = 0;
      for (size_t offset = 5; offset < blob.size / 4;) {
        const unsigned size = words[offset] >> 16;
        assert(size && offset + size <= blob.size / 4);
        minimums += (words[offset] & 0xffff) == spv::OpAtomicUMin;
        maximums += (words[offset] & 0xffff) == spv::OpAtomicUMax;
        offset += size;
      }
      assert(minimums == (inverted ? 0u : 1u));
      assert(maximums == (inverted ? 1u : 0u));
    }
  }
  puts("PASS: SDK selects all 22 depth permutations; nearest-depth atomics match standard/reversed Z.");
}
