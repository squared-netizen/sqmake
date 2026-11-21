// SPDX-License-Identifier: MIT
//
// sqmake.graph: what a build is (docs/spec.md §2.1, §6.1).
//
// A target is the files one command makes from its inputs. Inputs are
// files, or the outputs of other targets; a command is an argument list,
// run without a shell. A target with no command is an alias: building it
// builds its inputs. An action is a named command run whenever asked for
// (install, run): what it does is the build file's, not sqmake's. The plan is every target a goal needs, inputs before
// what is made from them; a cycle is refused, naming it.

export module sqmake.graph;

import std;

export namespace sqmake {

struct Command {
    std::vector<std::string> argv;  // argv[0] found on PATH; empty: an alias
    /// More commands after argv, in order, each run only when the one
    /// before it succeeded (an APK linked, then a library added to it).
    std::vector<std::vector<std::string>> then;
};

struct Target {
    std::vector<std::string> outputs;  // the first names the target
    std::vector<std::string> inputs;
    Command command;
    std::string depfile;      // written by the command (-MMD -MF): more inputs
    std::string description;  // what the progress line says: "cxx src/a.cpp"
    /// An action (install, run): a name, not a file. Its command runs each
    /// time a goal needs it, attached to the terminal, after the rest of the
    /// plan has finished; it is never up to date and leaves no state.
    bool action = false;
};

class Graph {
public:
    /// Adds a target; an output another target already makes is refused.
    std::expected<std::size_t, std::string> add(Target t) {
        if (t.outputs.empty()) return std::unexpected("a target with no output");
        for (const std::string& o : t.outputs)
            if (by_output_.contains(o)) return std::unexpected(std::format("{} is made by two targets", o));
        const std::size_t index = targets_.size();
        for (const std::string& o : t.outputs) by_output_.emplace(o, index);
        targets_.push_back(std::move(t));
        return index;
    }

    const std::vector<Target>& targets() const { return targets_; }
    const Target& at(std::size_t i) const { return targets_[i]; }

    /// The target that makes `path`, if one does.
    std::optional<std::size_t> maker(const std::string& path) const {
        if (auto it = by_output_.find(path); it != by_output_.end()) return it->second;
        return std::nullopt;
    }

    /// Every target the goals need, each after the targets making its
    /// inputs. A goal is an output (or an alias's name).
    std::expected<std::vector<std::size_t>, std::string> plan(const std::vector<std::string>& goals) const {
        std::vector<std::size_t> order;
        std::vector<int> mark(targets_.size(), 0);  // 0 unseen, 1 on the path, 2 done
        std::vector<std::size_t> path;
        std::function<std::expected<void, std::string>(std::size_t)> visit = [&](std::size_t i) -> std::expected<void, std::string> {
            if (mark[i] == 2) return {};
            if (mark[i] == 1) {
                std::string cycle;
                auto from = std::ranges::find(path, i);
                for (auto it = from; it != path.end(); ++it) cycle += targets_[*it].outputs[0] + " -> ";
                return std::unexpected("a cycle: " + cycle + targets_[i].outputs[0]);
            }
            mark[i] = 1;
            path.push_back(i);
            for (const std::string& in : targets_[i].inputs)
                if (auto m = maker(in))
                    if (auto r = visit(*m); !r) return r;
            path.pop_back();
            mark[i] = 2;
            order.push_back(i);
            return {};
        };
        for (const std::string& g : goals) {
            auto m = maker(g);
            if (!m) return std::unexpected(std::format("no target makes {}", g));
            if (auto r = visit(*m); !r) return std::unexpected(r.error());
        }
        return order;
    }

private:
    std::vector<Target> targets_;
    std::unordered_map<std::string, std::size_t> by_output_;
};

/// A stable hash of a command (FNV-1a, 64 bits): its argument list and its
/// dependency file's name. A command that changes rebuilds what it makes.
inline std::uint64_t command_hash(const Target& t, std::string_view extra = {}) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](std::string_view s) {
        for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ULL;
        h = (h ^ 0xff) * 0x100000001b3ULL;  // a separator no argument holds
    };
    for (const std::string& a : t.command.argv) mix(a);
    for (const auto& next : t.command.then) {
        mix("\x01then");
        for (const std::string& a : next) mix(a);
    }
    mix(t.depfile);
    mix(extra);
    return h;
}

/// An argument list as a shell would need it typed, for --explain.
inline std::string quoted(const std::vector<std::string>& argv) {
    std::string out;
    for (const std::string& a : argv) {
        if (!out.empty()) out += ' ';
        const bool plain = !a.empty() && std::ranges::all_of(a, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("-_./=:,+@%").contains(c);
        });
        if (plain) {
            out += a;
            continue;
        }
        out += '\'';
        for (char c : a) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        out += '\'';
    }
    return out;
}

}  // namespace sqmake
