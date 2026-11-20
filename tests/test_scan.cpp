// SPDX-License-Identifier: MIT
// sqmake.scan: what a source provides and imports, from its declarations.
#include <stdio.h>

import std;
import sqmake.scan;

using namespace sqmake;

int main() {
    int failures = 0;
    auto check = [&](bool ok, std::string_view what) {
        if (!ok) {
            std::println(stderr, "FAIL {}", what);
            ++failures;
        }
    };
    auto u = scan_source("module;\n#include <stdio.h>\nexport module a.b;\nexport import :part;\nimport std;\nimport c;\n");
    check(u && u->provides == "a.b" && u->interface, "an interface");
    check(u && u->imports == std::vector<std::string>{"a.b:part", "c", "std"}, "its imports, the partition resolved");
    auto p = scan_source("export module a.b:part;\nimport std;\n");
    check(p && p->provides == "a.b:part" && p->interface, "an interface partition");
    auto internal = scan_source("module a.b:impl;\n");
    check(internal && internal->provides == "a.b:impl" && !internal->interface, "an internal partition");
    auto impl = scan_source("module a.b;\nimport std;\n");
    check(impl && !impl->provides && impl->imports == std::vector<std::string>{"a.b", "std"}, "an implementation unit imports its interface");
    auto quiet = scan_source("// import x;\n/* export module y;\n import z; */\nauto s = \"import q;\";\nauto r = R\"(import w;)\";\nint main() {}\n");
    check(quiet && !quiet->provides && quiet->imports.empty(), "comments and strings hide nothing");
    auto plain = scan_source("#include <cstdio>\nint main() { std::puts(\"x\"); }\n");
    check(plain && !plain->provides && plain->imports.empty(), "a plain source");
    check(!scan_source("import <vector>;\n").has_value(), "a header unit refused");
    check(!scan_source("export module a;\nexport module b;\n").has_value(), "two module declarations refused");
    check(!scan_source("import :x;\n").has_value(), "a partition import outside a module refused");
    check(bmi_name("a.b:part") == "a.b-part.pcm", "a partition's BMI name");
    if (failures == 0) std::println("ok   scan");
    return failures == 0 ? 0 : 1;
}
