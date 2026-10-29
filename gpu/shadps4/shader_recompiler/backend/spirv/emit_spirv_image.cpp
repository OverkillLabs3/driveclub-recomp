// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>
#include "bbport_settings.h"
#include "world_repeat_policy.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/native_cube_coords.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

struct ImageOperands {
    void Add(spv::ImageOperandsMask new_mask, Id value) {
        if (!Sirit::ValidId(value)) {
            return;
        }
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value);
    }
    void Add(spv::ImageOperandsMask new_mask, Id value1, Id value2) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value1);
        operands.push_back(value2);
    }

    void AddOffset(EmitContext& ctx, const IR::Value& offset,
                   bool can_use_runtime_offsets = false) {
        if (offset.IsEmpty()) {
            return;
        }
        if (offset.IsImmediate()) {
            const s32 operand = offset.U32();
            Add(spv::ImageOperandsMask::ConstOffset, ctx.ConstS32(operand));
            return;
        }
        IR::Inst* const inst{offset.Inst()};
        if (inst->AreAllArgsImmediates()) {
            switch (inst->GetOpcode()) {
            case IR::Opcode::CompositeConstructU32x2:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32())));
                return;
            case IR::Opcode::CompositeConstructU32x3:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32()),
                                 static_cast<s32>(inst->Arg(2).U32())));
                return;
            default:
                break;
            }
        }
        if (can_use_runtime_offsets) {
            Add(spv::ImageOperandsMask::Offset, ctx.Def(offset));
        } else {
            LOG_WARNING(Render_Vulkan,
                        "Runtime offset provided to unsupported image sample instruction");
        }
    }

    void AddDerivatives(EmitContext& ctx, Id derivatives_dx, Id derivatives_dy) {
        if (!Sirit::ValidId(derivatives_dx) || !Sirit::ValidId(derivatives_dy)) {
            return;
        }
        Add(spv::ImageOperandsMask::Grad, derivatives_dx, derivatives_dy);
    }

    spv::ImageOperandsMask mask{};
    boost::container::static_vector<Id, 4> operands;
};

Id EmitImageHandle(EmitContext& ctx, Id, Id) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleRaw(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address1, Id address2,
                      Id address3, Id address4) {
    UNREACHABLE_MSG("Unreachable instruction");
}

// Startup-only setting: the shader cache separates the enabled and disabled
// modes.
static bool GlobalRepeatEnabled() { return BbSettings::Get().startup_texture_repetition; }

void PrepareWorldRepeat(const IR::Program& program) {
    WorldRepeat::selected.clear();
    if (GlobalRepeatEnabled())
        WorldRepeat::Prepare(program);
}

