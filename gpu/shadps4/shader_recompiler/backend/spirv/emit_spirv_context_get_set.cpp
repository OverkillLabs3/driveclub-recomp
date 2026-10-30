// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_vr.h" // VR multiview
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/patch.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

#include <algorithm>
#include <unordered_map>
#include <ranges>

#include <magic_enum/magic_enum.hpp>

namespace Shader::Backend::SPIRV {

using PointerType = EmitContext::PointerType;
using PointerSize = EmitContext::PointerSize;

static std::pair<Id, bool> OutputAttrComponentType(EmitContext& ctx, IR::Attribute attr) {
    if (IR::IsParam(attr)) {
        const u32 index{u32(attr) - u32(IR::Attribute::Param0)};
        const auto& info{ctx.output_params.at(index)};
        return {info.component_type, info.is_integer};
    }
    if (IR::IsMrt(attr)) {
        const u32 index{u32(attr) - u32(IR::Attribute::RenderTarget0)};
        const auto& info{ctx.frag_outputs.at(index)};
        return {info.component_type, info.is_integer};
    }
    switch (attr) {
    case IR::Attribute::Position0:
    case IR::Attribute::ClipDistance:
    case IR::Attribute::CullDistance:
    case IR::Attribute::Depth:
    case IR::Attribute::PointSize:
        return {ctx.F32[1], false};
    case IR::Attribute::RenderTargetIndex:
    case IR::Attribute::ViewportIndex:
    case IR::Attribute::SampleMask:
        return {ctx.U32[1], true};
    case IR::Attribute::StencilRef:
        return {ctx.S32[1], true};
    default:
        UNREACHABLE_MSG("Write attribute {}", attr);
    }
}

Id EmitGetUserData(EmitContext& ctx, IR::ScalarReg reg) {
    const u32 index = ctx.binding.user_data + ctx.info.ud_mask.Index(reg);
    const u32 half = PushData::UdRegsIndex + (index >> 2);
    const Id ud_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]),
                                      ctx.push_data_block, ctx.ConstU32(half),
                                      ctx.ConstU32(index & 3))};
    const Id ud_reg{ctx.OpLoad(ctx.U32[1], ud_ptr)};
    ctx.Name(ud_reg, fmt::format("ud_{}", u32(reg)));
    return ud_reg;
}

Id EmitReadConst(EmitContext& ctx, IR::Inst* inst, Id addr, Id offset) {
    const u32 flatbuf_off_dw = inst->Flags<u32>();
    if (!EmulatorSettings.IsDirectMemoryAccessEnabled()) {
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(flatbuf_off_dw));
    }
    if (flatbuf_off_dw == 0) {
        return ctx.OpFunctionCall(ctx.U32[1], ctx.read_const_dynamic, addr, offset);
    } else {
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(flatbuf_off_dw));
    }
}

// bbport: BB_WIDE_LOADS=1. GCN's S_BUFFER_LOAD_DWORDX2/X4/X8/X16 arrive here as runs of single-dword ReadConstBuffer
// and BUFFER_LOAD_DWORDX2/X3/X4 as LoadBuffer*xN, which this backend used to split into dword loads; the NVIDIA
// compiler does not merge 32-bit storage-buffer loads by itself: a 1440p pass reading 64 vec4 constants per pixel
// takes 865 us as dwords and 675-681 us as 16-byte loads on an RTX 3070 Laptop (a standalone Vulkan probe).
// 49% of the game's 12,675 constant reads sit in 16-byte-aligned runs of four and 28% more in 8-byte-aligned pairs.
// A run of four outside loops becomes one 16-byte load when its effective index (base + first + the binding's
// offset) is 4-aligned and the bytes lie inside the binding; otherwise the same dword loads as before. The result is
// the same bytes either way. Loops keep dword loads: a light loop reading a run of four per iteration, ps ec4eb2ed,
// went 0.21 -> 0.27 ms with the check inside the loop. Pairs and BUFFER_LOAD x2/x3/x4 measured about 3% slower. Part
// of the shader cache version (vk_pipeline_serialization.cpp). On by default (GPU busy -0.07 ms a frame, +0.9% FPS at
// 1440p render); =0 turns it off.
bool WideLoads() {
    static const bool on = [] {
        // VR multiview: off (guest buffers are per-eye arrays there, and the wide alias is made after EyeBuffers)
        if (Vr::MultiviewShaders()) {
            return false;
        }
        const char* env = std::getenv("BB_WIDE_LOADS");
        return !env || !env[0] || env[0] == '1';
    }();
    return on;
}

