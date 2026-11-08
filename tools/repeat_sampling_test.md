# Repeat sampler regression test

`test_repeat_sampling.py` extracts `ApplyGlobalRepeat` directly from the source
tree and compiles it inside `repeat_sampling_fixture.cpp`, a small Sirit adapter.
The adapter supplies synthetic shader inputs and descriptors. It does not contain
game shaders, textures, material lists or other game data, and never starts a GPU.

Requirements: Python 3.10 or later (standard library only), a GCC/Clang-style C++23
compiler, the source tree's Sirit static library built with a compatible compiler,
and SPIRV-Tools `spirv-val` and `spirv-dis`. The compiler and library must use
compatible ABIs and C++ standard libraries. The test does not install dependencies.

For example, from the repository root, with CMake and the dependencies available:

```sh
cmake -S gpu/third_party/sirit -B out/repeat-sirit -DSIRIT_TESTS=OFF
cmake --build out/repeat-sirit
python tools/test_repeat_sampling.py --build-dir out/repeat-sirit
```

Use `--source-root` to test another checkout and `--sirit-lib` to select an
existing library explicitly. Tool paths can be passed with `--cxx`, `--spirv-val`
and `--spirv-dis`. The compiler defaults to `CXX`, then `clang++`; additional
compiler/linker options use repeated `--cxx-flag=-flag`. `--output-dir` retains the
generated source, modules, disassembly, logs and JSON receipt for inspection;
otherwise they are temporary.

The 32 modules cover SPIR-V 1.3/Vulkan 1.1 and SPIR-V 1.4/Vulkan 1.2, 2D and
2D-array sampling, implicit and provided gradients (with guest bias), and all
four combinations of Wrap/Mirror axes. Every module invokes the actual helper
twice sequentially, checking control-flow integration after its first merge.

The validator checks SPIR-V validity. Additional checks follow the emitted CFG
and dataflow: complete shifted 4x4 cell support, dynamic inverse-Poisson counts
using a common0/1 path and a bounded rare-tail tree, compact C1 weights,
positive-weight guarded reads, weighted normalization and transparency
recurrences, empty/zero fallbacks, original alpha, external derivatives, array
layers and version-specific private interfaces. Count thresholds are recomputed
independently with high-precision Decimal arithmetic; the emitted count CFG is
executed at interval boundaries and additional integer inputs. Catalogue checks
follow the shifted lower floor using upward-rounded sqrt(2), retaining complete
support with the original radius and absolute site keys. Modules for different
Wrap/Mirror combinations must be identical, so same-UV colour/normal pairs
receive the same numeric translations. The old fixed lattice/gain kernel fails
these control-flow and sampling-contract checks. Deliberate mutations of radius,
catalogue coverage, normalization, gain, alpha and sampler period must also fail.

These are structural regression checks of the production emitter, not a second
copy of the sampler implementation or a statistical randomness proof. They do
not execute the shaders, measure performance, or qualify visual grain retention,
anisotropic filtering, lighting or temporal reconstruction. Runtime visual and
performance checks remain separate.
