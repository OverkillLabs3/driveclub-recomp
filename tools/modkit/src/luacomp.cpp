// SPDX-License-Identifier: GPL-2.0-or-later
// Compiles Lua source to the bytecode the game loads, with the real Lua 5.1.5 compiler (third_party/lua51, MIT).
// The game cannot run chunks it compiles from text itself, but runs bytecode, so mod scripts are shipped compiled.
#include "lua.hpp"

extern "C" {
#include "../../../third_party/lua51/lauxlib.h"
#include "../../../third_party/lua51/lua.h"
}

namespace dcmod {

namespace {

int Writer(lua_State*, const void* p, size_t n, void* ud) {
    Bytes* out = static_cast<Bytes*>(ud);
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out->insert(out->end(), b, b + n);
    return 0;
}

}  // namespace

Bytes CompileLua(const std::string& source, const std::string& chunk_name) {
    lua_State* L = luaL_newstate();
    if (!L) throw Error("cannot start the Lua compiler");
    Bytes out;
    std::string problem;
    if (luaL_loadbuffer(L, source.data(), source.size(), ("@" + chunk_name).c_str()) != 0) {
        problem = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
    } else if (lua_dump(L, Writer, &out) != 0) {
        problem = "the compiler could not write the bytecode";
    }
    lua_close(L);
    if (!problem.empty()) throw Error(problem);
    return out;
}

}  // namespace dcmod
