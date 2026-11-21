// SPDX-License-Identifier: MIT
//
// sqmake.call: sqmake as other tools call it (squared's
// docs/developer/sdk-tools.md, "Tools calling tools"): one JSON request,
// one JSON response, through sq.data.
//
//   {"sq": 1, "command": "build", "args": {"dir": "game", "goals": ["build/game"]}}
//   {"sq": 1, "ok": true, "result": {"ran": 4, "up_to_date": 2, "explained": false}}
//   {"sq": 1, "ok": false, "error": {"code": "build_failed", "message": "..."}}
//
// The commands (describe() lists them, with their arguments):
//
//   build   dir, file, goals, jobs, explain   what is out of date, built
//   plan    dir, file, goals                  the targets a build needs, in
//                                             order, each with its command
//   clean   dir, file                         the build file's outputs and
//                                             the state removed
//
// Progress and a failed command's output go to stderr: the response is all
// a caller reads from stdout. dir is entered for the call and left after.

module;

#include <stdio.h>  // stderr: a macro, not in the std module

export module sqmake.call;

import std;
import sq.data;
import sqmake.graph;
import sqmake.state;
import sqmake.run;
import sqmake.lua;
import sqmake.toolchain;

export namespace sqmake {

using sq::data::JsonValue;

inline constexpr std::string_view tool_version = "0.7.1";

namespace calldetail {

inline JsonValue ok(JsonValue result) {
    return JsonValue(JsonValue::Object{{"sq", JsonValue(std::int64_t{1})}, {"ok", JsonValue(true)}, {"result", std::move(result)}});
}
inline JsonValue fail(std::string_view code, std::string message) {
    return JsonValue(JsonValue::Object{
        {"sq", JsonValue(std::int64_t{1})},
        {"ok", JsonValue(false)},
        {"error", JsonValue(JsonValue::Object{{"code", JsonValue(code)}, {"message", JsonValue(std::move(message))}})}});
}

/// An argument: a string, a bool, a whole number or a list of strings, or
/// its default when absent. A value of another type is the caller's error.
struct Args {
    const JsonValue* object = nullptr;
    std::string error;

