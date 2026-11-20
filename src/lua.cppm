// SPDX-License-Identifier: MIT
//
// sqmake.lua: build files in Lua 5.4 (docs/spec.md §3, §7.2).
//
//   auto ctx = sqmake::Context::create({});            // its own Lua state
//   auto ctx = sqmake::Context::create({.lua = {L}});  // a host program's
//
// Alone, sqmake makes a state and closes it with the context. Embedded, it
// uses the host's, never closes it, and puts its API in the host's
// `sqmake` table. Either way a build file runs in an environment of its
// own: the API, the prelude's helpers and Lua's safe libraries (string,
// table, math, utf8, os.getenv), and none of the host's globals.
//
// The API:
//
//   target{out=..., ins={...}, cmd={...}, depfile=..., desc=...}
//                         one target; out a name or a list; ins and cmd
//                         flattened (a list may hold lists)
//   glob(pattern)         files matching it, sorted: * and ? within a
//                         name, ** across folders
//   default(name, ...)    what `sqmake` builds when given no goal
//
// and the prelude's (Lua, below): toolchain, rules.cxx, rules.cc, map, exe.

module;

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

export module sqmake.lua;

import std;
import sqmake.graph;

namespace sqmake {

constexpr std::string_view prelude = R"lua(
-- sqmake's prelude: what a build file can use without defining it.
-- $CXX and $CC as make takes them: words, so "ccache clang++" or
-- "clang++ -stdlib=libc++" work.
local function words(s)
  local out = {}
  for w in s:gmatch("%S+") do out[#out + 1] = w end
  return out
end
toolchain = {
  cxx = words(os.getenv("CXX") or "clang++"),
  cc = words(os.getenv("CC") or "clang"),
  cxxflags = {"-std=c++23", "-O2"},
  cflags = {"-O2"},
  ldflags = {},
  build = "build",
}
rules = {}
-- A C++ source compiled to an object; its headers found by -MMD.
function rules.cxx(src)
  local obj = toolchain.build .. "/obj/" .. src .. ".o"
  target{out = obj, ins = {src}, depfile = obj .. ".d", desc = "cxx " .. src,
         cmd = {toolchain.cxx, toolchain.cxxflags, "-MMD", "-MF", obj .. ".d", "-c", src, "-o", obj}}
  return obj
end
function rules.cc(src)
  local obj = toolchain.build .. "/obj/" .. src .. ".o"
  target{out = obj, ins = {src}, depfile = obj .. ".d", desc = "cc " .. src,
         cmd = {toolchain.cc, toolchain.cflags, "-MMD", "-MF", obj .. ".d", "-c", src, "-o", obj}}
  return obj
end
-- A list, each item through f.
function map(list, f)
  local out = {}
  for i, v in ipairs(list) do out[i] = f(v) end
  return out
end
-- A program linked from objects; built by default.
function exe(name, objs)
  local out = toolchain.build .. "/" .. name
  target{out = out, ins = objs, desc = "link " .. name, cmd = {toolchain.cxx, objs, toolchain.ldflags, "-o", out}}
  default(out)
  return out
end
)lua";

}  // namespace sqmake

export namespace sqmake {

struct LuaOptions {
    lua_State* state = nullptr;  // the host's; none: sqmake makes its own
};
struct Options {
    LuaOptions lua;
};

class Context {
public:
    /// A context, refusing any Lua but 5.4.
    static std::expected<std::unique_ptr<Context>, std::string> create(Options o) {
        std::unique_ptr<Context> c(new Context);
        c->owned_ = o.lua.state == nullptr;
        c->L_ = c->owned_ ? luaL_newstate() : o.lua.state;
        if (!c->L_) return std::unexpected("cannot make a Lua state");
        const lua_Number v = lua_version(c->L_);
        if (v < 504 || v >= 505) return std::unexpected(std::format("Lua {}: sqmake needs Lua 5.4", v));
        if (c->owned_) luaL_openlibs(c->L_);
        return c;
    }

    ~Context() {
        if (owned_ && L_) lua_close(L_);
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /// Runs a build file, in an environment of its own; its targets go into
    /// the graph.
    std::expected<void, std::string> load(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return std::unexpected("cannot read " + file.string());
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        lua_State* L = L_;
        // The environment: the API and Lua's safe libraries.
        lua_newtable(L);
        const int env = lua_gettop(L);
        auto api = [&](const char* name, lua_CFunction f) {
            lua_pushlightuserdata(L, this);
            lua_pushcclosure(L, f, 1);
            lua_setfield(L, env, name);
        };
        api("target", &Context::l_target);
        api("glob", &Context::l_glob);
        api("default", &Context::l_default);
        for (const char* lib : {"string", "table", "math", "utf8"}) {
            lua_getglobal(L, lib);
            lua_setfield(L, env, lib);
        }
        for (const char* f : {"ipairs", "pairs", "next", "type", "tostring", "tonumber", "error", "assert", "select", "pcall",
                              "print", "rawget", "rawset", "rawequal", "rawlen", "setmetatable", "getmetatable"}) {
            lua_getglobal(L, f);
            lua_setfield(L, env, f);
        }
        lua_newtable(L);
        lua_getglobal(L, "os");
        lua_getfield(L, -1, "getenv");
        lua_setfield(L, -3, "getenv");
        lua_pop(L, 1);
        lua_setfield(L, env, "os");
        // Embedded: the API also in the host's own `sqmake` table.
        if (!owned_) {
            lua_pushvalue(L, env);
            lua_setglobal(L, "sqmake");
        }
        for (auto [code, name] : {std::pair{std::string(prelude), std::string("=prelude")}, std::pair{text, "@" + file.string()}}) {
            if (luaL_loadbufferx(L, code.data(), code.size(), name.c_str(), "t") != LUA_OK) {
                std::string why = lua_tostring(L, -1);
                lua_settop(L, env - 1);
                return std::unexpected(why);
            }
            lua_pushvalue(L, env);
            lua_setupvalue(L, -2, 1);  // _ENV
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::string why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error";
                lua_settop(L, env - 1);
                return std::unexpected(why);
            }
        }
        // The build folder the prelude's toolchain names.
        lua_getfield(L, env, "toolchain");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "build");
            if (lua_isstring(L, -1)) build_dir_ = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_settop(L, env - 1);
        if (!error_.empty()) return std::unexpected(error_);
        return {};
    }

