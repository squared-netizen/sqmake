# sqmake

The build system of the [squared SDK](https://squared-netizen.github.io/squared-netizen/):
it builds what a project's `build.lua` describes, on its own.

**Status: phase 1 of 4 built** (sqmake 0.1.0): the engine. Toolchains and
C++ modules, conventions and building sqterm come next
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

A target is rebuilt when an output is missing, its command changed, or an
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
