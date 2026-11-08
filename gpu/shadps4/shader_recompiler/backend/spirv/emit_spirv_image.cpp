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
    // Keep the original sample and derivatives in the caller's block. Only the
    // explicit-gradient/LOD reads below may execute under this per-fragment branch.
    const Id before = ctx.last_label;
    const Id repeat_label = ctx.OpLabel(), merge = ctx.OpLabel();
    const Id apply_repeat = ctx.OpFOrdGreaterThan(ctx.U1[1], amount, f(0));
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(apply_repeat, repeat_label, merge);
    ctx.AddLabel(repeat_label);
    const Id cell = ctx.OpFloor(V, uv), frac = ctx.OpFSub(V, uv, cell);
    const auto component = [&](Id x, u32 i) { return ctx.OpCompositeExtract(F, x, i); };
    const Id zero = ctx.ConstU32(0u), one = ctx.ConstU32(1u);
    // Round sqrt(2) upward: no supported site can be excluded by float rounding.
    const Id lower = ctx.OpFloor(V, ctx.OpFSub(V, frac, v(f(0x1.6a09e8p+0f), f(0x1.6a09e8p+0f))));
    const Id cell_x = ctx.OpBitcast(U, ctx.OpConvertFToS(ctx.S32[1], component(cell, 0)));
    const Id cell_y = ctx.OpBitcast(U, ctx.OpConvertFToS(ctx.S32[1], component(cell, 1)));
    const auto hash = [&](Id x, Id y, Id j, u32 domain) {
        Id h = ctx.OpIAdd(U, ctx.OpIAdd(U, ctx.OpIMul(U, x, ctx.ConstU32(1664525u)),
                                       ctx.OpIMul(U, y, ctx.ConstU32(1013904223u))),
                         ctx.OpIAdd(U, ctx.OpIMul(U, j, ctx.ConstU32(3266489917u)),
                                    ctx.ConstU32(domain)));
        h = ctx.OpIMul(U, ctx.OpBitwiseXor(U, h, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(16u))),
                      ctx.ConstU32(2246822519u));
        h = ctx.OpIMul(U, ctx.OpBitwiseXor(U, h, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(13u))),
                      ctx.ConstU32(3266489917u));
        return ctx.OpBitwiseXor(U, h, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(16u)));
    };
    const auto uniform_pair = [&](Id h) {
        return v(ctx.OpFMul(F, ctx.OpConvertUToF(F, ctx.OpBitwiseAnd(U, h, ctx.ConstU32(65535u))),
                           f(1.f / 65536.f)),
                 ctx.OpFMul(F, ctx.OpConvertUToF(F, ctx.OpShiftRightLogical(U, h, ctx.ConstU32(16u))),
                           f(1.f / 65536.f)));
    };
    // Inverse Poisson(0.5) CDF for midpoint U32 uniforms. Count 10 is the
    // fallback; the omitted tail is smaller than one 32-bit random quantum.
    static constexpr std::array<u32, 10> poisson_cdf{
        2605029347u, 3907544021u, 4233172690u, 4287444134u, 4294228065u,
        4294906458u, 4294962991u, 4294967029u, 4294967281u, 4294967295u};
    // Invocation-private loop state avoids touching the game's deferred Phis.
    // Each expansion has separate state and resets it before the loops.
    const auto state = [&](Id type, Id initial) {
        const Id id = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                            spv::StorageClass::Private, initial);
        if (ctx.profile.supported_spirv >= 0x00010400)
            ctx.interfaces.push_back(id);
        return id;
    };
    const Id cell_index = state(U, zero), site_index = state(U, zero);
    const Id sum_value = state(C, ctx.ConstantNull(C));
    const Id sum_weight = state(F, f(0)), transparency = state(F, f(1));
    ctx.OpStore(cell_index, zero);
    ctx.OpStore(sum_value, ctx.ConstantNull(C));
    ctx.OpStore(sum_weight, f(0));
    ctx.OpStore(transparency, f(1));
    const Id image = ctx.OpLoad(tex.image_type, tex.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id cell_header = ctx.OpLabel(), cell_body = ctx.OpLabel();
    const Id cell_continue = ctx.OpLabel(), cell_merge = ctx.OpLabel();
    ctx.OpBranch(cell_header);
    ctx.AddLabel(cell_header);
    const Id ci = ctx.OpLoad(U, cell_index);
    const Id more_cells = ctx.OpULessThan(ctx.U1[1], ci, ctx.ConstU32(16u));
    ctx.OpLoopMerge(cell_merge, cell_continue, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(more_cells, cell_body, cell_merge);
    ctx.AddLabel(cell_body);
    const Id ox = ctx.OpUMod(U, ci, ctx.ConstU32(4u));
    const Id oy = ctx.OpUDiv(U, ci, ctx.ConstU32(4u));
    const Id cell_offset = v(ctx.OpFAdd(F, ctx.OpConvertUToF(F, ox), component(lower, 0)),
                             ctx.OpFAdd(F, ctx.OpConvertUToF(F, oy), component(lower, 1)));
    const Id key_x = ctx.OpIAdd(U, cell_x, ctx.OpBitcast(U, ctx.OpConvertFToS(ctx.S32[1], component(cell_offset, 0))));
    const Id key_y = ctx.OpIAdd(U, cell_y, ctx.OpBitcast(U, ctx.OpConvertFToS(ctx.S32[1], component(cell_offset, 1))));
    const Id count_hash = hash(key_x, key_y, zero, 0xb5297a4du);
    // Count 0/1 covers 90.98% of cells; only the rare tail traverses the tree.
    const Id common_count = ctx.OpLabel(), tail_count = ctx.OpLabel(), count_merge = ctx.OpLabel();
    const Id is_common = ctx.OpULessThan(ctx.U1[1], count_hash, ctx.ConstU32(poisson_cdf[1]));
    ctx.OpSelectionMerge(count_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(is_common, common_count, tail_count);
    ctx.AddLabel(common_count);
    const Id common_value = ctx.OpSelect(U, ctx.OpUGreaterThanEqual(ctx.U1[1], count_hash,
                                        ctx.ConstU32(poisson_cdf[0])), one, zero);
    ctx.OpBranch(count_merge);
    ctx.AddLabel(tail_count);
    const auto count_tree = [&](auto&& self, u32 lo, u32 hi) -> Id {
        if (lo == hi) return ctx.ConstU32(lo);
        const u32 mid = (lo + hi) / 2;
        const Id left = ctx.OpLabel(), right = ctx.OpLabel(), join = ctx.OpLabel();
        const Id is_left = ctx.OpULessThan(ctx.U1[1], count_hash, ctx.ConstU32(poisson_cdf[mid]));
        ctx.OpSelectionMerge(join, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_left, left, right);
        ctx.AddLabel(left);
        const Id a = self(self, lo, mid), a_label = ctx.last_label;
        ctx.OpBranch(join);
        ctx.AddLabel(right);
        const Id b = self(self, mid + 1, hi), b_label = ctx.last_label;
        ctx.OpBranch(join);
        ctx.AddLabel(join);
        return ctx.OpPhi(U, a, a_label, b, b_label);
    };
    const Id tail_value = count_tree(count_tree, 2, 10), tail_finish = ctx.last_label;
    ctx.OpBranch(count_merge);
    ctx.AddLabel(count_merge);
    const Id count = ctx.OpPhi(U, common_value, common_count, tail_value, tail_finish);
    ctx.OpStore(site_index, zero);
    const Id site_header = ctx.OpLabel(), site_body = ctx.OpLabel();
    const Id site_continue = ctx.OpLabel(), site_merge = ctx.OpLabel();
    ctx.OpBranch(site_header);
    ctx.AddLabel(site_header);
    const Id si = ctx.OpLoad(U, site_index);
    const Id more_sites = ctx.OpULessThan(ctx.U1[1], si, count);
    ctx.OpLoopMerge(site_merge, site_continue, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(more_sites, site_body, site_merge);
    ctx.AddLabel(site_body);
    // R=sqrt(2) has complete support in this shifted 4x4 neighbourhood. Independent
    // count, position and texture-phase domains remove fixed-cell strata.
    const Id point = ctx.OpFAdd(V, cell_offset, uniform_pair(hash(key_x, key_y, si, 0x68bc21ebu)));
    const Id delta = ctx.OpFSub(V, point, frac);
    const Id distance2 = ctx.OpDot(F, delta, delta);
    const Id radius_weight = ctx.OpFMax(F, f(0), ctx.OpFSub(F, f(1), ctx.OpFMul(F, distance2, f(.5f))));
    const Id weight = ctx.OpFMul(F, radius_weight, radius_weight);
    const Id previous_value = ctx.OpLoad(C, sum_value);
    const Id previous_weight = ctx.OpLoad(F, sum_weight);
    const Id previous_transparency = ctx.OpLoad(F, transparency);
    const Id sample_body = ctx.OpLabel(), sample_merge = ctx.OpLabel();
    const Id supported = ctx.OpFOrdGreaterThan(ctx.U1[1], weight, f(0));
    ctx.OpSelectionMerge(sample_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(supported, sample_body, sample_merge);
    ctx.AddLabel(sample_body);
    // Period 2 is common to Wrap and Mirror: paired layers keep identical
    // numeric translations even when their sampler modes differ.
    const Id phase = uniform_pair(hash(key_x, key_y, si, 2246822519u));
    const Id shifted = ctx.OpFAdd(
        V, uv, v(rx ? ctx.OpFMul(F, component(phase, 0),
                                f(2.f)) : f(0),
                 ry ? ctx.OpFMul(F, component(phase, 1),
                                f(2.f)) : f(0)));
    const Id sample_coords = tex.view_type == AmdGpu::ImageType::Color2DArray
                                 ? ctx.OpCompositeConstruct(ctx.F32[3], component(shifted, 0),
                                                            component(shifted, 1), component(coords, 2))
                                 : shifted;
    const Id combined = ctx.OpSampledImage(tex.sampled_type, image, sampler);
    const Id sample = ctx.OpImageSampleExplicitLod(C, combined, sample_coords,
                                                   spv::ImageOperandsMask::Grad, dx, dy);
    const Id added_value = ctx.OpFAdd(C, previous_value, ctx.OpVectorTimesScalar(C, sample, weight));
    const Id added_weight = ctx.OpFAdd(F, previous_weight, weight);
    const Id added_transparency = ctx.OpFMul(F, previous_transparency, ctx.OpFSub(F, f(1), weight));
    ctx.OpBranch(sample_merge);
    ctx.AddLabel(sample_merge);
    const Id next_value = ctx.OpPhi(C, added_value, sample_body, previous_value, site_body);
    const Id next_weight = ctx.OpPhi(F, added_weight, sample_body, previous_weight, site_body);
    const Id next_transparency = ctx.OpPhi(F, added_transparency, sample_body,
                                          previous_transparency, site_body);
    ctx.OpStore(sum_value, next_value);
    ctx.OpStore(sum_weight, next_weight);
    ctx.OpStore(transparency, next_transparency);
    ctx.OpBranch(site_continue);
    ctx.AddLabel(site_continue);
    ctx.OpStore(site_index, ctx.OpIAdd(U, si, one));
    ctx.OpBranch(site_header);
    ctx.AddLabel(site_merge);
    ctx.OpBranch(cell_continue);
    ctx.AddLabel(cell_continue);
    ctx.OpStore(cell_index, ctx.OpIAdd(U, ci, one));
    ctx.OpBranch(cell_header);
    ctx.AddLabel(cell_merge);
    const Id total_weight = ctx.OpLoad(F, sum_weight);
    const Id nonempty = ctx.OpLabel(), finish = ctx.OpLabel();
    const Id has_weight = ctx.OpFOrdGreaterThan(ctx.U1[1], total_weight, f(0));
    ctx.OpSelectionMerge(finish, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(has_weight, nonempty, finish);
    ctx.AddLabel(nonempty);
    const Id average = ctx.OpVectorTimesScalar(C, ctx.OpLoad(C, sum_value),
                                               ctx.OpFDiv(F, f(1), total_weight));
    // Opacity 1-product(1-K) tends to zero with W at empty support boundaries.
    // Convex coefficients preserve DC/bounds; no global mean or gain is used.
    const Id coverage = ctx.OpFSub(F, f(1), ctx.OpLoad(F, transparency));
    Id result = ctx.OpFMix(C, original, average, splat(ctx.OpFMul(F, amount, coverage)));
    result = ctx.OpCompositeInsert(C, ctx.OpCompositeExtract(F, original, 3), result, 3);
    ctx.OpBranch(finish);
    ctx.AddLabel(finish);
    const Id with_empty_fallback = ctx.OpPhi(C, result, nonempty, original, cell_merge);
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
    return ctx.OpPhi(C, with_empty_fallback, finish, original, before);
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
