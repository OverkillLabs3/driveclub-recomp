// SPDX-License-Identifier: GPL-2.0-or-later
// VR (dcport-vr): see emit_spirv_vr.h.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_vr.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Backend::SPIRV::Vr {

bool MultiviewShaders() {
    static const bool on = [] {
        const char* v = std::getenv("BB_VR_STEREO");
        return v && std::strcmp(v, "mv") == 0;
    }();
    return on;
}

bool ArrayedImage(AmdGpu::ImageType view_type) {
    return MultiviewShaders() && view_type == AmdGpu::ImageType::Color2D;
}

bool EyeBufferArrays() {
    return MultiviewShaders();
}

void Define(EmitContext& ctx) {
    if (!MultiviewShaders() || ctx.sw_stage == SwStage::Compute) {
        return;
    }
    ctx.AddExtension("SPV_KHR_multiview");
    ctx.AddCapability(spv::Capability::MultiView);
    ctx.vr_view_index = ctx.DefineVariable(ctx.U32[1], spv::BuiltIn::ViewIndex, spv::StorageClass::Input);
    if (ctx.sw_stage == SwStage::Fragment) {
        ctx.Decorate(ctx.vr_view_index, spv::Decoration::Flat);
    }
    ctx.Name(ctx.vr_view_index, "vr_view_index");
}

namespace {

// The eye this invocation draws: the view index in graphics stages, push data vr_eye in compute.
Id Layer(EmitContext& ctx) {
    if (ctx.sw_stage == SwStage::Compute) {
        const Id pointer = ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]);
        return ctx.OpLoad(ctx.U32[1], ctx.OpAccessChain(pointer, ctx.push_data_block,
                                                        ctx.ConstU32(PushData::VrEyeIndex)));
    }
    return ctx.OpLoad(ctx.U32[1], ctx.vr_view_index);
}

} // namespace

void EyeBuffers(EmitContext& ctx) {
    if (!EyeBufferArrays()) {
        return;
    }
    // The block types as DefineBuffer made them (Sirit gives the same ids for the same declarations).
    const auto data_type = [&](u32 alias) -> Id {
        switch (EmitContext::PointerType(alias)) {
        case EmitContext::PointerType::U8:
            return ctx.U8;
        case EmitContext::PointerType::U16:
            return ctx.U16;
        case EmitContext::PointerType::U32:
            return ctx.U32[1];
        case EmitContext::PointerType::F32:
            return ctx.F32[1];
        case EmitContext::PointerType::U64:
            return ctx.U64;
        default:
            return Id{};
        }
    };
    Id eye{};
    for (auto& buffer : ctx.buffers) {
        if (buffer.buffer_type != BufferType::Guest) {
            continue;
        }
        for (u32 i = 0; i < buffer.aliases.size(); ++i) {
            auto& alias = buffer.aliases[i];
            const Id type = data_type(i);
            if (!Sirit::ValidId(alias.id) || !Sirit::ValidId(type)) {
                continue;
            }
            if (!Sirit::ValidId(eye)) {
                eye = Layer(ctx);
            }
            const Id block = ctx.TypeStruct(ctx.TypeRuntimeArray(type));
            alias.id = ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::StorageBuffer, block), alias.id, eye);
        }
    }
}

Id Coords(EmitContext& ctx, const EmitContext::TextureDefinition& texture, Id image, Id coords, bool integer) {
    if (!texture.vr_array) {
        return coords;
    }
    Id layer = Layer(ctx);
    if (!integer) {
        return ctx.OpCompositeConstruct(ctx.F32[3], ctx.OpCompositeExtract(ctx.F32[1], coords, 0),
                                        ctx.OpCompositeExtract(ctx.F32[1], coords, 1),
                                        ctx.OpConvertUToF(ctx.F32[1], layer));
    }
    // Fetches and storage access out of the layers are undefined: one-layer images give layer 0.
    if (!Sirit::ValidId(image)) {
        image = ctx.OpLoad(texture.image_type, texture.id);
    }
    const Id size = texture.is_storage ? ctx.OpImageQuerySize(ctx.U32[3], image)
                                       : ctx.OpImageQuerySizeLod(ctx.U32[3], image, ctx.u32_zero_value);
    const Id last = ctx.OpISub(ctx.U32[1], ctx.OpCompositeExtract(ctx.U32[1], size, 2), ctx.ConstU32(1U));
    layer = ctx.OpUMin(ctx.U32[1], layer, last);
    return ctx.OpCompositeConstruct(ctx.U32[3], ctx.OpCompositeExtract(ctx.U32[1], coords, 0),
                                    ctx.OpCompositeExtract(ctx.U32[1], coords, 1), layer);
}

