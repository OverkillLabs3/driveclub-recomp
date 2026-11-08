# Repeat sampler regression test

`test_repeat_sampling.py` extracts the production `ApplyGlobalRepeat` function
and compiles it in `repeat_sampling_fixture.cpp`, a small Sirit adapter. It uses
synthetic shader inputs and descriptors, with no game assets or GPU execution.

Requirements: Python 3.10 or later (standard library only), a GCC/Clang-style
C++23 compiler, a compatible build of the source tree's Sirit static library,
and SPIRV-Tools `spirv-val` and `spirv-dis`. The compiler and library must use
compatible ABIs and C++ standard libraries. No dependencies are installed.

For example, from the repository root:

```sh
cmake -S gpu/third_party/sirit -B out/repeat-sirit -DSIRIT_TESTS=OFF
cmake --build out/repeat-sirit
python tools/test_repeat_sampling.py --build-dir out/repeat-sirit
```

Use `--sirit-lib` to select an existing library explicitly. `--source-root`
selects another checkout. `--cxx`, `--spirv-val` and `--spirv-dis` select tools;
the compiler defaults to `CXX`, then `clang++`. Additional compiler/linker flags
use repeated `--cxx-flag=-flag`. `--output-dir` retains generated source, modules,
disassembly, logs and the JSON receipt; otherwise those files are temporary.
Run without Python `-O`, since regression checks require assertions.

The 32 modules cover SPIR-V 1.3/Vulkan 1.1 and SPIR-V 1.4/Vulkan 1.2, 2D/array
textures, implicit/provided gradients with bias, and four Wrap/Mirror axis-mode
combinations. Each invokes the actual helper twice sequentially. SPIR-V
validation checks type/control-flow correctness. Additional emitted CFG and
dataflow checks require four normalized power8-weighted Grad reads per helper,
zero-amount bypass, original alpha, external gradients and array-layer handling.
They reject sampling loops, a coarsest-mip mean fetch and inverse-sqrt gain.
Five deliberate regressions exercise the gain, mean-read, alpha, normalization
and weight-power checks.

This verifies the production emitter's sampling contract. It does not execute
shaders or prove perceptual sharpness, absence of all phase-dependent contrast,
live material bindings, temporal reconstruction quality or GPU performance.
