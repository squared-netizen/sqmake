#!/usr/bin/env bash
# tests/run_check.sh SQMAKE: sqmake building a small C++ program, and
# rebuilding exactly what each change asks for (docs/spec.md §2.2).
#
#   first build    every object, then the link; the program runs
#   no change      nothing runs: "up to date"
#   a header       only the objects that include it, and the link
#   a source       only its object, and the link
#   a flag         every object (the command changed)
#   --explain      each command and why, nothing run
#   a failure      the build stops, the status is not 0, the output shown;
#                  fixed, it builds
#   a cycle, a missing input, a second maker: refused before anything runs
#   the toolchain  build/toolchain.lua found with no shell: a fake layout
#                  (a stub clang), the fingerprint toolchain.sh would give,
#                  each part's errors, values only
#   conventions    include(), library{} with usage requirements, exe{} and
#                  shlib{}, a target of several commands, apk{} (a stub
#                  apkpack that records what it is asked)
#   clean          the outputs and the state gone
#
# CXX is the compiler (clang++ by default).
set -euo pipefail

sqmake=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=$(mktemp -d)
trap 'rm -r -f "$work"' EXIT
cd "$work"
export CXX=${CXX:-clang++}
ok() { printf '  ok   %s\n' "$*"; }
die() { printf '  FAIL %s\n' "$*" >&2; exit 1; }
ran() { grep -c '^\[' "$1" || true; }  # progress lines: commands run

mkdir -p src include
cat > include/shared.h <<'H'
inline int shared() { return 40; }
H
cat > src/main.cpp <<'C'
#include <cstdio>
#include "../include/shared.h"
int two();
int main() { std::printf("%d\n", shared() + two()); }
C
cat > src/two.cpp <<'C'
int two() { return 2; }
C
cat > src/alone.cpp <<'C'
int alone() { return 1; }
C
cat > build.lua <<'L'
toolchain.cxxflags = {"-std=c++23", "-O1"}
exe("hello", map(glob("src/*.cpp"), rules.cxx))
L

"$sqmake" > out.txt || die "the first build: $(cat out.txt)"
[ "$(ran out.txt)" = 4 ] || die "the first build ran $(ran out.txt) commands, not 4: $(cat out.txt)"
[ "$(build/hello)" = 42 ] || die "the program says $(build/hello)"
ok "first build: 3 objects and a link; the program runs"

"$sqmake" > out.txt
grep -q 'up to date' out.txt && [ "$(ran out.txt)" = 0 ] || die "a second build ran something: $(cat out.txt)"
ok "no change: up to date"

sleep 1
printf 'inline int shared() { return 41; }\n' > include/shared.h
"$sqmake" > out.txt
[ "$(ran out.txt)" = 2 ] && grep -q 'cxx src/main.cpp' out.txt || die "a header change ran: $(cat out.txt)"
[ "$(build/hello)" = 43 ] || die "after the header change the program says $(build/hello)"
ok "a header: its one includer and the link"

sleep 1
printf 'int two() { return 3; }\n' > src/two.cpp
"$sqmake" > out.txt
[ "$(ran out.txt)" = 2 ] && grep -q 'cxx src/two.cpp' out.txt || die "a source change ran: $(cat out.txt)"
ok "a source: its object and the link"

sed -i 's/-O1/-O2/' build.lua
"$sqmake" > out.txt
[ "$(ran out.txt)" = 4 ] || die "a flag change ran $(ran out.txt), not 4: $(cat out.txt)"
ok "a flag: every object (their commands changed)"

touch src/alone.cpp
sleep 1
touch src/alone.cpp
"$sqmake" --explain > out.txt
grep -q 'because src/alone.cpp is newer' out.txt && grep -q -- '-c src/alone.cpp' out.txt || die "--explain: $(cat out.txt)"
"$sqmake" --explain > /dev/null
grep -q 'because src/alone.cpp is newer' <("$sqmake" --explain) || die "--explain ran the command"
ok "--explain: the command and why; nothing run"
"$sqmake" -j 4 > /dev/null

