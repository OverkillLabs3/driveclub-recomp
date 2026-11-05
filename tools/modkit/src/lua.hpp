// SPDX-License-Identifier: GPL-2.0-or-later
// Lua 5.1 bytecode: the game's scripts (scripts/**/*.lua.bin) are compiled with the standard 5.1 compiler.
// This reads that format, prints it as an assembly listing and collects the names a script uses, which is how the
// scripting interface of the game can be learned without its source.
#pragma once

#include "common.hpp"

#include <map>
#include <set>

namespace dcmod {

struct LuaConst {
    enum Type { Nil, Bool, Number, String } type = Nil;
    bool b = false;
    double n = 0;
    std::string s;
    std::string Show() const;  // as Lua source would write it
};

struct LuaProto {
    std::string source;  // chunk name, empty for stripped scripts
    int line_defined = 0, last_line = 0;
    uint8_t nups = 0, numparams = 0, is_vararg = 0, max_stack = 0;
    std::vector<uint32_t> code;
    std::vector<LuaConst> k;
    std::vector<LuaProto> protos;
    std::vector<int> lines;  // line of each instruction; empty when stripped
    struct Local {
        std::string name;
        int start = 0, end = 0;
    };
    std::vector<Local> locals;
    std::vector<std::string> upvalues;
};

struct LuaChunk {
    uint8_t header[12] = {};
    int size_t_bytes = 8;
    LuaProto main;
};

bool IsLuaBytecode(const Bytes& b);
LuaChunk ParseLua(const Bytes& b);  // throws Error
Bytes WriteLua(const LuaChunk& chunk);  // gives back the same bytes for a chunk that was read and not changed

// Assembly listing like `luac -l -l`, with the names of constants, locals and upvalues where the script has them.
std::string DisassembleLua(const LuaChunk& chunk, bool with_lines = true);

// What a script refers to by name.
struct LuaUsage {
    std::map<std::string, int> globals_read, globals_written;
    std::map<std::string, int> calls;    // "Gui.Show", "string.format", "obj:Method" ... as far as the code shows
    std::map<std::string, int> fields;   // names used as table keys: x.name or x["name"]
    std::set<std::string> strings;       // every string constant
    size_t functions = 0, instructions = 0;
};
void CollectLuaUsage(const LuaChunk& chunk, LuaUsage& out);

}  // namespace dcmod