// Finds the runs before the program is emitted. A run is four ReadConstBuffer in one block, of one guest
// buffer, at dwords first..first+3 from one base value, first 4-aligned, with nothing between the
// first and the last of them that may write memory or have another side effect, so the wide load can take the place
// of all of them at the first.
void PlanWideLoads(EmitContext& ctx, const IR::Program& program) {
    if (!WideLoads()) {
        return;
    }
    // Blocks inside a loop: between a header and the block that branches back to it (blocks are in program order).
    std::unordered_map<const IR::Block*, u32> order;
    for (const IR::Block* block : program.blocks) {
        order.emplace(block, u32(order.size()));
    }
    std::vector<bool> in_loop(program.blocks.size());
    for (const IR::Block* block : program.blocks) {
        const u32 header = order.at(block);
        for (const IR::Block* pred : block->ImmPredecessors()) {
            const auto it = order.find(pred);
            if (it != order.end() && it->second >= header) {
                std::fill(in_loop.begin() + header, in_loop.begin() + it->second + 1, true);
            }
        }
    }
    for (const IR::Block* block : program.blocks) {
        if (in_loop[order.at(block)]) {
            continue;
        }
        struct Slot {
            const IR::Inst* inst;
            u32 pos;
        };
        struct Run {
            u32 handle;
            const IR::Inst* base;
            u32 first;
            std::array<Slot, 4> slots;
        };
        std::vector<Run> runs;
        std::vector<u32> side_effects;
        u32 pos = 0;
        for (const IR::Inst& inst : block->Instructions()) {
            ++pos;
            if (inst.MayHaveSideEffects()) {
                side_effects.push_back(pos);
                continue;
            }
            if (inst.GetOpcode() != IR::Opcode::ReadConstBuffer || !inst.Arg(0).IsImmediate()) {
                continue;
            }
            const u32 handle = inst.Arg(0).U32();
            if (handle >= ctx.buffers.size() || ctx.buffers[handle].buffer_type != BufferType::Guest ||
                !Sirit::ValidId(ctx.buffers[handle].Alias(PointerType::U32).id)) {
                continue;
            }
            const IR::Inst* base = nullptr;
            u32 offset = 0;
            const IR::Value index = inst.Arg(1);
            if (index.IsImmediate()) {
                offset = index.U32();
            } else {
                const IR::Inst* def = index.Inst();
                if (def->GetOpcode() == IR::Opcode::IAdd32 && def->Arg(1).IsImmediate() &&
                    !def->Arg(0).IsImmediate()) {
                    base = def->Arg(0).Inst();
                    offset = def->Arg(1).U32();
                } else if (def->GetOpcode() == IR::Opcode::IAdd32 && def->Arg(0).IsImmediate() &&
                           !def->Arg(1).IsImmediate()) {
                    base = def->Arg(1).Inst();
                    offset = def->Arg(0).U32();
                } else {
                    base = def;
                }
            }
            const u32 first = offset & ~3u;
            auto it = std::ranges::find_if(
                runs, [&](const Run& r) { return r.handle == handle && r.base == base && r.first == first; });
            if (it == runs.end()) {
                runs.push_back({handle, base, first, {}});
                it = std::prev(runs.end());
            }
            Slot& slot = it->slots[offset & 3u];
            if (!slot.inst) {
                slot = {&inst, pos};
            }
        }
        for (const Run& run : runs) {
            const auto& slots = run.slots;
            if (!std::ranges::all_of(slots, [](const Slot& s) { return s.inst != nullptr; })) {
                continue;
            }
            const auto [lo, hi] = std::ranges::minmax(slots | std::views::transform(&Slot::pos));
            if (std::ranges::any_of(side_effects, [&](u32 p) { return p > lo && p < hi; })) {
                continue;
            }
            const u32 group = u32(ctx.wide_groups.size());
            ctx.wide_groups.push_back({run.handle, run.base, run.first, Id{}});
            for (u32 i = 0; i < 4; ++i) {
                ctx.wide_members[slots[i].inst] = {group, i};
            }
            ctx.DefineWideAlias(run.handle);
        }
    }
}

