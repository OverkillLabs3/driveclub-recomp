// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "common/types.h"

// BB_PASS_STATS=1: per presented game frame, how many render passes the GPU runs, how many attachment pixels
// they load from and store to memory (the tile traffic a tile-based GPU pays at every pass boundary), and
// how many draws, dispatches, submits and CPU waits go with them. Printed every 5 seconds; counts only.
namespace Vulkan::PassStats {

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PASS_STATS");
        return v && *v && *v != '0';
    }();
    return on;
}

struct Counters {
    std::atomic<u64> passes{}, small_passes{}, load_px{}, clear_px{}, store_px{}, draws{}, dispatches{},
        submits{}, waits{}, frames{}, draws_total{}, dispatches_total{}, pass_start{};
};

inline Counters& Get() {
    static Counters c;
    return c;
}

// BB_PASS_STATS=3 also prints one frame's pass sequence every 5 seconds.
inline bool Sequence() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PASS_STATS");
        return v && *v == '3';
    }();
    return on;
}

// BB_PASS_STATS=2 also lists the pass shapes moving the most attachment pixels.
inline bool Shapes() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PASS_STATS");
        return v && (*v == '2' || *v == '3');
    }();
    return on;
}

struct Shape {
    u32 w, h, layers, colors, depth, stencil, clears, color_fmt0;
    auto Key() const { return std::tie(w, h, layers, colors, depth, stencil, clears, color_fmt0); }
    bool operator<(const Shape& o) const { return Key() < o.Key(); }
};
struct ShapeTotals {
    u64 passes{}, traffic{}, draws{};
};
struct ShapeTable {
    std::mutex mutex;
    std::map<Shape, ShapeTotals> table;
    Shape open{};
    bool has_open = false;
    u64 draws_at_open = 0;
};
// Scissor rectangles of draws into large (>= 2048x2048) targets: where on the target the passes draw.
struct ScissorTable {
    std::mutex mutex;
    std::map<std::tuple<u32, u32, s32, s32, u32, u32>, u64> table;
};
inline ScissorTable& Scissors() {
    static ScissorTable t;
    return t;
}
inline void Scissor(u32 tw, u32 th, s32 x, s32 y, u32 w, u32 h) {
    if (!Shapes() || tw < 2048 || th < 2048) return;
    auto& t = Scissors();
    std::scoped_lock lk{t.mutex};
    ++t.table[{tw, th, x, y, w, h}];
}

// Rect-list draws (tessellation-emulated on MoltenVK), by vertex shader.
struct RectKey {
    u64 vs, ps;
    u32 indices, instances, attributes, indexed, first_in_pass, w, h;
    auto Key() const { return std::tie(vs, ps, indices, instances, attributes, indexed, first_in_pass, w, h); }
    bool operator<(const RectKey& o) const { return Key() < o.Key(); }
};
struct RectTable {
    std::mutex mutex;
    std::map<RectKey, u64> table;
};
inline RectTable& Rects() {
    static RectTable t;
    return t;
}
inline void Rect(const RectKey& key) {
    if (!Shapes()) return;
    auto& t = Rects();
    std::scoped_lock lk{t.mutex};
    ++t.table[key];
}

inline ShapeTable& Table() {
    static ShapeTable t;
    return t;
}

/// BB_PASS_LABELS=1: a debug label per render pass and dispatch (Metal System Trace shows them per encoder).
inline bool Labels() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PASS_LABELS");
        return v && *v && *v != '0';
    }();
    return on;
}
using LabelText = std::array<char, 112>;

inline void Add(std::atomic<u64>& counter, u64 n = 1) {
    if (Enabled()) counter.fetch_add(n, std::memory_order_relaxed);
}

struct SequenceState {
    bool armed = false, capturing = false;
    std::string text;
    u64 draws_at = 0, dispatches_at = 0;
};
inline SequenceState& Seq() {
    static SequenceState s;
    return s;
}

// Who ended the open pass (BB_PASS_STATS=3): the calling function, appended to the sequence line.
inline void Ended(void* caller, const char* how) {
    if (!Sequence()) return;
    auto& t = Table();
    std::scoped_lock lk{t.mutex};
    if (!Seq().capturing) return;
    Dl_info info{};
    const char* name = "?";
    char* demangled = nullptr;
    if (dladdr(caller, &info) && info.dli_sname) {
        int status = 0;
        demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        name = demangled ? demangled : info.dli_sname;
    }
    std::string n = name;
    if (const auto paren = n.find('('); paren != std::string::npos) n.resize(paren);
    Seq().text += std::string(" <") + how + " " + n + ">";
    std::free(demangled);
}

inline void Note(const std::string& text) {
    if (!Sequence()) return;
    auto& t = Table();
    std::scoped_lock lk{t.mutex};
    if (Seq().capturing) Seq().text += " {" + text + "}";
}