    Graph& graph() { return graph_; }
    const std::vector<std::string>& defaults() const { return defaults_; }
    const std::string& build_dir() const { return build_dir_; }

private:
    Context() = default;

    static Context& self(lua_State* L) { return *static_cast<Context*>(lua_touserdata(L, lua_upvalueindex(1))); }

    /// The strings in a value: a string, or a list of them, flattened.
    static void strings(lua_State* L, int index, std::vector<std::string>& out) {
        index = lua_absindex(L, index);
        if (lua_isnil(L, index)) return;
        if (lua_type(L, index) == LUA_TSTRING || lua_type(L, index) == LUA_TNUMBER) {
            out.emplace_back(lua_tostring(L, index));
            return;
        }
        if (!lua_istable(L, index)) luaL_error(L, "expected a string or a list of strings, not a %s", luaL_typename(L, index));
        const lua_Integer n = luaL_len(L, index);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, index, i);
            strings(L, -1, out);
            lua_pop(L, 1);
        }
    }

    static int l_target(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        Target t;
        lua_getfield(L, 1, "out");
        strings(L, -1, t.outputs);
        lua_getfield(L, 1, "ins");
        strings(L, -1, t.inputs);
        lua_getfield(L, 1, "cmd");
        strings(L, -1, t.command.argv);
        lua_getfield(L, 1, "depfile");
        if (lua_isstring(L, -1)) t.depfile = lua_tostring(L, -1);
        lua_getfield(L, 1, "desc");
        if (lua_isstring(L, -1)) t.description = lua_tostring(L, -1);
        lua_pop(L, 5);
        auto added = self(L).graph_.add(std::move(t));
        if (!added) return luaL_error(L, "target: %s", added.error().c_str());
        return 0;
    }

    static int l_default(lua_State* L) {
        for (int i = 1; i <= lua_gettop(L); ++i) strings(L, i, self(L).defaults_);
        return 0;
    }

    /// `*` and `?` within one name; `**` any number of folders.
    static bool match(std::string_view pattern, std::string_view path) {
        if (pattern.empty()) return path.empty();
        if (pattern.starts_with("**/")) {
            if (match(pattern.substr(3), path)) return true;
            for (std::size_t i = 0; i < path.size(); ++i)
                if (path[i] == '/' && match(pattern.substr(3), path.substr(i + 1))) return true;
            return false;
        }
        if (pattern[0] == '*') {
            for (std::size_t i = 0; i <= path.size(); ++i) {
                if (match(pattern.substr(1), path.substr(i))) return true;
                if (i < path.size() && path[i] == '/') break;
            }
            return false;
        }
        if (path.empty()) return false;
        if (pattern[0] == '?' ? path[0] == '/' : pattern[0] != path[0]) return false;
        return match(pattern.substr(1), path.substr(1));
    }

    static int l_glob(lua_State* L) {
        const std::string pattern = luaL_checkstring(L, 1);
        // Walk from the part before the first wildcard.
        const std::size_t wild = pattern.find_first_of("*?");
        std::string root = wild == std::string::npos ? pattern : pattern.substr(0, wild);
        root = root.substr(0, root.rfind('/') == std::string::npos ? 0 : root.rfind('/'));
        std::vector<std::string> found;
        std::error_code ec;
        const std::filesystem::path start = root.empty() ? "." : root;
        if (std::filesystem::is_directory(start, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(start, ec); it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string p = it->path().generic_string();
                if (p.starts_with("./")) p = p.substr(2);
                if (match(pattern, p)) found.push_back(p);
            }
        }
        std::ranges::sort(found);
        lua_createtable(L, static_cast<int>(found.size()), 0);
        for (std::size_t i = 0; i < found.size(); ++i) {
            lua_pushstring(L, found[i].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    lua_State* L_ = nullptr;
    bool owned_ = true;
    Graph graph_;
    std::vector<std::string> defaults_;
    std::string build_dir_ = "build";
    std::string error_;
};

}  // namespace sqmake
