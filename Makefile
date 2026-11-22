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
CXXFLAGS := -std=c++23 $(STDLIB) -pthread $(OPT) $(WARN) -Ithird_party/lua -isystem third_party/yyjson-0.12.0/src \
            -fprebuilt-module-path=$(BUILD)/pcm -fmodule-file=std=$(BUILD)/pcm/std.pcm
LUAFLAGS := -x c++ -std=c++23 $(STDLIB) $(OPT) -w -DLUA_USE_POSIX
LDFLAGS  := $(STDLIB) -pthread

STD_MANIFEST := $(shell readlink -f "$$($(CXX) $(STDLIB) -print-library-module-manifest-path 2>/dev/null)" 2>/dev/null)
comma := ,
# The std module's source, from libc++'s module manifest (its "source-path"),
# read with make's own functions: no sed. Set STD_CPPM when it is elsewhere.
STD_WORDS    := $(if $(wildcard $(STD_MANIFEST)),$(subst ", ,$(subst $(comma), ,$(file < $(STD_MANIFEST)))))
STD_SOURCE   := $(firstword $(filter %/std.cppm std.cppm,$(STD_WORDS)))
STD_CPPM     ?= $(if $(filter /%,$(STD_SOURCE)),$(STD_SOURCE),$(if $(STD_SOURCE),$(dir $(STD_MANIFEST))$(STD_SOURCE)))

# squared's data module (sq.data, vendored: tools/vendor_squared_data.sh),
# its units in the order they import each other (file:module name), then
# its implementation units; and yyjson, which only it uses.
SQD       := third_party/squared-data
SQD_UNITS := src/json_error_code:src.json_error_code src/json_error:src.json_error \
             src/json_parse_options:src.json_parse_options json_value:json_value \
             src/json_parse_result:src.json_parse_result src/json_write_options:src.json_write_options \
             src/json_write_result:src.json_write_result json:json
sqd_name   = $(lastword $(subst :, ,$(1)))
SQD_PCMS  := $(foreach u,$(SQD_UNITS),$(BUILD)/pcm/sq.data-$(call sqd_name,$(u)).pcm)
SQD_OBJS  := $(foreach u,$(SQD_UNITS),$(BUILD)/obj/sq.data/$(call sqd_name,$(u)).o) \
             $(BUILD)/obj/sq.data/module.o $(BUILD)/obj/sq.data/json.cpp.o $(BUILD)/obj/sq.data/json_value.cpp.o

LUA_SRC := $(filter-out third_party/lua/lua.c third_party/lua/luac.c,$(wildcard third_party/lua/*.c))
LUA_OBJ := $(LUA_SRC:third_party/lua/%.c=$(BUILD)/lua/%.o)
MODULES := graph state cache run scan sha256 toolchain lua call
OBJS    := $(BUILD)/obj/std.o $(MODULES:%=$(BUILD)/obj/%.o) $(BUILD)/liblua.a $(SQD_OBJS) $(BUILD)/obj/yyjson.o
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

define sqd_unit
$(BUILD)/pcm/sq.data-$(2).pcm $(BUILD)/obj/sq.data/$(2).o &: $(SQD)/$(1).cppm $(3)
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj/sq.data
	$$(CXX) $$(CXXFLAGS) -w -c $$< -fmodule-output=$(BUILD)/pcm/sq.data-$(2).pcm -o $(BUILD)/obj/sq.data/$(2).o
endef
SQDP := $(BUILD)/pcm/sq.data-
$(eval $(call sqd_unit,src/json_error_code,src.json_error_code,$(BUILD)/pcm/std.pcm))
$(eval $(call sqd_unit,src/json_error,src.json_error,$(SQDP)src.json_error_code.pcm))
$(eval $(call sqd_unit,src/json_parse_options,src.json_parse_options,$(SQDP)src.json_error.pcm))
$(eval $(call sqd_unit,json_value,json_value,$(SQDP)src.json_parse_options.pcm))
$(eval $(call sqd_unit,src/json_parse_result,src.json_parse_result,$(SQDP)json_value.pcm))
$(eval $(call sqd_unit,src/json_write_options,src.json_write_options,$(SQDP)src.json_parse_result.pcm))
$(eval $(call sqd_unit,src/json_write_result,src.json_write_result,$(SQDP)src.json_write_options.pcm))
$(eval $(call sqd_unit,json,json,$(SQDP)src.json_write_result.pcm))
$(BUILD)/pcm/sq.data.pcm $(BUILD)/obj/sq.data/module.o &: $(SQD)/module.cppm $(SQD_PCMS)
	$(CXX) $(CXXFLAGS) -w -c $< -fmodule-output=$(BUILD)/pcm/sq.data.pcm -o $(BUILD)/obj/sq.data/module.o
$(BUILD)/obj/sq.data/%.cpp.o: $(SQD)/src/%.cpp $(BUILD)/pcm/sq.data.pcm
	$(CXX) $(CXXFLAGS) -w -c $< -o $@
$(BUILD)/obj/yyjson.o: third_party/yyjson-0.12.0/src/yyjson.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -w -c $< -o $@

define unit
$(BUILD)/pcm/sqmake.$(1).pcm $(BUILD)/obj/$(1).o &: src/$(1).cppm $(BUILD)/pcm/std.pcm $(2)
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj
	$(CXX) $(CXXFLAGS) -c $$< -fmodule-output=$(BUILD)/pcm/sqmake.$(1).pcm -o $(BUILD)/obj/$(1).o
endef
$(eval $(call unit,graph,))
$(eval $(call unit,state,))
$(eval $(call unit,cache,$(BUILD)/pcm/sqmake.graph.pcm $(BUILD)/pcm/sqmake.sha256.pcm))
$(eval $(call unit,run,$(BUILD)/pcm/sqmake.graph.pcm $(BUILD)/pcm/sqmake.state.pcm $(BUILD)/pcm/sqmake.cache.pcm))
$(eval $(call unit,scan,))
$(eval $(call unit,sha256,))
$(eval $(call unit,toolchain,$(BUILD)/pcm/sqmake.run.pcm $(BUILD)/pcm/sqmake.sha256.pcm))
$(eval $(call unit,lua,$(BUILD)/pcm/sqmake.graph.pcm $(BUILD)/pcm/sqmake.run.pcm $(BUILD)/pcm/sqmake.scan.pcm $(BUILD)/pcm/sqmake.toolchain.pcm))
$(eval $(call unit,call,$(BUILD)/pcm/sqmake.lua.pcm $(BUILD)/pcm/sqmake.toolchain.pcm $(BUILD)/pcm/sqmake.cache.pcm $(BUILD)/pcm/sq.data.pcm))

$(BUILD)/sqmake: tools/sqmake.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/test_%: tests/test_%.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

test: all
	@set -e; for t in $(TESTS); do $$t; done; echo "all tests passed"
	@bash tests/run_check.sh $(BUILD)/sqmake

clean:
	rm -r -f -- build