namespace {

// ScanScreenParams' result for the shader being emitted (one shader per thread at a time).
struct ScreenParam {
    u32 index;   // output parameter
    u32 element; // its component
    float scale; // the output is scale * x / w + a constant
};
thread_local std::vector<ScreenParam> t_screen_params;

IR::Inst* Resolve(const IR::Value& value) {
    return value.IsImmediate() ? nullptr : value.TryInst();
}

// a / b as a * (1 / b) or a / b: {a, b}, else nulls.
std::pair<IR::Inst*, IR::Inst*> Divide(IR::Inst* inst) {
    if (!inst) {
        return {};
    }
    if (inst->GetOpcode() == IR::Opcode::FPDiv32) {
        return {Resolve(inst->Arg(0)), Resolve(inst->Arg(1))};
    }
    if (inst->GetOpcode() != IR::Opcode::FPMul32) {
        return {};
    }
    for (int i = 0; i < 2; ++i) {
        IR::Inst* recip = Resolve(inst->Arg(1 - i));
        if (recip && recip->GetOpcode() == IR::Opcode::FPRecip32) {
            return {Resolve(inst->Arg(i)), Resolve(recip->Arg(0))};
        }
    }
    return {};
}

// The eye's translation (ScanScreenParams): the position's vector, its matrix's rows x, y and w, and the outputs to
// evaluate again.
struct EyeOutput {
    IR::Attribute attr;
    u32 element;
    IR::Value value;
};
struct Coefficient {
    std::optional<IR::Value> value; // empty: 1
    bool negate;
};
struct EyeTranslation {
    bool valid = false;
    IR::Inst* leaf[3]{};
    Coefficient row[3][3]; // position x, y, w by leaf
    IR::Inst* position_w = nullptr;
    std::vector<EyeOutput> outputs;
    std::unordered_map<const IR::Inst*, u8> flags; // Analysis bits
};
thread_local EyeTranslation t_eye;

constexpr u8 DependsOnV = 1, Projective = 2, Unsupported = 4, Ray = 8;

struct Term {
    IR::Value a, b; // a * b, or a alone (b empty)
    bool negate;
};

// `value` as a sum of products (sums, differences, negations, multiplies and fused multiply-adds).
void LinearTerms(const IR::Value& value, bool negate, std::vector<Term>& terms, int depth = 0) {
    IR::Inst* inst = Resolve(value);
    if (inst && depth < 16) {
        switch (inst->GetOpcode()) {
        case IR::Opcode::FPAdd32:
            LinearTerms(inst->Arg(0), negate, terms, depth + 1);
            LinearTerms(inst->Arg(1), negate, terms, depth + 1);
            return;
        case IR::Opcode::FPSub32:
            LinearTerms(inst->Arg(0), negate, terms, depth + 1);
            LinearTerms(inst->Arg(1), !negate, terms, depth + 1);
            return;
        case IR::Opcode::FPNeg32:
            LinearTerms(inst->Arg(0), !negate, terms, depth + 1);
            return;
        case IR::Opcode::FPFma32:
            terms.push_back({inst->Arg(0), inst->Arg(1), negate});
            LinearTerms(inst->Arg(2), negate, terms, depth + 1);
            return;
        case IR::Opcode::FPMul32:
            terms.push_back({inst->Arg(0), inst->Arg(1), negate});
            return;
        default:
            break;
        }
    }
    terms.push_back({value, IR::Value{}, negate});
}

bool SupportedOp(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPFma32:
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPDiv32:
    case IR::Opcode::FPNeg32:
    case IR::Opcode::FPAbs32:
    case IR::Opcode::FPRecip32:
    case IR::Opcode::FPRecipSqrt32:
    case IR::Opcode::FPSqrt:
    case IR::Opcode::FPMax32:
    case IR::Opcode::FPMin32:
    case IR::Opcode::FPSaturate32:
    case IR::Opcode::FPClamp32:
    case IR::Opcode::SelectF32:
    case IR::Opcode::BitCastF32U32:
    case IR::Opcode::BitCastU32F32:
    case IR::Opcode::LogicalAnd:
    case IR::Opcode::LogicalOr:
    case IR::Opcode::LogicalNot:
    case IR::Opcode::FPOrdEqual32:
    case IR::Opcode::FPUnordEqual32:
    case IR::Opcode::FPOrdNotEqual32:
    case IR::Opcode::FPUnordNotEqual32:
    case IR::Opcode::FPOrdLessThan32:
    case IR::Opcode::FPUnordLessThan32:
    case IR::Opcode::FPOrdGreaterThan32:
    case IR::Opcode::FPUnordGreaterThan32:
    case IR::Opcode::FPOrdLessThanEqual32:
    case IR::Opcode::FPUnordLessThanEqual32:
    case IR::Opcode::FPOrdGreaterThanEqual32:
    case IR::Opcode::FPUnordGreaterThanEqual32:
        return true;
    default:
        return false;
    }
}

