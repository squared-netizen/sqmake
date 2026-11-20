# sqmake's own build (make, for now: sqmake builds sqmake once it can).
#
#   make CXX=/path/to/clang++   clang and libc++ 23 with `import std;`
#   make test                    the unit tests, then tests/run_check.sh
#
# Offline: Lua 5.4 is vendored in third_party/lua, compiled as C++ (so a
# Lua error unwinds C++ frames, destructors and all), and the std module is
# built here from the libc++ the compiler uses.

CXX      ?= clang++
BUILD    ?= build
OPT      ?= -O2 -g
STDLIB   := -stdlib=libc++
WARN     := -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -Wno-missing-field-initializers
CXXFLAGS := -std=c++23 $(STDLIB) -pthread $(OPT) $(WARN) -Ithird_party/lua \
            -fprebuilt-module-path=$(BUILD)/pcm -fmodule-file=std=$(BUILD)/pcm/std.pcm
LUAFLAGS := -x c++ -std=c++23 $(STDLIB) $(OPT) -w -DLUA_USE_POSIX
LDFLAGS  := $(STDLIB) -pthread

STD_MANIFEST := $(shell readlink -f "$$($(CXX) $(STDLIB) -print-library-module-manifest-path 2>/dev/null)" 2>/dev/null)
STD_SOURCE   := $(if $(STD_MANIFEST),$(shell sed -n 's/.*"source-path": *"\([^"]*std\.cppm\)".*/\1/p' $(STD_MANIFEST) 2>/dev/null | head -n 1))
STD_CPPM     ?= $(if $(filter /%,$(STD_SOURCE)),$(STD_SOURCE),$(if $(STD_SOURCE),$(dir $(STD_MANIFEST))$(STD_SOURCE)))

LUA_SRC := $(filter-out third_party/lua/lua.c third_party/lua/luac.c,$(wildcard third_party/lua/*.c))
LUA_OBJ := $(LUA_SRC:third_party/lua/%.c=$(BUILD)/lua/%.o)
MODULES := graph state run scan lua
OBJS    := $(BUILD)/obj/std.o $(MODULES:%=$(BUILD)/obj/%.o) $(BUILD)/liblua.a
TESTS   := $(patsubst tests/%.cpp,$(BUILD)/%,$(wildcard tests/test_*.cpp))

.PHONY: all test clean
all: $(BUILD)/sqmake $(TESTS)

$(BUILD)/pcm/std.pcm:
	@test -n "$(STD_CPPM)" && test -f "$(STD_CPPM)" || { echo "no libc++ std module (set STD_CPPM)" >&2; exit 1; }
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj
	$(CXX) -std=c++23 $(STDLIB) -pthread $(OPT) -Wno-reserved-module-identifier --precompile $(STD_CPPM) -o $@
$(BUILD)/obj/std.o: $(BUILD)/pcm/std.pcm
	$(CXX) $(OPT) -c $< -o $@

$(BUILD)/lua/%.o: third_party/lua/%.c
	@mkdir -p $(dir $@)
	$(CXX) $(LUAFLAGS) -c $< -o $@
$(BUILD)/liblua.a: $(LUA_OBJ)
	$(AR) rcs $@ $^

define unit
$(BUILD)/pcm/sqmake.$(1).pcm $(BUILD)/obj/$(1).o &: src/$(1).cppm $(BUILD)/pcm/std.pcm $(2)
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj
	$(CXX) $(CXXFLAGS) -c $$< -fmodule-output=$(BUILD)/pcm/sqmake.$(1).pcm -o $(BUILD)/obj/$(1).o
endef
$(eval $(call unit,graph,))
$(eval $(call unit,state,))
$(eval $(call unit,run,$(BUILD)/pcm/sqmake.graph.pcm $(BUILD)/pcm/sqmake.state.pcm))
$(eval $(call unit,scan,))
$(eval $(call unit,lua,$(BUILD)/pcm/sqmake.graph.pcm $(BUILD)/pcm/sqmake.run.pcm $(BUILD)/pcm/sqmake.scan.pcm))

$(BUILD)/sqmake: tools/sqmake.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/test_%: tests/test_%.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

test: all
	@set -e; for t in $(TESTS); do $$t; done; echo "all tests passed"
	@bash tests/run_check.sh $(BUILD)/sqmake

clean:
	rm -r -f -- build
