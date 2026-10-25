// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include <cstdlib>
#include "common/div_ceil.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

namespace {
// Out of range workgroup memory reads give zero and writes do nothing, as on the console GPU. The
// host leaves them undefined, and AMD fills the tile borders of some compute passes with garbage
// (a lattice of white squares). Profile::lds_bounds_checks: on unless BB_LDS_BOUNDS=0 (vk_pipeline_cache.cpp).
bool BoundsChecked(const EmitContext& ctx) {
    return ctx.profile.lds_bounds_checks;
}

Id SharedLoad(EmitContext& ctx, Id offset, u32 shift, Id elem_type, Id ptr_type, Id var, Id zero) {
    if (!BoundsChecked(ctx)) {
        const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, ctx.ConstU32(shift))};
        return ctx.OpLoad(elem_type, ctx.EmitSharedMemoryAccess(ptr_type, var, index));
    }
    const u32 count = Common::DivCeil(ctx.shared_bytes, 1u << shift);
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, ctx.ConstU32(shift))};
    const Id in_range = ctx.OpULessThan(ctx.U1[1], index, ctx.ConstU32(count));
    const Id before = ctx.OpLabel(), load_label = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpBranch(before);
    ctx.AddLabel(before);
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(in_range, load_label, merge);
    ctx.AddLabel(load_label);
    const Id loaded = ctx.OpLoad(elem_type, ctx.EmitSharedMemoryAccess(ptr_type, var, index));
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
    return ctx.OpPhi(elem_type, loaded, load_label, zero, before);
}

void SharedStore(EmitContext& ctx, Id offset, u32 shift, Id ptr_type, Id var, Id value) {
    if (!BoundsChecked(ctx)) {
        const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, ctx.ConstU32(shift))};
        ctx.OpStore(ctx.EmitSharedMemoryAccess(ptr_type, var, index), value);
        return;
    }
    const u32 count = Common::DivCeil(ctx.shared_bytes, 1u << shift);
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, ctx.ConstU32(shift))};
    const Id in_range = ctx.OpULessThan(ctx.U1[1], index, ctx.ConstU32(count));
    const Id store_label = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(in_range, store_label, merge);
    ctx.AddLabel(store_label);
    ctx.OpStore(ctx.EmitSharedMemoryAccess(ptr_type, var, index), value);
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
}
} // namespace

Id EmitLoadSharedU16(EmitContext& ctx, Id offset) {
    return SharedLoad(ctx, offset, 1, ctx.U16, ctx.shared_u16, ctx.shared_memory_u16,
                      ctx.Constant(ctx.U16, u32{0}));
}

Id EmitLoadSharedU32(EmitContext& ctx, Id offset) {
    return SharedLoad(ctx, offset, 2, ctx.U32[1], ctx.shared_u32, ctx.shared_memory_u32,
                      ctx.u32_zero_value);
}

Id EmitLoadSharedU64(EmitContext& ctx, Id offset) {
    return SharedLoad(ctx, offset, 3, ctx.U64, ctx.shared_u64, ctx.shared_memory_u64,
                      ctx.Constant(ctx.U64, u64{0}));
}

void EmitWriteSharedU16(EmitContext& ctx, Id offset, Id value) {
    SharedStore(ctx, offset, 1, ctx.shared_u16, ctx.shared_memory_u16, value);
}

void EmitWriteSharedU32(EmitContext& ctx, Id offset, Id value) {
    SharedStore(ctx, offset, 2, ctx.shared_u32, ctx.shared_memory_u32, value);
}

void EmitWriteSharedU64(EmitContext& ctx, Id offset, Id value) {
    SharedStore(ctx, offset, 3, ctx.shared_u64, ctx.shared_memory_u64, value);
}

} // namespace Shader::Backend::SPIRV