// DependsOnV / Projective (a division by something that depends on v) / Unsupported (an operation on v this cannot
// evaluate again).
u8 Analyse(EyeTranslation& eye, const IR::Value& value) {
    IR::Inst* inst = Resolve(value);
    if (!inst) {
        return 0;
    }
    for (IR::Inst* leaf : eye.leaf) {
        if (inst == leaf) {
            return DependsOnV;
        }
    }
    if (const auto it = eye.flags.find(inst); it != eye.flags.end()) {
        return it->second;
    }
    eye.flags[inst] = 0; // cycles (phis) end here
    u8 args = 0, flags = 0;
    u8 arg_flags[4]{};
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        const u8 f = Analyse(eye, inst->Arg(i));
        if (i < 4) {
            arg_flags[i] = f;
        }
        args |= f;
    }
    flags = args & (Projective | Unsupported | Ray);
    if (args & DependsOnV) {
        flags |= DependsOnV;
        const auto op = inst->GetOpcode();
        if (!SupportedOp(op)) {
            flags |= Unsupported;
        }
        if ((op == IR::Opcode::FPDiv32 && (arg_flags[1] & DependsOnV)) || op == IR::Opcode::FPRecip32 ||
            op == IR::Opcode::FPRecipSqrt32) {
            flags |= Projective;
            // Not by the position's w: a view ray or a view vector (the screen position alone is the clip shift's)
            if (Resolve(inst->Arg(op == IR::Opcode::FPDiv32 ? 1 : 0)) != eye.position_w) {
                flags |= Ray;
            }
        }
    }
    eye.flags[inst] = flags;
    return flags;
}