// Four dwords from `index` (in dwords, binding offset included): one 16-byte load when the index is a multiple of 4
// and the bytes lie inside the binding (OpArrayLength of the wide alias), else the same dword loads as before.
// Returns U32[4].
static Id EmitWideDwordLoad(EmitContext& ctx, const EmitContext::BufferDefinition& buffer, Id index) {
    const auto [wide_id, wide_pointer] = buffer.Alias(PointerType::U32x4);
    const Id wide_index{ctx.OpShiftRightLogical(ctx.U32[1], index, ctx.ConstU32(2U))};
    const auto load_wide = [&] {
        return ctx.OpLoad(ctx.U32[4], ctx.OpAccessChain(wide_pointer, wide_id, ctx.u32_zero_value, wide_index));
    };
    const auto [narrow_id, narrow_pointer] = buffer.Alias(PointerType::U32);
    const Id aligned{ctx.OpIEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], index, ctx.ConstU32(3U)),
                                  ctx.u32_zero_value)};
    const Id inside{ctx.OpULessThan(ctx.U1[1], wide_index, ctx.OpArrayLength(ctx.U32[1], wide_id, 0U))};
    const Id wide{ctx.OpLogicalAnd(ctx.U1[1], aligned, inside)};
    const Id before = ctx.OpLabel(), wide_label = ctx.OpLabel(), narrow_label = ctx.OpLabel(),
             merge = ctx.OpLabel();
    ctx.OpBranch(before);
    ctx.AddLabel(before);
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(wide, wide_label, narrow_label);
    ctx.AddLabel(wide_label);
    const Id loaded_wide{load_wide()};
    ctx.OpBranch(merge);
    ctx.AddLabel(narrow_label);
    std::array<Id, 4> dwords;
    for (u32 i = 0; i < 4; ++i) {
        const Id index_i = i == 0 ? index : ctx.OpIAdd(ctx.U32[1], index, ctx.ConstU32(i));
        dwords[i] = ctx.OpLoad(ctx.U32[1], ctx.OpAccessChain(narrow_pointer, narrow_id, ctx.u32_zero_value, index_i));
    }
    const Id loaded_narrow{ctx.OpCompositeConstruct(ctx.U32[4], dwords)};
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
    return ctx.OpPhi(ctx.U32[4], loaded_wide, wide_label, loaded_narrow, narrow_label);
}

Id EmitReadConstBuffer(EmitContext& ctx, IR::Inst* inst, u32 handle, Id index) {
    if (const auto it = ctx.wide_members.find(inst); it != ctx.wide_members.end()) {
        auto& group = ctx.wide_groups[it->second.first];
        if (!Sirit::ValidId(group.result)) {
            // The first member of the run in program order: every base is defined before it.
            const auto& buffer = ctx.buffers[group.handle];
            Id first = group.base ? group.base->Definition<Id>() : ctx.ConstU32(group.first);
            if (group.base && group.first != 0) {
                first = ctx.OpIAdd(ctx.U32[1], first, ctx.ConstU32(group.first));
            }
            if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
                first = ctx.OpIAdd(ctx.U32[1], first, offset);
            }
            group.result = EmitWideDwordLoad(ctx, buffer, first);
        }
        return ctx.OpCompositeExtract(ctx.U32[1], group.result, it->second.second);
    }
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index)};
    const Id result{ctx.OpLoad(ctx.U32[1], ptr)};
    return result;
}

