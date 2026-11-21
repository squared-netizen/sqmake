// SPDX-License-Identifier: MIT
//
// sqmake.run: checking and running a plan (docs/spec.md §2.2, §6.1).
//
// A target is out of date when an output is missing; its command's hash
// differs from the last clean build's; an input, or a header its last
// compile read (its dependency file), is newer than its oldest output; or
// a target it is made from ran in this build. An input no target makes
// and that does not exist stops the build before anything runs.
//
// Commands run without a shell (posix_spawnp), up to `jobs` at once, each
// with its output collected and printed whole when it ends, so two never
// mix. The first failure stops new commands; those running finish. The
// state is saved as targets finish, so an interrupted build loses nothing
// it finished.

module;

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;

export module sqmake.run;

import std;
import sqmake.graph;
import sqmake.state;

export namespace sqmake {

struct RunOptions {
    std::size_t jobs = 1;
    bool explain = false;  // print the plan and why, run nothing
    bool quiet = false;    // no progress lines
    std::string extra_hash;  // the toolchain's fingerprint, in every hash
    std::FILE* out = stdout;  // progress and commands' output (stderr under `call`)
};

struct RunResult {
    std::size_t ran = 0, failed = 0, up_to_date = 0;
};

/// One command's exit status and everything it wrote.
struct Ran {
    int status = -1;
    std::string output;
};

/// Runs argv without a shell, its stdout and stderr together into `output`.
inline Ran run_command(const std::vector<std::string>& argv) {
    Ran r;
    int fds[2];
    if (::pipe(fds) != 0) {
        r.output = "sqmake: cannot make a pipe\n";
        return r;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    const int err = ::posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (err != 0) {
        ::close(fds[0]);
        r.output = std::format("sqmake: cannot run {}: {}\n", argv[0], std::strerror(err));
        return r;
    }
    std::array<char, 8192> buf{};
    for (;;) {
        const ssize_t n = ::read(fds[0], buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        r.output.append(buf.data(), static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return r;
}

class Runner {
public:
    Runner(const Graph& graph, State& state, RunOptions options) : graph_(graph), state_(state), o_(std::move(options)) {}

    /// Builds the planned targets, in order where they depend on each other.
    std::expected<RunResult, std::string> run(const std::vector<std::size_t>& plan) {
        RunResult result;
        // Inputs no target makes must exist before anything runs.
        for (std::size_t i : plan) {
            for (const std::string& in : graph_.at(i).inputs) {
                if (!graph_.maker(in) && !std::filesystem::exists(in))
                    return std::unexpected(std::format("{} needs {}, which does not exist and no target makes", graph_.at(i).outputs[0], in));
            }
        }
        std::map<std::size_t, bool> ran;  // targets that ran (or are known dirty) this build
        std::set<std::size_t> planned(plan.begin(), plan.end());
        std::map<std::size_t, std::string> why;
        // Waiting on: the targets each one's inputs come from.
        std::map<std::size_t, std::set<std::size_t>> waits;
        for (std::size_t i : plan)
            for (const std::string& in : graph_.at(i).inputs)
                if (auto m = graph_.maker(in); m && planned.contains(*m)) waits[i].insert(*m);

        std::deque<std::size_t> pending(plan.begin(), plan.end());
        std::size_t running = 0;
        bool stopped = false;
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::pair<std::size_t, Ran>> finished;
        std::vector<std::jthread> threads;
        const std::size_t total = std::ranges::count_if(plan, [&](std::size_t i) { return !graph_.at(i).command.argv.empty(); });
        std::size_t counter = 0;

        auto ready = [&](std::size_t i) {
            for (std::size_t w : waits[i])
                if (!done_.contains(w)) return false;
            return true;
        };
        auto settle = [&](std::size_t i, const Ran& r) {
            const Target& t = graph_.at(i);
            if (r.status != 0) {
                ++result.failed;
                stopped = true;
                std::print(o_.out, "{}", r.output);
                std::println(o_.out, "sqmake: {} failed (status {}): {}", t.outputs[0], r.status, quoted(t.command.argv));
                return;
            }
            if (!r.output.empty()) std::print(o_.out, "{}", r.output);
            Record rec{command_hash(t, o_.extra_hash), {}};
            if (!t.depfile.empty())
                if (auto deps = read_depfile(t.depfile)) rec.deps = *deps;
            state_.set(t.outputs[0], std::move(rec));
            ++result.ran;
            done_.insert(i);
            ran[i] = true;
            state_.save();
        };

        while (!pending.empty() || running > 0) {
            // Launch what is ready.
            for (auto it = pending.begin(); !stopped && it != pending.end() && running < std::max<std::size_t>(o_.jobs, 1);) {
                const std::size_t i = *it;
                if (!ready(i)) {
                    ++it;
                    continue;
                }
                it = pending.erase(it);
                const Target& t = graph_.at(i);
                const std::string reason = out_of_date(i, ran);
                if (reason.empty()) {
                    ++result.up_to_date;
                    done_.insert(i);
                    continue;
                }
                if (t.command.argv.empty()) {  // an alias: its inputs are its work
                    done_.insert(i);
                    ran[i] = true;
                    continue;
                }
                ++counter;
                if (o_.explain) {
                    ++result.ran;  // with explain: what would run
                    std::println(o_.out, "[{}/{}] {}\n    because {}\n    {}", counter, total, t.description.empty() ? t.outputs[0] : t.description, reason,
                                 quoted(t.command.argv));
                    for (const auto& next : t.command.then) std::println(o_.out, "    {}", quoted(next));
                    done_.insert(i);
                    ran[i] = true;
                    continue;
                }
                if (!o_.quiet) std::println(o_.out, "[{}/{}] {}", counter, total, t.description.empty() ? t.outputs[0] : t.description);
                state_.forget(t.outputs[0]);
                std::error_code ec;
                for (const std::string& out : t.outputs)
                    if (auto parent = std::filesystem::path(out).parent_path(); !parent.empty()) std::filesystem::create_directories(parent, ec);
                ++running;
                threads.emplace_back([&, i] {
                    const Command& c = graph_.at(i).command;
                    Ran r = run_command(c.argv);
                    for (const auto& next : c.then) {
                        if (r.status != 0) break;
                        Ran more = run_command(next);
                        r.status = more.status;
                        r.output += more.output;
                    }
                    std::lock_guard lock(mu);
                    finished.emplace_back(i, std::move(r));
                    cv.notify_one();
                });
            }
            if (running == 0) {
                if (stopped || pending.empty()) break;
                // Nothing running and nothing ready: only a failure upstream does this.
                break;
            }
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return !finished.empty(); });
            auto batch = std::move(finished);
            finished.clear();
            lock.unlock();
            for (auto& [i, r] : batch) {
                --running;
                settle(i, r);
            }
        }
        state_.save();
        return result;
    }

private:
    /// Why target i must be built, or "" when it is up to date.
    std::string out_of_date(std::size_t i, const std::map<std::size_t, bool>& ran) const {
        const Target& t = graph_.at(i);
        if (t.command.argv.empty()) {
            for (const std::string& in : t.inputs)
                if (auto m = graph_.maker(in); m && ran.contains(*m)) return "an input was built";
            return "";
        }
        std::error_code ec;
        std::filesystem::file_time_type oldest = std::filesystem::file_time_type::max();
        for (const std::string& out : t.outputs) {
            if (!std::filesystem::exists(out, ec)) return std::format("{} does not exist", out);
            oldest = std::min(oldest, std::filesystem::last_write_time(out, ec));
        }
        const Record* rec = state_.find(t.outputs[0]);
        if (!rec) return "it has not been built cleanly before";
        if (rec->hash != command_hash(t, o_.extra_hash)) return "its command changed";
        for (const std::string& in : t.inputs) {
            if (auto m = graph_.maker(in); m && ran.contains(*m)) return std::format("{} was built", in);
            const auto when = std::filesystem::last_write_time(in, ec);
            if (ec) return std::format("{} cannot be read", in);
            if (when > oldest) return std::format("{} is newer", in);
        }
        for (const std::string& d : rec->deps) {
            const auto when = std::filesystem::last_write_time(d, ec);
            if (ec) return std::format("{}, which it read, is gone", d);
            if (when > oldest) return std::format("{}, which it read, is newer", d);
        }
        return "";
    }

    const Graph& graph_;
    State& state_;
    RunOptions o_;
    std::set<std::size_t> done_;
};

}  // namespace sqmake
