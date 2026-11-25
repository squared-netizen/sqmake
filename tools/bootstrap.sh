#!/bin/sh
# tools/bootstrap.sh: the first sqmake, from a compiler and a shell, nothing
# else (no make); then that sqmake builds sqmake (build.lua).
#
#   sh tools/bootstrap.sh            build/bootstrap/sqmake, then build/sqmake
#   CXX=/path/to/clang++ CC=... AR=... sh tools/bootstrap.sh
#
# The compiler is clang and libc++ 23 with `import std;`. The std module's
# source is read from libc++'s module manifest (its "source-path"), with the
# shell's own expansions (no sed, grep or awk); set STD_CPPM when it is
# elsewhere. The compiles are sqmake's own, in the order its modules import
# each other: what build.lua says again, the one place it is said by hand.

set -eu

CXX=${CXX:-clang++}
CC=${CC:-clang}
AR=${AR:-ar}
B=${BUILD:-build}/bootstrap
STDLIB=-stdlib=libc++
OPT=${OPT:--O2}

if [ -z "${STD_CPPM:-}" ]; then
  manifest=$("$CXX" $STDLIB -print-library-module-manifest-path 2>/dev/null || true)
  [ -f "$manifest" ] || { echo "bootstrap: no libc++ module manifest from $CXX (set STD_CPPM)" >&2; exit 1; }
  text=$(cat "$manifest")
  # Each "source-path" in turn, until the one that is std.cppm.
  while :; do
    case $text in *'"source-path"'*) ;; *) break ;; esac
    text=${text#*\"source-path\"}
    text=${text#*\"}
    path=${text%%\"*}
    text=${text#*\"}
    case $path in */std.cppm|std.cppm)
      case $path in /*) STD_CPPM=$path ;; *) STD_CPPM=${manifest%/*}/$path ;; esac
      break ;;
    esac
  done
fi
[ -f "${STD_CPPM:-}" ] || { echo "bootstrap: no libc++ std.cppm (set STD_CPPM)" >&2; exit 1; }

mkdir -p "$B/pcm" "$B/obj/lua" "$B/obj/sq.data"
say() { echo "bootstrap: $*"; }
FLAGS="-std=c++23 $STDLIB -pthread $OPT -w -Ithird_party/lua -isystem third_party/yyjson-0.12.0/src -fprebuilt-module-path=$B/pcm -fmodule-file=std=$B/pcm/std.pcm"

say "std"
"$CXX" -std=c++23 $STDLIB -pthread $OPT -Wno-reserved-module-identifier --precompile "$STD_CPPM" -o "$B/pcm/std.pcm"
"$CXX" $OPT -c "$B/pcm/std.pcm" -o "$B/obj/std.o"

say "lua"
lua_objs=
for f in third_party/lua/*.c; do
  case $f in */lua.c|*/luac.c) continue ;; esac
  o=$B/obj/lua/${f##*/}.o
  "$CXX" -x c++ -std=c++23 $STDLIB $OPT -w -DLUA_USE_POSIX -c "$f" -o "$o"
  lua_objs="$lua_objs $o"
done
"$AR" rcs "$B/liblua.a" $lua_objs

say "sq.data"
SQD=third_party/squared-data
unit() {  # unit SOURCE NAME: one interface unit, its BMI and its object
  "$CXX" $FLAGS -c "$1" -fmodule-output="$B/pcm/$2.pcm" -o "$B/obj/$2.o"
}
for u in src/json_error_code:src.json_error_code src/json_error:src.json_error \
         src/json_parse_options:src.json_parse_options json_value:json_value \
         src/json_parse_result:src.json_parse_result src/json_write_options:src.json_write_options \
         src/json_write_result:src.json_write_result json:json; do
  unit "$SQD/${u%%:*}.cppm" "sq.data-${u#*:}"
done
unit "$SQD/module.cppm" sq.data
"$CXX" $FLAGS -c "$SQD/src/json.cpp" -o "$B/obj/sq.data/json.cpp.o"
"$CXX" $FLAGS -c "$SQD/src/json_value.cpp" -o "$B/obj/sq.data/json_value.cpp.o"
"$CC" $OPT -w -c third_party/yyjson-0.12.0/src/yyjson.c -o "$B/obj/yyjson.o"

say "sqmake"
for m in graph state sha256 cache run scan toolchain lua call; do
  unit "src/$m.cppm" "sqmake.$m"
done
objs="$B/obj/std.o"
for o in "$B"/obj/*.o "$B"/obj/sq.data/*.o; do
  [ "$o" = "$B/obj/std.o" ] || objs="$objs $o"
done
"$CXX" $FLAGS tools/sqmake.cpp $objs "$B/liblua.a" $STDLIB -pthread -o "$B/sqmake"

say "$B/sqmake made; now sqmake builds sqmake"
exec "$B/sqmake"
