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
//   write_file(path, text)  the file holds the text (written only when it
//                         differs): a list the build derives
//   read_file(path [, max])  its text (at most max bytes), or nil
//   include(path)         another Lua file run in this environment (a
//                         template's or a kit's conventions); what it
//                         returns
//   target{..., cmds={{...}, {...}}}
//                         commands in order, each after the one before
//
// and the prelude's (Lua, below): toolchain, rules.cxx, rules.cc,
// rules.module (each with a source's own flags), map, lib, exe, and with
// usage requirements library{}, exe{}, shlib{}, apk{} (apkpack); verb()
// (build, run, install, test: what each does) and action{} (a project's own
// words), mode (--debug), exec() inside a verb's call; toolchain.sdk(path) for the SDK's own
// toolchain (squared's build/toolchain.lua, resolved by sqmake.toolchain;
// an older squared's build/toolchain.sh, through bash).
//
// C++ modules (spec §5): rules.module(src) scans the source's declarations
// (sqmake.scan) at once; when the build file has run, every unit becomes a
// target, an interface's BMI in build/modules/<fingerprint>/ among its
// outputs and each module it imports among its inputs, so a module is
// compiled before what imports it. `import std` adds one std target, from
// the toolchain's libc++ std.cppm, and its object to every program.

module;

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

export module sqmake.lua;

import std;
import sqmake.graph;
import sqmake.run;
import sqmake.scan;
import sqmake.toolchain;

namespace sqmake {

constexpr std::string_view prelude = R"lua(
-- sqmake's prelude: what a build file can use without defining it.
local verb_names = {build = true, run = true, install = true, test = true}
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
-- flags: the source's own (a library's include folders and defines).
function rules.cxx(src, flags, opts)
  local obj = toolchain.build .. "/obj/" .. src .. ".o"
  target{out = obj, ins = {src, (opts or {}).after or {}}, depfile = obj .. ".d", desc = "cxx " .. src,
         cmd = {toolchain.cxx, toolchain.cxxflags, flags or {}, "-MMD", "-MF", obj .. ".d", "-c", src, "-o", obj}}
  return obj
end
function rules.cc(src, flags, opts)
  local obj = toolchain.build .. "/obj/" .. src .. ".o"
  target{out = obj, ins = {src, (opts or {}).after or {}}, depfile = obj .. ".d", desc = "cc " .. src,
         cmd = {toolchain.cc, toolchain.cflags, flags or {}, "-MMD", "-MF", obj .. ".d", "-c", src, "-o", obj}}
  return obj
end
-- A C++ source that is, or imports, a module: scanned now, its target
-- made when the build file has run (inputs: the BMIs it imports).
-- opts: after = files it waits for (generated headers); imports = its
-- imports, for a generated source (not there yet to scan).
function rules.module(src, flags, opts)
  local obj = toolchain.build .. "/obj/" .. src .. ".o"
  module_unit(src, obj, flags or {}, opts or {})
  return obj
end
-- The SDK's toolchain: squared's build/toolchain.lua, found here by sqmake
-- itself (no shell), or an older squared's build/toolchain.sh. The
-- commands carry every flag; its link flags go before the build file's.
function toolchain.sdk(file)
  local t = sdk_toolchain(file, toolchain.build)
  toolchain.cxx = t.cxx
  toolchain.cc = t.cc
  toolchain.ar = t.ar
  toolchain.fingerprint = t.fingerprint
  toolchain.std_cppm = t.std_cppm
  if t.ldflags then toolchain.ldflags = {t.ldflags, toolchain.ldflags} end
  return t
end
-- A list, each item through f.
function map(list, f)
  local out = {}
  for i, v in ipairs(list) do out[i] = f(v) end
  return out
end
-- A static library of objects.
function lib(name, objs)
  local out = toolchain.build .. "/lib" .. name .. ".a"
  target{out = out, ins = objs, desc = "ar lib" .. name .. ".a", cmd = {toolchain.ar or "ar", "rcs", out, objs}}
  return out
end
-- A program linked from objects (and libraries); built by default. A
-- program whose modules import std links std's object too. Or a table:
-- exe{name=, srcs=, objs=, flags=, deps=, link=}, as shlib's.
function exe(name, objs)
  if type(name) == "table" then return linked(name, "exe") end
  local out = toolchain.build .. "/" .. name
  if std_used() then objs = {objs, toolchain.build .. "/obj/std.o"} end
  target{out = out, ins = objs, desc = "link " .. name, cmd = {toolchain.cxx, objs, toolchain.ldflags, "-o", out}}
  default(out)
  return out
end

