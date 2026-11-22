// SPDX-License-Identifier: MIT
//
// sqmake.cache: compiles shared between builds on one device (docs/spec.md
// §6.3).
//
// A target that writes a dependency file says exactly what it read: its
// command, its inputs, and the headers the depfile names. Such a target's
// outputs are kept in a cache, under a key made of its command (which
// carries the toolchain's fingerprint) and its inputs' contents; with them,
// the list of what it read and each file's sha256. A later build anywhere
// on the device, of the same command over the same inputs, whose headers
// still hash the same, takes the outputs from there instead of compiling.
// Two workspaces building squared from its one installed copy run the same
// commands, so the second compiles none of it.
//
// Entries are written to a temporary folder and renamed into place, so two
// builds writing the same entry at once leave one whole entry. Nothing is
// read that does not check out: a wrong cache is never worse than none.

export module sqmake.cache;

import std;
import sqmake.graph;
import sqmake.sha256;

export namespace sqmake {

class Cache {
public:
    /// A cache in `dir`; an empty dir is no cache at all.
    explicit Cache(std::string dir) : dir_(std::move(dir)) {}

    [[nodiscard]] bool enabled() const noexcept { return !dir_.empty(); }

    /// Puts the target's outputs (and its depfile) in place from an entry
    /// that matches, true when it did.
    bool restore(const Target& t, std::uint64_t command_hash) {
        if (!enabled() || t.depfile.empty() || t.outputs.empty()) return false;
        const auto key = key_of(t, command_hash);
        if (!key) return false;
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::path(dir_) / key->substr(0, 2) / *key;
        if (!std::filesystem::is_directory(base, ec)) return false;
        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            if (!entry.is_directory(ec) || entry.path().filename().string().starts_with(".")) continue;
            if (!matches(entry.path())) continue;
            // Every output first, then the depfile: a copy that fails half
            // way leaves an output missing, which the next build rebuilds.
            bool whole = true;
            for (std::size_t i = 0; i < t.outputs.size() && whole; ++i) {
                const std::filesystem::path out = t.outputs[i];
                if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
                whole = std::filesystem::copy_file(entry.path() / std::format("out{}", i), out,
                                                   std::filesystem::copy_options::overwrite_existing, ec);
            }
            if (whole) {
                const std::filesystem::path d = t.depfile;
                if (d.has_parent_path()) std::filesystem::create_directories(d.parent_path(), ec);
                whole = std::filesystem::copy_file(entry.path() / "depfile", d, std::filesystem::copy_options::overwrite_existing, ec);
            }
            if (whole) return true;
        }
        return false;
    }

    /// Keeps what a target just made, with what it read (its depfile).
    void store(const Target& t, std::uint64_t command_hash, const std::vector<std::string>& read) {
        if (!enabled() || t.depfile.empty() || t.outputs.empty()) return;
        const auto key = key_of(t, command_hash);
        if (!key) return;
        // What it read, and each file's sum: the entry's manifest and name.
        std::string manifest;
        for (const std::string& f : read) {
            const auto sum = file_sum(f);
            if (!sum) return;  // a file it read is gone already: keep nothing
            manifest += *sum + " " + f + "\n";
        }
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::path(dir_) / key->substr(0, 2) / *key;
        const std::filesystem::path entry = base / sha256_hex(manifest).substr(0, 16);
        if (std::filesystem::is_directory(entry, ec)) return;  // kept already
        const std::filesystem::path tmp =
            base / std::format(".tmp-{}-{}", sha256_hex(manifest).substr(0, 8), std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(tmp, ec);
        if (ec) return;
        bool whole = true;
        for (std::size_t i = 0; i < t.outputs.size() && whole; ++i)
            whole = std::filesystem::copy_file(t.outputs[i], tmp / std::format("out{}", i), ec);
        whole = whole && std::filesystem::copy_file(t.depfile, tmp / "depfile", ec);
        if (whole) {
            std::ofstream(tmp / "read", std::ios::binary) << manifest;
            std::filesystem::rename(tmp, entry, ec);  // another build's, kept first: fine
        }
        std::filesystem::remove_all(tmp, ec);
    }

    /// A target made these (or put them back): their sums are new.
    void forget(const std::vector<std::string>& paths) {
        for (const std::string& p : paths) sums_.erase(p);
    }

private:
    /// The command's hash and each input's contents: what the outputs are
    /// made from, before the headers it reads.
    std::optional<std::string> key_of(const Target& t, std::uint64_t command_hash) {
        std::string text = std::format("{:016x}", command_hash);
        for (const std::string& in : t.inputs) {
            const auto sum = file_sum(in);
            if (!sum) return std::nullopt;
            text += "\n" + in + " " + *sum;
        }
        return sha256_hex(text);
    }

    /// Whether every file an entry read still has the sum it had.
    bool matches(const std::filesystem::path& entry) {
        std::ifstream in(entry / "read", std::ios::binary);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            const auto space = line.find(' ');
            if (space == std::string::npos) return false;
            const auto sum = file_sum(line.substr(space + 1));
            if (!sum || *sum != line.substr(0, space)) return false;
        }
        return true;
    }

    /// A file's sha256, once a build (headers are read by many compiles).
    std::optional<std::string> file_sum(const std::string& path) {
        if (auto it = sums_.find(path); it != sums_.end()) return it->second;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        return sums_[path] = sha256_hex(text);
    }

    std::string dir_;
    std::map<std::string, std::string> sums_;
};

/// Where the cache is: $SQMAKE_CACHE ("off": none), else $PREFIX/var/cache/
/// sqmake (sqterm's), else ~/.cache/sqmake.
inline std::string cache_dir() {
    if (const char* c = std::getenv("SQMAKE_CACHE"); c != nullptr && *c != '\0')
        return std::string_view(c) == "off" ? std::string() : std::string(c);
    if (const char* p = std::getenv("PREFIX"); p != nullptr && *p != '\0') return std::string(p) + "/var/cache/sqmake";
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') return std::string(h) + "/.cache/sqmake";
    return {};
}

}  // namespace sqmake
