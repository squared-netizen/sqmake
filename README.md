# sqmake

The build system of the [squared SDK](https://squared-netizen.github.io/squared-netizen/):
it builds an application from a workspace, knowing how to build it rather
than what a workspace is.

**Status: planned.** Nothing is built yet; this repository holds the work as
it starts.

## What it will be

- **It runs the build itself**, with no generated make, CMake or Ninja
  files.
- **make's model with Ninja's engine**: command hashes, and header
  dependencies written by the compiler.
- **Cargo's conventions**, **CMake's usage requirements**, and **Lua 5.4**
  as the escape hatch for what a convention does not cover.
- **A library, `libsqmake`, with a thin command line.** Offline, no daemon,
  and `--explain` shows the whole resolved graph.

## Where it sits

In the SDK's workflow sqpg creates a workspace, square manages it, and
sqmake builds it. sqmake asks square what to build (`square build-inputs`)
and knows nothing about workspaces itself.

## Licence

MIT: see [LICENSE](LICENSE). Security issues: see [SECURITY.md](SECURITY.md).