static Id ApplyGlobalRepeat(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id shader_bias,
                            const IR::Value& offset, Id original, Id provided_dx = {},
                            Id provided_dy = {}) {
    if (!GlobalRepeatEnabled() || !WorldRepeat::selected.contains(inst) ||
        ctx.sw_stage != SwStage::Fragment || !offset.IsEmpty())
        return original;
    const auto& tex = ctx.images[handle & 0xffff];
    if (tex.is_integer || tex.is_storage || tex.native_cube ||
        (tex.view_type != AmdGpu::ImageType::Color2D &&
         tex.view_type != AmdGpu::ImageType::Color2DArray))
        return original;
    const auto& ir_image = ctx.info.images[handle & 0xffff];
    const auto sharp = ir_image.GetSharp(ctx.info);
    const auto& ir_sampler = ctx.info.samplers[handle >> 16];
    const auto samp = ir_sampler.GetSharp(ctx.info);
    if (ir_sampler.is_depth || samp.force_unnormalized || sharp.NumLevels() <= 1)
        return original;
    const auto wraps = [](auto mode) {
        return mode == AmdGpu::ClampMode::Wrap || mode == AmdGpu::ClampMode::Mirror;
    };
    const bool rx = wraps(samp.clamp_x), ry = wraps(samp.clamp_y);
    if (!rx && !ry)
        return original;
    const auto F = ctx.F32[1], V = ctx.F32[2], C = ctx.F32[4], U = ctx.U32[1];
    const auto f = [&](float x) { return ctx.ConstF32(x); };
    const auto v = [&](Id x, Id y) { return ctx.OpCompositeConstruct(V, x, y); };
    const auto splat = [&](Id x) { return ctx.OpCompositeConstruct(C, x, x, x, x); };
    const Id uv = ctx.OpVectorShuffle(V, coords, coords, 0, 1);
    Id dx = Sirit::ValidId(provided_dx) ? provided_dx : ctx.OpDPdx(V, uv);
    Id dy = Sirit::ValidId(provided_dy) ? provided_dy : ctx.OpDPdy(V, uv);
    const Id footprint = ctx.OpFMax(F, ctx.OpLength(F, dx), ctx.OpLength(F, dy));
    Id amount = ctx.OpFClamp(
        F,
        ctx.OpFMul(F, ctx.OpFAdd(F, ctx.OpLog2(F, ctx.OpFMax(F, footprint, f(1e-12f))), f(9)),
                   f(.25f)),
        f(0), f(1));
    amount = ctx.OpFMul(F, ctx.OpFMul(F, amount, amount),
                        ctx.OpFSub(F, f(3), ctx.OpFMul(F, f(2), amount)));
    // Bias supplied by the guest instruction belongs in the footprint too.
    if (Sirit::ValidId(shader_bias)) {
        const Id scale = ctx.OpExp2(F, shader_bias);
        dx = ctx.OpVectorTimesScalar(V, dx, scale);
        dy = ctx.OpVectorTimesScalar(V, dy, scale);
    }
    const Id cell = ctx.OpFloor(V, uv), frac = ctx.OpFSub(V, uv, cell);
    const auto component = [&](Id x, u32 i) { return ctx.OpCompositeExtract(F, x, i); };
    const auto smooth = [&](Id x) {
        return ctx.OpFMul(F, ctx.OpFMul(F, x, x), ctx.OpFSub(F, f(3), ctx.OpFMul(F, f(2), x)));
    };
    const Id fx = rx ? smooth(component(frac, 0)) : f(0),
             fy = ry ? smooth(component(frac, 1)) : f(0);
    const Id ax = ctx.OpFSub(F, f(1), fx), ay = ctx.OpFSub(F, f(1), fy);
    std::array<Id, 4> weights{ctx.OpFMul(F, ax, ay), ctx.OpFMul(F, fx, ay), ctx.OpFMul(F, ax, fy),
                              ctx.OpFMul(F, fx, fy)};
    Id sum = f(0);
    for (auto& weight : weights) {
        weight = ctx.OpPow(F, weight, f(8));
        sum = ctx.OpFAdd(F, sum, weight);
    }
    const Id image = ctx.OpLoad(tex.image_type, tex.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id combined = ctx.OpSampledImage(tex.sampled_type, image, sampler);
    Id blended = ctx.ConstantNull(C), square_sum = f(0);
    for (u32 i = 0; i < 4; ++i) {
        const Id cx = rx ? ctx.OpFAdd(F, component(cell, 0), f(float(i & 1))) : f(0);
        const Id cy = ry ? ctx.OpFAdd(F, component(cell, 1), f(float(i >> 1))) : f(0);
        const Id ix = ctx.OpBitcast(U, ctx.OpConvertFToS(ctx.S32[1], cx));
        const Id iy = ctx.OpBitcast(U, ctx.OpConvertFToS(ctx.S32[1], cy));
        Id h = ctx.OpIAdd(U,
                          ctx.OpIAdd(U, ctx.OpIMul(U, ix, ctx.ConstU32(1664525u)),
                                     ctx.OpIMul(U, iy, ctx.ConstU32(1013904223u))),
                          ctx.ConstU32(2246822519u));
        const auto mix = [&](Id h, u32 shift, u32 mul) {
            return ctx.OpIMul(
                U, ctx.OpBitwiseXor(U, h, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(shift))),
                ctx.ConstU32(mul));
        };
        h = mix(h, 16, 2246822519u);
        h = mix(h, 13, 3266489917u);
        h = ctx.OpBitwiseXor(U, h, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(16u)));
        const Id ox =
            rx ? ctx.OpFMul(F, ctx.OpConvertUToF(F, ctx.OpBitwiseAnd(U, h, ctx.ConstU32(65535u))),
                            f((samp.clamp_x == AmdGpu::ClampMode::Mirror ? 2.f : 1.f) / 65536.f))
               : f(0);
        const Id oy =
            ry ? ctx.OpFMul(F,
                            ctx.OpConvertUToF(F, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(16u))),
                            f((samp.clamp_y == AmdGpu::ClampMode::Mirror ? 2.f : 1.f) / 65536.f))
               : f(0);
        const Id shifted = ctx.OpFAdd(V, uv, v(ox, oy));
        const Id sample_coords =
            tex.view_type == AmdGpu::ImageType::Color2DArray
                ? ctx.OpCompositeConstruct(ctx.F32[3], component(shifted, 0), component(shifted, 1),
                                           ctx.OpCompositeExtract(F, coords, 2))
                : shifted;
        const Id sample = ctx.OpImageSampleExplicitLod(C, combined, sample_coords,
                                                       spv::ImageOperandsMask::Grad, dx, dy);
        const Id weight = ctx.OpFDiv(F, weights[i], sum);
        blended = ctx.OpFAdd(C, blended, ctx.OpVectorTimesScalar(C, sample, weight));
        square_sum = ctx.OpFAdd(F, square_sum, ctx.OpFMul(F, weight, weight));
    }
    ctx.AddCapability(spv::Capability::ImageQuery);
    const Id levels = ctx.OpImageQueryLevels(U, image);
    const Id lod = ctx.OpConvertUToF(F, ctx.OpISub(U, levels, ctx.ConstU32(1u)));
    const Id centre = tex.view_type == AmdGpu::ImageType::Color2DArray
                          ? ctx.OpCompositeConstruct(ctx.F32[3], f(.5f), f(.5f),
                                                     ctx.OpCompositeExtract(F, coords, 2))
                          : v(f(.5f), f(.5f));
    const Id mean =
        ctx.OpImageSampleExplicitLod(C, combined, centre, spv::ImageOperandsMask::Lod, lod);
    const Id altered = ctx.OpFAdd(
        C, mean,
        ctx.OpVectorTimesScalar(C, ctx.OpFSub(C, blended, mean), ctx.OpInverseSqrt(F, square_sum)));
    Id result = ctx.OpFMix(C, original, altered, splat(amount));
    // Never alter opacity/cutout semantics through the generic colour hook.
    result = ctx.OpCompositeInsert(C, ctx.OpCompositeExtract(F, original, 3), result, 3);
    return result;
}

