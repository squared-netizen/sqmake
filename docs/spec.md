# sqmake: draft specification

**Status: draft 0.2, 2026-11-20.** Phase 1 (§11) is built. What this
specification decides is marked as decided; what it only proposes is marked
as proposed, and the open questions are listed at the end. It grows from the
research note "sqpg Build System: Research and Recommendations" (2026-11-16)
and the SDK's plan for one C++23 toolchain (2026-11-19).

## 1. What sqmake is

sqmake is the squared SDK's build system. It builds what a project's
`build.lua` describes, and nothing else tells it what to build.

**It stands alone** (decided 2026-11-20). sqmake depends on no other tool of
the SDK: it reads no file, path or setting of sqpg's or square's, and runs
none of their commands. They may use it, through its command line or
`libsqmake`, as any program may; it never uses them. A project with a
`build.lua` builds with sqmake and nothing else, however it was made.

It keeps make's model (targets, prerequisites, a target rebuilt when what it
is made from changes) and drops make's syntax. It runs the build itself: it
never writes make, CMake or Ninja files for another tool to run.

### 1.1 Requirements (decided)

- **Small and self-contained.** One binary with a library behind it; no JVM,
  no Python, no daemon. It runs on the phone, in sqterm, as well as on a
  desktop or CI.
- **Offline.** A build makes no network request and downloads nothing.
- **Programmable.** Lua 5.4 describes builds; other programs use sqmake as a
  library (`libsqmake`).
- **Explainable.** `sqmake build --explain` prints the whole resolved graph
  and every command it would run. Nothing sqmake infers is hidden.
- **A strong reason for every concept make does not have.**

### 1.2 Where its ideas come from

| Idea | From | In sqmake |
| --- | --- | --- |
| The engine | Ninja | A graph run in-process, with command hashes and compiler-written header dependencies |
| The front door | Cargo | Conventions and a short declarative description: most projects need almost no build code |
| The escape hatch | Cargo's `build.rs`, Zig's `build.zig` | Real code, in Lua, for what conventions miss; no second language |
| Usage requirements | CMake | A library declares what its users need (include paths, flags, modules), and linking it applies them |

## 2. The model

### 2.1 Targets, rules and commands (decided)

A **target** is a file sqmake makes. It has **inputs** (files, and other
targets) and one **command** that makes it from them. A **rule** is a Lua
function that, given inputs and an output, returns a command. A **command**
is an argument list: a program and its arguments, run without a shell, so
there is no quoting and no shell syntax.

### 2.2 When a target is out of date (decided)

A target is rebuilt when any of these holds:

1. it does not exist, or an input is newer than it;
2. its command's hash differs from the last time it was built;
3. a header its compile read has changed (the compiler's dependency file,
   `-MMD -MF`, records which);
4. a module it imports has been rebuilt (§5);
5. the toolchain's fingerprint (§4.2) differs from the one it was built with.
   The fingerprint is part of every command's hash, so a new compiler or C++
   library rebuilds what it compiled even when the command line reads the
   same.

What sqmake learns between builds (command hashes, dependency files read,
fingerprints) lives in one small state file in the build folder.

## 3. Describing a build

### 3.1 A conventional project (proposed syntax)

Templates imply a layout, so a project following one names itself, its kind
and what it depends on; sqmake infers its sources and outputs:

```lua
project "mygame" {
  template = "terminal",
  deps     = { "squared" },
}
```

The C++ standard is not a project's choice: it is the toolchain's (§4.3).

### 3.2 The escape hatch (proposed syntax)

When a convention does not fit, the same file defines rules and targets:

```lua
local objs = map(glob "src/*.cpp", rules.cxx)
exe("mygame", objs)
```

Each of make's pain points has a plain replacement:

| make | sqmake |
| --- | --- |
| `$@`, `$<`, `$^` | named parameters: `out`, `ins` |
| pattern and implicit rules | an explicit map from sources to objects |
| `=`, `:=`, `?=` | one assignment, evaluated when it runs |
| tabs and shell quoting | commands as argument lists, no shell |
| `$(call ...)` and includes | ordinary functions, and `require` |

