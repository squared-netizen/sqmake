-- build.lua: sqmake's own build, by sqmake. The first sqmake comes from
-- tools/bootstrap.sh (a compiler and a shell, nothing else); from then on
-- sqmake builds sqmake.
--
--   sqmake          build/sqmake, and the unit tests' programs
--   sqmake test     the unit tests, then tests/run_check.sh with build/sqmake
--   sqmake --debug  -O0 (build/debug)
--
-- The compiler is $CXX (and $CC), clang and libc++ 23 with `import std;`.
-- Offline: Lua 5.4 is vendored in third_party/lua, compiled as C++ (so a
-- Lua error unwinds C++ frames, destructors and all); squared's sq.data in
-- third_party/squared-data (tools/vendor_squared_data.sh), with yyjson.

local opt = mode.debug and {"-O0", "-g"} or {"-O2", "-g"}
toolchain.cxxflags = {"-std=c++23", "-stdlib=libc++", "-pthread", opt}
toolchain.cflags = {opt}
toolchain.ldflags = {"-stdlib=libc++", "-pthread"}

local warn = {"-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wno-sign-conversion",
              "-Wno-missing-field-initializers"}
local includes = {"-Ithird_party/lua", "-isystem", "third_party/yyjson-0.12.0/src"}

-- Lua, as C++: its own warnings left to it.
local lua_srcs = {}
for _, f in ipairs(glob("third_party/lua/*.c")) do
  if not f:match("/luac?%.c$") then lua_srcs[#lua_srcs + 1] = f end
end
local lua = library{name = "lua", srcs = lua_srcs,
                    flags = {"-x", "c++", "-std=c++23", "-stdlib=libc++", "-w", "-DLUA_USE_POSIX"}}

-- sq.data (squared's), and the yyjson only it uses.
local sqd = "third_party/squared-data"
local data_srcs = {"third_party/yyjson-0.12.0/src/yyjson.c"}
for _, f in ipairs(glob(sqd .. "/**/*.cppm")) do data_srcs[#data_srcs + 1] = f end
for _, f in ipairs(glob(sqd .. "/src/*.cpp")) do data_srcs[#data_srcs + 1] = f end
local data = library{name = "sqdata", srcs = data_srcs, flags = {"-w", includes}}

-- sqmake's own modules (sqmake scans their imports for the order).
local core = library{name = "sqmake_core", srcs = glob("src/*.cppm"), deps = {lua, data},
                     flags = {warn, includes}}

local bin = exe{name = "sqmake", srcs = {"tools/sqmake.cpp"}, deps = {core}, flags = {warn, includes}}

local tests = {}
for _, src in ipairs(glob("tests/test_*.cpp")) do
  local t = src:match("([^/]+)%.cpp$")
  tests[#tests + 1] = exe{name = t, srcs = {src}, deps = {core}, flags = {warn, includes}}
end

verb("test", {needs = {bin, tests}, call = function()
  for _, t in ipairs(tests) do
    if exec{t} ~= 0 then return 1 end
  end
  print("all tests passed")
  return exec{"bash", "tests/run_check.sh", bin}
end})