Id EmitGetAttribute(EmitContext& ctx, IR::Attribute attr, u32 comp, u32 index) {
    if (IR::IsParam(attr)) {
        const u32 param_index{u32(attr) - u32(IR::Attribute::Param0)};
        const auto& param{ctx.input_params.at(param_index)};
        const Id value = [&] {
            if (param.is_array) {
                ASSERT(param.num_components > 1);
                if (param.is_loaded) {
                    return ctx.OpCompositeExtract(param.component_type, param.id_array[index],
                                                  comp);
                } else {
                    return ctx.OpLoad(param.component_type,
                                      ctx.OpAccessChain(param.pointer_type, param.id,
                                                        ctx.ConstU32(index), ctx.ConstU32(comp)));
                }
            } else {
                ASSERT(!param.is_loaded);
                if (param.num_components > 1) {
                    return ctx.OpLoad(
                        param.component_type,
                        ctx.OpAccessChain(param.pointer_type, param.id, ctx.ConstU32(comp)));
                } else {
                    return ctx.OpLoad(param.component_type, param.id);
                }
            }
        }();
        return param.is_integer ? ctx.OpBitcast(ctx.F32[1], value) : value;
    }
    if (IR::IsBarycentricCoord(attr) && attr != IR::Attribute::BaryCoordPullModel &&
        ctx.profile.supports_fragment_shader_barycentric) {
        ++comp;
    }
    switch (attr) {
    case IR::Attribute::Position0:
        ASSERT(ctx.sw_stage == SwStage::Geometry);
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.gl_in, ctx.ConstU32(index),
                                            ctx.ConstU32(0U), ctx.ConstU32(comp)));
    case IR::Attribute::FragCoord: {
        const Id value = ctx.OpLoad(ctx.F32[1],
            ctx.OpAccessChain(ctx.input_f32, ctx.frag_coord, ctx.ConstU32(comp)));
        if (comp >= 2) return value;
        // Guest shaders still address native-sized textures and constants. Convert the
        // reduced framebuffer's pixel coordinates back to that logical coordinate space.
        const Id packed = ctx.OpLoad(ctx.U32[1], ctx.OpAccessChain(
            ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]),
            ctx.push_data_block, ctx.ConstU32(PushData::SceneSizeIndex)));
        const Id dimension = comp == 0
            ? ctx.OpBitwiseAnd(ctx.U32[1], packed, ctx.ConstU32(65535u))
            : ctx.OpShiftRightLogical(ctx.U32[1], packed, ctx.ConstU32(16u));
        const Id native = ctx.ConstU32(comp == 0 ? 1920u : 1080u);
        const Id safe = ctx.OpSelect(ctx.U32[1],
            ctx.OpIEqual(ctx.U1[1], dimension, ctx.u32_zero_value), native, dimension);
        return ctx.OpFMul(ctx.F32[1], value, ctx.OpFDiv(ctx.F32[1],
            ctx.OpConvertUToF(ctx.F32[1], native), ctx.OpConvertUToF(ctx.F32[1], safe)));
    }
    case IR::Attribute::TessellationEvaluationPointU:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.tess_coord, ctx.u32_zero_value));
    case IR::Attribute::TessellationEvaluationPointV:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.tess_coord, ctx.ConstU32(1U)));
    case IR::Attribute::BaryCoordSmooth:
        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_smooth,
                                                            ctx.ConstU32(comp)));
        } else {
            return ctx.OpCompositeExtract(ctx.F32[1], ctx.OpLoad(ctx.F32[3], ctx.bary_coord), comp);
        }
    case IR::Attribute::BaryCoordSmoothCentroid:
        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(ctx.F32[1],
                              ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_smooth_centroid,
                                                ctx.ConstU32(comp)));
        } else {
            return ctx.OpCompositeExtract(
                ctx.F32[1], ctx.OpInterpolateAtCentroid(ctx.F32[3], ctx.bary_coord), comp);
        }
    case IR::Attribute::BaryCoordSmoothSample:
        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(
                ctx.F32[1],
                ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_smooth_sample, ctx.ConstU32(comp)));
        } else {
            return ctx.OpCompositeExtract(
                ctx.F32[1],
                ctx.OpInterpolateAtSample(ctx.F32[3], ctx.bary_coord,
                                          ctx.OpLoad(ctx.U32[1], ctx.sample_index)),
                comp);
        }
    case IR::Attribute::BaryCoordNoPersp:
        return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_nopersp,
                                                        ctx.ConstU32(comp)));
    case IR::Attribute::BaryCoordNoPerspSample:
        return ctx.OpLoad(
            ctx.F32[1],
            ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_nopersp_sample, ctx.ConstU32(comp)));
    case IR::Attribute::BaryCoordPullModel: {
        ASSERT_MSG(comp < 3, "Invalid BaryCoordPullModel component {}", comp);

        if (ctx.profile.supports_amd_shader_explicit_vertex_parameter) {
            return ctx.OpLoad(
                ctx.F32[1],
                ctx.OpAccessChain(ctx.input_f32, ctx.bary_coord_pull_model, ctx.ConstU32(comp)));
        }

        // FragCoord.w is reciprocal clip W at the fragment center. Perspective barycentrics
        // are (I/W)/(1/W) and (J/W)/(1/W), so multiplying by FragCoord.w recovers the
        // pull-model values expected by the guest.
        const Id inv_w = ctx.OpLoad(
            ctx.F32[1], ctx.OpAccessChain(ctx.input_f32, ctx.frag_coord, ctx.ConstU32(3U)));
        if (comp == 2) {
            return inv_w;
        }
        const Id ij =
            ctx.OpCompositeExtract(ctx.F32[1], ctx.OpLoad(ctx.F32[3], ctx.bary_coord), comp + 1);
        return ctx.OpFMul(ctx.F32[1], ij, inv_w);
    }
    default:
        UNREACHABLE_MSG("Read attribute {}", attr);
    }
}

