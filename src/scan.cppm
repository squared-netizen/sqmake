// SPDX-License-Identifier: MIT
//
// sqmake.scan: what a C++ source provides and imports (docs/spec.md §5.1).
//
// Read from the source's own declarations, without the preprocessor:
//
//   export module a.b;          provides a.b (an interface)
//   export module a.b:part;     provides a.b:part (an interface partition)
//   module a.b:part;            provides a.b:part (an internal partition)
//   module a.b;                 imports a.b (an implementation unit)
//   import x.y;  export import x.y;      imports x.y
//   import :part;               imports <this module>:part
//
// `module;` (the global module fragment) and `module :private;` provide and
// import nothing. Comments and string literals are skipped; a declaration
// is a statement at the start of a line. A declaration made by a macro, or
// inside #if, is not seen: squared writes neither. A header unit
// (import <x>; import "x";) is refused: sqmake builds named modules only.

export module sqmake.scan;

import std;

export namespace sqmake {

struct ModuleUnit {
    std::optional<std::string> provides;  // a.b or a.b:part
    bool interface = false;               // export module ...
    std::vector<std::string> imports;     // full names, partitions resolved
};

namespace scandetail {
/// The text with comments and string and character literals blanked (kept
/// as spaces, line breaks kept), so declarations are found where they are.
inline std::string blank(std::string_view s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            out[i] = out[i + 1] = ' ';
            for (i += 2; i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/'); ++i)
                if (out[i] != '\n') out[i] = ' ';
            if (i < out.size()) out[i] = out[i + 1] = ' ', ++i;
        } else if (out[i] == 'R' && i + 1 < out.size() && out[i + 1] == '"') {  // R"delim( ... )delim"
            const std::size_t open = out.find('(', i);
            if (open == std::string::npos) continue;
            const std::string close = ")" + out.substr(i + 2, open - i - 2) + "\"";
            const std::size_t end = out.find(close, open);
            const std::size_t stop = end == std::string::npos ? out.size() : end + close.size();
            for (std::size_t k = i; k < stop; ++k)
                if (out[k] != '\n') out[k] = ' ';
            i = stop - 1;
        } else if (out[i] == '"' || (out[i] == '\'' && !(i > 0 && std::isxdigit(static_cast<unsigned char>(out[i - 1]))))) {
            const char q = out[i];
            out[i] = ' ';
            for (++i; i < out.size() && out[i] != q && out[i] != '\n'; ++i) {
                if (out[i] == '\\' && i + 1 < out.size()) out[i++] = ' ';
                out[i] = ' ';
            }
            if (i < out.size() && out[i] == q) out[i] = ' ';
        }
    }
    return out;
}
}  // namespace scandetail

/// The unit a source is, from its declarations.
inline std::expected<ModuleUnit, std::string> scan_source(std::string_view text) {
    const std::string s = scandetail::blank(text);
    ModuleUnit u;
    std::string primary;  // this unit's module, for import :part
    std::istringstream lines(s);
    std::string line;
    int number = 0;
    std::vector<std::pair<int, std::string>> partition_imports;
    while (std::getline(lines, line)) {
        ++number;
        std::string_view l(line);
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        bool exported = false;
        if (l.starts_with("export") && l.size() > 6 && (l[6] == ' ' || l[6] == '\t')) {
            exported = true;
            l.remove_prefix(6);
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        }
        const bool is_module = l.starts_with("module") && (l.size() == 6 || l[6] == ' ' || l[6] == ';' || l[6] == '\t');
        const bool is_import = l.starts_with("import") && (l.size() > 6 && (l[6] == ' ' || l[6] == ':' || l[6] == '<' || l[6] == '"' || l[6] == '\t'));
        if (!is_module && !is_import) continue;
        std::string_view rest = l.substr(6);
        const std::size_t semi = rest.find(';');
        if (semi == std::string_view::npos) continue;  // not a declaration on one line
        std::string name;
        for (char c : rest.substr(0, semi))
            if (c != ' ' && c != '\t') name += c;
        if (is_module) {
            if (name.empty() || name == ":private") continue;  // module; and module :private;
            if (u.provides || !primary.empty()) return std::unexpected(std::format("line {}: a second module declaration", number));
            const std::size_t colon = name.find(':');
            primary = name.substr(0, colon);
            if (exported || colon != std::string::npos) {
                u.provides = name;
                u.interface = exported;
            } else {
                u.imports.push_back(name);  // an implementation unit imports its interface
            }
        } else {
            if (!name.empty() && (name[0] == '<' || name[0] == '"'))
                return std::unexpected(std::format("line {}: import {}: header units are not built by sqmake", number, name));
            if (!name.empty() && name[0] == ':') partition_imports.emplace_back(number, name);
            else if (!name.empty()) u.imports.push_back(name);
        }
    }
    for (auto& [n, part] : partition_imports) {
        if (primary.empty()) return std::unexpected(std::format("line {}: import {} outside a module", n, part));
        u.imports.push_back(primary + part);
    }
    std::ranges::sort(u.imports);
    u.imports.erase(std::unique(u.imports.begin(), u.imports.end()), u.imports.end());
    return u;
}

/// The file clang looks for a module's BMI under (-fprebuilt-module-path):
/// a.b:part is a.b-part.pcm.
inline std::string bmi_name(std::string_view module) {
    std::string out(module);
    std::ranges::replace(out, ':', '-');
    return out + ".pcm";
}

}  // namespace sqmake
