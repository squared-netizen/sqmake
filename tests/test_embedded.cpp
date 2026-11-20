// SPDX-License-Identifier: MIT
// docs/spec.md §7.2: sqmake in a host program's Lua state.
// The host's state is used and left open; the API is in its `sqmake`
// table; a build file sees none of the host's globals.
#include <stdio.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

import std;
import sqmake.graph;
import sqmake.lua;

int main() {
    int failures = 0;
    auto check = [&](bool ok, std::string_view what) {
        if (!ok) {
            std::println(stderr, "FAIL {}", what);
            ++failures;
        }
    };
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushstring(L, "the host's secret");
    lua_setglobal(L, "secret");
    const auto file = std::filesystem::temp_directory_path() / std::format("sqmake-embedded-{}.lua", std::random_device{}());
    {
        std::ofstream f(file);
        f << "assert(secret == nil, 'a host global reached the build file')\n"
             "assert(io == nil and os.execute == nil, 'unsafe libraries reached the build file')\n"
             "target{out = 'x', cmd = {'true'}}\n"
             "default('x')\n";
    }
    {
        auto ctx = sqmake::Context::create({.lua = {L}});
        check(ctx.has_value(), "create with the host's state");
        auto loaded = (*ctx)->load(file);
        check(loaded.has_value(), loaded ? "load" : "load: " + loaded.error());
        check((*ctx)->graph().targets().size() == 1 && (*ctx)->defaults() == std::vector<std::string>{"x"}, "the target and default");
    }
    lua_getglobal(L, "sqmake");
    check(lua_istable(L, -1), "the API in the host's sqmake table");
    lua_getfield(L, -1, "target");
    check(lua_isfunction(L, -1), "sqmake.target");
    lua_settop(L, 0);
    lua_getglobal(L, "secret");
    check(lua_isstring(L, -1), "the host's state still open, its globals intact");
    lua_close(L);
    std::filesystem::remove(file);
    if (failures == 0) std::println("ok   embedded");
    return failures == 0 ? 0 : 1;
}