Id EmitGetAttributeU1(EmitContext& ctx, IR::Attribute attr, u32 comp) {
    ASSERT(comp == 0);
    switch (attr) {
    case IR::Attribute::IsFrontFace:
        return ctx.OpLoad(ctx.U1[1], ctx.front_facing);
    case IR::Attribute::IsHelperInvocation:
        return ctx.OpLoad(ctx.U1[1], ctx.helper_invocation);
    default:
        UNREACHABLE_MSG("Unsupported U1 attribute {}", attr);
    }
}

Id EmitGetAttributeU32(EmitContext& ctx, IR::Attribute attr, u32 comp) {
    switch (attr) {
    case IR::Attribute::VertexId:
        return ctx.OpLoad(ctx.U32[1], ctx.vertex_index);
    case IR::Attribute::InstanceId:
        return ctx.OpLoad(ctx.U32[1], ctx.instance_id);
    case IR::Attribute::BaseVertex:
        return ctx.OpLoad(ctx.U32[1], ctx.base_vertex);
    case IR::Attribute::BaseInstance:
        return ctx.OpLoad(ctx.U32[1], ctx.base_instance);
    case IR::Attribute::WorkgroupIndex:
        return ctx.workgroup_index_id;
    case IR::Attribute::WorkgroupId:
        return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), comp);
    case IR::Attribute::LocalInvocationId:
        return ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id),
                                      comp);
    case IR::Attribute::LocalInvocationIndex:
        return ctx.OpLoad(ctx.U32[1], ctx.local_invocation_index);
    case IR::Attribute::SampleIndex:
        return ctx.OpLoad(ctx.U32[1], ctx.sample_index);
    case IR::Attribute::SampleMask:
        return ctx.OpLoad(ctx.U32[1],
                          ctx.OpAccessChain(ctx.input_u32, ctx.sample_mask_in, ctx.u32_zero_value));
    case IR::Attribute::RenderTargetIndex:
        return ctx.OpLoad(ctx.U32[1], ctx.output_layer);
    case IR::Attribute::PrimitiveId:
        return ctx.OpLoad(ctx.U32[1], ctx.primitive_id);
    case IR::Attribute::InvocationId:
        ASSERT(ctx.info.sw_stage == SwStage::Geometry ||
               ctx.info.sw_stage == SwStage::TessellationControl);
        return ctx.OpLoad(ctx.U32[1], ctx.invocation_id);
    case IR::Attribute::SubgroupLtMask:
        return ctx.OpLoad(
            ctx.U32[1], ctx.OpAccessChain(ctx.input_u32, ctx.subgroup_lt_mask, ctx.ConstU32(comp)));
    case IR::Attribute::PatchVertices:
        ASSERT(ctx.info.sw_stage == SwStage::TessellationControl);
        return ctx.OpLoad(ctx.U32[1], ctx.patch_vertices);
    case IR::Attribute::PackedHullInvocationInfo: {
        ASSERT(ctx.info.sw_stage == SwStage::TessellationControl);
        // [0:8]: patch id within VGT
        // [8:12]: output control point id
        // But 0:8 should be treated as 0 for attribute addressing purposes
        if (ctx.runtime_info.sw.tcs.IsPassthrough()) {
            // Gcn shader would run with 1 thread, but we need to run a thread for
            // each output control point.
            // If Gcn shader uses this value, we should make sure all threads in the
            // Vulkan shader use 0
            return ctx.u32_zero_value;
        } else {
            const Id invocation_id = ctx.OpLoad(ctx.U32[1], ctx.invocation_id);
            return ctx.OpShiftLeftLogical(ctx.U32[1], invocation_id, ctx.ConstU32(8u));
        }
    }
    default:
        UNREACHABLE_MSG("Read U32 attribute {}", attr);
    }
}

