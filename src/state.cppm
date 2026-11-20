// SPDX-License-Identifier: MIT
//
// sqmake.state: what sqmake learns between builds (docs/spec.md §2.2), in
// one small file in the build folder: for each target that last built
// cleanly, its command's hash and the inputs its dependency file named.
//
//   sqmake state 1
//   <hash, 16 hex digits> <output>\t<input>\t<input>...
//
// A target that fails, or never ran, has no line: it builds next time.
// Read whole, written whole (to a part file renamed into place).

export module sqmake.state;

import std;

export namespace sqmake {

struct Record {
    std::uint64_t hash = 0;
    std::vector<std::string> deps;
};

class State {
public:
    static State load(const std::filesystem::path& file) {
        State s;
        s.file_ = file;
        std::ifstream in(file);
        std::string line;
        if (!std::getline(in, line) || line != "sqmake state 1") return s;
        while (std::getline(in, line)) {
            const std::size_t space = line.find(' ');
            if (space != 16) continue;
            Record r;
            if (std::from_chars(line.data(), line.data() + 16, r.hash, 16).ec != std::errc()) continue;
            std::vector<std::string> fields;
            for (auto part : std::string_view(line).substr(17) | std::views::split('\t')) fields.emplace_back(part.begin(), part.end());
            if (fields.empty()) continue;
            r.deps.assign(fields.begin() + 1, fields.end());
            s.records_[fields[0]] = std::move(r);
        }
        return s;
    }

    const Record* find(const std::string& output) const {
        auto it = records_.find(output);
        return it == records_.end() ? nullptr : &it->second;
    }
    void forget(const std::string& output) { records_.erase(output); }
    void set(const std::string& output, Record r) { records_[output] = std::move(r); }

    bool save() const {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        const auto part = file_.string() + ".part";
        {
            std::ofstream out(part, std::ios::trunc);
            if (!out) return false;
            out << "sqmake state 1\n";
            for (const auto& [output, r] : records_) {
                out << std::format("{:016x} {}", r.hash, output);
                for (const std::string& d : r.deps) out << '\t' << d;
                out << '\n';
            }
            if (!out) return false;
        }
        std::filesystem::rename(part, file_, ec);
        return !ec;
    }

private:
    std::filesystem::path file_;
    std::map<std::string, Record> records_;
};

/// The inputs a dependency file (make syntax, as clang -MMD writes it)
/// names: everything after the first unescaped colon, "\ " a space,
/// backslash-newline a continuation.
inline std::optional<std::vector<std::string>> read_depfile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::string> deps;
    std::string word;
    bool after_colon = false;
    auto flush = [&] {
        if (!word.empty() && after_colon) deps.push_back(word);
        word.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char n = text[i + 1];
            if (n == '\n' || (n == '\r' && i + 2 < text.size() && text[i + 2] == '\n')) {
                flush();
                i += n == '\n' ? 1 : 2;
                continue;
            }
            if (n == ' ' || n == '#' || n == '\\') {
                word += n;
                ++i;
                continue;
            }
        }
        if (c == ':' && !after_colon && (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\n' || text[i + 1] == '\t')) {
            word.clear();
            after_colon = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            flush();
            continue;
        }
        if (c == '\n') {
            flush();
            // A second rule (-MP's empty rules for each header): its target
            // was already listed as an input.
            break;
        }
        word += c;
    }
    flush();
    return deps;
}

}  // namespace sqmake
