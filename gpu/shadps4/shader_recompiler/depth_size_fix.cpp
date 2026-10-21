// SPDX-License-Identifier: GPL-2.0-or-later
// Changed for this port. The upstream sources of the vendored tree are listed in
// gpu/VENDOR.txt and in NOTICE.
//
#include "shader_recompiler/depth_size_fix.h"

namespace Shader::Gcn {

namespace {

constexpr u32 SOP2_S_BFE_U32 = 39;
constexpr u32 WIDTH_LITERAL = 960;
constexpr u32 HEIGHT_LITERAL = 540;

bool IsMovLiteral(u32 w) {
    // SOP1 s_mov_b32 sdst, literal
    return (w >> 23) == 0x17D && ((w >> 8) & 0xFF) == 3 && (w & 0xFF) == 0xFF;
}

// Turns "v_cmp_<op> ..., sN" into the next comparison (< becomes <=) for the two size registers.
bool BumpCompares(std::vector<u32>& code, size_t from, u32 wx, u32 wy) {
    int hits = 0;
    for (size_t k = 0; k < 12 && from + k + 2 <= code.size(); ++k) {
        const size_t at = from + k;
        const u32 c = code[at];
        if ((c >> 25) == 0x3E && ((c >> 17) & 0xFF) == 0xC4 && ((c & 0x1FF) == wx || (c & 0x1FF) == wy)) {
            code[at] = c + (2u << 17);
            ++hits;
        } else if ((c >> 26) == 0x34 && ((c >> 17) & 0x1FF) == 0xC4) {
            const u32 s0 = code[at + 1] & 0x1FF;
            if (s0 == wx || s0 == wy) {
                code[at] = c + (2u << 17);
                ++hits;
            }
        }
    }
    return hits == 2;
}

} // namespace

bool ApplyDepthSizeFix(std::span<const u32> input, std::vector<u32>& patched) {
    const size_t n = input.size();
    if (n < 16) {
        return false;
    }
    // The first image load names the descriptor registers; the size lives in its third dword.
    u32 base = 0;
    bool found = false;
    for (size_t i = 0; i + 2 <= n; ++i) {
        const u32 w = input[i];
        if ((w >> 26) == 0x3C && ((w >> 18) & 0x7F) == 8) {
            base = ((input[i + 1] >> 16) & 0x1F) * 4;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    for (size_t i = 0; i + 16 <= n; ++i) {
        const u32 a = input[i], b = input[i + 2];
        if (!IsMovLiteral(a) || !IsMovLiteral(b)) {
            continue;
        }
        if (input[i + 1] != WIDTH_LITERAL || input[i + 3] != HEIGHT_LITERAL) {
            continue;
        }
        const u32 wx = (a >> 16) & 0x7F, wy = (b >> 16) & 0x7F;
        std::vector<u32> copy(input.begin(), input.end());
        if (!BumpCompares(copy, i + 4, wx, wy)) {
            continue;
        }
        // s_bfe_u32 sdst, <descriptor dword 2>, literal: width = bits 0..13, height = bits 14..27
        copy[i] = 0x80000000u | (SOP2_S_BFE_U32 << 23) | (wx << 16) | (0xFFu << 8) | (base + 2);
        copy[i + 1] = 0x000E0000u;
        copy[i + 2] = 0x80000000u | (SOP2_S_BFE_U32 << 23) | (wy << 16) | (0xFFu << 8) | (base + 2);
        copy[i + 3] = 0x000E000Eu;
        patched = std::move(copy);
        return true;
    }
    return false;
}

} // namespace Shader::Gcn

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace Shader::Gcn {

namespace {
enum : u32 {
    OpExecutionMode = 16, OpTypeInt = 21, OpTypeVector = 23, OpTypeImage = 25, OpTypePointer = 32,
    OpConstant = 43, OpVariable = 59, OpLoad = 61, OpCompositeExtract = 81, OpImageFetch = 95,
    OpImageWrite = 99, OpImageQuerySize = 104, OpUGreaterThan = 172,
};
struct Ins { size_t at; u32 wc, op; };
} // namespace

bool ApplyDepthSizeFixSpirv(std::vector<u32>& spv) {
    if (spv.size() < 8 || spv[0] != 0x07230203) {
        return false;
    }
    std::vector<Ins> ins;
    for (size_t i = 5; i < spv.size();) {
        const u32 wc = spv[i] >> 16;
        if (wc == 0 || i + wc > spv.size()) {
            return false;
        }
        ins.push_back({i, wc, spv[i] & 0xFFFF});
        i += wc;
    }
    std::map<u32, u32> consts;  // value -> id
    bool fetch = false, write = false, query = false, local88 = false;
    u32 u32t = 0, v2u32 = 0;
    std::set<u32> storage_types;
    std::map<u32, u32> ptr_to_img;
    std::set<u32> written;
    std::map<u32, std::pair<u32, u32>> load_of;
    for (const auto& in : ins) {
        const u32* w = &spv[in.at];
        switch (in.op) {
        case OpConstant: if (in.wc >= 4) consts.emplace(w[3], w[2]); break;
        case OpImageFetch: fetch = true; break;
        case OpImageWrite: write = true; if (in.wc >= 4) written.insert(w[1]); break;
        case OpImageQuerySize: query = true; break;
        case OpExecutionMode: if (in.wc >= 6 && w[2] == 17) local88 = w[3] == 8 && w[4] == 8 && w[5] == 1; break;
        case OpTypeInt: if (in.wc >= 4 && w[2] == 32 && w[3] == 0) u32t = w[1]; break;
        case OpTypeImage: if (in.wc >= 9 && w[7] == 2) storage_types.insert(w[1]); break;
        case OpTypePointer: if (in.wc >= 4 && w[2] == 0) ptr_to_img[w[1]] = w[3]; break;
        case OpLoad: if (in.wc >= 4) load_of[w[2]] = {w[1], w[3]}; break;
        default: break;
        }
    }
    if (!consts.count(960) || !consts.count(540) || !fetch || !write || query || !local88 || !u32t) {
        return false;
    }
    for (const auto& in : ins) {
        const u32* w = &spv[in.at];
        if (in.op == OpTypeVector && in.wc >= 4 && w[2] == u32t && w[3] == 2) v2u32 = w[1];
    }
    if (!v2u32) {
        return false;
    }
    u32 dst_var = 0, dst_type = 0;
    for (u32 res : written) {
        const auto it = load_of.find(res);
        if (it != load_of.end() && storage_types.count(it->second.first)) {
            dst_type = it->second.first;
            dst_var = it->second.second;
        }
    }
    if (!dst_var) {
        return false;
    }
    for (const auto& in : ins) {
        const u32* w = &spv[in.at];
        if (in.op == OpVariable && in.wc >= 4 && w[2] == dst_var) {
            const auto p = ptr_to_img.find(w[1]);
            if (p == ptr_to_img.end() || p->second != dst_type) {
                return false;
            }
        }
    }
    int cmp_w = -1, cmp_h = -1;
    for (size_t k = 0; k < ins.size(); ++k) {
        if (ins[k].op == OpUGreaterThan && ins[k].wc == 5) {
            const u32 rhs = spv[ins[k].at + 3];
            if (rhs == consts[960]) cmp_w = int(k);
            else if (rhs == consts[540]) cmp_h = int(k);
        }
    }
    if (cmp_w < 0 || cmp_h < 0) {
        return false;
    }
    const u32 bound = spv[3];
    const u32 id_img = bound, id_size = bound + 1, id_w = bound + 2, id_h = bound + 3;
    const auto enc = [](u32 op, std::initializer_list<u32> ops) {
        std::vector<u32> v{(u32(ops.size() + 1) << 16) | op};
        v.insert(v.end(), ops.begin(), ops.end());
        return v;
    };
    std::vector<u32> inject;
    for (const auto& part : {enc(OpLoad, {dst_type, id_img, dst_var}),
                             enc(OpImageQuerySize, {v2u32, id_size, id_img}),
                             enc(OpCompositeExtract, {u32t, id_w, id_size, 0}),
                             enc(OpCompositeExtract, {u32t, id_h, id_size, 1})}) {
        inject.insert(inject.end(), part.begin(), part.end());
    }
    const int first = std::min(cmp_w, cmp_h);
    std::vector<u32> out(spv.begin(), spv.begin() + 5);
    for (size_t k = 0; k < ins.size(); ++k) {
        if (int(k) == first) out.insert(out.end(), inject.begin(), inject.end());
        const size_t base = out.size();
        out.insert(out.end(), spv.begin() + ins[k].at, spv.begin() + ins[k].at + ins[k].wc);
        if (int(k) == cmp_w) out[base + 3] = id_w;
        else if (int(k) == cmp_h) out[base + 3] = id_h;
    }
    out[3] = bound + 4;
    spv = std::move(out);
    return true;
}

} // namespace Shader::Gcn
