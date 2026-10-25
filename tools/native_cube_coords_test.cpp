#include "../gpu/shadps4/shader_recompiler/backend/spirv/native_cube_coords.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
struct Value {
    std::array<float, 4> v{};
};
struct Eval {
    int F32[5]{}, U1[2]{};
    Value ConstF32(float f) { return {{f, 0, 0, 0}}; }
    Value OpCompositeExtract(int, Value a, unsigned i) { return ConstF32(a.v[i]); }
    Value OpFSub(int, Value a, Value b) { return ConstF32(a.v[0] - b.v[0]); }
    Value OpFMul(int, Value a, Value b) { return ConstF32(a.v[0] * b.v[0]); }
    Value OpFDiv(int, Value a, Value b) { return ConstF32(a.v[0] / b.v[0]); }
    Value OpFloor(int, Value a) { return ConstF32(std::floor(a.v[0])); }
    Value OpFNegate(int, Value a) { return ConstF32(-a.v[0]); }
    Value OpFOrdEqual(int, Value a, Value b) { return ConstF32(a.v[0] == b.v[0]); }
    Value OpSelect(int, Value c, Value a, Value b) { return c.v[0] ? a : b; }
    Value OpCompositeConstruct(int, Value x, Value y, Value z, Value w) {
        return {{x.v[0], y.v[0], z.v[0], w.v[0]}};
    }
};
int main() {
    // Independent face normal/right/down bases, rather than duplicate branch
    // code.
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float r[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    const float d[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    const float uv[] = {0.f, 1e-6f, .1f, .25f, .5f, .75f, .9f, .999999f, 1.f};
    Eval e;
    unsigned cases = 0;
    for (unsigned cube : {0u, 1u, 7u, 20u, 100u})
        for (unsigned face = 0; face < 6; ++face)
            for (float u : uv)
                for (float v : uv) {
                    Value input{{u, v, float(cube * 6 + face), 0}};
                    auto out = Shader::Backend::SPIRV::NativeCubeCoordinates(e, input);
                    assert(out.v[3] == cube);
                    for (unsigned i = 0; i < 3; ++i) {
                        float want =
                            n[face][i] + (2 * u - 1) * r[face][i] + (2 * v - 1) * d[face][i];
                        assert(std::abs(out.v[i] - want) < 1e-6f);
                    }
                    ++cases;
                }
    std::printf("PASS: actual SPIR-V coordinate helper evaluates to independent "
                "cube-face bases for %u edge/interior/cube-array cases.\n",
                cases);
}