void EmitSetAttribute(EmitContext& ctx, IR::Attribute attr, Id value, u32 element) {
    const auto op_store = [&](Id pointer) {
        const auto [component_type, is_integer] = OutputAttrComponentType(ctx, attr);
        if (is_integer) {
            ctx.OpStore(pointer, ctx.OpBitcast(component_type, value));
        } else {
            ctx.OpStore(pointer, value);
        }
    };
    if (IR::IsParam(attr)) {
        const u32 attr_index{u32(attr) - u32(IR::Attribute::Param0)};
        if (ctx.hw_stage == HwStage::Local) {
            const auto component_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
            return op_store(ctx.OpAccessChain(component_ptr, ctx.output_attr_array,
                                              ctx.ConstU32(attr_index), ctx.ConstU32(element)));
        } else {
            const auto& info{ctx.output_params.at(attr_index)};
            ASSERT(info.num_components > 0);
            if (info.num_components == 1) {
                return op_store(info.id);
            } else {
                return op_store(
                    ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(element)));
            }
        }
    }
    if (IR::IsMrt(attr)) {
        const u32 index{u32(attr) - u32(IR::Attribute::RenderTarget0)};
        const auto& info{ctx.frag_outputs.at(index)};
        if (element < 3 && ctx.runtime_info.hw.fs.color_buffers[index].blend_self_scale) {
            // Emulates GCN's factor-scaled min/max blend: min/max(src*src, dst*dst).
            value = ctx.OpFMul(ctx.F32[1], value, value);
        }
        if (info.num_components == 1) {
            return op_store(info.id);
        } else {
            return op_store(ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(element)));
        }
    }
    switch (attr) {
    case IR::Attribute::Position0:
        return op_store(
            ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.ConstU32(element)));
    case IR::Attribute::ClipDistance:
        return op_store(
            ctx.OpAccessChain(ctx.output_f32, ctx.clip_distances, ctx.ConstU32(element)));
    case IR::Attribute::CullDistance:
        return op_store(
            ctx.OpAccessChain(ctx.output_f32, ctx.cull_distances, ctx.ConstU32(element)));
    case IR::Attribute::PointSize:
        return op_store(ctx.output_point_size);
    case IR::Attribute::RenderTargetIndex:
        return op_store(ctx.output_layer);
    case IR::Attribute::ViewportIndex:
        return op_store(ctx.output_viewport_index);
    case IR::Attribute::Depth:
        return op_store(ctx.frag_depth);
    case IR::Attribute::SampleMask:
        return op_store(ctx.OpAccessChain(ctx.output_u32, ctx.sample_mask, ctx.u32_zero_value));
    case IR::Attribute::StencilRef:
        if (ctx.profile.supports_shader_stencil_export) {
            return op_store(ctx.stencil_ref);
        }
        return;
    default:
        UNREACHABLE_MSG("Write attribute {}", attr);
    }
}

