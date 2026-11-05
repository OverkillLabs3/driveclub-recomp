// SPDX-License-Identifier: GPL-2.0-or-later
#include "lua.hpp"

#include <cstdio>

namespace dcmod {

namespace {

enum Mode { kABC, kABx, kAsBx };
enum Op {
    MOVE, LOADK, LOADBOOL, LOADNIL, GETUPVAL, GETGLOBAL, GETTABLE, SETGLOBAL, SETUPVAL, SETTABLE, NEWTABLE, SELF, ADD,
    SUB, MUL, DIV, MOD, POW, UNM, NOT, LEN, CONCAT, JMP, EQ, LT, LE, TEST, TESTSET, CALL, TAILCALL, RETURN, FORLOOP,
    FORPREP, TFORLOOP, SETLIST, CLOSE, CLOSURE, VARARG, kOpCount
};

struct OpInfo {
    const char* name;
    Mode mode;
    bool b_rk, c_rk;  // operand B / C may name a constant (value 256 and up)
    bool uses_b, uses_c;
};

// Same as Lua 5.1's lopcodes.c.
const OpInfo kOps[kOpCount] = {
    {"MOVE", kABC, false, false, true, false},      {"LOADK", kABx, false, false, false, false},
    {"LOADBOOL", kABC, false, false, true, true},   {"LOADNIL", kABC, false, false, true, false},
    {"GETUPVAL", kABC, false, false, true, false},  {"GETGLOBAL", kABx, false, false, false, false},
    {"GETTABLE", kABC, false, true, true, true},    {"SETGLOBAL", kABx, false, false, false, false},
    {"SETUPVAL", kABC, false, false, true, false},  {"SETTABLE", kABC, true, true, true, true},
    {"NEWTABLE", kABC, false, false, true, true},   {"SELF", kABC, false, true, true, true},
    {"ADD", kABC, true, true, true, true},          {"SUB", kABC, true, true, true, true},
    {"MUL", kABC, true, true, true, true},          {"DIV", kABC, true, true, true, true},
    {"MOD", kABC, true, true, true, true},          {"POW", kABC, true, true, true, true},
    {"UNM", kABC, false, false, true, false},       {"NOT", kABC, false, false, true, false},
    {"LEN", kABC, false, false, true, false},       {"CONCAT", kABC, false, false, true, true},
    {"JMP", kAsBx, false, false, false, false},     {"EQ", kABC, true, true, true, true},
    {"LT", kABC, true, true, true, true},           {"LE", kABC, true, true, true, true},
    {"TEST", kABC, false, false, false, true},      {"TESTSET", kABC, false, false, true, true},
    {"CALL", kABC, false, false, true, true},       {"TAILCALL", kABC, false, false, true, true},
    {"RETURN", kABC, false, false, true, false},    {"FORLOOP", kAsBx, false, false, false, false},
    {"FORPREP", kAsBx, false, false, false, false}, {"TFORLOOP", kABC, false, false, false, true},
    {"SETLIST", kABC, false, false, true, true},    {"CLOSE", kABC, false, false, false, false},
    {"CLOSURE", kABx, false, false, false, false},  {"VARARG", kABC, false, false, true, false}};

constexpr int kBias = 131071;  // sBx is stored as Bx plus this

struct Ins {
    unsigned op, a, b, c, bx;
    int sbx;
};

Ins Decode(uint32_t i) {
    Ins d;
    d.op = i & 0x3F;
    d.a = (i >> 6) & 0xFF;
    d.c = (i >> 14) & 0x1FF;
    d.b = (i >> 23) & 0x1FF;
    d.bx = i >> 14;
    d.sbx = int(d.bx) - kBias;
    return d;
}

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\%03u", c);
                    out += buf;
                } else {
                    out.push_back(char(c));
                }
        }
    }
    return out + "\"";
}

std::string ReadString(ByteReader& r, int size_bytes) {
    const uint64_t n = size_bytes == 8 ? r.U64() : r.U32();
    if (n == 0) return {};
    if (n > r.left()) throw Error("a string in the Lua script is longer than the script");
    const uint8_t* p = r.Take(size_t(n));
    return std::string(reinterpret_cast<const char*>(p), size_t(n) - 1);
}

