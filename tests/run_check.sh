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
"$sqmake" describe | json 'd["tool"] + " " + ",".join(sorted(d["commands"]))' | grep -qx 'sqmake build,clean,plan' || die "describe"
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

"$sqmake" clean > /dev/null
[ ! -e build/hello ] && [ ! -e build/.sqmake/state ] || die "clean left files"
ok "clean: outputs and state gone"
echo "run check passed"