Id EmitGetTessGenericAttribute(EmitContext& ctx, Id vertex_index, Id attr_index, Id comp_index) {
    const auto attr_comp_ptr = ctx.TypePointer(spv::StorageClass::Input, ctx.F32[1]);
    return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(attr_comp_ptr, ctx.input_attr_array,
                                                    vertex_index, attr_index, comp_index));
}

Id EmitReadTcsGenericOuputAttribute(EmitContext& ctx, Id vertex_index, Id attr_index,
                                    Id comp_index) {
    const auto attr_comp_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
    return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(attr_comp_ptr, ctx.output_attr_array,
                                                    vertex_index, attr_index, comp_index));
}

void EmitSetTcsGenericAttribute(EmitContext& ctx, Id value, Id attr_index, Id comp_index) {
    // Implied vertex index is invocation_id
    const auto component_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
    Id pointer =
        ctx.OpAccessChain(component_ptr, ctx.output_attr_array,
                          ctx.OpLoad(ctx.U32[1], ctx.invocation_id), attr_index, comp_index);
    ctx.OpStore(pointer, value);
}

Id EmitGetPatch(EmitContext& ctx, IR::Patch patch) {
    const u32 index{IR::GenericPatchIndex(patch)};
    const Id element{ctx.ConstU32(IR::GenericPatchElement(patch))};
    const Id type{ctx.sw_stage == SwStage::TessellationControl ? ctx.output_f32 : ctx.input_f32};
    const Id pointer{ctx.OpAccessChain(type, ctx.patches.at(index), element)};
    return ctx.OpLoad(ctx.F32[1], pointer);
}

void EmitSetPatch(EmitContext& ctx, IR::Patch patch, Id value) {
    const Id pointer{[&] {
        if (IR::IsGeneric(patch)) {
            const u32 index{IR::GenericPatchIndex(patch)};
            const Id element{ctx.ConstU32(IR::GenericPatchElement(patch))};
            return ctx.OpAccessChain(ctx.output_f32, ctx.patches.at(index), element);
        }
        switch (patch) {
        case IR::Patch::TessellationLodLeft:
        case IR::Patch::TessellationLodRight:
        case IR::Patch::TessellationLodTop:
        case IR::Patch::TessellationLodBottom: {
            const u32 index{static_cast<u32>(patch) - u32(IR::Patch::TessellationLodLeft)};
            const Id index_id{ctx.ConstU32(index)};
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_outer, index_id);
        }
        case IR::Patch::TessellationLodInteriorU:
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_inner,
                                     ctx.u32_zero_value);
        case IR::Patch::TessellationLodInteriorV:
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_inner, ctx.ConstU32(1u));
        default:
            UNREACHABLE_MSG("Patch {}", u32(patch));
        }
    }()};
    ctx.OpStore(pointer, value);
}

