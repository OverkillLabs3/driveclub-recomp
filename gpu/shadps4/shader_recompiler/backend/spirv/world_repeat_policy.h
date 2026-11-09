// SPDX-License-Identifier: GPL-2.0-or-later
// DriveClub repeat-layer classifiers. No texture names or IDs.
#pragma once
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/program.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Shader::Backend::SPIRV {
namespace WorldRepeat {
using IR::Inst;
using IR::Opcode;
using IR::Value;
inline thread_local std::unordered_set<const Inst*> selected;
inline bool IsSample(const Inst* i) {
    return i->GetOpcode() == Opcode::ImageSampleImplicitLod ||
           i->GetOpcode() == Opcode::ImageGradient;
}
// Track components separately: alpha-only blend/height/mask dependencies must
// not turn a texture into an albedo or normal layer. Do not cross another
// texture read.
using Roles = std::map<const Inst*, unsigned>;
inline void Trace(Value v, int component, Roles& roles,
                  std::set<std::pair<const Inst*, int>>& seen) {
    const Inst* i = v.TryInst();
    if (!i || !seen.emplace(i, component).second)
        return;
    if (IsSample(i)) {
        roles[i] |= component < 0 ? 15u : 1u << component;
        return;
    }
    const auto op = i->GetOpcode();
    const std::string name{IR::NameOf(op)};
    if (name.starts_with("Image"))
        return;
    if (name.starts_with("CompositeExtract")) {
        Trace(i->Arg(0), int(i->Arg(1).U32()), roles, seen);
        return;
    }
    if (name.starts_with("CompositeConstruct") && component >= 0) {
        if (size_t(component) < i->NumArgs())
            Trace(i->Arg(component), -1, roles, seen);
        return;
    }
    for (size_t a = 0; a < i->NumArgs(); ++a)
        Trace(i->Arg(a), -1, roles, seen);
}
struct UV {
    bool valid = false, attribute = false, uniform = false, scaled = false;
    std::string key;
};
inline UV Describe(Value value, std::unordered_map<const Inst*, UV>& memo, unsigned depth = 0) {
    const Inst* i = value.TryInst();
    if (!i)
        return {true, false, false, false,
                std::to_string(unsigned(value.Type())) + ":" + std::to_string(value.Hash())};
    if (depth > 128)
        return {};
    if (auto it = memo.find(i); it != memo.end())
        return it->second;
    const auto op = i->GetOpcode();
    UV out{true, false, false, false,
           std::string(IR::NameOf(op)) + ":" + std::to_string(i->Flags<u64>()) + "("};
    if (op == Opcode::GetAttribute) {
        const auto attr = i->Arg(0).Attribute();
        out.attribute = IR::IsParam(attr) || IR::IsBarycentricCoord(attr);
        out.valid = out.attribute;
    } else if (op == Opcode::ReadConstBuffer) {
        out.uniform = true;
        // Dynamic/texture-driven addressing is not an affine material UV scale.
        for (size_t a = 0; a < i->NumArgs(); ++a)
            if (!i->Arg(a).IsImmediate())
                out.valid = false;
    } else {
        switch (op) {
        case Opcode::CompositeConstructF32x2:
        case Opcode::CompositeConstructF32x3:
        case Opcode::CompositeConstructF32x4:
        case Opcode::CompositeExtractF32x2:
        case Opcode::CompositeExtractF32x3:
        case Opcode::CompositeExtractF32x4:
        case Opcode::FPAdd32:
        case Opcode::FPSub32:
        case Opcode::FPMul32:
        case Opcode::FPFma32:
        case Opcode::FPNeg32:
        case Opcode::FPRecip32:
        case Opcode::BitCastF32U32:
        case Opcode::BitCastU32F32:
            break;
        default:
            out.valid = false;
            break;
        }
    }
    // Fail closed immediately on nonlinear/procedural coordinates. Expanding an
    // unsupported SSA DAG as a string can otherwise grow exponentially.
    if (!out.valid) {
        memo.emplace(i, UV{});
        return {};
    }
    std::array<UV, 6> children{};
    for (size_t a = 0; a < i->NumArgs(); ++a) {
        children[a] = Describe(i->Arg(a), memo, depth + 1);
        const auto& child = children[a];
        if (!child.valid || out.key.size() + child.key.size() > 32768) {
            memo.emplace(i, UV{});
            return {};
        }
        out.attribute |= child.attribute;
        out.uniform |= child.uniform;
        out.scaled |= child.scaled;
        out.key += child.key + ",";
    }
    if (op == Opcode::FPRecip32 && out.attribute)
        out.valid = false;
    if (op == Opcode::FPMul32 || op == Opcode::FPFma32) {
        const auto& a = children[0];
        const auto& b = children[1];
        out.scaled |= (a.attribute && b.uniform && !b.attribute) ||
                      (b.attribute && a.uniform && !a.attribute);
    }
    out.key += ")";
    memo.emplace(i, out);
    return out;
}
// Interior coordinates must stay affine in the interpolated material UV.
// The native IR expands interpolation into Param differences times barycentrics;
// allow that construction, but not arbitrary products of varying coordinates.
struct InteriorUV {
    bool valid = true, param = false, bary = false;
};
inline InteriorUV InteriorAffine(Value value, std::unordered_map<const Inst*, InteriorUV>& memo,
                                 unsigned depth = 0) {
    const auto* i = value.TryInst();
    if (!i)
        return {};
    if (depth > 128)
        return {false};
    if (const auto it = memo.find(i); it != memo.end())
        return it->second;
    InteriorUV out;
    const auto op = i->GetOpcode();
    if (op == Opcode::GetAttribute) {
        out.param = IR::IsParam(i->Arg(0).Attribute());
        out.bary = IR::IsBarycentricCoord(i->Arg(0).Attribute());
        out.valid = out.param || out.bary;
    } else if (op == Opcode::ReadConstBuffer) {
        for (size_t a = 0; a < i->NumArgs(); ++a)
            out.valid &= i->Arg(a).IsImmediate();
    } else {
        switch (op) {
        case Opcode::CompositeConstructF32x2:
        case Opcode::CompositeConstructF32x3:
        case Opcode::CompositeConstructF32x4:
        case Opcode::CompositeExtractF32x2:
        case Opcode::CompositeExtractF32x3:
        case Opcode::CompositeExtractF32x4:
        case Opcode::FPAdd32:
        case Opcode::FPSub32:
        case Opcode::FPMul32:
        case Opcode::FPFma32:
        case Opcode::FPNeg32:
        case Opcode::FPRecip32:
        case Opcode::BitCastF32U32:
        case Opcode::BitCastU32F32:
            break;
        default:
            return {false};
        }
    }
    std::array<InteriorUV, 6> children{};
    for (size_t a = 0; a < i->NumArgs(); ++a) {
        children[a] = InteriorAffine(i->Arg(a), memo, depth + 1);
        out.valid &= children[a].valid;
        out.param |= children[a].param;
        out.bary |= children[a].bary;
    }
    if (op == Opcode::FPRecip32 && (out.param || out.bary))
        out.valid = false;
    if (op == Opcode::FPMul32 || op == Opcode::FPFma32) {
        const auto& a = children[0];
        const auto& b = children[1];
        const bool both_vary = (a.param || a.bary) && (b.param || b.bary);
        const bool interpolation = (a.param && !a.bary && b.bary && !b.param) ||
                                   (b.param && !b.bary && a.bary && !a.param);
        if (both_vary && !interpolation)
            out.valid = false;
    }
    memo.emplace(i, out);
    return out;
}
inline bool FloatIs(Value v, float f) {
    return v.IsImmediate() && v.Type() == IR::Type::F32 && v.F32() == f;
}
// Recognize the RG * 2 - 1 normal decoding seen in native fabric programs.
// Stopping at a decode also lets the caller reject any parallel raw/alpha path
// from that same sample into the normal output.
inline const Inst* SignedRGDecode(const Inst* i, unsigned& component) {
    Value channel;
    if (i->GetOpcode() == Opcode::FPFma32 && FloatIs(i->Arg(2), -1.f)) {
        if (FloatIs(i->Arg(0), 2.f))
            channel = i->Arg(1);
        else if (FloatIs(i->Arg(1), 2.f))
            channel = i->Arg(0);
    } else if (i->GetOpcode() == Opcode::FPAdd32) {
        const auto* mul = (FloatIs(i->Arg(0), -1.f) ? i->Arg(1) :
                          FloatIs(i->Arg(1), -1.f) ? i->Arg(0) : Value()).TryInst();
        if (mul && mul->GetOpcode() == Opcode::FPMul32) {
            if (FloatIs(mul->Arg(0), 2.f))
                channel = mul->Arg(1);
            else if (FloatIs(mul->Arg(1), 2.f))
                channel = mul->Arg(0);
        }
    }
    const auto* extract = channel.TryInst();
    if (!extract || extract->GetOpcode() != Opcode::CompositeExtractF32x4 ||
        !extract->Arg(1).IsImmediate() || extract->Arg(1).U32() > 1)
        return nullptr;
    const auto* sample = extract->Arg(0).TryInst();
    if (!sample || !IsSample(sample))
        return nullptr;
    component = extract->Arg(1).U32();
    return sample;
}
inline void TraceDecodedNormal(Value v, int component, Roles& decoded, Roles& raw,
                               std::set<std::pair<const Inst*, int>>& seen) {
    const auto* i = v.TryInst();
    if (!i || !seen.emplace(i, component).second)
        return;
    unsigned channel = 0;
    if (const auto* sample = SignedRGDecode(i, channel)) {
        decoded[sample] |= 1u << channel;
        return;
    }
    if (IsSample(i)) {
        raw[i] |= component < 0 ? 15u : 1u << component;
        return;
    }
    const std::string name{IR::NameOf(i->GetOpcode())};
    if (name.starts_with("Image"))
        return;
    if (name.starts_with("CompositeExtract")) {
        TraceDecodedNormal(i->Arg(0), int(i->Arg(1).U32()), decoded, raw, seen);
        return;
    }
    if (name.starts_with("CompositeConstruct") && component >= 0) {
        if (size_t(component) < i->NumArgs())
            TraceDecodedNormal(i->Arg(component), -1, decoded, raw, seen);
        return;
    }
    for (size_t a = 0; a < i->NumArgs(); ++a)
        TraceDecodedNormal(i->Arg(a), -1, decoded, raw, seen);
}
inline bool RepeatImage(const Info& info, u32 handle) {
    const auto& image = info.images[handle & 65535];
    const auto sharp = image.GetSharp(info);
    const auto& sampler = info.samplers[handle >> 16];
    const auto samp = sampler.GetSharp(info);
    const auto repeat = [](auto x) {
        return x == AmdGpu::ClampMode::Wrap || x == AmdGpu::ClampMode::Mirror;
    };
    const auto type = sharp.GetViewType(image.is_array);
    return !image.is_written && !sampler.is_depth && !samp.force_unnormalized &&
           sharp.NumLevels() > 1 &&
           (type == AmdGpu::ImageType::Color2D || type == AmdGpu::ImageType::Color2DArray) &&
           !sharp.IsCube() && !AmdGpu::IsInteger(sharp.GetNumberFmt()) && repeat(samp.clamp_x) &&
           repeat(samp.clamp_y);
}
inline void Prepare(const IR::Program& program) {
    selected.clear();
    const auto& info = program.info;
    if (info.sw_stage != SwStage::Fragment)
        return;
    static constexpr u64 programs[] = {
#include "world_repeat_programs.inc"
    };
    static constexpr u64 interior_programs[] = {
#include "interior_repeat_programs.inc"
    };
    const bool world = std::binary_search(std::begin(programs), std::end(programs), info.pgm_hash);
    const bool interior = std::binary_search(std::begin(interior_programs),
                                           std::end(interior_programs), info.pgm_hash);
    if (!world && !interior)
        return;
    std::array<Roles, 3> roles;
    Roles all_roles, decoded_normals, raw_normals, decoded_outputs, raw_outputs;
    std::vector<const Inst*> samples;
    for (const auto* block : program.blocks)
        for (const auto& i : block->Instructions()) {
            if (i.GetOpcode() == Opcode::Discard || i.GetOpcode() == Opcode::DiscardCond)
                return;
            if (IsSample(&i))
                samples.push_back(&i);
            if (i.GetOpcode() != Opcode::SetAttribute)
                continue;
            const auto target = i.Arg(0).Attribute();
            if (target > IR::Attribute::RenderTarget2)
                continue;
            if (interior) {
                std::set<std::pair<const Inst*, int>> seen;
                Trace(i.Arg(1), -1, all_roles, seen);
                std::set<std::pair<const Inst*, int>> output_seen;
                TraceDecodedNormal(i.Arg(1), -1, decoded_outputs, raw_outputs, output_seen);
                if (target == IR::Attribute::RenderTarget1 && i.Arg(2).U32() <= 2) {
                    std::set<std::pair<const Inst*, int>> decoded_seen;
                    TraceDecodedNormal(i.Arg(1), -1, decoded_normals, raw_normals, decoded_seen);
                }
            }
            if (i.Arg(2).U32() <= 2) {
                std::set<std::pair<const Inst*, int>> seen;
                Trace(i.Arg(1), -1, roles[size_t(target)], seen);
            }
        }
    if (roles[0].empty() || roles[1].empty() || !info.stores.GetAny(IR::Attribute::RenderTarget2))
        return;
    std::unordered_map<const Inst*, UV> memo;
    std::unordered_map<const Inst*, InteriorUV> interior_memo;
    std::map<std::string, std::vector<const Inst*>> groups;
    for (const auto* sample : samples) {
        if (!sample->Arg(0).IsImmediate())
            continue;
        const auto h = sample->Arg(0).U32();
        if (!RepeatImage(info, h))
            continue;
        const size_t offset_arg = sample->GetOpcode() == Opcode::ImageGradient ? 4 : 3;
        if (!sample->Arg(offset_arg).IsEmpty())
            continue;
        const auto uv = Describe(sample->Arg(1), memo);
        const auto interior_uv = interior ? InteriorAffine(sample->Arg(1), interior_memo) : InteriorUV{};
        if (uv.valid && uv.attribute && uv.scaled &&
            (!interior || (interior_uv.valid && interior_uv.param)))
            groups[uv.key].push_back(sample);
    }
    for (const auto& [key, group] : groups) {
        std::vector<const Inst*> colours, normals;
        if (interior) {
            for (const auto* sample : group) {
                const auto h = sample->Arg(0).U32();
                const auto sharp = info.images[h & 65535].GetSharp(info);
                const auto format = sharp.GetDataFmt();
                // Alpha participates in fabric control data and remains original.
                // Only RG may feed the normal; never select albedo by its format.
                if ((roles[1][sample] & 3u) != 3u || (roles[1][sample] & 12u))
                    continue;
                const bool bc5 = format == AmdGpu::DataFormat::FormatBc5 &&
                                 (sharp.GetNumberFmt() == AmdGpu::NumberFormat::Snorm ||
                                  sharp.GetNumberFmt() == AmdGpu::NumberFormat::Unorm);
                const bool fabric = (format == AmdGpu::DataFormat::FormatBc1 ||
                                     format == AmdGpu::DataFormat::FormatBc7) &&
                                    sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb &&
                                    decoded_normals[sample] == 3u && !raw_normals[sample] &&
                                    !(raw_outputs[sample] & 7u);
                if (bc5 || fabric)
                    normals.push_back(sample);
            }
            selected.insert(normals.begin(), normals.end());
            if (!normals.empty())
                for (const auto* sample : group) {
                    const auto sharp = info.images[sample->Arg(0).U32() & 65535].GetSharp(info);
                    // BC4 scalar paired with a grain normal on this exact repeat UV.
                    if (sharp.GetDataFmt() == AmdGpu::DataFormat::FormatBc4 &&
                        sharp.GetNumberFmt() == AmdGpu::NumberFormat::Unorm &&
                        all_roles[sample] == 1u && !roles[1][sample])
                        selected.insert(sample);
                }
            continue;
        }
        for (const auto* sample : group) {
            const auto h = sample->Arg(0).U32();
            const auto sharp = info.images[h & 65535].GetSharp(info);
            if (sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb && (roles[0][sample] & 7))
                colours.push_back(sample);
            // Require a two-channel BC5 input that actually reaches normal RGB.
            if (sharp.GetDataFmt() == AmdGpu::DataFormat::FormatBc5 && (roles[1][sample] & 3))
                normals.push_back(sample);
        }
        if (colours.empty() || normals.empty())
            continue;
        selected.insert(colours.begin(), colours.end());
        selected.insert(normals.begin(), normals.end());
    }
}
} // namespace WorldRepeat
} // namespace Shader::Backend::SPIRV
