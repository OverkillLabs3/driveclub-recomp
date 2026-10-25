// GCN cube instructions already produced face-local UV and a 6-face array
// index. Invert that projection for a Vulkan cube-array view; hardware then
// filters neighboring faces without changing the game's LOD, sampler or
// lighting math.
#pragma once

namespace Shader::Backend::SPIRV {
template <class Context, class Id> Id NativeCubeCoordinates(Context& ctx, Id coords) {
    const auto f = ctx.F32[1];
    const Id one = ctx.ConstF32(1.f);
    const Id minus_one = ctx.ConstF32(-1.f);
    const Id u =
        ctx.OpFSub(f, ctx.OpFMul(f, ctx.ConstF32(2.f), ctx.OpCompositeExtract(f, coords, 0)), one);
    const Id v =
        ctx.OpFSub(f, ctx.OpFMul(f, ctx.ConstF32(2.f), ctx.OpCompositeExtract(f, coords, 1)), one);
    const Id layer = ctx.OpCompositeExtract(f, coords, 2);
    const Id cube = ctx.OpFloor(f, ctx.OpFDiv(f, layer, ctx.ConstF32(6.f)));
    const Id face = ctx.OpFSub(f, layer, ctx.OpFMul(f, cube, ctx.ConstF32(6.f)));
    const Id nu = ctx.OpFNegate(f, u);
    const Id nv = ctx.OpFNegate(f, v);
    const auto pick = [&](unsigned index, Id yes, Id no) {
        return ctx.OpSelect(f, ctx.OpFOrdEqual(ctx.U1[1], face, ctx.ConstF32(float(index))), yes,
                            no);
    };
    const Id x = pick(0, one, pick(1, minus_one, pick(5, nu, u)));
    const Id y = pick(2, one, pick(3, minus_one, nv));
    const Id z = pick(0, nu, pick(1, u, pick(2, v, pick(3, nv, pick(4, one, minus_one)))));
    return ctx.OpCompositeConstruct(ctx.F32[4], x, y, z, cube);
}
} // namespace Shader::Backend::SPIRV