template <u32 N, PointerType alias>
static Id EmitLoadBufferB32xN(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto& data_types = alias == PointerType::U32 ? ctx.U32 : ctx.F32;
    const auto [id, pointer_type] = spv_buffer.Alias(alias);

    boost::container::static_vector<Id, N> ids;
    for (u32 i = 0; i < N; i++) {
        const Id index_i = i == 0 ? address : ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(i));
        const Id ptr_i = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index_i);
        const Id result_i = ctx.OpLoad(data_types[1], ptr_i);
        ids.push_back(result_i);
    }

    const Id result = N == 1 ? ids[0] : ctx.OpCompositeConstruct(data_types[N], ids);
    return result;
}

Id EmitLoadBufferU8(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B8); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U8);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address)};
    const Id result{ctx.OpLoad(ctx.U8, ptr)};
    return result;
}

Id EmitLoadBufferU16(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B16); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U16);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address)};
    const Id result{ctx.OpLoad(ctx.U16, ptr)};
    return result;
}

Id EmitLoadBufferU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<1, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<2, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<3, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<4, PointerType::U32>(ctx, inst, handle, address);
}

Id EmitLoadBufferU64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B64); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U64);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u64_zero_value, address)};
    const Id result{ctx.OpLoad(ctx.U64, ptr)};
    return result;
}

Id EmitLoadBufferF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<1, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferF32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<2, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferF32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<3, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferF32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return EmitLoadBufferB32xN<4, PointerType::F32>(ctx, inst, handle, address);
}

Id EmitLoadBufferFormatF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    UNREACHABLE_MSG("SPIR-V instruction");
}

template <u32 N, PointerType alias>
static void EmitStoreBufferB32xN(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address,
                                 Id value) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto& data_types = alias == PointerType::U32 ? ctx.U32 : ctx.F32;
    const auto [id, pointer_type] = spv_buffer.Alias(alias);

    for (u32 i = 0; i < N; i++) {
        const Id index_i = i == 0 ? address : ctx.OpIAdd(ctx.U32[1], address, ctx.ConstU32(i));
        const Id ptr_i = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index_i);
        const Id value_i = N == 1 ? value : ctx.OpCompositeExtract(data_types[1], value, i);
        ctx.OpStore(ptr_i, value_i);
    }
}

void EmitStoreBufferU8(EmitContext& ctx, IR::Inst*, u32 handle, Id address, Id value) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B8); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U8);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address)};
    ctx.OpStore(ptr, value);
}

void EmitStoreBufferU16(EmitContext& ctx, IR::Inst*, u32 handle, Id address, Id value) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B16); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U16);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address)};
    ctx.OpStore(ptr, value);
}

void EmitStoreBufferU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<1, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<2, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<3, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<4, PointerType::U32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferU64(EmitContext& ctx, IR::Inst*, u32 handle, Id address, Id value) {
    const auto& spv_buffer = ctx.buffers[handle];
    if (const Id offset = spv_buffer.Offset(PointerSize::B64); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = spv_buffer.Alias(PointerType::U64);
    const Id ptr{ctx.OpAccessChain(pointer_type, id, ctx.u64_zero_value, address)};
    ctx.OpStore(ptr, value);
}

void EmitStoreBufferF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<1, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferF32x2(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<2, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferF32x3(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<3, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferF32x4(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    EmitStoreBufferB32xN<4, PointerType::F32>(ctx, inst, handle, address, value);
}

void EmitStoreBufferFormatF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    UNREACHABLE_MSG("SPIR-V instruction");
}

void EmitGetScalarRegister(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetScalarRegister(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVectorRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVectorRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVirtualRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVirtualRegister(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetGotoVariable(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetGotoVariable(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

} // namespace Shader::Backend::SPIRV