-- Usage requirements (spec 3.3). A library made by library{} carries what
-- its users need: public flags (include folders, defines) for their
-- compiles, and its archive and public link flags for their links, its own
-- dependencies' after its own, so a static link finds every symbol.
-- A list of lists as one list of words, each once: its first place, or for
-- archives its last (a library's dependencies after everything using them).
local function flat(list, out)
  out = out or {}
  for _, v in ipairs(list) do
    if type(v) == "table" then flat(v, out) else out[#out + 1] = v end
  end
  return out
end
local function once(list, keep_last)
  local words, seen, out = flat(list), {}, {}
  if keep_last then
    for i = #words, 1, -1 do
      if not seen[words[i]] then seen[words[i]] = true; table.insert(out, 1, words[i]) end
    end
  else
    for _, w in ipairs(words) do
      if not seen[w] then seen[w] = true; out[#out + 1] = w end
    end
  end
  return out
end
local function needs(deps)
  local flags, archives, link = {}, {}, {}
  for _, d in ipairs(deps or {}) do
    flags[#flags + 1] = d.public_flags
    archives[#archives + 1] = d.archives
    link[#link + 1] = d.link
  end
  return once(flags), once(archives, true), once(link)
end
-- A source by its kind: C, or C++ (a module unit or not: scanned either way).
-- A source is a path, or {path, imports = {...}} for one a target generates
-- (its imports given, as it cannot be read yet). after: what every compile
-- waits for (the generator's headers).
local function compile(src, flags, after)
  local opts = {after = after}
  if type(src) == "table" then
    opts.imports = src.imports or {}
    src = src[1]
  end
  if src:match("%.c$") then return rules.cc(src, flags, opts) end
  return rules.module(src, flags, opts)
end
-- library{name=, srcs={...}, flags={...}, public={flags={...}, link={...}},
-- deps={other libraries}, after={generated files}}: libNAME.a, and what a
-- user of it needs.
function library(t)
  local public = t.public or {}
  local dflags, darchives, dlink = needs(t.deps)
  local own = {t.flags or {}, public.flags or {}, dflags}
  local objs = {t.objs or {}}
  for _, src in ipairs(t.srcs or {}) do objs[#objs + 1] = compile(src, own, flat({t.after or {}})) end
  local out = lib(t.name, objs)
  return {name = t.name, out = out,
          public_flags = {public.flags or {}, dflags},
          archives = {out, darchives},
          link = {public.link or {}, dlink}}
end
-- What exe{} and shlib{} share: the sources compiled with the deps' public
-- flags, the link with their archives and link flags.
function linked(t, kind)
  local dflags, darchives, dlink = needs(t.deps)
  local objs = {t.objs or {}}
  for _, src in ipairs(t.srcs or {}) do objs[#objs + 1] = compile(src, {t.flags or {}, dflags}, flat({t.after or {}})) end
  if std_used() then objs[#objs + 1] = toolchain.build .. "/obj/std.o" end
  local out, shape = t.out, {}
  if kind == "shlib" then
    out = out or (toolchain.build .. "/lib" .. t.name .. ".so")
    shape = {"-shared", "-Wl,-soname,lib" .. t.name .. ".so"}
  else
    out = out or (toolchain.build .. "/" .. t.name)
  end
  target{out = out, ins = {objs, darchives}, desc = "link " .. (kind == "shlib" and "lib" .. t.name .. ".so" or t.name),
         cmd = {toolchain.cxx, shape, objs, darchives, dlink, t.link or {}, toolchain.ldflags, "-o", out}}
  if t.default ~= false then default(out) end
  return out
end
-- shlib{name=, srcs=, objs=, flags=, deps=, link=, out=, after=}: libNAME.so (an
-- Android app's native library); built by default.
function shlib(t) return linked(t, "shlib") end

-- action{name=, needs={...}, cmd={...} or cmds={{...}, ...}, desc=}: a
-- named command (`sqmake install`), run each time a goal names it, after
-- what it needs is built, attached to the terminal. Never a default. What
-- it does is the build file's: sqmake only runs it.
function action(t)
  local name = t.name or error("action: a name", 2)
  if verb_names[name] then
    error("action: " .. name .. " is one of sqmake's verbs: verb(\"" .. name .. "\", {...}) says what it does", 2)
  end
  target{out = name, ins = t.needs or {}, cmd = t.cmd, cmds = t.cmds, desc = t.desc or name, action = true}
  return name
end

-- Verbs (spec §3.4): build, run, install, test, the words every project
-- answers. sqmake gives them no meaning; a platform file, the template or
-- the build file says what each does, and the last to say wins:
--   verb("install", {needs = {apk}, cmd = {tool, "install", apk}})
--   verb("run", {needs = {apk}, call = function(m) return exec{...} end})
-- `call` is the escape hatch: a Lua function run when the verb runs, given
-- mode, with exec() for commands; its status is the verb's. `build` is
-- the defaults, then its own step if it has one. `clean` is sqmake's.
-- mode = {debug, name}: `sqmake --debug VERB` (the template says what a
-- debug build is).
local said_verbs = {}
function verb(name, how)
  if not verb_names[name] then
    error("verb: " .. tostring(name) .. " is not one of sqmake's verbs (build, run, install, test); "
          .. "a project's own words are action{}", 2)
  end
  if type(how) ~= "table" then error("verb: " .. name .. " takes {needs=, cmd= or cmds= or call=, desc=}", 2) end
  said_verbs[name] = how
end
function _finish_verbs()
  local b = said_verbs.build or {}
  local step = b.cmd or b.cmds or b.call
  target{out = "build", ins = {defaults(), b.needs or {}}, cmd = b.cmd, cmds = b.cmds, call = b.call,
         desc = b.desc or "build", action = step ~= nil}
  for _, name in ipairs({"run", "install", "test"}) do
    local how = said_verbs[name]
    if how then
      target{out = name, ins = how.needs or {}, cmd = how.cmd, cmds = how.cmds, call = how.call,
             desc = how.desc or name, action = true}
    end
  end
end

-- An APK, packed and signed by apkpack (no Java, no Android SDK):
--   apk{name=, manifest=, res=, assets=, min_sdk=, target_sdk=,
--       files={{"lib/arm64-v8a/libNAME.so", so}, {"classes.dex", dex}},
--       key=, debuggable=, apkpack=}
-- The key is apkpack's (~/.android/apkpack.pem by default), made once with
-- `apkpack keygen`; sqmake never makes one. Built by default.
function apk(t)
  local apkpack = t.apkpack or toolchain.apkpack or "apkpack"
  local dir = toolchain.build .. "/apk/" .. t.name
  local unsigned = dir .. "/unsigned.apk"
  local out = t.out or (toolchain.build .. "/" .. t.name .. ".apk")
  local key = t.key or ((os.getenv("HOME") or "") .. "/.android/apkpack.pem")
  local inside, staged = {}, {}
  for _, f in ipairs(t.files or {}) do
    local at = dir .. "/" .. f[1]
    target{out = at, ins = {f[2]}, desc = "stage " .. f[1], cmd = {"cp", f[2], at}}
    inside[#inside + 1] = f[1]
    staged[#staged + 1] = at
  end
  local ins = {t.manifest, staged}
  if t.res then ins[#ins + 1] = glob(t.res .. "/**/*") end
  if t.assets then ins[#ins + 1] = glob(t.assets .. "/**/*") end
  local link = {apkpack, "link", "-o", unsigned, "--manifest", t.manifest,
                "--min-sdk", tostring(t.min_sdk or 28), "--target-sdk", tostring(t.target_sdk or 28)}
  if t.res then link = {link, "--res", t.res} end
  if t.assets then link = {link, "--assets", t.assets} end
  if t.debuggable then link = {link, "--debuggable"} end
  local cmds = {link}
  if #inside > 0 then cmds[#cmds + 1] = {apkpack, "add", unsigned, "-C", dir, inside} end
  target{out = unsigned, ins = ins, desc = "apkpack link " .. t.name, cmds = cmds}
  target{out = out, ins = {unsigned, key}, desc = "apkpack sign " .. t.name .. ".apk",
         cmds = {{apkpack, "sign", key, unsigned, out}, {apkpack, "verify", out}}}
  default(out)
  return out
end
)lua";

}  // namespace sqmake

export namespace sqmake {

/// A toolchain description (build/toolchain.lua), values only; defined below.
std::expected<ToolchainDescription, std::string> read_toolchain_description(const std::filesystem::path& file);

struct LuaOptions {
    lua_State* state = nullptr;  // the host's; none: sqmake makes its own
};
struct Options {
    LuaOptions lua;
    /// The debug build (`sqmake --debug`): mode.debug for the build file, and
    /// its own build folder (build/debug) unless .sqmake.lua says
    /// separate_debug = false.
    bool debug = false;
    /// Where exec() and actions write: sqmake's stdout (1), or stderr (2)
    /// under `call`, whose stdout is the response.
    int out = 1;
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
        c->debug_ = o.debug;
        c->out_ = o.out;
        return c;
    }

    ~Context() {
        if (!owned_ && L_ && env_ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, env_ref_);
        if (!owned_ && L_)
            for (int ref : refs_) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
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
        lua_pushvalue(L, env);
        env_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        auto api = [&](const char* name, lua_CFunction f) {
            lua_pushlightuserdata(L, this);
            lua_pushcclosure(L, f, 1);
            lua_setfield(L, env, name);
        };
        api("target", &Context::l_target);
        api("glob", &Context::l_glob);
        api("default", &Context::l_default);
        api("module_unit", &Context::l_module_unit);
        api("std_used", &Context::l_std_used);
        api("sdk_toolchain", &Context::l_sdk_toolchain);
        api("include", &Context::l_include);
        api("write_file", &Context::l_write_file);
        api("exec", &Context::l_exec);
        api("read_file", &Context::l_read_file);
        api("defaults", &Context::l_defaults);
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
        including_ = {file.string()};  // a file that includes it again is a cycle
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
            if (name == "=prelude")
                if (auto r = set_mode(env, file); !r) {
                    lua_settop(L, env - 1);
                    return r;
                }
        }
        // The verbs the build file said, as targets.
        lua_getfield(L, env, "_finish_verbs");
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error";
            lua_settop(L, env - 1);
            return std::unexpected(why);
        }
        // The toolchain the build file settled on.
        std::vector<std::string> cxx, cxxflags;
        std::string std_cppm;
        lua_getfield(L, env, "toolchain");
        if (lua_istable(L, -1)) {
            const int tc = lua_gettop(L);
            lua_getfield(L, tc, "build");
            if (lua_isstring(L, -1)) build_dir_ = lua_tostring(L, -1);
            lua_getfield(L, tc, "cxx");
            strings(L, -1, cxx);
            lua_getfield(L, tc, "cxxflags");
            strings(L, -1, cxxflags);
            lua_getfield(L, tc, "fingerprint");
            if (lua_isstring(L, -1)) fingerprint_ = lua_tostring(L, -1);
            lua_getfield(L, tc, "std_cppm");
            if (lua_isstring(L, -1)) std_cppm = lua_tostring(L, -1);
            lua_settop(L, tc);
        }
        lua_settop(L, env - 1);
        if (!error_.empty()) return std::unexpected(error_);
        if (fingerprint_.empty()) fingerprint_ = compute_fingerprint(cxx, cxxflags);
        if (!units_.empty())
            if (auto r = finish_modules(cxx, cxxflags, std_cppm); !r) return r;
        return {};
    }

    const std::string& fingerprint() const { return fingerprint_; }

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
        lua_getfield(L, 1, "action");
        t.action = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        // call: a Lua function the action runs (verb{call=}), kept until the
        // context goes.
        lua_getfield(L, 1, "call");
        if (lua_isfunction(L, -1)) {
            Context* c = &self(L);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            c->refs_.push_back(ref);
            t.call = [c, ref] { return c->call_ref(ref); };
        } else {
            lua_pop(L, 1);
        }
        // cmds: commands in order, each after the one before succeeded.
        lua_getfield(L, 1, "cmds");
        if (lua_istable(L, -1)) {
            if (!t.command.argv.empty()) return luaL_error(L, "target: cmd or cmds, not both");
            const lua_Integer n = luaL_len(L, -1);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_geti(L, -1, i);
                std::vector<std::string> argv;
                strings(L, -1, argv);
                lua_pop(L, 1);
                if (argv.empty()) continue;
                if (t.command.argv.empty()) t.command.argv = std::move(argv);
                else t.command.then.push_back(std::move(argv));
            }
        }
        lua_pop(L, 6);
        auto added = self(L).graph_.add(std::move(t));
        if (!added) return luaL_error(L, "target: %s", added.error().c_str());
        return 0;
    }

    /// mode = {debug, name} for the build file, and the build folder:
    /// build/debug for a debug build unless .sqmake.lua (values only, beside
    /// the build file) says separate_debug = false.
    std::expected<void, std::string> set_mode(int env, const std::filesystem::path& file) {
        lua_State* L = L_;
        bool separate = true;
        const std::filesystem::path config = file.parent_path() / ".sqmake.lua";
        if (std::filesystem::exists(config)) {
            if (luaL_loadfilex(L, config.string().c_str(), "t") != LUA_OK) {
                std::string why = lua_tostring(L, -1);
                lua_pop(L, 1);
                return std::unexpected(why);
            }
            lua_newtable(L);  // values only: an empty environment
            lua_pushvalue(L, -1);
            lua_setupvalue(L, -3, 1);
            lua_insert(L, -2);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::string why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error";
                lua_pop(L, 2);
                return std::unexpected(config.string() + ": " + why);
            }
            lua_getfield(L, -1, "separate_debug");
            if (lua_isboolean(L, -1)) separate = lua_toboolean(L, -1) != 0;
            else if (!lua_isnil(L, -1)) {
                lua_pop(L, 2);
                return std::unexpected(config.string() + ": separate_debug is true or false");
            }
            lua_pop(L, 2);
        }
        lua_newtable(L);
        lua_pushboolean(L, debug_);
        lua_setfield(L, -2, "debug");
        lua_pushstring(L, debug_ ? "debug" : "release");
        lua_setfield(L, -2, "name");
        lua_setfield(L, env, "mode");
        if (debug_ && separate) {
            lua_getfield(L, env, "toolchain");
            lua_pushstring(L, "build/debug");
            lua_setfield(L, -2, "build");
            lua_pop(L, 1);
        }
        return {};
    }

    /// Runs a verb's Lua function, given mode: its status (nil or true 0,
    /// false 1, a number itself; an error 1, said).
    int call_ref(int ref) {
        lua_State* L = L_;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, env_ref_);
        lua_getfield(L, -1, "mode");
        lua_remove(L, -2);
        in_call_ = true;
        const int r = lua_pcall(L, 1, 1, 0);
        in_call_ = false;
        int status = 0;
        if (r != LUA_OK) {
            std::println(stderr, "sqmake: {}", lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error");
            status = 1;
        } else if (lua_isinteger(L, -1)) {
            status = static_cast<int>(lua_tointeger(L, -1));
        } else if (lua_isboolean(L, -1)) {
            status = lua_toboolean(L, -1) ? 0 : 1;
        }
        lua_settop(L, top);
        return status;
    }

    /// exec(argv): a command, attached, from a verb's call; its exit status.
    static int l_exec(lua_State* L) {
        Context& c = self(L);
        if (!c.in_call_) return luaL_error(L, "exec: only inside a verb's call (when it runs), not while the build file is read");
        std::vector<std::string> argv;
        for (int i = 1; i <= lua_gettop(L); ++i) strings(L, i, argv);
        if (argv.empty()) return luaL_error(L, "exec: a command");
        std::fflush(stdout);
        lua_pushinteger(L, run_attached(argv, c.out_));
        return 1;
    }

    /// defaults(): what `sqmake` builds when given no goal, so far.
    static int l_defaults(lua_State* L) {
        const auto& d = self(L).defaults_;
        lua_createtable(L, static_cast<int>(d.size()), 0);
        for (std::size_t i = 0; i < d.size(); ++i) {
            lua_pushstring(L, d[i].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
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

    struct Unit {
        std::string src, obj;
        ModuleUnit scanned;
        std::vector<std::string> flags;  // its own: a library's include folders and defines
        std::vector<std::string> after;  // files it waits for: generated headers
    };

    /// read_file(path [, max_bytes]): the file's text (at most max_bytes of
    /// it), or nil when it cannot be read: state a build keeps for itself
    /// (a bridge's port and secret), or bytes from /dev/urandom.
    static int l_read_file(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        const lua_Integer max = luaL_optinteger(L, 2, -1);
        if (lua_gettop(L) >= 2 && max < 0) return luaL_error(L, "read_file: max_bytes is 0 or more");
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            lua_pushnil(L);
            return 1;
        }
        std::string text;
        if (max >= 0) {
            text.resize(static_cast<std::size_t>(max));
            in.read(text.data(), max);
            text.resize(static_cast<std::size_t>(in.gcount()));
        } else {
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    /// write_file(path, text): the file holds the text, written now and only
    /// when it differs (so what depends on it is not rebuilt for nothing): a
    /// list the build derives, such as squared's asset index. Its folder is
    /// made. Returns the path.
    static int l_write_file(lua_State* L) {
        const std::string path = luaL_checkstring(L, 1);
        std::size_t n = 0;
        const char* data = luaL_checklstring(L, 2, &n);
        const std::string_view text(data, n);
        std::error_code ec;
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                const std::string old((std::istreambuf_iterator<char>(in)), {});
                if (old == text) {
                    lua_pushvalue(L, 1);
                    return 1;
                }
            }
        }
        if (auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) std::filesystem::create_directories(parent, ec);
        const std::string temp = path + ".sqmake-new";
        {
            std::ofstream out(temp, std::ios::binary);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out) return luaL_error(L, "write_file: cannot write %s", path.c_str());
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) return luaL_error(L, "write_file: cannot write %s: %s", path.c_str(), ec.message().c_str());
        lua_pushvalue(L, 1);
        return 1;
    }

    /// include(path): another Lua file of the workspace, run in the build
    /// file's own environment (square's sq/build/template.lua and the like).
    static int l_include(lua_State* L) {
        Context& c = self(L);
        const std::string file = luaL_checkstring(L, 1);
        if (std::ranges::find(c.including_, file) != c.including_.end()) return luaL_error(L, "include: %s includes itself", file.c_str());
        std::ifstream in(file, std::ios::binary);
        if (!in) return luaL_error(L, "include: cannot read %s", file.c_str());
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        const std::string name = "@" + file;
        if (luaL_loadbufferx(L, text.data(), text.size(), name.c_str(), "t") != LUA_OK) return lua_error(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, c.env_ref_);
        lua_setupvalue(L, -2, 1);  // _ENV: the build file's
        c.including_.push_back(file);
        const int base = lua_gettop(L) - 1;  // below the chunk
        const int status = lua_pcall(L, 0, LUA_MULTRET, 0);
        c.including_.pop_back();
        if (status != LUA_OK) return lua_error(L);
        return lua_gettop(L) - base;  // what the file returns (its libraries, say)
    }

    static int l_module_unit(lua_State* L) {
        Context& c = self(L);
        const std::string src = luaL_checkstring(L, 1), obj = luaL_checkstring(L, 2);
        // Options (4th): after = files the compile waits for (generated
        // headers), imports = a generated source's imports, given because
        // it does not exist yet when the build file runs.
        std::vector<std::string> after, declared;
        bool generated = false;
        if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
            lua_getfield(L, 4, "after");
            if (lua_istable(L, -1)) strings(L, lua_gettop(L), after);
            lua_pop(L, 1);
            lua_getfield(L, 4, "imports");
            if (lua_istable(L, -1)) { strings(L, lua_gettop(L), declared); generated = true; }
            lua_pop(L, 1);
        }
        std::string text;
        if (generated) {
            text.clear();
        } else {
            std::ifstream in(src, std::ios::binary);
            if (!in) return luaL_error(L, "rules.module: cannot read %s", src.c_str());
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        auto unit = scan_source(text);
        if (unit) for (std::string& m : declared) unit->imports.push_back(std::move(m));
        if (!unit) return luaL_error(L, "%s: %s", src.c_str(), unit.error().c_str());
        // What the headers it includes by quotes import, theirs too: the
        // source waits for those modules as for its own imports.
        {
            std::set<std::string> seen;
            std::vector<std::pair<std::filesystem::path, std::string>> todo;  // header, its text
            for (const std::string& inc : quoted_includes(text)) todo.emplace_back(std::filesystem::path(src).parent_path() / inc, "");
            while (!todo.empty()) {
                const std::filesystem::path header = todo.back().first.lexically_normal();
                todo.pop_back();
                if (!seen.insert(header.string()).second) continue;
                std::ifstream h(header, std::ios::binary);
                if (!h) continue;  // not found beside it: a system or -I header
                const std::string htext((std::istreambuf_iterator<char>(h)), {});
                auto hunit = scan_source(htext);
                if (!hunit) return luaL_error(L, "%s (included by %s): %s", header.string().c_str(), src.c_str(), hunit.error().c_str());
                for (std::string& m : hunit->imports) unit->imports.push_back(std::move(m));
                for (const std::string& inc : quoted_includes(htext)) todo.emplace_back(header.parent_path() / inc, "");
            }
            std::ranges::sort(unit->imports);
            unit->imports.erase(std::unique(unit->imports.begin(), unit->imports.end()), unit->imports.end());
        }
        if (std::ranges::find(unit->imports, "std") != unit->imports.end() ||
            std::ranges::find(unit->imports, "std.compat") != unit->imports.end())
            c.std_used_ = true;
        Unit u{src, obj, std::move(*unit), {}, std::move(after)};
        if (lua_gettop(L) >= 3 && lua_istable(L, 3)) strings(L, 3, u.flags);
        c.units_.push_back(std::move(u));
        return 0;
    }

    static int l_std_used(lua_State* L) {
        lua_pushboolean(L, self(L).std_used_);
        return 1;
    }

    /// The SDK's toolchain: {cxx, cc, ar, fingerprint, std_cppm, ldflags}.
    /// A .lua description is resolved here (sqmake.toolchain); a .sh one,
    /// an older squared's, is asked with bash, unless a .lua is beside it.
    static int l_sdk_toolchain(lua_State* L) {
        std::string file = luaL_checkstring(L, 1);
        const std::string build = luaL_optstring(L, 2, "build");
        if (file.ends_with(".sh")) {
            const std::string beside = file.substr(0, file.size() - 3) + ".lua";
            if (std::filesystem::exists(beside)) file = beside;
        }
        auto list = [L](const std::vector<std::string>& v) {
            lua_createtable(L, static_cast<int>(v.size()), 0);
            for (std::size_t i = 0; i < v.size(); ++i) {
                lua_pushstring(L, v[i].c_str());
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
        };
        if (file.ends_with(".lua")) {
            auto description = read_toolchain_description(file);
            if (!description) return luaL_error(L, "%s", description.error().c_str());
            auto t = resolve_toolchain(*description);
            if (!t) return luaL_error(L, "toolchain (%s): %s", file.c_str(), t.error().c_str());
            const std::string dir = std::filesystem::absolute(build).string();
            if (auto made = make_resource_dir(*t, dir); !made) return luaL_error(L, "toolchain: %s", made.error().c_str());
            lua_createtable(L, 0, 6);
            list(t->cxx(dir));
            lua_setfield(L, -2, "cxx");
            list(t->cc(dir));
            lua_setfield(L, -2, "cc");
            lua_pushstring(L, t->ar().c_str());
            lua_setfield(L, -2, "ar");
            lua_pushstring(L, t->fingerprint.c_str());
            lua_setfield(L, -2, "fingerprint");
            lua_pushstring(L, t->std_cppm.c_str());
            lua_setfield(L, -2, "std_cppm");
            list(t->link);
            lua_setfield(L, -2, "ldflags");
            // Where it was found: for a build that names a library by path
            // (Android's libEGL.so in the sysroot, say).
            for (auto [key, value] : {std::pair{"sysroot", &t->sysroot}, {"runtimes", &t->runtimes},
                                      {"llvm_bin", &t->llvm_bin}, {"target", &t->triple}}) {
                lua_pushstring(L, value->c_str());
                lua_setfield(L, -2, key);
            }
            return 1;
        }
        const std::string& script = file;
        Ran env = run_command({"bash", script, "env"});
        if (env.status != 0) return luaL_error(L, "%s env: %s", script.c_str(), env.output.c_str());
        std::map<std::string, std::string> v;
        std::istringstream lines(env.output);
        for (std::string line; std::getline(lines, line);)
            if (const std::size_t eq = line.find('='); eq != std::string::npos) v[line.substr(0, eq)] = line.substr(eq + 1);
        const std::string fp = v["SQ_TOOLCHAIN_FINGERPRINT"];
        if (fp.empty()) return luaL_error(L, "%s env printed no SQ_TOOLCHAIN_FINGERPRINT", script.c_str());
        const std::string dir = (std::filesystem::absolute(build) / ("toolchain-" + fp)).string();
        if (!std::filesystem::exists(dir + "/clang++")) {
            Ran w = run_command({"bash", script, "wrappers", dir});
            if (w.status != 0) return luaL_error(L, "%s wrappers: %s", script.c_str(), w.output.c_str());
        }
        lua_createtable(L, 0, 5);
        list({dir + "/clang++"});
        lua_setfield(L, -2, "cxx");
        list({dir + "/clang"});
        lua_setfield(L, -2, "cc");
        lua_pushstring(L, (v["SQ_LLVM_BIN"] + "/llvm-ar").c_str());
        lua_setfield(L, -2, "ar");
        lua_pushstring(L, fp.c_str());
        lua_setfield(L, -2, "fingerprint");
        lua_pushstring(L, v["SQ_STD_CPPM"].c_str());
        lua_setfield(L, -2, "std_cppm");
        return 1;
    }

    /// A toolchain's fingerprint when none is given: the compiler's
    /// version line and every flag, hashed (spec §4.2).
    static std::string compute_fingerprint(const std::vector<std::string>& cxx, const std::vector<std::string>& flags) {
        if (cxx.empty()) return "none";
        std::vector<std::string> version = cxx;
        version.push_back("--version");
        const Ran r = run_command(version);
        Target t{{"fingerprint"}, {}, Command{cxx}, {}, {}};
        for (const std::string& f : flags) t.command.argv.push_back(f);
        return std::format("{:016x}", command_hash(t, r.output.substr(0, r.output.find('\n'))));
    }

    /// The module units' targets, now that every unit is known.
    std::expected<void, std::string> finish_modules(const std::vector<std::string>& cxx, const std::vector<std::string>& flags,
                                                    std::string std_cppm) {
        const std::string bmis = build_dir_ + "/modules/" + fingerprint_;
        std::map<std::string, std::string> provider;  // module -> its BMI
        for (const Unit& u : units_) {
            if (!u.scanned.provides) continue;
            if (provider.contains(*u.scanned.provides)) return std::unexpected(std::format("two units provide module {}", *u.scanned.provides));
            provider[*u.scanned.provides] = bmis + "/" + bmi_name(*u.scanned.provides);
        }
        auto base = [&] {
            std::vector<std::string> argv = cxx;
            argv.insert(argv.end(), flags.begin(), flags.end());
            argv.push_back("-fprebuilt-module-path=" + bmis);
            return argv;
        };
        if (std_used_) {
            if (std_cppm.empty()) {
                // The compiler's own libc++ module manifest names it.
                std::vector<std::string> ask = cxx;
                ask.insert(ask.end(), {"-stdlib=libc++", "-print-library-module-manifest-path"});
                const Ran r = run_command(ask);
                const std::filesystem::path manifest = r.output.substr(0, r.output.find('\n'));
                std::ifstream in(manifest);
                const std::string json((std::istreambuf_iterator<char>(in)), {});
                const std::size_t at = json.find("\"source-path\"");
                if (r.status == 0 && at != std::string::npos) {
                    const std::size_t q1 = json.find('"', json.find(':', at) + 1), q2 = json.find('"', q1 + 1);
                    std::filesystem::path src = json.substr(q1 + 1, q2 - q1 - 1);
                    if (src.filename() == "std.cppm") std_cppm = (src.is_absolute() ? src : manifest.parent_path() / src).lexically_normal().string();
                }
            }
            if (std_cppm.empty() || !std::filesystem::exists(std_cppm))
                return std::unexpected("import std: no libc++ std.cppm (set toolchain.std_cppm, or use toolchain.sdk)");
            const std::string obj = build_dir_ + "/obj/std.o", pcm = bmis + "/std.pcm";
            std::vector<std::string> argv = base();
            argv.insert(argv.end(), {"-Wno-reserved-module-identifier", "-MMD", "-MF", obj + ".d", "-c", std_cppm,
                                     "-fmodule-output=" + pcm, "-o", obj});
            if (auto r = graph_.add(Target{{obj, pcm}, {std_cppm}, Command{argv}, obj + ".d", "module std"}); !r) return std::unexpected(r.error());
            provider["std"] = pcm;
        }
        for (const Unit& u : units_) {
            Target t;
            t.outputs = {u.obj};
            t.inputs = {u.src};
            t.inputs.insert(t.inputs.end(), u.after.begin(), u.after.end());
            for (const std::string& m : u.scanned.imports) {
                auto it = provider.find(m == "std.compat" ? "std" : m);
                if (it == provider.end()) return std::unexpected(std::format("{} imports {}, which no unit provides", u.src, m));
                t.inputs.push_back(it->second);
            }
            t.command.argv = base();
            t.command.argv.insert(t.command.argv.end(), u.flags.begin(), u.flags.end());
            t.depfile = u.obj + ".d";
            t.command.argv.insert(t.command.argv.end(), {"-MMD", "-MF", t.depfile, "-c", u.src});
            if (u.scanned.provides) {
                const std::string pcm = provider[*u.scanned.provides];
                t.outputs.push_back(pcm);
                t.command.argv.push_back("-fmodule-output=" + pcm);
            }
            t.command.argv.insert(t.command.argv.end(), {"-o", u.obj});
            t.description = (u.scanned.provides ? "module " + *u.scanned.provides + " " : "cxx ") + u.src;
            if (auto r = graph_.add(std::move(t)); !r) return std::unexpected(r.error());
        }
        return {};
    }

    std::vector<Unit> units_;
    bool std_used_ = false;
    std::string fingerprint_;
    lua_State* L_ = nullptr;
    bool owned_ = true;
    bool debug_ = false;
    int out_ = 1;
    bool in_call_ = false;  // exec() runs only inside a verb's call
    int env_ref_ = LUA_NOREF;     // the build file's environment, for include()
    std::vector<std::string> including_;  // the files being included, for a cycle
    Graph graph_;
    std::vector<std::string> defaults_;
    std::vector<int> refs_;  // verbs' Lua functions, in the registry
    std::string build_dir_ = "build";
    std::string error_;
};

/// A toolchain description (squared's build/toolchain.lua), read as a
/// settings file is: in a Lua state of its own with nothing in reach, no
/// functions at all, so it can only say what it says (squared's
/// docs/developer/sdk-tools.md).
std::expected<ToolchainDescription, std::string> read_toolchain_description(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read {}", file.string()));
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    lua_State* L = luaL_newstate();
    if (!L) return std::unexpected("cannot make a Lua state");
    struct Close {
        lua_State* L;
        ~Close() { lua_close(L); }
    } close{L};
    const std::string name = "@" + file.string();
    if (luaL_loadbufferx(L, text.data(), text.size(), name.c_str(), "t") != LUA_OK) return std::unexpected(lua_tostring(L, -1));
    lua_newtable(L);
    const int values = lua_gettop(L);
    lua_pushvalue(L, values);
    lua_setupvalue(L, -3, 1);  // _ENV: an empty table, then what the file sets
    lua_pushvalue(L, values - 1);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) return std::unexpected(lua_tostring(L, -1) ? lua_tostring(L, -1) : "an error");
    std::string why;
    auto field = [&](int table, const char* key) {
        lua_getfield(L, table, key);
        return lua_gettop(L);
    };
    auto text_of = [&](int table, const char* key, bool required) {
        const int at = field(table, key);
        std::string out;
        if (lua_type(L, at) == LUA_TSTRING) out = lua_tostring(L, at);
        else if (required && why.empty()) why = std::format("{}: {} is not text", file.string(), key);
        lua_settop(L, at - 1);
        return out;
    };
    auto number_of = [&](int table, const char* key) {
        const int at = field(table, key);
        int out = 0;
        if (lua_isinteger(L, at)) out = static_cast<int>(lua_tointeger(L, at));
        else if (why.empty()) why = std::format("{}: {} is not a whole number", file.string(), key);
        lua_settop(L, at - 1);
        return out;
    };
    auto list_of = [&](int table, const char* key) {
        const int at = field(table, key);
        std::vector<std::string> out;
        if (lua_istable(L, at)) {
            const lua_Integer n = luaL_len(L, at);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_geti(L, at, i);
                if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1));
                else if (why.empty()) why = std::format("{}: {}[{}] is not text", file.string(), key, i);
                lua_pop(L, 1);
            }
        } else if (why.empty()) why = std::format("{}: {} is not a list", file.string(), key);
        lua_settop(L, at - 1);
        return out;
    };
    auto place_of = [&](const char* key) {
        ToolchainPlace p;
        const int at = field(values, key);
        if (!lua_istable(L, at)) {
            if (why.empty()) why = std::format("{}: {} is not a table", file.string(), key);
        } else {
            p.env = text_of(at, "env", true);
            const int under = field(at, "prefix");
            if (lua_type(L, under) == LUA_TSTRING) p.under_prefix = lua_tostring(L, under);
            lua_settop(L, under - 1);
            p.otherwise = text_of(at, "otherwise", false);
            p.install = text_of(at, "install", false);
        }
        lua_settop(L, at - 1);
        return p;
    };
    ToolchainDescription d;
    d.clang_major = number_of(values, "clang");
    d.api = number_of(values, "api");
    d.api_env = text_of(values, "api_env", false);
    d.triple = text_of(values, "triple", true);
    d.flags = list_of(values, "flags");
    d.cxx_flags = list_of(values, "cxx_flags");
    d.link_flags = list_of(values, "link_flags");
    d.llvm_bin = place_of("llvm_bin");
    d.sysroot = place_of("sysroot");
    d.runtimes = place_of("runtimes");
    if (!why.empty()) return std::unexpected(why);
    return d;
}

}  // namespace sqmake