// The eye's translation for the program, or why not.
const char* ScanEyeTranslation(const IR::Program& program, EyeTranslation& eye) {
    IR::Value pos[4];
    bool have_pos[4]{};
    std::vector<EyeOutput> params;
    std::unordered_set<u32> stored;
    const IR::Block* last = nullptr;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::Epilogue) {
                last = block;
            }
        }
    }
    // The outputs' block: the epilogue's, or its only predecessor (their values then reach the epilogue).
    const IR::Block* outputs_block = last;
    if (last && last->ImmPredecessors().size() == 1) {
        bool stores_here = false;
        for (const IR::Inst& inst : last->Instructions()) {
            stores_here |= inst.GetOpcode() == IR::Opcode::SetAttribute;
        }
        if (!stores_here) {
            outputs_block = last->ImmPredecessors()[0];
        }
    }
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            const IR::Attribute attr = inst.Arg(0).Attribute();
            const u32 element = inst.Arg(2).U32();
            const bool position = attr == IR::Attribute::Position0;
            if (!position && !IR::IsParam(attr)) {
                continue;
            }
            if (block != outputs_block) {
                return "outputs in a branch";
            }
            if (!stored.insert(u32(attr) * 8 + element).second) {
                return "an output stored twice";
            }
            if (position && element < 4) {
                pos[element] = inst.Arg(1);
                have_pos[element] = true;
            } else if (!position) {
                params.push_back({attr, element, inst.Arg(1)});
            }
        }
    }
    if (!last || !have_pos[0] || !have_pos[1] || !have_pos[2] || !have_pos[3]) {
        return "no position";
    }
    // v: the values multiplied in each of x, y and w.
    const int rows[3] = {0, 1, 3};
    std::vector<Term> terms[3];
    std::vector<IR::Inst*> common;
    for (int r = 0; r < 3; ++r) {
        LinearTerms(pos[rows[r]], false, terms[r]);
        std::vector<IR::Inst*> factors;
        for (const Term& t : terms[r]) {
            for (const IR::Value* f : {&t.a, &t.b}) {
                if (IR::Inst* inst = f->IsEmpty() ? nullptr : Resolve(*f)) {
                    factors.push_back(inst);
                }
            }
        }
        if (r == 0) {
            common = factors;
        } else {
            std::erase_if(common, [&](IR::Inst* inst) {
                return std::find(factors.begin(), factors.end(), inst) == factors.end();
            });
        }
    }
    std::sort(common.begin(), common.end());
    common.erase(std::unique(common.begin(), common.end()), common.end());
    if (common.size() != 3) {
        return "no matrix times a 3-vector";
    }
    std::copy(common.begin(), common.end(), eye.leaf);
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            int found = 0;
            for (const Term& t : terms[r]) {
                const bool a = !t.a.IsEmpty() && Resolve(t.a) == eye.leaf[k];
                const bool b = !t.b.IsEmpty() && Resolve(t.b) == eye.leaf[k];
                if (a == b) {
                    if (a) {
                        return "v squared";
                    }
                    continue;
                }
                const IR::Value& other = a ? t.b : t.a;
                if (!other.IsEmpty() && (Analyse(eye, other) & DependsOnV)) {
                    return "a coefficient depends on v";
                }
                eye.row[r][k] = {other.IsEmpty() ? std::nullopt : std::optional<IR::Value>{other}, t.negate};
                ++found;
            }
            if (found != 1) {
                return "v used twice in a row";
            }
        }
    }
    eye.position_w = Resolve(pos[3]);
    eye.flags.clear(); // Ray depends on the position's w
    for (int e = 0; e < 4; ++e) {
        if (Analyse(eye, pos[e]) & Unsupported) {
            return "position unsupported";
        }
        eye.outputs.push_back({IR::Attribute::Position0, u32(e), pos[e]});
    }
    u32 view_dependent = 0, rays = 0;
    for (const EyeOutput& out : params) {
        const u8 f = Analyse(eye, out.value);
        if ((f & DependsOnV) && (f & Projective)) {
            if (f & Unsupported) {
                return "output unsupported";
            }
            eye.outputs.push_back(out);
            ++view_dependent;
            rays += (f & Ray) != 0;
        }
    }
    // Only shaders that pass view rays on (the deferred lights): screen positions alone keep the clip shift and the
    // x / w outputs' shift (translated geometry shaders made the rear lights and shadows noisy).
    if (!rays) {
        return "no view-dependent outputs";
    }
    if (!view_dependent) {
        return "no view-dependent outputs";
    }
    eye.valid = true;
    return nullptr;
}

// `inst` as scale * (a / b) + c with constant scale and c: the divide and the scale.
std::pair<IR::Inst*, float> ScaledDivide(IR::Inst* inst) {
    if (!inst) {
        return {};
    }
    if (Divide(inst).first) {
        return {inst, 1.f};
    }
    const auto op = inst->GetOpcode();
    if (op == IR::Opcode::FPMul32 || op == IR::Opcode::FPFma32) {
        for (int i = 0; i < 2; ++i) {
            IR::Inst* div = Resolve(inst->Arg(i));
            const IR::Value other = inst->Arg(1 - i);
            if (Divide(div).first && other.IsImmediate() &&
                (op == IR::Opcode::FPMul32 || inst->Arg(2).IsImmediate())) {
                return {div, other.F32()};
            }
        }
    } else if (op == IR::Opcode::FPAdd32) {
        for (int i = 0; i < 2; ++i) {
            if (inst->Arg(1 - i).IsImmediate()) {
                return ScaledDivide(Resolve(inst->Arg(i)));
            }
        }
    }
    return {};
}

} // namespace

