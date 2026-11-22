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

### 3.1 A conventional project (decided 2026-11-21)

The conventions are Lua files in the workspace, not knowledge inside
sqmake (square's spec §4): a template's `sq/build/template.lua` knows its
layout, and a project's `build.lua` includes it. sqmake gives them the
building blocks (0.5.0):

```lua
-- sq/build/template.lua (a template's; generated)
local squared = include("squared/build/squared.lua")   -- squared's own libraries
local app = shlib{name = "mygame", srcs = glob("sq_app/src/*.cpp"), deps = squared}
apk{name = "mygame", manifest = "sq_android/AndroidManifest.xml", res = "sq_android/res",
    assets = "sq_android/assets", files = {{"lib/arm64-v8a/libmygame.so", app}}}

-- build.lua (the developer's)
include("sq/build/template.lua")
```

- `include(path)`: another Lua file, run in the build file's environment.
- `library{name, srcs, flags, public = {flags, link}, deps}`, `exe{...}`,
  `shlib{...}`: libraries with usage requirements (§3.3); a source is a
  module unit or not (scanned either way), or C.
- `target{..., cmds = {{...}, {...}}}`: commands in order, each run after
  the one before succeeded.
- `apk{...}`: apkpack's link (manifest, `res/`, `assets/`), add (the native
  library, `classes.dex`), sign and verify. The key is apkpack's
  (`~/.android/apkpack.pem`), made once with `apkpack keygen`: sqmake never
  makes one, so a squared key is never replaced.
- `write_file(path, text)` (0.6.0): a generated file written while the build
  file runs, and only when its content changes (so what reads it is not
  rebuilt for nothing); squared's asset index is one.
