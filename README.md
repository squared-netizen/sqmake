# sqmake

The build system of the [squared SDK](https://squared-netizen.github.io/squared-netizen/):
it builds what a project's `build.lua` describes, on its own.

**Status: phases 1 and 2 of 4 built, phase 3 begun** (sqmake 0.5.0: `call`, the toolchain found with no shell, and the conventions' building blocks): the engine, and C++
modules with the SDK's toolchain. Conventions and building sqterm come next
([docs/spec.md §11](docs/spec.md)).

```sh
make CXX="clang++ -stdlib=libc++"     # clang and libc++ 23; make test checks it
build/sqmake                          # builds what build.lua's default() names
build/sqmake --explain                # each command that would run, and why
build/sqmake -j 4 GOAL                # a goal, four commands at once
build/sqmake clean
```

A `build.lua`:

```lua
toolchain.cxxflags = {"-std=c++23", "-O2"}
exe("hello", map(glob("src/*.cpp"), rules.cxx))
```

C++ modules, with the SDK's own toolchain (squared's `build/toolchain.lua`,
data that sqmake resolves itself: no shell, no wrapper scripts):

```lua
toolchain.sdk("squared/build/toolchain.lua")  -- clang 23, libc++ 23, bionic
lib("game", map(glob("src/**/*.cpp*"), rules.module))
```

Libraries with usage requirements, a native library and an APK (apkpack),
in a file a template's build includes:

```lua
local greet = library{name = "greet", srcs = glob("greet/src/*.cpp"),
                      public = {flags = {"-Igreet/include"}}}
local so = shlib{name = "game", srcs = glob("src/*.cpp"), deps = {greet}}
local game = apk{name = "game", manifest = "sq_android/AndroidManifest.xml", res = "sq_android/res",
                 assets = "sq_android/assets", files = {{"lib/arm64-v8a/libgame.so", so}}}
-- `sqmake install` (a verb: every project's same word; what it does is said here,
-- or by a platform file): built first, then this command, attached to the terminal.
verb("install", {needs = {game}, cmd = {"mydevicetool", "install", game}})
```

Each source's `export module` / `import` lines order the compiles; BMIs go
in `build/modules/<fingerprint>/`; `import std` builds libc++'s std module
once. Other tools call it as the SDK's tools call each other (squared's
`docs/developer/sdk-tools.md`): one JSON request, one JSON response.

```sh
sqmake toolchain                                    # what it found, where, and why
sqmake describe                                     # what it can be asked
echo '{"sq": 1, "command": "build", "args": {"dir": "game"}}' | sqmake call
# {"sq": 1, "ok": true, "result": {"ran": 4, "up_to_date": 2, "explained": false}}
```

`build`, `plan` (every target and its command, in order) and `clean`;
progress on stderr, the response alone on stdout. A target is rebuilt when an output is missing, its command changed, or an
input or a header its last compile read is newer. Commands run as argument
lists, without a shell, each one's output printed whole.

## What it will be

- **It runs the build itself**, with no generated make, CMake or Ninja
  files.
- **make's model with Ninja's engine**: command hashes, and header
  dependencies written by the compiler.
- **Cargo's conventions**, **CMake's usage requirements**, and **Lua 5.4**
  as the escape hatch for what a convention does not cover.
- **A library, `libsqmake`, with a thin command line.** Offline, no daemon,
  and `--explain` shows the whole resolved graph.

## The specification

[docs/spec.md](docs/spec.md): a draft (0.2, 2026-11-20).

## Where it sits

sqmake stands alone. It builds what a project's `build.lua` describes, and
depends on no other tool of the SDK: it reads nothing of sqpg's or square's
and runs none of their commands. They, and any other program, may use it
through its command line or its library; it never uses them.

## Licence

MIT: see [LICENSE](LICENSE). Security issues: see [SECURITY.md](SECURITY.md).
