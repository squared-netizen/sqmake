// SPDX-License-Identifier: MIT
//
// sqmake: the squared SDK's build system (docs/spec.md).
//
//   sqmake [-C DIR] [-f FILE] [-j N] [--debug] [--explain] [GOAL...]
//                      build.lua (or FILE) in DIR read, and the goals (or
//                      its defaults) built: what is out of date, N at once
//                      (every core by default). A goal is a file, a verb
//                      (build, run, install, test: what each does is the
//                      build file's) or an action. --debug: the debug build
//                      (mode.debug; in build/debug unless .sqmake.lua says
//                      separate_debug = false). --explain prints each
//                      command that would run and why, and runs nothing.
//   sqmake clean [-C DIR] [-f FILE] [--debug]
//                      every output build.lua names removed, and the state
//   sqmake toolchain [FILE]
//                      the SDK's toolchain (FILE: squared/build/toolchain.lua)
//                      found here: each part, where and why, the fingerprint
//   sqmake call        one JSON request on stdin, one JSON response on
//                      stdout (squared's docs/developer/sdk-tools.md)
//   sqmake describe    what `call` can be asked, as JSON
//   sqmake --help | --version

#include <stdio.h>  // stdout, stderr: macros, not in the std module

import std;
import sqmake.cache;
import sqmake.graph;
import sqmake.state;
import sqmake.run;
import sqmake.lua;
import sqmake.call;
import sqmake.toolchain;
import sq.data;

namespace {

constexpr std::string_view version = sqmake::tool_version;

int usage(std::FILE* out = stderr, int status = 2) {
    std::println(out, "usage: sqmake [-C DIR] [-f FILE] [-j N] [--debug] [--explain] [GOAL...]\n"
                      "       sqmake clean [-C DIR] [-f FILE] [--debug]\n"
                      "GOAL: a file the build makes, a verb (build, run, install, test) or an action\n"
                      "       sqmake toolchain [FILE]\n"
                      "       sqmake call < REQUEST.json\n"
                      "       sqmake describe\n"
                      "       sqmake --help | --version");
    return status;
}

int fail(std::string_view why) {
    std::println(stderr, "sqmake: {}", why);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) return usage(stdout, 0);
    if (!args.empty() && (args[0] == "--version" || args[0] == "version")) {
        std::println("sqmake {}", version);
        return 0;
    }
    if (!args.empty() && args[0] == "describe") {
        auto text = sq::data::write_json(sqmake::describe(), {.pretty = true, .newline_at_end = true});
        std::print("{}", text.text);
        return text ? 0 : 1;
    }
    if (!args.empty() && args[0] == "toolchain") {
        const std::string file = args.size() > 1 ? std::string(args[1]) : "squared/build/toolchain.lua";
        auto description = sqmake::read_toolchain_description(file);
        if (!description) return fail(description.error());
        auto t = sqmake::resolve_toolchain(*description);
        if (!t) return fail("toolchain: " + t.error());
        std::println("  {:<12} {} (clang {}; from {})", "llvm", t->llvm_bin, t->clang_version, t->llvm_bin_from);
        std::println("  {:<12} {} (from {})", "sysroot", t->sysroot, t->sysroot_from);
        std::println("  {:<12} {} (libc++ {}; from {})", "runtimes", t->runtimes, t->libcxx_version, t->runtimes_from);
        std::println("  {:<12} {}", "std.cppm", t->std_cppm.empty() ? "(none: import std cannot be built)" : t->std_cppm);
        std::println("  {:<12} {}", "target", t->triple);
        std::println("  {:<12} {}", "fingerprint", t->fingerprint);
        return 0;
    }
    if (!args.empty() && args[0] == "call") {
        const std::string request((std::istreambuf_iterator<char>(std::cin)), {});
        int status = 0;
        std::print("{}", sqmake::call_text(request, status));
        return status;
    }
    std::string dir, file = "build.lua";
    std::size_t jobs = std::max(1U, std::thread::hardware_concurrency());
    bool explain = false, clean = false, debug = false;
    std::vector<std::string> goals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "-C" && has_value) dir = args[++i];
        else if (a == "-f" && has_value) file = args[++i];
        else if (a == "-j" && has_value) {
            const std::string_view v = args[++i];
            if (std::from_chars(v.data(), v.data() + v.size(), jobs).ec != std::errc() || jobs == 0) return fail("-j: a number, 1 or more");
        } else if (a.starts_with("-j") && a.size() > 2) {
            if (std::from_chars(a.data() + 2, a.data() + a.size(), jobs).ec != std::errc() || jobs == 0) return fail("-j: a number, 1 or more");
        } else if (a == "--explain") explain = true;
        else if (a == "--debug") debug = true;
        else if (a == "clean" && goals.empty() && !clean) clean = true;
        else if (a.starts_with("-")) return usage();
        else goals.emplace_back(a);
    }
    std::error_code ec;
    if (!dir.empty()) {
        std::filesystem::current_path(dir, ec);
        if (ec) return fail("cannot enter " + dir);
    }
    auto ctx = sqmake::Context::create({.debug = debug});
    if (!ctx) return fail(ctx.error());
    if (auto loaded = (*ctx)->load(file); !loaded) return fail(loaded.error());
    sqmake::Graph& graph = (*ctx)->graph();
    const std::filesystem::path state_file = std::filesystem::path((*ctx)->build_dir()) / ".sqmake" / "state";

    if (clean) {
        std::size_t removed = 0;
        for (const sqmake::Target& t : graph.targets()) {
            if (t.alias() || t.action) continue;  // an action's name is no file
            for (const std::string& o : t.outputs) removed += std::filesystem::remove(o, ec) ? 1 : 0;
            if (!t.depfile.empty()) std::filesystem::remove(t.depfile, ec);
        }
        std::filesystem::remove(state_file, ec);
        std::println("sqmake: {} file(s) removed", removed);
        return 0;
    }
    if (goals.empty()) goals = (*ctx)->defaults();
    if (goals.empty()) return fail(file + " names no default (default(...)) and no goal was given");
    auto plan = graph.plan(goals);
    if (!plan) return fail(plan.error());
    sqmake::State state = sqmake::State::load(state_file);
    sqmake::Runner runner(graph, state, {.jobs = jobs, .explain = explain, .extra_hash = (*ctx)->fingerprint(), .cache = sqmake::cache_dir()});
    auto result = runner.run(*plan);
    if (!result) return fail(result.error());
    if (result->failed) return 1;
    if (!explain && result->ran == 0) std::println("sqmake: up to date");
    if (result->cached > 0) std::println("sqmake: {} of {} from the cache", result->cached, result->ran);
    if (const std::size_t gone = sqmake::prune_cache(sqmake::cache_dir(), sqmake::cache_limit()); gone > 0)
        std::println("sqmake: the cache pruned: {} entries used longest ago removed", gone);
    return 0;
}