- `read_file(path [, max_bytes])` (0.8.1): a file's text, at most
  max_bytes of it, or nil: state a build keeps for itself beside what it
  makes (squared's Console bridge: its port and secret, made once), or
  bytes from /dev/urandom.
- `toolchain.sdk()`'s table also says where the toolchain's parts are
  (`sysroot`, `runtimes`, `llvm_bin`, `target`), for what a build file names
  by path: Android's libraries in the sysroot, llvm-objcopy.

- `action{name, needs, cmd | cmds, desc}` (0.7.0): a named command,
  `sqmake install`, run each time a goal names it, once what it needs is
  built and nothing else is running, attached to the terminal (its stdin,
  its output as it comes: a log followed until interrupted works). Never a
  default, never up to date, no state; `clean` leaves its name alone. What
  an action does is the build file's: sqmake knows no devices, installers
  or platforms, so an Android template's `install` names its own tool, and
  another platform's names another.

A generator's output (0.6.0): a source written as `{path, imports = {...}}`
is one a target makes, its imports given because it cannot be read while
the build file runs; `after = {...}` on `library{}`, `exe{}` or `shlib{}`
makes every compile wait for those files (the generator's headers).
template.android.cpp's sqgen step is the case.

Flags gathered from usage requirements are kept once each, at their first
place; archives at their last, so a link still finds each after its users.

There is no `project{...}` built into sqmake.

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
ones, and passes on those it re-exports. In 0.5.0: `library{}`'s `flags` are
its own, `public.flags` its users' too (and its own), `public.link` its
users' links'; its archive comes before its dependencies' on every link that
uses it, so a static link finds every symbol.

### 3.4 Verbs and the debug build (decided 2026-11-21, 0.8.0)

Every project answers the same few words, so a person, an editor or
sqgui never learns a project's own: `sqmake build`, `run`, `install`,
`test`, and sqmake's own `clean`. sqmake fixes what each means, not what
it does:

| verb | means |
|---|---|
| build | the product made (the build file's defaults), then its own step if it says one |
| install | the product put where it is used |
| run | the product started (installed first where that is how it runs) |
| test | the project's tests run |
| clean | what the build made removed (sqmake's own) |

What each does is said, in Lua, by whatever knows: a platform file
(squared's build/android.lua: install through the device, run through
the launcher), the template, or the build file; the last to say wins, so
a project overrides its platform with one line:

```lua
verb("install", {needs = {apk}, cmd = {device, "install", apk}})
verb("test", {needs = {tests}, call = function(m) return exec{"build/tests"} end})
```

`cmd` or `cmds` is the usual way; `call` is the escape hatch, a Lua
function run when the verb runs, given `mode`, with `exec()` for commands
(only there: a build file never runs a command while it is read), its
status the verb's. A verb nothing says is an error naming the verb. A
project's own words (Android's `logcat`, `devices`) stay `action{}`,
which may not take a verb's name. sqmake knows no platform.

`--debug`, on any verb, is the debug build: `mode = {debug, name}` for
the build file, which says what that is (squared's template: the
Console, DEV_LOG, a debuggable APK). It builds in `build/debug`, so
switching never rebuilds the other; a project opts out in `.sqmake.lua`
beside the build file (values only): `separate_debug = false`.

## 4. Toolchains

### 4.1 One toolchain, defined once (decided)

A toolchain is explicit: sqmake never detects compilers. The SDK has one, for
every C++ module it builds: **clang 23, libc++ 23 and bionic**, from sqterm's
own `llvm`, `runtimes` and `sysroot` packages on the phone, and from
apt.llvm.org's clang 23 with the same `sysroot` and `runtimes` on CI. It is
described once, in squared, as data: `build/toolchain.lua` (values only:
the clang version, the API level, the target, the flags, and where each part
is). sqmake reads it and finds each part itself (changed 2026-11-21: from
`toolchain.sh`, run with bash): no shell, no wrapper scripts, and each part
checked by looking (`std::filesystem`, permission bits) rather than by
libc's `access()`, which bionic answers wrongly for bash's `[ -x ]`. A part
not found is named, with where it was looked for and how to get it (`no
clang and clang++ at ... (set SQ_LLVM_BIN; in sqterm: pkg install llvm)`);
`sqmake toolchain` prints what it found and why. make's build reads
`toolchain.sh`, the same toolchain in bash, until make goes (phase 4);
squared's `tools/check_toolchain.sh` holds the two to the same flags and
fingerprint.

The Android NDK serves only while the SDK moves to that toolchain (the plan
"One C++23 toolchain"): until the `sysroot` package carries Android's own
API headers and stub libraries.

### 4.2 The fingerprint (decided)

Every toolchain has a **fingerprint**: a short hash of the compiler's
version, the C++ library's version, the target triple and API level, and
every flag that changes a compiled module (§4.3). The SDK's is
`toolchain.sh`'s, byte for byte (the first 16 hex digits of the SHA-256 of
five lines: clang's version line, `libc++ <version>`, the flags on every
compile, the C++ flags, the link flags), so make's build and sqmake's name
the same BMI folders. It names the folder that
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
compiler inside itself, but the SDK's compilers were toolchain.sh's
wrappers, whose sysroot, headers and target flags it would not see, so scanning
squared failed on its headers. The declarations alone need no toolchain and
are quick on the phone. The limit: a module or import declaration made by a
macro, or under `#if`, is not seen; squared writes neither. A header unit
(`import <x>;`) is refused. Imports written in a header the source includes
with quotes (`#include "x.h"`, found beside the file that includes it, and
the headers it includes) are the source's too (0.6.0): squared's headers
import its modules.

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

### 6.3 The shared cache (decided 2026-11-22, 0.9.0)

Compiles are shared between builds on one device. A target that writes a
dependency file says exactly what it read, so its outputs are kept in a
cache under a key of its command (which carries the toolchain's
fingerprint) and its inputs' contents, with the list of what it read and
each file's sha256. A build anywhere on the device running the same
command over the same inputs, whose headers still hash the same, takes
the outputs from there: `[n/N] cxx ... (cached)`, and at the end `M of N
from the cache`. A header changed is compiled again; changed back, it is
the cache's once more.

So the commands must be the same in every workspace: they name the build
folder as the build file does (relative; every command runs in the build
file's folder), the toolchain's resource directory included. A framework
installed once (squared's, `$PREFIX/share/squared/VERSION`) is then
compiled once a device: sqterm's APK in a second workspace took 423 of
449 steps from the cache (2 s, against 19 s), its library byte for byte
the first's.

The cache is `$SQMAKE_CACHE`, else `$PREFIX/var/cache/sqmake` (sqterm's),
else `~/.cache/sqmake`; `SQMAKE_CACHE=off` is none. Entries are written
whole, beside, then renamed into place, so builds at once never see half
of one. Not yet: pruning it (one APK's worth is about 70 MB).

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
   `build/toolchain.sh` (its wrappers, fingerprint, std.cppm), and from
   0.4.0 `build/toolchain.lua`, found with no shell (§4.1); the
   fingerprint is in every command's hash and names the BMI folder; module
   declarations order the compiles; one std module. And `sqmake call` and
   `describe` (0.3.0, done): the SDK's tool protocol (squared's
   docs/developer/sdk-tools.md), JSON through sq.data (vendored).
3. **Conventions**: squared, the app and the APK (apkpack) built from Lua
   files in the workspace. The building blocks (0.5.0, done: §3.1):
   `include()`, `library{}`/`exe{}`/`shlib{}` with usage requirements, a
   target of several commands, `apk{}`. Then (0.6.0, done): `write_file`,
   headers' imports, flags kept once; squared's `build/squared.lua` (its
   libraries) and template.android.cpp's `sq/build/template.lua` build
   sqterm's APK with no make and no shell.
4. **The switch**: sqterm built and installed with sqmake, on the phone.