printf 'int two() { return }\n' > src/two.cpp
if "$sqmake" > out.txt 2>&1; then die "a broken source built"; fi
grep -q 'src/two.cpp' out.txt && grep -q 'failed (status' out.txt || die "the failure's report: $(cat out.txt)"
printf 'int two() { return 2; }\n' > src/two.cpp
"$sqmake" > out.txt || die "the fix did not build: $(cat out.txt)"
[ "$(build/hello)" = 43 ] || die "after the fix the program says $(build/hello)"
ok "a failure: stopped, reported; fixed, built"

cat > cycle.lua <<'L'
target{out = "a", ins = {"b"}, cmd = {"true"}}
target{out = "b", ins = {"a"}, cmd = {"true"}}
default("a")
L
"$sqmake" -f cycle.lua > out.txt 2>&1 && die "a cycle built"
grep -q 'a cycle' out.txt || die "the cycle's message: $(cat out.txt)"
cat > missing.lua <<'L'
target{out = "x", ins = {"nowhere.txt"}, cmd = {"true"}}
default("x")
L
"$sqmake" -f missing.lua > out.txt 2>&1 && die "a missing input built"
grep -q 'nowhere.txt, which does not exist' out.txt || die "the missing input's message: $(cat out.txt)"
cat > twice.lua <<'L'
target{out = "x", cmd = {"true"}}
target{out = "x", cmd = {"false"}}
L
"$sqmake" -f twice.lua > out.txt 2>&1 && die "two makers loaded"
grep -q 'made by two targets' out.txt || die "two makers' message: $(cat out.txt)"
ok "a cycle, a missing input, two makers: refused"

# $CXX in words, as make takes it: a compiler and its own flags.
CXX="$CXX -DFROM_CXX=1" "$sqmake" --explain | grep -q -- "-DFROM_CXX=1 -std=c++23" || die "CXX was not split into words"
ok "CXX in words"

# C++ modules: an interface, its partition, an implementation unit, and a
# program importing it and std. Built in order; std built once; a change
# to the partition rebuilds what imports it, not std.
mkdir -p mods/src
cat > mods/src/greet.cppm <<'C'
export module greet;
export import :words;
import std;
export std::string greet(std::string_view who);
C
cat > mods/src/words.cppm <<'C'
export module greet:words;
import std;
export inline std::string hello() { return "hello"; }
C
cat > mods/src/greet.cpp <<'C'
module greet;
import std;
std::string greet(std::string_view who) { return hello() + ", " + std::string(who); }
C
cat > mods/src/main.cpp <<'C'
// import nothing; is a comment
import std;
import greet;
int main() { std::println("{}", greet("modules")); }
C
cat > mods/build.lua <<'L'
toolchain.cxxflags = {"-std=c++23", "-stdlib=libc++", "-O1"}
toolchain.ldflags = {"-stdlib=libc++"}
exe("hello", map(glob("src/*.cpp*"), rules.module))
L
"$sqmake" -C mods > out.txt 2>&1 || die "the modules build: $(cat out.txt)"
[ "$(mods/build/hello)" = "hello, modules" ] || die "the modules program says $(mods/build/hello)"
order=$(grep -o 'module [a-z:]*\|cxx src/[a-z.]*' out.txt | tr '\n' ',')
case $order in
  *"module greet:words"*"module greet,"*"cxx src/greet.cpp"*) ;;
  *) die "the modules' order: $order" ;;