void ScanScreenParams(const IR::Program& program) {
    t_screen_params.clear();
    t_eye = EyeTranslation{};
    const Info& info = program.info;
    if (!MultiviewShaders() || info.hw_stage != HwStage::Vertex || info.sw_stage != SwStage::Vertex) {
        return;
    }
    static const bool eye_off = [] {
        const char* v = std::getenv("BB_VR_EYE_TRANSLATION"); // development: 0 = the clip shift only
        return v && v[0] == '0';
    }();
    static const bool log_eye = std::getenv("BB_VR_LOG_SCREEN_PARAMS") != nullptr;
    if (!eye_off) {
        const char* why = ScanEyeTranslation(program, t_eye);
        if (log_eye && (!why || std::strcmp(why, "no view-dependent outputs") != 0)) {
            std::printf("VR eye translation: vs %#llx %s (%zu outputs)\n", (unsigned long long)info.pgm_hash,
                        why ? why : "yes", t_eye.outputs.size());
            std::fflush(stdout);
        }
        if (!why) {
            return; // the screen positions are evaluated again too
        }
        t_eye = EyeTranslation{};
    }
    IR::Inst* pos[4]{};
    std::vector<std::pair<const IR::Inst*, IR::Inst*>> params; // (store, value) of each output parameter
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            const IR::Attribute attr = inst.Arg(0).Attribute();
            const u32 element = inst.Arg(2).U32();
            if (attr == IR::Attribute::Position0 && element < 4) {
                pos[element] = Resolve(inst.Arg(1));
            } else if (IR::IsParam(attr)) {
                params.emplace_back(&inst, Resolve(inst.Arg(1)));
            }
        }
    }
    if (!pos[0] || !pos[3]) {
        return;
    }
    std::vector<ScreenParam> found;
    u32 others = 0; // outputs divided by something else (view rays, another frame's position)
    for (const auto& [store, value] : params) {
        const auto [div, scale] = ScaledDivide(value);
        if (!div) {
            continue;
        }
        const auto [a, b] = Divide(div);
        others += b != pos[3];
        if (b == pos[3] && a == pos[0]) {
            found.push_back({u32(store->Arg(0).Attribute()) - u32(IR::Attribute::Param0), store->Arg(2).U32(), scale});
        }
    }
    static const bool off = std::getenv("BB_VR_NO_SCREEN_PARAMS") != nullptr; // development: A/B
    if (off) {
        return;
    }
    static const bool log = std::getenv("BB_VR_LOG_SCREEN_PARAMS") != nullptr;
    if (log && !found.empty()) {
        std::printf("VR screen params: vs %#llx param %u.%u scale %g (%zu; %u other divides)\n",
                    (unsigned long long)info.pgm_hash, found[0].index, found[0].element, found[0].scale, found.size(),
                    others);
        std::fflush(stdout);
    }
    t_screen_params = std::move(found);
}