Id EmitImageSampleImplicitLod(EmitContext &ctx, IR::Inst *inst, u32 handle,
                              Id coords, Id bias, const IR::Value &offset) {
  const auto &texture = ctx.images[handle & 0xFFFF];
  const Id image = ctx.OpLoad(texture.image_type, texture.id);
  const Id result_type = texture.data_types->Get(4);
  const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
  const Id sampled_image =
      ctx.OpSampledImage(texture.sampled_type, image, sampler);
  ImageOperands operands;
  operands.Add(spv::ImageOperandsMask::Bias, bias);
  operands.AddOffset(ctx, offset);
  const Id sample = ctx.OpImageSampleImplicitLod(
      result_type, sampled_image, coords, operands.mask, operands.operands);
  return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample)
                            : ApplyGlobalRepeat(ctx, inst, handle, coords, bias,
                                                offset, sample);
}

Id EmitImageSampleExplicitLod(EmitContext &ctx, IR::Inst *inst, u32 handle,
                              Id coords, Id lod, const IR::Value &offset) {
  const auto &texture = ctx.images[handle & 0xFFFF];
  const Id image = ctx.OpLoad(texture.image_type, texture.id);
  const Id result_type = texture.data_types->Get(4);
  const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
  const Id sampled_image =
      ctx.OpSampledImage(texture.sampled_type, image, sampler);
  if (texture.native_cube) {
    coords = NativeCubeCoordinates(ctx, coords);
  }
  ImageOperands operands;
  operands.Add(spv::ImageOperandsMask::Lod, lod);
  operands.AddOffset(ctx, offset);
  const Id sample = ctx.OpImageSampleExplicitLod(
      result_type, sampled_image, coords, operands.mask, operands.operands);
  return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id bias, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleDrefImplicitLod(result_type, sampled_image, coords, dref,
                                                       operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id lod, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleDrefExplicitLod(result_type, sampled_image, coords, dref,
                                                       operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageGather(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                   const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const u32 comp = inst->Flags<IR::TextureInstInfo>().gather_comp.Value();
    ImageOperands operands;
    operands.AddOffset(ctx, offset, true);
    const Id texels = ctx.OpImageGather(result_type, sampled_image, coords, ctx.ConstU32(comp),
                                        operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                       const IR::Value& offset, Id dref) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.AddOffset(ctx, offset, true);
    const Id texels = ctx.OpImageDrefGather(result_type, sampled_image, coords, dref, operands.mask,
                                            operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, u32 handle, Id lod, bool has_mips) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const auto sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
    const Id zero = ctx.u32_zero_value;
    const auto mips{[&] { return has_mips ? ctx.OpImageQueryLevels(ctx.U32[1], image) : zero; }};
    const bool uses_lod{texture.view_type != AmdGpu::ImageType::Color2DMsaa && !texture.is_storage};
    const auto query{[&](Id type) {
        return uses_lod ? ctx.OpImageQuerySizeLod(type, image, lod)
                        : ctx.OpImageQuerySize(type, image);
    }};
    switch (texture.view_type) {
    case AmdGpu::ImageType::Color1D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[1]), zero, zero, mips());
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color3D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[3]), mips());
    default:
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

Id EmitImageQueryLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id zero{ctx.f32_zero_value};
    return ctx.OpImageQueryLod(ctx.F32[2], sampled_image, coords);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id derivatives_dx,
                     Id derivatives_dy, const IR::Value& offset, const IR::Value& lod_clamp) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.AddDerivatives(ctx, derivatives_dx, derivatives_dy);
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : ApplyGlobalRepeat(ctx, inst, handle, coords, {}, offset, sample, derivatives_dx, derivatives_dy);
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id color_type = texture.data_types->Get(4);
    ImageOperands operands;
    Id texel;
    if (!texture.is_storage) {
        const Id image = ctx.OpLoad(texture.image_type, texture.id);
        if (texture.view_type == AmdGpu::ImageType::Color2DMsaa) {
            // GCN hardware wraps out-of-range MSAA sample indices
            if (Sirit::ValidId(ms)) {
                const Id sample_count = ctx.OpImageQuerySamples(ctx.U32[1], image);
                const Id wrapped_ms = ctx.OpUMod(ctx.U32[1], ms, sample_count);
                operands.Add(spv::ImageOperandsMask::Sample, wrapped_ms);
            }
        } else {
            if (Sirit::ValidId(ms)) {
                LOG_ERROR(Render_Recompiler, "image is not MS but ms operand is provided");
            }
            operands.Add(spv::ImageOperandsMask::Lod, lod);
            operands.Add(spv::ImageOperandsMask::Sample, ms);
        }
        texel = ctx.OpImageFetch(color_type, image, coords, operands.mask, operands.operands);
    } else {
        Id image_ptr = texture.id;
        if (ctx.profile.supports_image_load_store_lod) {
            operands.Add(spv::ImageOperandsMask::Lod, lod);
        } else if (Sirit::ValidId(lod)) {
#if 1
            // It's confusing what interactions will cause this code path so leave it as
            // unreachable until a case is found.
            // Normally IMAGE_LOAD_MIP should translate -> OpImageFetch
            UNREACHABLE_MSG("Unsupported ImageRead with Lod");
#else
            LOG_WARNING(Render, "Fallback for ImageRead with LOD");
            ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
            const Id single_image_ptr_type =
                ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
            image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
#endif
        }
        const Id image = ctx.OpLoad(texture.image_type, image_ptr);
        texel = ctx.OpImageRead(color_type, image, coords, operands.mask, operands.operands);
    }
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texel) : texel;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms,
                    Id color) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id image_ptr = texture.id;
    const Id color_type = texture.data_types->Get(4);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Sample, ms);
    if (ctx.profile.supports_image_load_store_lod) {
        operands.Add(spv::ImageOperandsMask::Lod, lod);
    } else if (Sirit::ValidId(lod)) {
        LOG_WARNING(Render, "Fallback for ImageWrite with LOD");
        ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
        const Id single_image_ptr_type =
            ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
        image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
    }
    const Id image = ctx.OpLoad(texture.image_type, image_ptr);
    Id texel = texture.is_integer ? ctx.OpBitcast(color_type, color) : color;
    // Same clamping as the render target export, for the game's compute passes that write 11/11/10 floats
    // directly: negative and NaN give 0, large values stop at 65024.
    const auto sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
    const auto dfmt = sharp.GetDataFmt();
    if (!texture.is_integer && sharp.GetNumberFmt() == AmdGpu::NumberFormat::Float &&
        (dfmt == AmdGpu::DataFormat::Format10_11_11 || dfmt == AmdGpu::DataFormat::Format11_11_10)) {
        std::array<Id, 4> parts;
        for (u32 i = 0; i < 4; ++i) {
            const Id part = ctx.OpCompositeExtract(ctx.F32[1], texel, i);
            parts[i] = i == 3 ? part
                              : ctx.OpSelect(ctx.F32[1],
                                             ctx.OpFOrdGreaterThan(ctx.U1[1], part, ctx.ConstF32(0.f)),
                                             ctx.OpFMin(ctx.F32[1], part, ctx.ConstF32(65024.f)),
                                             ctx.ConstF32(0.f));
        }
        texel = ctx.OpCompositeConstruct(ctx.F32[4], parts);
    }
    ctx.OpImageWrite(image, coords, texel, operands.mask, operands.operands);
}

Id EmitCubeFaceIndex(EmitContext& ctx, IR::Inst* inst, Id cube_coords) {
    if (ctx.profile.supports_native_cube_calc) {
        return ctx.OpCubeFaceIndexAMD(ctx.F32[1], cube_coords);
    } else {
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

} // namespace Shader::Backend::SPIRV