LuaProto LoadFunction(ByteReader& r, int size_bytes, int depth) {
    if (depth > 200) throw Error("Lua functions are nested too deeply");
    LuaProto f;
    f.source = ReadString(r, size_bytes);
    f.line_defined = int(r.U32());
    f.last_line = int(r.U32());
    f.nups = r.U8();
    f.numparams = r.U8();
    f.is_vararg = r.U8();
    f.max_stack = r.U8();
    const uint32_t ncode = r.U32();
    if (uint64_t(ncode) * 4 > r.left()) throw Error("the Lua code is longer than the script");
    f.code.resize(ncode);
    for (uint32_t& c : f.code) c = r.U32();
    const uint32_t nk = r.U32();
    if (nk > r.left()) throw Error("too many Lua constants");
    f.k.resize(nk);
    for (LuaConst& k : f.k) {
        switch (r.U8()) {
            case 0: k.type = LuaConst::Nil; break;
            case 1:
                k.type = LuaConst::Bool;
                k.b = r.U8() != 0;
                break;
            case 3: {
                k.type = LuaConst::Number;
                const uint64_t bits = r.U64();
                std::memcpy(&k.n, &bits, 8);
                break;
            }
            case 4:
                k.type = LuaConst::String;
                k.s = ReadString(r, size_bytes);
                break;
            default: throw Error("a Lua constant has an unknown type");
        }
    }
    const uint32_t np = r.U32();
    if (np > r.left()) throw Error("too many Lua functions");
    for (uint32_t i = 0; i < np; ++i) f.protos.push_back(LoadFunction(r, size_bytes, depth + 1));
    const uint32_t nl = r.U32();
    if (uint64_t(nl) * 4 > r.left()) throw Error("the Lua line table is longer than the script");
    f.lines.resize(nl);
    for (int& l : f.lines) l = int(r.U32());
    const uint32_t nloc = r.U32();
    if (nloc > r.left()) throw Error("too many Lua locals");
    for (uint32_t i = 0; i < nloc; ++i) {
        LuaProto::Local l;
        l.name = ReadString(r, size_bytes);
        l.start = int(r.U32());
        l.end = int(r.U32());
        f.locals.push_back(std::move(l));
    }
    const uint32_t nup = r.U32();
    if (nup > r.left()) throw Error("too many Lua upvalue names");
    for (uint32_t i = 0; i < nup; ++i) f.upvalues.push_back(ReadString(r, size_bytes));
    return f;
}

std::string KName(const LuaProto& f, unsigned index) {
    return index < f.k.size() ? f.k[index].Show() : "K?";
}

void DisassembleFunction(const LuaProto& f, const std::string& title, bool with_lines, std::string& out) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "\n; function %s, lines %d-%d, %u params%s, %u slots, %u upvalues, %zu constants, %zu functions\n",
                  title.c_str(), f.line_defined, f.last_line, unsigned(f.numparams), f.is_vararg ? " and varargs" : "",
                  unsigned(f.max_stack), unsigned(f.nups), f.k.size(), f.protos.size());
    out += buf;
    if (!f.source.empty() && title == "main") out += "; chunk name: " + f.source + "\n";
    for (size_t i = 0; i < f.upvalues.size(); ++i) out += "; upvalue " + std::to_string(i) + " = " + f.upvalues[i] + "\n";
    for (size_t i = 0; i < f.locals.size(); ++i) {
        std::snprintf(buf, sizeof buf, "; local %s, instructions %d-%d\n", f.locals[i].name.c_str(), f.locals[i].start, f.locals[i].end);
        out += buf;
    }
    for (size_t pc = 0; pc < f.code.size(); ++pc) {
        const Ins d = Decode(f.code[pc]);
        if (d.op >= kOpCount) {
            std::snprintf(buf, sizeof buf, "%5zu  ?? opcode %u\n", pc, d.op);
            out += buf;
            continue;
        }
        const OpInfo& info = kOps[d.op];
        std::string line;
        std::snprintf(buf, sizeof buf, "%5zu  ", pc);
        line = buf;
        if (with_lines && pc < f.lines.size()) {
            std::snprintf(buf, sizeof buf, "[%4d]  ", f.lines[pc]);
            line += buf;
        }
        std::snprintf(buf, sizeof buf, "%-9s ", info.name);
        line += buf;
        std::string note;
        const auto rk = [&](unsigned v) {
            if (v >= 256) {
                if (!note.empty()) note += ", ";
                note += KName(f, v - 256);
                return "K" + std::to_string(v - 256);
            }
            return "R" + std::to_string(v);
        };
        switch (info.mode) {
            case kABC: {
                line += "A=" + std::to_string(d.a);
                if (info.uses_b) line += " B=" + (info.b_rk ? rk(d.b) : std::to_string(d.b));
                if (info.uses_c) line += " C=" + (info.c_rk ? rk(d.c) : std::to_string(d.c));
                break;
            }
            case kABx:
                line += "A=" + std::to_string(d.a) + " Bx=" + std::to_string(d.bx);
                if (d.op == LOADK || d.op == GETGLOBAL || d.op == SETGLOBAL) note = KName(f, d.bx);
                if (d.op == CLOSURE) note = "function " + std::to_string(d.bx);
                break;
            case kAsBx: {
                if (d.op != JMP) line += "A=" + std::to_string(d.a) + " ";
                line += "sBx=" + std::to_string(d.sbx);
                note = "to " + std::to_string(int(pc) + 1 + d.sbx);
                break;
            }
        }
        if (d.op == GETUPVAL || d.op == SETUPVAL) {
            const unsigned u = d.b;
            if (u < f.upvalues.size()) note = f.upvalues[u];
        }
        while (line.size() < 46) line.push_back(' ');
        out += line;
        if (!note.empty()) out += " ; " + note;
        out.push_back('\n');
    }
    for (size_t i = 0; i < f.protos.size(); ++i) {
        DisassembleFunction(f.protos[i], title + "/" + std::to_string(i), with_lines, out);
    }
}