namespace {

// `value` evaluated again with v replaced (EyeTranslation).
Id EvaluateAgain(EmitContext& ctx, const IR::Value& value, const Id (&leaf)[3], std::unordered_map<IR::Inst*, Id>& done) {
    IR::Inst* inst = Resolve(value);
    if (!inst || !(t_eye.flags.count(inst) ? t_eye.flags[inst] & DependsOnV : true)) {
        return ctx.Def(value);
    }
    for (int k = 0; k < 3; ++k) {
        if (inst == t_eye.leaf[k]) {
            return leaf[k];
        }
    }
    if (const auto it = done.find(inst); it != done.end()) {
        return it->second;
    }
    const auto arg = [&](size_t i) { return EvaluateAgain(ctx, inst->Arg(i), leaf, done); };
    Id id{};
    switch (inst->GetOpcode()) {
    case IR::Opcode::FPAdd32:
        id = EmitFPAdd32(ctx, inst, arg(0), arg(1));
        break;
    case IR::Opcode::FPSub32:
        id = EmitFPSub32(ctx, inst, arg(0), arg(1));
        break;
    case IR::Opcode::FPFma32:
        id = EmitFPFma32(ctx, inst, arg(0), arg(1), arg(2));
        break;
    case IR::Opcode::FPMul32:
        id = EmitFPMul32(ctx, inst, arg(0), arg(1));
        break;
    case IR::Opcode::FPDiv32:
        id = EmitFPDiv32(ctx, inst, arg(0), arg(1));
        break;
    case IR::Opcode::FPNeg32:
        id = EmitFPNeg32(ctx, arg(0));
        break;
    case IR::Opcode::FPAbs32:
        id = EmitFPAbs32(ctx, arg(0));
        break;
    case IR::Opcode::FPRecip32:
        id = EmitFPRecip32(ctx, arg(0));
        break;
    case IR::Opcode::FPRecipSqrt32:
        id = EmitFPRecipSqrt32(ctx, arg(0));
        break;
    case IR::Opcode::FPSqrt:
        id = EmitFPSqrt(ctx, arg(0));
        break;
    case IR::Opcode::FPMax32:
        id = EmitFPMax32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPMin32:
        id = EmitFPMin32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPSaturate32:
        id = EmitFPSaturate32(ctx, arg(0));
        break;
    case IR::Opcode::FPClamp32:
        id = EmitFPClamp32(ctx, arg(0), arg(1), arg(2));
        break;
    case IR::Opcode::SelectF32:
        id = EmitSelectF32(ctx, arg(0), arg(1), arg(2));
        break;
    case IR::Opcode::BitCastF32U32:
        id = EmitBitCastF32U32(ctx, arg(0));
        break;
    case IR::Opcode::BitCastU32F32:
        id = EmitBitCastU32F32(ctx, arg(0));
        break;
    case IR::Opcode::LogicalAnd:
        id = EmitLogicalAnd(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::LogicalOr:
        id = EmitLogicalOr(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::LogicalNot:
        id = EmitLogicalNot(ctx, arg(0));
        break;
    case IR::Opcode::FPOrdEqual32:
        id = EmitFPOrdEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPUnordEqual32:
        id = EmitFPUnordEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPOrdNotEqual32:
        id = EmitFPOrdNotEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPUnordNotEqual32:
        id = EmitFPUnordNotEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPOrdLessThan32:
        id = EmitFPOrdLessThan32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPUnordLessThan32:
        id = EmitFPUnordLessThan32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPOrdGreaterThan32:
        id = EmitFPOrdGreaterThan32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPUnordGreaterThan32:
        id = EmitFPUnordGreaterThan32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPOrdLessThanEqual32:
        id = EmitFPOrdLessThanEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPUnordLessThanEqual32:
        id = EmitFPUnordLessThanEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPOrdGreaterThanEqual32:
        id = EmitFPOrdGreaterThanEqual32(ctx, arg(0), arg(1));
        break;
    case IR::Opcode::FPUnordGreaterThanEqual32:
        id = EmitFPUnordGreaterThanEqual32(ctx, arg(0), arg(1));
        break;
    default:
        id = ctx.Def(value); // not reached (ScanEyeTranslation)
        break;
    }
    done.emplace(inst, id);
    return id;
}

// The eye's translation: v moved by -e (M e = (-eye_shift, 0, *, 0)), the position and the view-dependent outputs
// evaluated again.
void TranslateEye(EmitContext& ctx, Id eye_shift) {
    Id row[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            const Coefficient& c = t_eye.row[r][k];
            Id id = c.value ? ctx.Def(*c.value) : ctx.ConstF32(1.f);
            row[r][k] = c.negate ? ctx.OpFNegate(ctx.F32[1], id) : id;
        }
    }
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    // d = row y x row w: no change in y or w; e = d * t with row x . e = -eye_shift.
    const Id(&y)[3] = row[1];
    const Id(&w)[3] = row[2];
    const Id d[3] = {sub(mul(y[1], w[2]), mul(y[2], w[1])), sub(mul(y[2], w[0]), mul(y[0], w[2])),
                     sub(mul(y[0], w[1]), mul(y[1], w[0]))};
    const Id denom = add(add(mul(row[0][0], d[0]), mul(row[0][1], d[1])), mul(row[0][2], d[2]));
    const Id usable = ctx.OpFOrdGreaterThan(ctx.U1[1], EmitFPAbs32(ctx, denom), ctx.ConstF32(1e-30f));
    const Id t = ctx.OpSelect(ctx.F32[1], usable, ctx.OpFDiv(ctx.F32[1], eye_shift, denom), ctx.f32_zero_value);
    Id leaf[3];
    for (int k = 0; k < 3; ++k) {
        leaf[k] = add(ctx.Def(IR::Value{t_eye.leaf[k]}), mul(d[k], t)); // v - e
    }
    std::unordered_map<IR::Inst*, Id> done;
    std::vector<std::pair<Id, Id>> stores; // evaluated before any store
    for (const EyeOutput& out : t_eye.outputs) {
        Id pointer{};
        if (out.attr == IR::Attribute::Position0) {
            pointer = ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.ConstU32(out.element));
        } else {
            const auto& info = ctx.output_params.at(u32(out.attr) - u32(IR::Attribute::Param0));
            if (!Sirit::ValidId(info.id) || out.element >= info.num_components) {
                continue;
            }
            pointer = info.num_components == 1
                          ? info.id
                          : ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(out.element));
        }
        stores.emplace_back(pointer, EvaluateAgain(ctx, out.value, leaf, done));
    }
    for (const auto& [pointer, value] : stores) {
        ctx.OpStore(pointer, value);
    }
}