### 3.3 Usage requirements (decided)

A library declares, as public or private, the include paths, flags,
libraries and modules its users need. A target that links it gets the public
ones, and passes on those it re-exports.

## 4. Toolchains

### 4.1 One toolchain, defined once (decided)

A toolchain is explicit: sqmake never detects compilers. The SDK has one, for
every C++ module it builds: **clang 23, libc++ 23 and bionic**, from sqterm's
own `llvm`, `runtimes` and `sysroot` packages on the phone, and from
apt.llvm.org's clang 23 with the same `sysroot` and `runtimes` on CI. It is
defined once, in squared (`build/toolchain.mk`, and the same as
`toolchain.sh` for scripts); sqmake reads that same definition, so squared's
own build and sqmake can never disagree about it.

The Android NDK serves only while the SDK moves to that toolchain (the plan
"One C++23 toolchain"): until the `sysroot` package carries Android's own
API headers and stub libraries.

### 4.2 The fingerprint (decided)

Every toolchain has a **fingerprint**: a short hash of the compiler's
version, the C++ library's version, the target triple and API level, and
every flag that changes a compiled module (§4.3). It names the folder that
holds compiled modules (§5.2), and it is part of every command's hash (§2.2).

### 4.3 Flags that change a compiled module (decided)

These belong to the toolchain, never to a target, and go on every compile and
link alike: the C++ standard (`-std=c++23`), the C++ library
(`-stdlib=libc++`), the runtime and unwind libraries (`-rtlib=compiler-rt`,
`-unwindlib=libunwind`), threads (`-pthread`), bionic's ctype macro
(`-D__BIONIC_CTYPE_INLINE=inline`) and the optimisation level. A target that
asks for another value is refused with a plain message naming the flag,
before clang's "module file was compiled with ..." can happen.

`-rtlib=compiler-rt` looks like a link flag, but it turns on target features
(outline atomics, function multiversioning): given at the link alone, it made
CI's clang refuse its own std module (sqterm #21, 2026-11-19).

## 5. C++ modules

### 5.1 Order (decided)

Before it compiles, sqmake reads each source's module declarations itself
(`export module`, `module`, `import`, `export import`, partitions; comments
and strings skipped) and orders the compiles by them: a module's interface is
compiled before what imports it, its BMI an input of each importer.

Not clang-scan-deps (changed 2026-11-20, from draft 0.1): it runs the
compiler inside itself, but the SDK's compilers are toolchain.sh's wrappers,
whose sysroot, headers and target flags it would not see, so scanning
squared failed on its headers. The declarations alone need no toolchain and
are quick on the phone. The limit: a module or import declaration made by a
macro, or under `#if`, is not seen; squared writes neither. A header unit
(`import <x>;`) is refused.

### 5.2 Compiled modules (decided)

A compiled module (a BMI, `.pcm`) is an output in the graph, like an object
file, kept in `build/modules/<fingerprint>/`. A BMI made by another compiler
or with other flags is never found there, rather than found and refused.

### 5.3 The standard library module (decided)