void Collect(const LuaProto& f, LuaUsage& u) {
    ++u.functions;
    u.instructions += f.code.size();
    for (const LuaConst& k : f.k) {
        if (k.type == LuaConst::String) u.strings.insert(k.s);
    }
    std::vector<std::string> reg(size_t(f.max_stack) + 260);
    const auto str_k = [&](unsigned v, std::string& out) {
        if (v >= 256) v -= 256;
        else return false;
        if (v < f.k.size() && f.k[v].type == LuaConst::String) {
            out = f.k[v].s;
            return true;
        }
        return false;
    };
    const auto clear_from = [&](unsigned a, unsigned n) {
        for (unsigned i = a; i < a + n && i < reg.size(); ++i) reg[i].clear();
    };
    for (uint32_t word : f.code) {
        const Ins d = Decode(word);
        std::string key;
        switch (d.op) {
            case GETGLOBAL:
                if (d.bx < f.k.size() && f.k[d.bx].type == LuaConst::String) {
                    u.globals_read[f.k[d.bx].s]++;
                    reg[d.a] = f.k[d.bx].s;
                } else {
                    reg[d.a].clear();
                }
                break;
            case SETGLOBAL:
                if (d.bx < f.k.size() && f.k[d.bx].type == LuaConst::String) u.globals_written[f.k[d.bx].s]++;
                break;
            case GETTABLE:
                if (str_k(d.c, key)) {
                    u.fields[key]++;
                    reg[d.a] = (reg[d.b].empty() ? "?" : reg[d.b]) + "." + key;
                } else {
                    reg[d.a].clear();
                }
                break;
            case SETTABLE:
                if (str_k(d.b, key)) u.fields[key]++;
                break;
            case SELF:
                if (str_k(d.c, key)) {
                    u.fields[key]++;
                    const std::string base = reg[d.b];
                    reg[d.a + 1] = base;
                    reg[d.a] = (base.empty() ? "?" : base) + ":" + key;
                } else {
                    reg[d.a].clear();
                    reg[d.a + 1].clear();
                }
                break;
            case MOVE: reg[d.a] = reg[d.b]; break;
            case GETUPVAL:
                reg[d.a] = d.b < f.upvalues.size() ? f.upvalues[d.b] : std::string();
                break;
            case CALL:
            case TAILCALL:
                if (!reg[d.a].empty()) u.calls[reg[d.a]]++;
                clear_from(d.a, d.c > 1 ? d.c : 8);
                break;
            case LOADNIL: clear_from(d.a, d.b >= d.a ? d.b - d.a + 1 : 1); break;
            case TFORLOOP: clear_from(d.a + 3, d.c); break;
            case VARARG: clear_from(d.a, d.b > 1 ? d.b - 1 : 8); break;
            case SETUPVAL: case SETLIST: case CLOSE: case JMP: case EQ: case LT: case LE: case TEST: case RETURN:
            case FORLOOP: case FORPREP:
                break;
            default:
                if (d.op < kOpCount) reg[d.a].clear();
        }
    }
    for (const LuaProto& p : f.protos) Collect(p, u);
}

}  // namespace

std::string LuaConst::Show() const {
    switch (type) {
        case Nil: return "nil";
        case Bool: return b ? "true" : "false";
        case Number: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.14g", n);
            return buf;
        }
        case String: return Quote(s);
    }
    return {};
}

bool IsLuaBytecode(const Bytes& b) { return b.size() >= 12 && std::memcmp(b.data(), "\x1b" "Lua", 4) == 0 && b[4] == 0x51; }

LuaChunk ParseLua(const Bytes& b) {
    if (!IsLuaBytecode(b)) throw Error("this is not compiled Lua 5.1 code");
    if (b[5] != 0 || b[6] != 1 || b[7] != 4 || (b[8] != 4 && b[8] != 8) || b[9] != 4 || b[10] != 8 || b[11] != 0) {
        throw Error("this Lua code was compiled for a different machine (sizes or byte order differ from the game's)");
    }
    LuaChunk c;
    c.size_t_bytes = b[8];
    ByteReader r(b, "the Lua script");
    r.Seek(12);
    c.main = LoadFunction(r, c.size_t_bytes, 0);
    if (r.left() != 0) throw Error("the Lua script has bytes after its end");
    return c;
}

std::string DisassembleLua(const LuaChunk& chunk, bool with_lines) {
    std::string out = "; Lua 5.1 bytecode, " + std::to_string(chunk.size_t_bytes * 8) + " bit sizes\n";
    DisassembleFunction(chunk.main, "main", with_lines, out);
    return out;
}

void CollectLuaUsage(const LuaChunk& chunk, LuaUsage& out) { Collect(chunk.main, out); }

}  // namespace dcmod