// The position's x plus vr_shift[view] when w is not 1, in multiview pipelines (specialisation constant 200); outputs
// holding x / w (ScanScreenParams) plus scale * vr_shift[view] / w. With the eye's translation, those evaluated again.
void ShiftPosition(EmitContext& ctx) {
    const Id pointer = ctx.TypePointer(spv::StorageClass::PushConstant, ctx.F32[1]);
    const Id shift = ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(pointer, ctx.push_data_block,
                                                              ctx.ConstU32(PushData::VrShiftIndex),
                                                              ctx.OpLoad(ctx.U32[1], ctx.vr_view_index)));
    const Id x_pointer = ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.ConstU32(0U));
    const Id w = ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(ctx.output_f32, ctx.output_position, ctx.ConstU32(3U)));
    // Specialisation constant 200: the pipeline is the multiview twin (mono passes keep their positions).
    if (!Sirit::ValidId(ctx.vr_multiview)) {
        ctx.vr_multiview = ctx.SpecConstantFalse(ctx.U1[1]);
        ctx.Decorate(ctx.vr_multiview, spv::Decoration::SpecId, 200U);
        ctx.Name(ctx.vr_multiview, "vr_multiview");
    }
    const Id multiview = ctx.vr_multiview;
    const Id perspective =
        ctx.OpLogicalAnd(ctx.U1[1], multiview, ctx.OpFOrdNotEqual(ctx.U1[1], w, ctx.ConstF32(1.f)));
    const Id x = ctx.OpLoad(ctx.F32[1], x_pointer);
    const Id eye_shift = ctx.OpSelect(ctx.F32[1], perspective, shift, ctx.f32_zero_value);
    if (ctx.sw_stage == SwStage::Vertex && t_eye.valid) {
        TranslateEye(ctx, eye_shift);
        return;
    }
    ctx.OpStore(x_pointer, ctx.OpFAdd(ctx.F32[1], x, eye_shift));
    if (ctx.sw_stage != SwStage::Vertex) {
        return;
    }
    for (const ScreenParam& param : t_screen_params) {
        const auto& out = ctx.output_params.at(param.index);
        if (!Sirit::ValidId(out.id) || param.element >= out.num_components) {
            continue;
        }
        const Id pointer =
            out.num_components == 1 ? out.id : ctx.OpAccessChain(out.pointer_type, out.id, ctx.ConstU32(param.element));
        const Id moved = ctx.OpFMul(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], eye_shift, w), ctx.ConstF32(param.scale));
        ctx.OpStore(pointer, ctx.OpFAdd(ctx.F32[1], ctx.OpLoad(ctx.F32[1], pointer), moved));
    }
}

} // namespace

void Epilogue(EmitContext& ctx) {
    // The stage that feeds the rasteriser: the hardware VS (a vertex or tessellation evaluation shader).
    if (!MultiviewShaders() || ctx.hw_stage != HwStage::Vertex ||
        (ctx.sw_stage != SwStage::Vertex && ctx.sw_stage != SwStage::TessellationEval) ||
        ctx.runtime_info.hw.vs.clip_disable || !Sirit::ValidId(ctx.output_position)) {
        return;
    }
    static const bool no_tess = std::getenv("BB_VR_NO_TESS_SHIFT") != nullptr; // development: A/B
    if (no_tess && ctx.sw_stage == SwStage::TessellationEval) {
        return;
    }
    ShiftPosition(ctx);
}

void BeforeEmitVertex(EmitContext& ctx) {
    if (!MultiviewShaders() || ctx.sw_stage != SwStage::Geometry || !Sirit::ValidId(ctx.output_position)) {
        return;
    }
    ShiftPosition(ctx);
}

} // namespace Shader::Backend::SPIRV::Vr