esac
ls mods/build/modules/*/std.pcm mods/build/modules/*/greet.pcm mods/build/modules/*/greet-words.pcm > /dev/null || die "the BMIs are not in build/modules/<fingerprint>/"
sleep 1
touch mods/src/words.cppm
"$sqmake" -C mods > out.txt
grep -q 'module greet:words' out.txt && grep -q 'module greet ' out.txt && ! grep -q 'module std' out.txt || die "a partition's change rebuilt: $(cat out.txt)"
ok "modules: built in order, BMIs per fingerprint, std once; a partition's change rebuilds its importers"

# sqmake as other tools call it: one JSON request, one JSON response.
json() { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
"$sqmake" describe | json 'd["tool"] + " " + ",".join(sorted(d["commands"]))' | grep -qx 'sqmake build,clean,plan,toolchain' || die "describe"
echo '{"sq": 1, "command": "plan", "args": {"dir": "mods"}}' | "$sqmake" call > resp.json || die "call plan: $(cat resp.json)"
[ "$(json 'len(d["result"]["targets"])' < resp.json)" = 6 ] || die "call plan's targets: $(cat resp.json)"
touch mods/src/main.cpp
echo '{"sq": 1, "command": "build", "args": {"dir": "mods", "explain": true}}' | "$sqmake" call 2> /dev/null > resp.json
[ "$(json 'd["ok"], d["result"]["ran"], d["result"]["explained"]' < resp.json)" = "(True, 2, True)" ] || die "call build, explain: $(cat resp.json)"
echo '{"sq": 1, "command": "build", "args": {"dir": "mods", "jobs": 2}}' | "$sqmake" call 2> err.txt > resp.json || die "call build: $(cat resp.json)"
[ "$(json 'd["result"]["ran"]' < resp.json)" = 2 ] && grep -q 'cxx src/main.cpp' err.txt || die "call build: progress on stderr, the response on stdout: $(cat resp.json)"
for req in '{"sq": 2, "command": "build"}:version:1' '{"sq": 1, "command": "fly"}:unknown_command:1' 'not json:bad_request:2' \
           '{"sq": 1, "command": "build", "args": {"jobs": "four"}}:bad_request:2'; do
    body=${req%:*:*}; rest=${req#"$body":}; code=${rest%:*}; want=${rest#*:}
    status=0
    echo "$body" | "$sqmake" call > resp.json 2> /dev/null || status=$?
    [ "$status" = "$want" ] && [ "$(json 'd["error"]["code"]' < resp.json)" = "$code" ] || die "call $body: status $status, $(cat resp.json)"
done
ok "call and describe: plan, build (explain), errors with their codes and statuses"

# The SDK's toolchain from its description, found by sqmake alone: a fake
# layout (a stub clang that only answers for its version), and the
# fingerprint build/toolchain.sh makes of the same five lines.
mkdir -p tc/bin tc/pfx/share/sysroot/usr/include tc/pfx/lib/aarch64-unknown-linux-android28 \
         tc/pfx/include/c++/v1 tc/pfx/share/libc++/v1
cat > tc/bin/clang <<'SH'
#!/bin/sh
case $1 in -dumpversion) echo "${FAKE_CLANG:-23}.1.5" ;; --version) echo "clang version ${FAKE_CLANG:-23}.1.5 (fake)"; echo "Target: x" ;; esac
SH
chmod +x tc/bin/clang && cp tc/bin/clang tc/bin/clang++
: > tc/pfx/lib/aarch64-unknown-linux-android28/libc++.a
printf '#ifndef _LIBCPP_CONFIG\n#  define _LIBCPP_VERSION 230105\n#endif\n' > tc/pfx/include/c++/v1/__config
: > tc/pfx/share/libc++/v1/std.cppm
cat > tc/toolchain.lua <<'L'
clang = 23
api = 28
api_env = "SQ_API"
triple = "aarch64-unknown-linux-android"
flags = {"-rtlib=compiler-rt", "-unwindlib=libunwind", "-pthread", "-D__BIONIC_CTYPE_INLINE=inline"}
cxx_flags = {"-std=c++23", "-stdlib=libc++", "-nostdinc++"}
link_flags = {"-fuse-ld=lld", "-static-libstdc++", "-Wl,-z,max-page-size=16384"}
llvm_bin = {env = "SQ_LLVM_BIN", prefix = "bin", otherwise = "/usr/lib/llvm-23/bin", install = "pkg install llvm"}
sysroot = {env = "SQ_SYSROOT", prefix = "share/sysroot", install = "pkg install sysroot"}
runtimes = {env = "SQ_RUNTIMES", prefix = "", install = "pkg install runtimes"}
L
pfx=$PWD/tc/pfx
cp tc/bin/clang tc/bin/clang++ "$pfx/share/" && mkdir -p "$pfx/bin" && cp tc/bin/clang tc/bin/clang++ "$pfx/bin/"
want=$(printf '%s\n' "clang version 23.1.5 (fake)" "libc++ 230105" \
    "--target=aarch64-unknown-linux-android28 -rtlib=compiler-rt -unwindlib=libunwind -pthread -D__BIONIC_CTYPE_INLINE=inline" \
    "-std=c++23 -stdlib=libc++ -nostdinc++" "-fuse-ld=lld -static-libstdc++ -Wl,-z,max-page-size=16384" | sha256sum | cut -c 1-16)
env -u SQ_LLVM_BIN -u SQ_SYSROOT -u SQ_RUNTIMES PREFIX="$pfx" "$sqmake" toolchain tc/toolchain.lua > out.txt 2>&1 || die "toolchain: $(cat out.txt)"
grep -q "fingerprint  $want" out.txt && grep -q "llvm .*$pfx/bin (clang 23.1.5; from \$PREFIX)" out.txt &&
    grep -q "libc++ 230105" out.txt || die "toolchain, under \$PREFIX: $(cat out.txt) (want $want)"
SQ_LLVM_BIN=$PWD/tc/bin PREFIX="$pfx" "$sqmake" toolchain tc/toolchain.lua | grep -q "tc/bin (clang 23.1.5; from SQ_LLVM_BIN)" ||
    die "toolchain: SQ_LLVM_BIN does not win"
echo '{"sq": 1, "command": "toolchain", "args": {"file": "tc/toolchain.lua"}}' | PREFIX="$pfx" "$sqmake" call > resp.json
[ "$(json 'd["result"]["fingerprint"], d["result"]["target"]' < resp.json)" = "('$want', 'aarch64-unknown-linux-android28')" ] ||
    die "call toolchain: $(cat resp.json)"
SQ_API=30 PREFIX="$pfx" "$sqmake" toolchain tc/toolchain.lua > out.txt 2>&1 && die "SQ_API=30 found libc++ for 28"
grep -q 'no libc++ for aarch64-unknown-linux-android30' out.txt || die "SQ_API=30: $(cat out.txt)"
FAKE_CLANG=22 PREFIX="$pfx" "$sqmake" toolchain tc/toolchain.lua > out.txt 2>&1 && die "clang 22 accepted"
grep -q 'clang 22.1.5 at .*: the toolchain is clang 23' out.txt || die "clang 22: $(cat out.txt)"
SQ_SYSROOT=$PWD/none PREFIX="$pfx" "$sqmake" toolchain tc/toolchain.lua > out.txt 2>&1 && die "a missing sysroot accepted"
grep -q 'no sysroot at .*none .*pkg install sysroot' out.txt || die "no sysroot: $(cat out.txt)"
env -u PREFIX SQ_LLVM_BIN=$PWD/none "$sqmake" toolchain tc/toolchain.lua > out.txt 2>&1 && die "no clang accepted"
grep -q 'no clang and clang++ at .*none .*pkg install llvm' out.txt || die "no clang: $(cat out.txt)"
# Values only: a description that calls anything fails.
printf 'clang = tonumber("23")\n' > tc/bad.lua
"$sqmake" toolchain tc/bad.lua > out.txt 2>&1 && die "a description could call a function"
grep -q "tonumber" out.txt || die "values only: $(cat out.txt)"
ok "toolchain: found under \$PREFIX or its variables, toolchain.sh's fingerprint, every part's error, values only"

# The conventions' building blocks: a template's Lua file included; a
# library whose public flags reach its users' compiles and whose archive
# reaches their links; a program and a shared library from it; a target of
# two commands; an APK through apkpack (a stub that writes what it is asked
# and makes the files a real one would).
mkdir -p conv/greet/include conv/greet/src conv/app conv/sq_android/res/values conv/sq_android/assets conv/bin
cat > conv/greet/include/greet.h <<'H'
const char* greeting();
H
cat > conv/greet/src/greet.cpp <<'C'
#include "greet.h"
const char* greeting() { return GREETING; }
C
cat > conv/app/main.cpp <<'C'
#include <cstdio>
#include "greet.h"
int main() { std::printf("%s\n", greeting()); }
C
cat > conv/app/native.cpp <<'C'
#include "greet.h"
extern "C" const char* native_greeting() { return greeting(); }
C
printf '<manifest/>\n' > conv/sq_android/AndroidManifest.xml
printf '<resources/>\n' > conv/sq_android/res/values/strings.xml
printf 'hello\n' > conv/sq_android/assets/hello.txt
cat > conv/bin/apkpack <<'SH'
#!/bin/sh
echo "$*" >> "$APKPACK_LOG"
case $1 in
  link) shift; while [ $# -gt 0 ]; do [ "$1" = -o ] && { printf 'apk' > "$2"; shift; }; shift; done ;;
  sign) printf 'signed' > "$4" ;;
esac
SH
chmod +x conv/bin/apkpack
: > conv/key.pem
cat > conv/conventions.lua <<'L'
-- What a template's sq/build/template.lua would hold.
local greet = include("greet.lua")
exe{name = "hello", srcs = {"app/main.cpp"}, deps = {greet}}
local so = shlib{name = "native", srcs = {"app/native.cpp"}, deps = {greet}, flags = {"-fPIC"}}
target{out = "build/twice.txt", ins = {"conventions.lua"}, desc = "twice",
       cmds = {{"cp", "conventions.lua", "build/twice.txt"}, {"touch", "build/twice.txt"}}}
default("build/twice.txt")
apk{name = "app", manifest = "sq_android/AndroidManifest.xml", res = "sq_android/res",
    assets = "sq_android/assets", key = "key.pem", apkpack = "bin/apkpack",
    files = {{"lib/arm64-v8a/libnative.so", so}}}
L
cat > conv/greet.lua <<'L'
return library{name = "greet", srcs = {"greet/src/greet.cpp"},
               public = {flags = {"-Igreet/include", "-DGREETING=\"hello, library\""}}}
L
cat > conv/build.lua <<'L'
toolchain.cxxflags = {"-std=c++23", "-O1", "-fPIC"}
include("conventions.lua")
L
(cd conv && APKPACK_LOG=$PWD/apkpack.log "$sqmake" > out.txt 2>&1) || die "the conventions' build: $(cat conv/out.txt)"
[ "$(conv/build/hello)" = "hello, library" ] || die "exe{} with a library's public flags: $(conv/build/hello)"
grep -q 'SONAME.*libnative.so' <(llvm-readelf -d conv/build/libnative.so 2>/dev/null || readelf -d conv/build/libnative.so) ||
    die "shlib{}: no soname libnative.so"
[ -f conv/build/twice.txt ] && grep -q 'link app' conv/out.txt || die "cmds / apk: $(cat conv/out.txt)"
grep -q '^link -o build/apk/app/unsigned.apk --manifest sq_android/AndroidManifest.xml --min-sdk 28 --target-sdk 28 --res sq_android/res --assets sq_android/assets$' conv/apkpack.log &&
    grep -q '^add build/apk/app/unsigned.apk -C build/apk/app lib/arm64-v8a/libnative.so$' conv/apkpack.log &&
    grep -q '^sign key.pem build/apk/app/unsigned.apk build/app.apk$' conv/apkpack.log &&
    grep -q '^verify build/app.apk$' conv/apkpack.log && [ "$(cat conv/build/app.apk)" = signed ] &&
    cmp -s conv/build/libnative.so conv/build/apk/app/lib/arm64-v8a/libnative.so || die "apk{}: $(cat conv/apkpack.log)"
(cd conv && APKPACK_LOG=$PWD/apkpack.log "$sqmake" > out.txt 2>&1) && grep -q 'up to date' conv/out.txt || die "conventions: a second build ran: $(cat conv/out.txt)"
sleep 1
printf 'hello\nagain\n' > conv/sq_android/assets/hello.txt
(cd conv && APKPACK_LOG=$PWD/apkpack.log "$sqmake" > out.txt 2>&1)
grep -q 'apkpack link app' conv/out.txt && grep -q 'apkpack sign app' conv/out.txt && ! grep -q 'link hello' conv/out.txt ||
    die "an asset's change: $(cat conv/out.txt)"
printf 'include("build.lua")\n' > conv/loop.lua
printf 'include("loop.lua")\n' >> conv/build.lua
(cd conv && "$sqmake" > out.txt 2>&1) && die "an include cycle was accepted"
grep -q 'includes itself' conv/out.txt || die "an include cycle: $(cat conv/out.txt)"
ok "conventions: include(), library{} usage requirements, exe{}, shlib{}, cmds, apk{} (link, add, sign, verify)"

"$sqmake" clean > /dev/null
[ ! -e build/hello ] && [ ! -e build/.sqmake/state ] || die "clean left files"
ok "clean: outputs and state gone"
echo "run check passed"