`import std` is one node per fingerprint: built once from the C++ library's
own `std.cppm` (the `runtimes` package's) and shared by every target that
imports it.

### 5.4 Libraries written as modules (decided)

A library written as modules (sqcart, sqdrive) is vendored as sources and
built by its consumer's toolchain. A compiled module never travels between
repositories or machines.

## 6. Running a build

### 6.1 Stages (decided)

1. **Load:** Lua evaluates the build file; it registers toolchains, rules
   and targets through sqmake's API.
2. **Resolve:** conventions fill in sources and outputs; usage requirements
   flow along dependency edges.
3. **Plan:** a directed acyclic graph of files and commands, module
   dependencies included (§5.1); a cycle is refused.
4. **Check:** each node is marked out of date or not (§2.2).
5. **Run:** out-of-date nodes whose inputs are ready run on a thread pool;
   progress is reported as they finish.

### 6.2 What it is built from (decided)

C++23 modules, `import std`. The standard library does the work it can:
`std::filesystem` for paths and times, `std::jthread` and a queue for
parallelism, `std::ranges` and `std::span` for the graph, `std::expected` for
each step's result, `std::print` for output. Custom code is the graph, the
dependency-file reader and the hash, and each is justified in the developer
documentation.

Design patterns: **Command** (each step: hashable, loggable, printable by
`--explain`), **Strategy** (toolchains), **Observer** (progress and logs,
apart from the scheduler), **Builder** (Lua's registration of targets and
rules), **Factory** (`Context::create`).

## 7. The library and the command line

### 7.1 Packaging (decided)

- **`libsqmake`**: the graph, the executor and the Lua bindings. It knows
  nothing of the programs that use it (§1).
- **`sqmake`**: a thin command line over the library. A feature the command
  line needs and the library lacks is a gap in the library.
- **Interface:** C++23 modules. A small C interface over them can come later,
  if a tool in another language needs one.
- **Its own program:** sqmake is installed, versioned and released on its
  own (in sqterm: `pkg install sqmake`), never folded into another tool's
  binary.

### 7.2 Lua state (decided)

sqmake works alone and embedded; the host decides who owns the Lua state:

```cpp
auto ctx = sqmake::Context::create({});              // sqmake makes its own state
auto ctx = sqmake::Context::create({.lua = {L}});    // the host passes one
```

- **Alone** (the command line): sqmake makes a state and closes it when the
  context goes.
- **Embedded** (any program with a Lua 5.4 state of its own: an editor, an
  IDE, another of the SDK's tools): sqmake uses the host's state, never
  closes it, and puts its API in its own `sqmake` table. The host gets no
  hook into sqmake beyond that API.
- **Version:** `create()` refuses anything but Lua 5.4, as a
  `std::expected` error.
- **Sandbox:** a build file runs in its own environment table and reaches the
  host's globals only where the host allows it.

## 8. Not sqmake's

- **Code with its own build** (a library built with CMake, say) is a prebuilt
  artifact or one opaque step, never reimplemented.
- **Making projects**: workspaces, templates and kits are other tools'
  (sqpg, square). What they make, sqmake builds through its `build.lua`,
  like any project's.

## 9. Risks

- **Growing into CMake.** Every concept needs a strong reason (§1.1).
- **Inference hiding behaviour.** `--explain` always shows the resolved graph.
- **Dependency files.** Incremental builds rely on reading clang's (and
  GCC's) dependency files exactly.
- **Full Lua permits unreadable build files.** Whether to narrow the API, as
  Meson narrows its language, is open.

## 10. Decisions (2026-11-20)

The questions draft 0.1 left open, decided as recommended, and one more:

- [x] **sqmake is independent of sqpg** (and of square): it never reads or
  runs them, and needs neither to build; §1.

- [x] **sqmake builds the whole workspace**, squared and the kits included:
  modules are built by their consumer's toolchain (§5.4), so a workspace's
  build is one graph.
- [x] **The template gives the layout; `build.lua` declares** the name, the
  kind and what it depends on (§3.1).
- [x] **Make stays** until sqmake builds and installs sqterm on the phone.
- [x] **The Lua API is 0.x**: no stability promised to other tools yet.
- [x] **The build file is `build.lua`.**

## 11. Phases

1. **The engine** (sqmake 0.1.0, done): targets, the plan, commands as argument
   lists, command hashes and dependency files in a state file, a parallel
   runner, `--explain`, `clean`, build files in Lua 5.4, alone or embedded.
2. **Toolchains and modules** (sqmake 0.2.0, done): `toolchain.sdk()` reads
   `build/toolchain.sh` (its wrappers, fingerprint, std.cppm); the
   fingerprint is in every command's hash and names the BMI folder; module
   declarations order the compiles; one std module. Then `sqmake call` and
   `describe` (the SDK's tool protocol, squared's docs/developer/sdk-tools.md).
3. **Conventions**: `project{...}` builds squared, the app and the APK
   (apkpack).
4. **The switch**: sqterm built and installed with sqmake, on the phone.
