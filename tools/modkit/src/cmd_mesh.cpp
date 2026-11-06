// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod mesh ...: the geometry of resource packs as glTF 2.0 binary (.glb), for Blender and other editors.
//
// A model pack has "mesh" resources. Each depends on a position stream (a vertex buffer of 12 byte XYZ floats), a
// attribute stream (normals, texture coordinates and so on, not decoded here) and an index buffer, and holds one or more
// sub meshes: four 32 bit numbers (lowest vertex, highest vertex, first index, index count) after the bytes 02 03.
// The indices are absolute positions in the vertex buffer and form a triangle list.
#include "cli.hpp"
#include "json.hpp"
#include "rpk.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dcmod {

namespace {

int Usage() {
    std::puts(
        "dcmod mesh: model geometry of resource packs as glTF 2.0 (.glb)\n"
        "\n"
        "  dcmod mesh info PACK                        the sub meshes: name, vertices, triangles\n"
        "  dcmod mesh export PACK -o FILE.glb [NAME...]  one glTF mesh per sub mesh (names may use * and ?)\n"
        "  dcmod mesh import PACK FILE.glb -o OUT.rpk  write the pack again with the positions (and triangles) of the\n"
        "                                              glTF objects that came from it\n"
        "\n"
        "PACK is a file or @path in the game's archive. Objects are named ID_N_NAME (the mesh's identifier and the\n"
        "sub mesh number); keep that prefix when renaming. An object must keep its number of vertices and triangles:\n"
        "positions can be moved and triangles reconnected, the other vertex data (normals, texture coordinates, skin\n"
        "weights) stays as it was. Axes are the game's own (Y up).");
    return 2;
}

struct Sub {
    uint64_t mesh = 0, pos = 0, ib = 0, attr = 0;
    uint32_t pos_stride = 12;
    bool half = false;  // positions are three half floats
    int uv_off = -1;  // where the texture coordinate (two floats) sits in a vertex of the attribute stream
    uint32_t stride = 0;
    uint32_t lo = 0, hi = 0, first = 0, count = 0, k = 0;
    std::string name;
    uint32_t vertices() const { return hi - lo + 1; }
};

// Position streams are told by their stream format: three floats (12 bytes), three floats and 4 other bytes (16) or
// three half floats and 2 padding bytes (8), or the same with 4 other bytes after them (12).
int PositionKind(const RpkResource& vb) {
    if (vb.data.size() < 28 || vb.deps.empty()) return 0;
    switch (vb.deps[0]) {
        case 0x0002ea626d3b6d17ull: return 1;
        case 0x00021bb5417fb6e5ull: return 1;
        case 0x000234fef1a20d63ull: return 2;
        case 0x0002874b47ba4c22ull: return 2;
        default: return 0;
    }
}

float HalfToFloat(uint16_t h) {
    const int e = (h >> 10) & 31;
    const float m = float(h & 1023);
    float f = e == 0 ? std::ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(m + 1024.f, e - 25);
    return h & 0x8000 ? -f : f;
}

uint16_t FloatToHalf(float f) {
    if (std::isnan(f)) return 0x7E00;
    const uint16_t sign = std::signbit(f) ? 0x8000 : 0;
    f = std::fabs(f);
    if (f >= 65520.f) return sign | 0x7C00;
    if (f < 6.1035156e-5f) return sign | uint16_t(std::lrint(std::ldexp(f, 24)));
    int e;
    const float m = std::frexp(f, &e);  // f = m * 2^e, m in [0.5, 1)
    uint32_t mant = uint32_t(std::lrint(std::ldexp(m, 11))) - 1024;  // 10 bits
    int exp = e + 14;
    if (mant >= 1024) mant = 0, ++exp;
    return sign | uint16_t(exp << 10) | uint16_t(mant);
}

uint32_t VbCount(const RpkResource& r) { return r.data.size() >= 28 ? Rd32(r.data.data() + 24) : 0; }

// The last place before `at` where the mesh data names the resource `id` (its identifier as 8 little endian bytes).
long LastMention(const Bytes& d, uint64_t id, size_t at) {
    uint8_t key[8];
    for (int i = 0; i < 8; ++i) key[i] = uint8_t(id >> (8 * i));
    for (size_t i = std::min(at, d.size() - 8) + 1; i-- > 0;) {
        if (i + 8 <= at && std::memcmp(&d[i], key, 8) == 0) return long(i);
    }
    return -1;
}

std::vector<Sub> FindSubs(const RpkPack& pack) {
    const int vb_type = RpkTypeFromName("vertex_buffer"), ib_type = RpkTypeFromName("index_buffer"), mesh_type = RpkTypeFromName("mesh");
    std::vector<Sub> out;
    for (const RpkResource& m : pack.resources) {
        if (int(m.Type()) != mesh_type) continue;
        std::vector<uint64_t> poss, ibs, attrs;
        for (uint64_t d : m.deps) {
            const RpkResource* r = pack.Find(d);
            if (!r) continue;
            if (int(r->Type()) == vb_type && r->data.size() >= 28) (PositionKind(*r) ? poss : attrs).push_back(d);
            if (int(r->Type()) == ib_type) ibs.push_back(d);
        }
        if (poss.empty() || ibs.empty()) continue;
        // each sub mesh record is preceded by the identifiers of the buffers it uses; take the nearest of each kind
        auto nearest = [&](const std::vector<uint64_t>& list, size_t at) {
            long best = -1;
            uint64_t who = list.front();
            for (uint64_t d : list) {
                const long p = LastMention(m.data, d, at);
                if (p > best) best = p, who = d;
            }
            return who;
        };
        uint32_t k = 0;
        std::vector<std::array<uint64_t, 5>> seen;
        for (size_t i = 0; i + 18 <= m.data.size(); ++i) {
            if (m.data[i] != 2 || m.data[i + 1] != 3) continue;
            const uint32_t lo = Rd32(&m.data[i + 2]), hi = Rd32(&m.data[i + 6]), first = Rd32(&m.data[i + 10]), cnt = Rd32(&m.data[i + 14]);
            const uint64_t pos = nearest(poss, i), ib = nearest(ibs, i);
            const uint32_t nv = VbCount(*pack.Find(pos)), ni = VbCount(*pack.Find(ib));
            if (!cnt || cnt % 3 || hi < lo || hi >= nv || first + cnt > ni) continue;
            const uint8_t* ix = &pack.Find(ib)->data[28 + first * 2];
            bool inside = true;  // a record whose indices leave its range is not a sub mesh
            for (uint32_t t = 0; t < cnt && inside; ++t) inside = Rd16(ix + t * 2) >= lo && Rd16(ix + t * 2) <= hi;
            if (!inside) continue;
            const std::array<uint64_t, 5> key{pos, ib, lo, hi, (uint64_t(first) << 32) | cnt};
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            Sub s;
            s.mesh = m.id, s.pos = pos, s.pos_stride = Rd16(pack.Find(pos)->data.data() + 22), s.half = PositionKind(*pack.Find(pos)) == 2, s.ib = ib, s.lo = lo, s.hi = hi, s.first = first, s.count = cnt, s.k = k++;
            // the attribute stream that has as many vertices as the position stream
            std::vector<uint64_t> same;
            for (uint64_t a : attrs) {
                if (VbCount(*pack.Find(a)) == nv) same.push_back(a);
            }
            if (!same.empty()) {
                s.attr = nearest(same, i);
                const RpkResource* av = pack.Find(s.attr);
                const RpkResource* fmt = av->deps.empty() ? nullptr : pack.Find(av->deps[0]);
                s.stride = Rd16(av->data.data() + 22);
                if (fmt && fmt->data.size() >= 21) {
                    // the format: a count, then five bytes per field (kind, type, index, 0, 0); the type gives the size
                    const size_t n = fmt->data[20];
                    uint32_t at = 0;
                    for (size_t f = 0; f < n && 21 + f * 5 + 5 <= fmt->data.size(); ++f) {
                        const uint8_t kind = fmt->data[21 + f * 5], type = fmt->data[22 + f * 5];
                        if (kind == 5 && type == 8 && s.uv_off < 0) s.uv_off = int(at);
                        at += type == 9 ? 12 : type == 8 || type == 6 ? 8 : 4;
                    }
                    if (at != s.stride) s.uv_off = -1;
                }
            }
            std::string n = m.DisplayName();
            const size_t bar = n.rfind('|');
            if (bar != std::string::npos && bar + 1 < n.size()) n = n.substr(bar + 1);
            s.name = HexId(m.id) + "_" + std::to_string(s.k) + "_" + n;
            out.push_back(s);
        }
    }
    return out;
}

float Clean(float v) { return std::isfinite(v) && std::fabs(v) < 1e6f ? v : 0.f; }

void Put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void PadTo4(Bytes& b, uint8_t fill) { while (b.size() % 4) b.push_back(fill); }

Bytes MakeGlb(const RpkPack& pack, const std::vector<Sub>& subs) {
    Bytes bin;
    Json nodes = Json::Array(), meshes = Json::Array(), accessors = Json::Array(), views = Json::Array(), scene = Json::Array();
    for (const Sub& s : subs) {
        const Bytes& pv = pack.Find(s.pos)->data;
        const Bytes& iv = pack.Find(s.ib)->data;
        // positions
        PadTo4(bin, 0);
        const size_t pos_at = bin.size();
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t i = s.lo; i <= s.hi; ++i) {
            for (int c = 0; c < 3; ++c) {
                float f;
                if (s.half) f = HalfToFloat(Rd16(&pv[28 + size_t(i) * s.pos_stride + size_t(c) * 2]));
                else std::memcpy(&f, &pv[28 + size_t(i) * s.pos_stride + size_t(c) * 4], 4);
                f = Clean(f);
                mn[c] = std::min(mn[c], f), mx[c] = std::max(mx[c], f);
                uint32_t u;
                std::memcpy(&u, &f, 4);
                Put32(bin, u);
            }
        }
        // texture coordinates, when the attribute stream has them (glTF counts v from the top)
        size_t uv_at = 0;
        const bool has_uv = s.uv_off >= 0;
        if (has_uv) {
            PadTo4(bin, 0);
            uv_at = bin.size();
            const Bytes& av = pack.Find(s.attr)->data;
            for (uint32_t i = s.lo; i <= s.hi; ++i) {
                float uv[2];
                std::memcpy(uv, &av[28 + size_t(i) * s.stride + size_t(s.uv_off)], 8);
                uv[1] = 1.f - uv[1];
                for (float f : uv) {
                    uint32_t u;
                    const float c = Clean(f);
                    std::memcpy(&u, &c, 4);
                    Put32(bin, u);
                }
            }
        }
        // indices, relative to the first vertex
        PadTo4(bin, 0);
        const size_t idx_at = bin.size();
        for (uint32_t i = 0; i < s.count; ++i) {
            const uint32_t v = Rd16(&iv[28 + (s.first + i) * 2]) - s.lo;
            bin.push_back(uint8_t(v)), bin.push_back(uint8_t(v >> 8));
        }
        const size_t idx_end = bin.size();
        auto view = [&](size_t at, size_t len, int target) {
            Json v = Json::Object();
            v.Set("buffer", 0), v.Set("byteOffset", int64_t(at)), v.Set("byteLength", int64_t(len)), v.Set("target", target);
            views.Push(v);
            return int(views.Items().size()) - 1;
        };
        const int pv_i = view(pos_at, s.vertices() * 12, 34962), iv_i = view(idx_at, idx_end - idx_at, 34963);
        const int uv_i = has_uv ? view(uv_at, s.vertices() * 8, 34962) : -1;
        Json pa = Json::Object();
        pa.Set("bufferView", pv_i), pa.Set("componentType", 5126), pa.Set("count", int64_t(s.vertices())), pa.Set("type", "VEC3");
        Json jmn = Json::Array(), jmx = Json::Array();
        for (int c = 0; c < 3; ++c) jmn.Push(double(mn[c])), jmx.Push(double(mx[c]));
        pa.Set("min", jmn), pa.Set("max", jmx);
        accessors.Push(pa);
        Json ia = Json::Object();
        ia.Set("bufferView", iv_i), ia.Set("componentType", 5123), ia.Set("count", int64_t(s.count)), ia.Set("type", "SCALAR");
        accessors.Push(ia);
        const int pa_i = int(accessors.Items().size()) - 2;
        Json prim = Json::Object(), attrs = Json::Object();
        attrs.Set("POSITION", pa_i);
        if (has_uv) {
            Json ua = Json::Object();
            ua.Set("bufferView", uv_i), ua.Set("componentType", 5126), ua.Set("count", int64_t(s.vertices())), ua.Set("type", "VEC2");
            accessors.Push(ua);
            attrs.Set("TEXCOORD_0", int(accessors.Items().size()) - 1);
        }
        prim.Set("attributes", attrs), prim.Set("indices", pa_i + 1), prim.Set("mode", 4);
        Json prims = Json::Array();
        prims.Push(prim);
        Json mesh = Json::Object();
        mesh.Set("name", s.name), mesh.Set("primitives", prims);
        meshes.Push(mesh);
        Json node = Json::Object();
        node.Set("name", s.name), node.Set("mesh", int(meshes.Items().size()) - 1);
        nodes.Push(node);
        scene.Push(int(nodes.Items().size()) - 1);
    }
    PadTo4(bin, 0);
    Json root = Json::Object(), asset = Json::Object(), sc = Json::Object(), scenes = Json::Array(), buffers = Json::Array(), buf = Json::Object();
    asset.Set("version", "2.0"), asset.Set("generator", "dcmod mesh");
    sc.Set("nodes", scene), scenes.Push(sc);
    buf.Set("byteLength", int64_t(bin.size())), buffers.Push(buf);
    root.Set("asset", asset), root.Set("scene", 0), root.Set("scenes", scenes), root.Set("nodes", nodes), root.Set("meshes", meshes);
    root.Set("accessors", accessors), root.Set("bufferViews", views), root.Set("buffers", buffers);
    std::string js = root.Dump(0);
    while (js.size() % 4) js.push_back(' ');
    Bytes out;
    Put32(out, 0x46546C67), Put32(out, 2), Put32(out, uint32_t(12 + 8 + js.size() + 8 + bin.size()));
    Put32(out, uint32_t(js.size())), Put32(out, 0x4E4F534A);
    out.insert(out.end(), js.begin(), js.end());
    Put32(out, uint32_t(bin.size())), Put32(out, 0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

struct Glb {
    Json json;
    Bytes bin;
};

Glb ReadGlb(const Bytes& d) {
    if (d.size() < 20 || Rd32(d.data()) != 0x46546C67) throw Error("not a .glb file");
    Glb g;
    size_t p = 12;
    while (p + 8 <= d.size()) {
        const uint32_t len = Rd32(&d[p]), type = Rd32(&d[p + 4]);
        if (p + 8 + len > d.size()) throw Error("damaged .glb");
        if (type == 0x4E4F534A) g.json = Json::Parse(std::string_view(reinterpret_cast<const char*>(&d[p + 8]), len));
        if (type == 0x004E4942) g.bin.assign(d.begin() + p + 8, d.begin() + p + 8 + len);
        p += 8 + len;
    }
    return g;
}

// The accessor's data as numbers. Handles float and unsigned integer scalars and VEC3 floats.
std::vector<double> Accessor(const Glb& g, int64_t index, int comps) {
    const Json& acc = g.json.Find("accessors")->Items().at(size_t(index));
    const Json& view = g.json.Find("bufferViews")->Items().at(size_t(acc.GetInt("bufferView")));
    const size_t base = size_t(view.GetInt("byteOffset") + acc.GetInt("byteOffset")), n = size_t(acc.GetInt("count"));
    const int ct = int(acc.GetInt("componentType"));
    const size_t size = ct == 5126 || ct == 5125 ? 4 : ct == 5123 ? 2 : 1;
    const size_t stride = view.GetInt("byteStride") ? size_t(view.GetInt("byteStride")) : size * size_t(comps);
    std::vector<double> out;
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < comps; ++c) {
            const size_t at = base + i * stride + size_t(c) * size;
            if (at + size > g.bin.size()) throw Error("accessor outside the buffer");
            const uint8_t* q = &g.bin[at];
            if (ct == 5126) {
                float f;
                std::memcpy(&f, q, 4);
                out.push_back(f);
            } else {
                out.push_back(size == 4 ? Rd32(q) : size == 2 ? Rd16(q) : q[0]);
            }
        }
    }
    return out;
}

