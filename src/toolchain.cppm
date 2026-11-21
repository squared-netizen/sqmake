// SPDX-License-Identifier: MIT
//
// sqmake.toolchain: the SDK's toolchain found with no shell (docs/spec.md
// §4.3). A description, the data in squared's build/toolchain.lua (read by
// sqmake.lua, values only), is resolved on this machine: each place from its
// environment variable, else under $PREFIX, else its default; each checked
// for what must be there, by looking (std::filesystem, permission bits), not
// by asking libc's access(), which bionic answers wrongly for some flags.
// clang is run once for its version, as an argument list.
//
// What a build gets: the compile commands with every flag in them (no
// wrapper scripts), the link flags, llvm-ar, libc++'s std.cppm, and the
// fingerprint, the same as build/toolchain.sh's byte for byte (SHA-256 of
// the same five lines), so make's build and sqmake's name the same BMI
// folders. A clang resource directory (its own headers with the runtimes'
// builtins) is made once in the build folder, as toolchain.sh wrappers did.

module;

#include <stdlib.h>  // getenv: a function the std module does not export

export module sqmake.toolchain;

import std;
import sqmake.run;
import sqmake.sha256;

export namespace sqmake {

/// Where one part of the toolchain is: the environment variable that says,
/// else this path under $PREFIX (sqterm's), else `otherwise`; `install`
/// says how to get it, for the error when it is not there.
struct ToolchainPlace {
    std::string env;
    std::optional<std::string> under_prefix;
    std::string otherwise;
    std::string install;
};

/// build/toolchain.lua's data.
struct ToolchainDescription {
    int clang_major = 0;
    int api = 0;
    std::string api_env;
    std::string triple;  // the API level is appended
    std::vector<std::string> flags, cxx_flags, link_flags;
    ToolchainPlace llvm_bin, sysroot, runtimes;
};

/// The toolchain as found here.
struct Toolchain {
    std::string llvm_bin, sysroot, runtimes;
    std::string llvm_bin_from, sysroot_from, runtimes_from;  // "SQ_LLVM_BIN", "$PREFIX", "the default"
    std::string triple, clang_version, clang_version_line, libcxx_version, std_cppm, fingerprint;
    std::vector<std::string> common, cxx_only, link;  // what changes a BMI (common has --target)

    /// The resource directory made for this toolchain: clang's headers with
    /// the runtimes' builtins.
    std::string resource_dir(const std::string& build) const { return build + "/toolchain-" + fingerprint + "/resource-dir"; }