    const JsonValue* get(std::string_view name) const { return object ? object->find(name) : nullptr; }
    std::string string(std::string_view name, std::string fallback) {
        const JsonValue* v = get(name);
        if (!v) return fallback;
        if (auto s = v->string_if()) return *s;
        error = std::format("args.{}: a string", name);
        return fallback;
    }
    bool boolean(std::string_view name, bool fallback) {
        const JsonValue* v = get(name);
        if (!v) return fallback;
        if (auto b = v->boolean_if()) return *b;
        error = std::format("args.{}: true or false", name);
        return fallback;
    }
    std::size_t count(std::string_view name, std::size_t fallback) {
        const JsonValue* v = get(name);
        if (!v) return fallback;
        if (auto i = v->signed_integer_if(); i && *i > 0) return static_cast<std::size_t>(*i);
        if (auto u = v->unsigned_integer_if(); u && *u > 0) return static_cast<std::size_t>(*u);
        error = std::format("args.{}: a whole number, 1 or more", name);
        return fallback;
    }
    std::vector<std::string> strings(std::string_view name) {
        std::vector<std::string> out;
        const JsonValue* v = get(name);
        if (!v) return out;
        const auto* list = v->array_if();
        if (!list) {
            error = std::format("args.{}: a list of strings", name);
            return out;
        }
        for (const JsonValue& item : *list) {
            if (auto s = item.string_if()) out.push_back(*s);
            else error = std::format("args.{}: a list of strings", name);
        }
        return out;
    }
};

inline JsonValue strings_json(const std::vector<std::string>& list) {
    JsonValue::Array a;
    for (const std::string& s : list) a.emplace_back(s);
    return JsonValue(std::move(a));
}

/// The working directory for one call, restored after it.
class InDir {
public:
    explicit InDir(const std::string& dir) {
        std::error_code ec;
        before_ = std::filesystem::current_path(ec);
        if (!dir.empty() && dir != ".") std::filesystem::current_path(dir, ec);
        ok_ = !ec;
    }
    ~InDir() {
        std::error_code ec;
        std::filesystem::current_path(before_, ec);
    }
    InDir(const InDir&) = delete;
    InDir& operator=(const InDir&) = delete;
    bool ok() const { return ok_; }

private:
    std::filesystem::path before_;
    bool ok_ = false;
};

}  // namespace calldetail

/// What sqmake can be asked, as JSON (the protocol's `describe`).
inline JsonValue describe() {
    using O = JsonValue::Object;
    auto arg = [](std::string_view type, std::string_view summary) {
        return JsonValue(O{{"type", JsonValue(type)}, {"required", JsonValue(false)}, {"summary", JsonValue(summary)}});
    };
    const JsonValue dir = arg("string", "the folder holding the build file (default: the current one)");
    const JsonValue file = arg("string", "the build file (default: build.lua)");
    const JsonValue goals = arg("array of strings", "what to build (default: the build file's default())");
    return JsonValue(O{
        {"sq", JsonValue(std::int64_t{1})},
        {"tool", JsonValue("sqmake")},
        {"version", JsonValue(tool_version)},
        {"speaks", JsonValue(JsonValue::Array{JsonValue(std::int64_t{1})})},
        {"commands",
         JsonValue(O{
             {"build", JsonValue(O{{"summary", JsonValue("build what is out of date")},
                                   {"args", JsonValue(O{{"dir", dir},
                                                        {"file", file},
                                                        {"goals", goals},
                                                        {"jobs", arg("integer", "commands at once (default: every core)")},
                                                        {"explain", arg("boolean", "run nothing; say what would run, and why")}})},
                                   {"result", JsonValue("{ran, up_to_date, explained}")}})},
             {"plan", JsonValue(O{{"summary", JsonValue("the targets a build needs, in order, with their commands")},
                                  {"args", JsonValue(O{{"dir", dir}, {"file", file}, {"goals", goals}})},
                                  {"result", JsonValue("{targets: [{outputs, inputs, command, description, action}]}")}})},
             {"clean", JsonValue(O{{"summary", JsonValue("remove the build file's outputs and the state")},
                                   {"args", JsonValue(O{{"dir", dir}, {"file", file}})},
                                   {"result", JsonValue("{removed}")}})},
             {"toolchain", JsonValue(O{{"summary", JsonValue("the SDK's toolchain found here: each part, where, and the fingerprint")},
                                       {"args", JsonValue(O{{"dir", dir},
                                                            {"file", arg("string", "the description (default: squared/build/toolchain.lua)")}})},
                                       {"result", JsonValue("{llvm_bin, clang, sysroot, runtimes, libcxx, std_cppm, target, fingerprint, from}")}})},
         })},
    });
}

/// One request answered (the protocol's `call`).
inline JsonValue call(const JsonValue& request) {
    using namespace calldetail;
    // The version: a whole number (sq.data reads a positive one as unsigned).
    const JsonValue* version = request.find("sq");
    std::optional<std::int64_t> v;
    if (version) {
        if (auto i = version->signed_integer_if()) v = *i;
        else if (auto u = version->unsigned_integer_if(); u && *u <= 1000000) v = static_cast<std::int64_t>(*u);
    }
    if (!v) return fail("bad_request", "the request has no \"sq\" version");
    if (*v != 1) return fail("version", std::format("protocol version {}; sqmake speaks 1", *v));
    const JsonValue* command_json = request.find("command");
    const std::string* command = command_json ? command_json->string_if() : nullptr;
    if (!command) return fail("bad_request", "the request has no \"command\"");
    Args a{request.find("args")};
    if (a.object && !a.object->object_if()) return fail("bad_request", "\"args\" is not an object");

    const std::string dir = a.string("dir", "."), file = a.string("file", "build.lua");
    std::vector<std::string> goals = a.strings("goals");
    const std::size_t jobs = a.count("jobs", std::max(1U, std::thread::hardware_concurrency()));
    const bool explain = a.boolean("explain", false);
    if (!a.error.empty()) return fail("bad_request", a.error);
    if (*command == "toolchain") {
        InDir in(dir);
        if (!in.ok()) return fail("no_dir", "cannot enter " + dir);
        auto description = read_toolchain_description(a.string("file", "squared/build/toolchain.lua"));
        if (!description) return fail("bad_toolchain", description.error());
        auto t = resolve_toolchain(*description);
        if (!t) return fail("toolchain_missing", t.error());
        using O = JsonValue::Object;
        return ok(JsonValue(O{{"llvm_bin", JsonValue(t->llvm_bin)},
                              {"clang", JsonValue(t->clang_version)},
                              {"sysroot", JsonValue(t->sysroot)},
                              {"runtimes", JsonValue(t->runtimes)},
                              {"libcxx", JsonValue(t->libcxx_version)},
                              {"std_cppm", JsonValue(t->std_cppm)},
                              {"target", JsonValue(t->triple)},
                              {"fingerprint", JsonValue(t->fingerprint)},
                              {"from", JsonValue(O{{"llvm_bin", JsonValue(t->llvm_bin_from)},
                                                   {"sysroot", JsonValue(t->sysroot_from)},
                                                   {"runtimes", JsonValue(t->runtimes_from)}})}}));
    }
    if (*command != "build" && *command != "plan" && *command != "clean")
        return fail("unknown_command", std::format("{}: sqmake's commands are build, plan, clean and toolchain", *command));

    InDir in(dir);
    if (!in.ok()) return fail("no_dir", "cannot enter " + dir);
    auto ctx = Context::create({});
    if (!ctx) return fail("load_failed", ctx.error());
    if (auto loaded = (*ctx)->load(file); !loaded) return fail("load_failed", loaded.error());
    Graph& graph = (*ctx)->graph();
    const std::filesystem::path state_file = std::filesystem::path((*ctx)->build_dir()) / ".sqmake" / "state";

    if (*command == "clean") {
        std::error_code ec;
        std::int64_t removed = 0;
        for (const Target& t : graph.targets()) {
            if (t.command.argv.empty() || t.action) continue;  // an action's name is no file
            for (const std::string& o : t.outputs) removed += std::filesystem::remove(o, ec) ? 1 : 0;
            if (!t.depfile.empty()) std::filesystem::remove(t.depfile, ec);
        }
        std::filesystem::remove(state_file, ec);
        return ok(JsonValue(JsonValue::Object{{"removed", JsonValue(removed)}}));
    }
    if (goals.empty()) goals = (*ctx)->defaults();
    if (goals.empty()) return fail("no_goal", file + " names no default() and the request no goals");
    auto plan = graph.plan(goals);
    if (!plan) return fail("plan_failed", plan.error());
    if (*command == "plan") {
        JsonValue::Array targets;
        for (std::size_t i : *plan) {
            const Target& t = graph.at(i);
            targets.emplace_back(JsonValue::Object{{"outputs", strings_json(t.outputs)},
                                                   {"inputs", strings_json(t.inputs)},
                                                   {"command", strings_json(t.command.argv)},
                                                   {"description", JsonValue(t.description)},
                                                   {"action", JsonValue(t.action)}});
        }
        return ok(JsonValue(JsonValue::Object{{"targets", JsonValue(std::move(targets))}}));
    }
    State state = State::load(state_file);
    Runner runner(graph, state, {.jobs = jobs, .explain = explain, .extra_hash = (*ctx)->fingerprint(), .out = stderr});
    auto result = runner.run(*plan);
    if (!result) return fail("build_failed", result.error());
    if (result->failed) return fail("build_failed", std::format("{} command(s) failed; their output is on stderr", result->failed));
    return ok(JsonValue(JsonValue::Object{{"ran", JsonValue(static_cast<std::int64_t>(result->ran))},
                                          {"up_to_date", JsonValue(static_cast<std::int64_t>(result->up_to_date))},
                                          {"explained", JsonValue(explain)}}));
}

/// A request's text answered with a response's text, and the exit status
/// the protocol gives it: 0 ok, 1 not, 2 when the request could not be read.
inline std::string call_text(std::string_view request, int& status) {
    JsonValue response;
    auto parsed = sq::data::parse_json(request);
    if (!parsed) {
        response = calldetail::fail("bad_request", "the request is not JSON: " + parsed.error.message);
        status = 2;
    } else {
        response = call(parsed.value);
        const JsonValue* okv = response.find("ok");
        status = okv && okv->boolean_if() && *okv->boolean_if() ? 0 : 1;
        if (status == 1) {
            const JsonValue* e = response.find("error");
            const JsonValue* code = e ? e->find("code") : nullptr;
            if (code && code->string_if() && *code->string_if() == "bad_request") status = 2;
        }
    }
    auto text = sq::data::write_json(response);
    return text ? text.text + "\n" : "{\"sq\": 1, \"ok\": false, \"error\": {\"code\": \"internal\", \"message\": \"cannot write the response\"}}\n";
}

}  // namespace sqmake