RpkPack LoadPack(const Args& args, const std::string& spec) {
    if (spec.empty() || spec[0] != '@') return RpkPack::Parse(ReadFile(spec));
    auto archive = OpenGame(args);
    const IndexEntry* e = archive->Find(GamePath(spec.substr(1)));
    if (!e) throw Error("not in the archive: " + spec);
    return RpkPack::Parse(archive->Read(*e));
}

}  // namespace

int RunMesh(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game", "-o", "--output"});
    if (args.positional.size() < 2) return Usage();
    RpkPack pack = LoadPack(args, args.positional[1]);
    std::vector<Sub> subs = FindSubs(pack);

    if (cmd == "info") {
        size_t tris = 0;
        for (const Sub& s : subs) {
            std::printf("%-60s %6u vertices %7u triangles\n", s.name.c_str(), s.vertices(), s.count / 3);
            tris += s.count / 3;
        }
        std::printf("%zu sub meshes, %zu triangles\n", subs.size(), tris);
        return 0;
    }
    if (cmd == "export") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty()) throw Error("export needs -o FILE.glb");
        if (args.positional.size() > 2) {
            std::vector<Sub> keep;
            for (const Sub& s : subs) {
                for (size_t i = 2; i < args.positional.size(); ++i) {
                    if (WildMatch(Lower(args.positional[i]), Lower(s.name))) {
                        keep.push_back(s);
                        break;
                    }
                }
            }
            subs = keep;
        }
        if (subs.empty()) throw Error("no sub meshes to write");
        WriteFile(out, MakeGlb(pack, subs));
        std::printf("wrote %zu sub meshes to %s\n", subs.size(), out.c_str());
        return 0;
    }
    if (cmd == "import") {
        const std::string out = args.Get("-o", args.Get("--output"));
        if (out.empty() || args.positional.size() < 3) throw Error("import needs FILE.glb and -o OUT.rpk");
        const Glb g = ReadGlb(ReadFile(args.positional[2]));
        const Json* nodes = g.json.Find("nodes");
        const Json* meshes = g.json.Find("meshes");
        if (!nodes || !meshes) throw Error("the file has no objects");
        size_t used = 0;
        for (const Json& node : nodes->Items()) {
            const std::string name = node.GetString("name");
            if (name.size() < 18 || name[16] != '_' || !node.Has("mesh")) continue;
            uint64_t id = 0;
            try {
                id = ParseHexId(name.substr(0, 16));
            } catch (const Error&) {
                continue;
            }
            const size_t us = name.find('_', 17);
            const uint32_t k = uint32_t(std::strtoul(name.substr(17, us == std::string::npos ? std::string::npos : us - 17).c_str(), nullptr, 10));
            const Sub* sub = nullptr;
            for (const Sub& s : subs) {
                if (s.mesh == id && s.k == k) sub = &s;
            }
            if (!sub) continue;
            const Json& prim = meshes->Items().at(size_t(node.GetInt("mesh"))).Find("primitives")->Items().at(0);
            const std::vector<double> p = Accessor(g, prim.Find("attributes")->GetInt("POSITION"), 3);
            if (p.size() != size_t(sub->vertices()) * 3) {
                throw Error(name + ": has " + std::to_string(p.size() / 3) + " vertices, the game's mesh has " + std::to_string(sub->vertices()) + "; the number of vertices cannot change");
            }
            RpkResource* vb = pack.Find(sub->pos);
            for (uint32_t i = 0; i < sub->vertices(); ++i) {
                uint8_t* dst = &vb->data[28 + size_t(sub->lo + i) * sub->pos_stride];
                for (int c = 0; c < 3; ++c) {
                    const float f = float(p[i * 3 + size_t(c)]);
                    // vertices the game left unset (or a position that is the same after rounding) stay as they are
                    if (sub->half) {
                        const uint16_t h = FloatToHalf(f);
                        if (HalfToFloat(h) != Clean(HalfToFloat(Rd16(dst + c * 2))) && h != Rd16(dst + c * 2)) std::memcpy(dst + c * 2, &h, 2);
                    } else {
                        float old;
                        std::memcpy(&old, dst + c * 4, 4);
                        if (f != Clean(old)) std::memcpy(dst + c * 4, &f, 4);
                    }
                }
            }
            if (prim.Has("indices")) {
                const std::vector<double> idx = Accessor(g, prim.GetInt("indices"), 1);
                if (idx.size() != sub->count) {
                    throw Error(name + ": has " + std::to_string(idx.size() / 3) + " triangles, the game's mesh has " + std::to_string(sub->count / 3) + "; the number of triangles cannot change");
                }
                RpkResource* ib = pack.Find(sub->ib);
                for (uint32_t i = 0; i < sub->count; ++i) {
                    const uint32_t v = uint32_t(idx[i]) + sub->lo;
                    if (v > sub->hi) throw Error(name + ": a triangle uses a vertex that does not exist");
                    ib->data[28 + (sub->first + i) * 2] = uint8_t(v);
                    ib->data[29 + (sub->first + i) * 2] = uint8_t(v >> 8);
                }
            }
            ++used;
        }
        if (!used) throw Error("none of the objects in the file came from this pack (their names start with the mesh identifier)");
        WriteFile(out, pack.Write());
        std::printf("updated %zu sub meshes, wrote %s\n", used, out.c_str());
        return 0;
    }
    return Usage();
}

}  // namespace dcmod
