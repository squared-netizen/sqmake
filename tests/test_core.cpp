// SPDX-License-Identifier: MIT
// The graph (plan order, cycles, two makers), the command hash, the
// dependency-file reader and the state file.
#include <stdio.h>

import std;
import sqmake.graph;
import sqmake.state;

using namespace sqmake;

namespace {
int failures = 0;
void check(bool ok, std::string_view what) {
    if (!ok) {
        std::println(stderr, "FAIL {}", what);
        ++failures;
    }
}
Target t(std::string out, std::vector<std::string> ins, std::vector<std::string> argv = {"true"}) {
    return Target{{std::move(out)}, std::move(ins), Command{std::move(argv)}, {}, {}};
}
}  // namespace

int main() {
    Graph g;
    check(g.add(t("app", {"a.o", "b.o"})).has_value(), "add app");
    check(g.add(t("a.o", {"a.cpp"})).has_value(), "add a.o");
    check(g.add(t("b.o", {"b.cpp"})).has_value(), "add b.o");
    check(!g.add(t("a.o", {"other.cpp"})).has_value(), "a second maker of a.o refused");
    auto plan = g.plan({"app"});
    check(plan && plan->size() == 3 && g.at(plan->back()).outputs[0] == "app", "app planned last");
    check(!g.plan({"nothing"}).has_value(), "an unknown goal refused");

    Graph c;
    (void)c.add(t("x", {"y"}));
    (void)c.add(t("y", {"z"}));
    (void)c.add(t("z", {"x"}));
    auto cyc = c.plan({"x"});
    check(!cyc && cyc.error().find("a cycle") != std::string::npos, "a cycle refused, named");

    const Target one = t("o", {}, {"cc", "-c", "a"});
    check(command_hash(one) == command_hash(t("o", {}, {"cc", "-c", "a"})), "the same command, the same hash");
    check(command_hash(one) != command_hash(t("o", {}, {"cc", "-ca"})), "argument boundaries count");
    check(command_hash(one) != command_hash(one, "fingerprint"), "the fingerprint counts");

    const auto dir = std::filesystem::temp_directory_path() / std::format("sqmake-test-{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    {
        std::ofstream d(dir / "a.d");
        d << "build/a.o: src/a.cpp include/a\\ b.h \\\n  include/c.h\n\ninclude/c.h:\n";
    }
    auto deps = read_depfile(dir / "a.d");
    check(deps && *deps == std::vector<std::string>{"src/a.cpp", "include/a b.h", "include/c.h"}, "a dependency file read");

    State s = State::load(dir / "state");
    s.set("build/a.o", Record{0x1234, {"src/a.cpp", "include/c.h"}});
    check(s.save(), "state saved");
    State back = State::load(dir / "state");
    const Record* r = back.find("build/a.o");
    check(r && r->hash == 0x1234 && r->deps.size() == 2, "state read back");
    std::filesystem::remove_all(dir);

    if (failures == 0) std::println("ok   core");
    return failures == 0 ? 0 : 1;
}