inline void Pass(const Shape& shape, u64 traffic, u64 depth_view = 0) {
    if (!Shapes()) return;
    Get().pass_start.store(Get().draws_total.load(std::memory_order_relaxed), std::memory_order_relaxed);
    auto& t = Table();
    std::scoped_lock lk{t.mutex};
    if (Sequence() && Seq().capturing) {
        auto& q = Seq();
        const u64 d = Get().draws_total.load(std::memory_order_relaxed);
        const u64 x = Get().dispatches_total.load(std::memory_order_relaxed);
        char line[256];
        std::snprintf(line, sizeof(line), " [%llu draws, then %llu dispatches]\n  %ux%u x%u c%u v%#x d%u:%#x s%u clr%#x",
                      (unsigned long long)(d - q.draws_at), (unsigned long long)(x - q.dispatches_at), shape.w,
                      shape.h, shape.layers, shape.colors, shape.color_fmt0, shape.depth, u32(depth_view),
                      shape.stencil, shape.clears);
        q.text += line;
        q.draws_at = d;
        q.dispatches_at = x;
    }
    const u64 draws = Get().draws_total.load(std::memory_order_relaxed);
    if (t.has_open) t.table[t.open].draws += draws - t.draws_at_open;
    auto& e = t.table[shape];
    ++e.passes;
    e.traffic += traffic;
    t.open = shape;
    t.has_open = true;
    t.draws_at_open = draws;
}

inline void PrintShapes(double frames) {
    auto& t = Table();
    std::scoped_lock lk{t.mutex};
    std::vector<std::pair<Shape, ShapeTotals>> rows(t.table.begin(), t.table.end());
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.traffic > b.second.traffic; });
    for (size_t i = 0; i < rows.size() && i < 20; ++i) {
        const auto& [k, v] = rows[i];
        std::printf("  shape %ux%u x%u layers, %u color (view %#x) depth %u stencil %u clears %#x: %.1f passes, "
                    "%.1f Mpx, %.1f draws/pass\n",
                    k.w, k.h, k.layers, k.colors, k.color_fmt0, k.depth, k.stencil, k.clears, v.passes / frames,
                    v.traffic / frames / 1e6, v.passes ? double(v.draws) / v.passes : 0.0);
    }
    t.table.clear();
    {
        auto& sc = Scissors();
        std::scoped_lock slk{sc.mutex};
        std::vector<std::pair<std::tuple<u32, u32, s32, s32, u32, u32>, u64>> srows(sc.table.begin(),
                                                                                      sc.table.end());
        std::sort(srows.begin(), srows.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < srows.size() && i < 16; ++i) {
            const auto& [k, n] = srows[i];
            std::printf("  scissor on %ux%u: at %d,%d size %ux%u: %.1f draws\n", std::get<0>(k), std::get<1>(k),
                        std::get<2>(k), std::get<3>(k), std::get<4>(k), std::get<5>(k), n / frames);
        }
        sc.table.clear();
    }
    {
        auto& rt = Rects();
        std::scoped_lock rlk{rt.mutex};
        std::vector<std::pair<RectKey, u64>> rrows(rt.table.begin(), rt.table.end());
        std::sort(rrows.begin(), rrows.end(), [](auto& a, auto& b) { return a.second > b.second; });
        u64 total = 0;
        for (auto& r : rrows) total += r.second;
        std::printf("  rect lists: %.1f draws/frame\n", total / frames);
        for (size_t i = 0; i < rrows.size() && i < 25; ++i) {
            const auto& [k, n] = rrows[i];
            std::printf("  rect vs %016llx ps %016llx: %u indices x%u%s, %u fetch attributes, %s, target %ux%u: "
                        "%.1f/frame\n",
                        (unsigned long long)k.vs, (unsigned long long)k.ps, k.indices, k.instances,
                        k.indexed ? " indexed" : "", k.attributes, k.first_in_pass ? "first in pass" : "inside pass",
                        k.w, k.h, n / frames);
        }
        rt.table.clear();
    }
    // The draws since the open pass began count again from here.
    t.draws_at_open = Get().draws_total.load(std::memory_order_relaxed);
}

inline void Frame() {
    if (!Enabled()) return;
    if (Sequence()) {
        auto& t = Table();
        std::scoped_lock lk{t.mutex};
        auto& q = Seq();
        if (q.capturing) {
            std::printf("Pass sequence of one frame:%s\n", q.text.c_str());
            q.capturing = false;
            q.text.clear();
        } else if (q.armed) {
            q.armed = false;
            q.capturing = true;
            q.draws_at = Get().draws_total.load(std::memory_order_relaxed);
            q.dispatches_at = Get().dispatches_total.load(std::memory_order_relaxed);
        }
    }
    auto& c = Get();
    c.frames.fetch_add(1, std::memory_order_relaxed);
    static auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - start < std::chrono::seconds(5)) return;
    const double f = double(c.frames.exchange(0));
    const double s = std::chrono::duration<double>(now - start).count();
    start = now;
    const auto per = [&](std::atomic<u64>& v) { return double(v.exchange(0)) / f; };
    const double passes = per(c.passes), small = per(c.small_passes);
    const double load = per(c.load_px) / 1e6, clear = per(c.clear_px) / 1e6, store = per(c.store_px) / 1e6;
    const double draws = per(c.draws), dispatches = per(c.dispatches), submits = per(c.submits),
                 waits = per(c.waits);
    std::printf("Pass stats: %.1f fps; per frame %.0f passes (%.0f under 256x256), Mpx load %.1f clear %.1f "
                "store %.1f, %.0f draws, %.0f dispatches, %.1f submits, %.1f CPU waits\n",
                f / s, passes, small, load, clear, store, draws, dispatches, submits, waits);
    if (Shapes()) PrintShapes(f);
    if (Sequence()) Seq().armed = true;
    std::fflush(stdout);
}

} // namespace Vulkan::PassStats