    /// Every flag a C compile or a link takes (paths included).
    std::vector<std::string> cc(const std::string& build) const {
        std::vector<std::string> argv{llvm_bin + "/clang"};
        argv.insert(argv.end(), common.begin(), common.end());
        append_paths(argv, build);
        return argv;
    }
    /// Every flag a C++ compile or a link takes: and libc++'s headers.
    std::vector<std::string> cxx(const std::string& build) const {
        std::vector<std::string> argv{llvm_bin + "/clang++"};
        argv.insert(argv.end(), common.begin(), common.end());
        append_paths(argv, build);
        argv.insert(argv.end(), cxx_only.begin(), cxx_only.end());
        for (const std::string& dir : {runtimes + "/include/" + triple + "/c++/v1", runtimes + "/include/c++/v1"}) {
            argv.push_back("-isystem");
            argv.push_back(dir);
        }
        return argv;
    }
    std::string ar() const { return llvm_bin + "/llvm-ar"; }

private:
    void append_paths(std::vector<std::string>& argv, const std::string& build) const {
        argv.push_back("--sysroot=" + sysroot);
        argv.push_back("-resource-dir=" + resource_dir(build));
        argv.push_back("-L" + runtimes + "/lib/" + triple);
        // A link flag on a compile is not worth a warning on every call.
        argv.push_back("-Wno-unused-command-line-argument");
    }
};

namespace toolchaindetail {

inline std::string env(const std::string& name) {
    if (name.empty()) return {};
    const char* v = ::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

/// A regular file (a link followed) that someone may run: its permission
/// bits, read, not asked of access().
inline bool executable(const std::filesystem::path& p) {
    std::error_code ec;
    const auto st = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::is_regular_file(st)) return false;
    using std::filesystem::perms;
    return (st.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
}

inline bool is_dir(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}
inline bool is_file(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

inline std::string join(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}

/// One place: its variable, else under $PREFIX when `found` says the
/// candidate is right (or no default is left), else the default.
template <class Found>
std::pair<std::string, std::string> place(const ToolchainPlace& p, Found found) {
    if (const std::string v = env(p.env); !v.empty()) return {v, p.env};
    const std::string prefix = env("PREFIX");
    if (p.under_prefix && !prefix.empty()) {
        const std::string candidate = p.under_prefix->empty() ? prefix : prefix + "/" + *p.under_prefix;
        if (found(candidate) || p.otherwise.empty()) return {candidate, "$PREFIX"};
    }
    if (!p.otherwise.empty()) return {p.otherwise, "the default"};
    return {"/nonexistent", "nothing ($PREFIX is not set)"};
}

/// `#define _LIBCPP_VERSION N` in libc++'s __config: N, as toolchain.sh's
/// pattern reads it ("#" %s* "define" %s* "_LIBCPP_VERSION" %s* {%d*}).
inline std::string libcxx_version(const std::filesystem::path& config) {
    std::ifstream in(config);
    for (std::string line; std::getline(in, line);) {
        std::size_t i = 0;
        auto spaces = [&] {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        };
        auto word = [&](std::string_view w) {
            if (line.compare(i, w.size(), w) != 0) return false;
            i += w.size();
            return true;
        };
        if (!word("#")) continue;
        spaces();
        if (!word("define")) continue;
        spaces();
        if (!word("_LIBCPP_VERSION")) continue;
        spaces();
        const std::size_t from = i;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        return line.substr(from, i - from);
    }
    return {};
}

/// The first share/libc++/v*/std.cppm, by name (as `ls` lists them).
inline std::string std_cppm(const std::string& runtimes) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(runtimes + "/share/libc++", ec)) {
        const std::string name = e.path().filename().string();
        if (name.starts_with("v") && is_file(e.path() / "std.cppm")) found.push_back(e.path() / "std.cppm");
    }
    std::ranges::sort(found);
    return found.empty() ? std::string() : found.front().string();
}

inline std::string first_line(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

}  // namespace toolchaindetail

/// The description resolved here, or what is missing and how to get it.
inline std::expected<Toolchain, std::string> resolve_toolchain(const ToolchainDescription& d) {
    using namespace toolchaindetail;
    Toolchain t;
    int api = d.api;
    if (const std::string v = env(d.api_env); !v.empty()) {
        if (std::from_chars(v.data(), v.data() + v.size(), api).ec != std::errc())
            return std::unexpected(std::format("{}={} is not an API level", d.api_env, v));
    }
    t.triple = d.triple + std::to_string(api);

    std::tie(t.llvm_bin, t.llvm_bin_from) = place(d.llvm_bin, [](const std::string& dir) { return executable(dir + "/clang++"); });
    std::tie(t.sysroot, t.sysroot_from) = place(d.sysroot, [](const std::string& dir) { return is_dir(dir + "/usr/include"); });
    std::tie(t.runtimes, t.runtimes_from) = place(d.runtimes, [](const std::string&) { return true; });

    auto missing = [&](std::string_view what, const std::string& where, const ToolchainPlace& p) {
        return std::unexpected(std::format("no {} at {} (found by {}; set {}{})", what, where,
                                           where == p.otherwise ? "the default" : p.env + " or $PREFIX", p.env,
                                           p.install.empty() ? "" : "; in sqterm: " + p.install));
    };
    if (!executable(t.llvm_bin + "/clang++") || !executable(t.llvm_bin + "/clang"))
        return missing("clang and clang++", t.llvm_bin, d.llvm_bin);
    const Ran version = run_command({t.llvm_bin + "/clang", "-dumpversion"});
    if (version.status != 0) return std::unexpected(std::format("{}/clang -dumpversion failed: {}", t.llvm_bin, version.output));
    t.clang_version = first_line(version.output);
    if (t.clang_version.substr(0, t.clang_version.find('.')) != std::to_string(d.clang_major))
        return std::unexpected(std::format("clang {} at {}: the toolchain is clang {}", t.clang_version, t.llvm_bin, d.clang_major));
    if (!is_dir(t.sysroot + "/usr/include")) return missing("sysroot", t.sysroot, d.sysroot);
    if (!is_file(t.runtimes + "/lib/" + t.triple + "/libc++.a"))
        return missing(std::format("libc++ for {}", t.triple), t.runtimes, d.runtimes);
    t.std_cppm = std_cppm(t.runtimes);
    t.libcxx_version = libcxx_version(t.runtimes + "/include/c++/v1/__config");

    t.common = {"--target=" + t.triple};
    t.common.insert(t.common.end(), d.flags.begin(), d.flags.end());
    t.cxx_only = d.cxx_flags;
    t.link = d.link_flags;
    const Ran line = run_command({t.llvm_bin + "/clang", "--version"});
    t.clang_version_line = first_line(line.output);
    // build/toolchain.sh's fingerprint: the same five lines, hashed alike.
    const std::string text = std::format("{}\nlibc++ {}\n{}\n{}\n{}\n", t.clang_version_line, t.libcxx_version, join(t.common),
                                         join(t.cxx_only), join(t.link));
    t.fingerprint = sha256_hex(text).substr(0, 16);
    return t;
}

/// The resource directory, made once: clang's own include/ and each of the
/// runtimes' lib/clang/*/lib/.
inline std::expected<void, std::string> make_resource_dir(const Toolchain& t, const std::string& build) {
    namespace fs = std::filesystem;
    const fs::path rd = t.resource_dir(build), done = rd / ".made";
    std::error_code ec;
    if (fs::exists(done, ec)) return {};
    const Ran printed = run_command({t.llvm_bin + "/clang", "-print-resource-dir"});
    const std::string own = toolchaindetail::first_line(printed.output);
    if (printed.status != 0 || !toolchaindetail::is_dir(own + "/include"))
        return std::unexpected(std::format("{}/clang -print-resource-dir: no include/ ({})", t.llvm_bin, printed.output));
    fs::remove_all(rd, ec);
    fs::create_directories(rd, ec);
    const auto options = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
    fs::copy(own + "/include", rd / "include", options, ec);
    if (ec) return std::unexpected(std::format("cannot copy {}/include into {}: {}", own, rd.string(), ec.message()));
    for (const auto& e : fs::directory_iterator(t.runtimes + "/lib/clang", ec)) {
        if (!toolchaindetail::is_dir(e.path() / "lib")) continue;
        fs::copy(e.path() / "lib", rd / "lib", options, ec);
        if (ec) return std::unexpected(std::format("cannot copy {} into {}: {}", (e.path() / "lib").string(), rd.string(), ec.message()));
    }
    std::ofstream(done) << t.fingerprint << '\n';
    return {};
}

}  // namespace sqmake
