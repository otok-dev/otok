// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/core/jobserver.hpp"

// Running tasks with dependencies concurrently (docs/PLAN.md decisions 10, 20, 61): each on a thread of its
// own, with the jobs of a broker, which gives them in the order of the tasks' priority.
namespace otok {

// Before another: more tasks after it (what waits for it), then the earlier.
struct job_priority
{
    std::size_t weight = 0;
    std::uint64_t order = std::numeric_limits<std::uint64_t>::max();

    bool before(job_priority const& other) const noexcept
    {
        return weight != other.weight ? weight > other.weight : order < other.order;
    }
};

// The jobs of the machine (docs/PLAN.md decision 61): `jobs` of them, given to tasks and to the build tools they run.
// - A task holds one job while it runs; between its commands it gives it back when a task before it wants one
//   (job_boundary), so the jobs of tasks that wait or configure go to the one first.
// - Each slice has a jobserver of its own (a lease), which the broker fills, a token at a time, while all its tokens
//   are in use, and empties of the tokens that lie unused in it, for 200 ms; when no job is free, a slice before others
//   takes the tokens they give back as their compilations end.
// - A task starts when a job is free and nothing before it wants one.
// Executors sharing a broker (the builds of `otok publish`) run as many jobs together as one would.
class job_broker
{
public:
    // A slice's jobserver, filled by the broker; its tokens go back when it is destroyed.
    class lease
    {
    public:
        ~lease();
        lease(lease const&) = delete;
        lease& operator=(lease const&) = delete;

        jobserver& server() noexcept { return server_; }

        // Jobs for a tool without the jobserver, which runs a fixed number: up to `most` besides its own, as many as
        // nothing before it wants, but at least half the jobs with its own (or `most`): until as many are free, and
        // nothing before it wants one, the task's job goes to others and it waits, keeping none for itself
        // (docs/PLAN.md decision 123). Give them back with put_back().
        std::int64_t hold(std::int64_t most);
        void put_back(std::int64_t held);

        // While a command of the slice runs: only then can it want tokens, and only when it has the jobserver (`clients`:
        // MAKEFLAGS in its environment); a tool without, which runs a fixed number of jobs, never takes them.
        void set_running(bool running, bool clients = true);

        // The tokens its clients have, at the last look: the jobs it runs besides the one of its task.
        std::int64_t in_use();

    private:
        friend class job_broker;
        lease(job_broker& broker, jobserver server, job_priority priority) : broker_{broker}, server_{std::move(server)}, priority_{priority} {}

        job_broker& broker_;
        jobserver server_;
        job_priority priority_;
        std::int64_t given = 0;         // its tokens: in its jobserver or taken by its clients
        std::int64_t idle = 0;          // in its jobserver, at the last look
        std::int64_t held = 0;          // of `given`, held for a tool without the jobserver
        bool wants = false;             // every token in use at the last look
        bool running = false;           // a command of the slice runs
        bool clients = false;           // and it has the jobserver
        std::chrono::steady_clock::time_point idle_since{};
        std::chrono::steady_clock::time_point probe_after{};
    };

    // `dir`: where the fifos of slices are (unused on Windows). `ticking`: a thread fills and empties the
    // jobservers of the leases twenty times a second; tests call tick() themselves.
    job_broker(std::int64_t jobs, std::filesystem::path dir, bool ticking = true);
    ~job_broker();
    job_broker(job_broker const&) = delete;
    job_broker& operator=(job_broker const&) = delete;

    std::int64_t jobs() const noexcept { return jobs_; }

    // A job for a task of priority `p`, once it is free and nothing before it wants one; false after `timeout`.
    bool take(job_priority p, std::chrono::milliseconds timeout);
    void give();

    // Whether anything before `p` waits for a job: a task, or a slice whose tokens are all in use.
    bool wanted_before(job_priority p);

    // A jobserver for a slice of priority `p`, empty at first.
    result<std::unique_ptr<lease>> attach(job_priority p);

    // An order for a task that becomes ready: those ready earlier come first.
    std::uint64_t next_order();

    // One look at the leases: their unused tokens back, tokens to those using all theirs.
    void tick();

    // The jobs not given, for tests.
    std::int64_t free_jobs();

private:
    void detach(lease& l);
    bool wanted_before_locked(job_priority p) const;
    bool waiter_first(job_priority p) const;   // with m_ held: whether `p` is first of the waiting tasks

    std::int64_t jobs_;
    std::filesystem::path dir_;
    std::mutex m_;
    std::condition_variable changed_;
    std::int64_t free_;
    std::vector<lease*> leases_;
    std::vector<job_priority> waiting_;     // the tasks in take()
    std::uint64_t orders_ = 0;
    std::uint64_t names_ = 0;
    bool stop_ = false;
    std::thread ticker_;
};

// While a task waits for another one (an entry of the cache that another build creates), its job runs others:
// constructed, it gives the job of the task on this thread back, telling that the task waits for `what`; destroyed,
// it takes one again, waiting for it. Outside a task it does nothing.
class lent_job
{
public:
    explicit lent_job(std::string const& what);
    ~lent_job();
    lent_job(lent_job const&) = delete;
    lent_job& operator=(lent_job const&) = delete;

private:
    bool lent_ = false;
};

// Between two commands of the task on this thread: its job goes to a task or slice before it that wants one, and
// it waits for one again (docs/PLAN.md decision 61). Outside a task it does nothing.
void job_boundary();

// Told when the task on this thread starts and stops waiting, to show it: for a job (job_boundary; `what` empty) or for
// what a lent_job names. Empty: nothing. Outside a task it does nothing.
void on_job_wait(std::function<void(bool waits, std::string const& what)> notice);

// The broker and the priority of the task on this thread; nothing outside a task.
job_broker* current_broker() noexcept;
job_priority current_priority() noexcept;

struct executor_task
{
    std::vector<std::size_t> after;     // indices of the tasks this one needs
    // `running`: the number of tasks running when this one starts, itself included.
    std::function<result<void>(std::size_t running)> run;
};

// Runs every task after those it needs, each with a job of `broker`, those that more tasks wait for first; at most
// `most` at once. After a failure no further task starts; the running ones finish. Returns the errors of the
// failed tasks, in the order they failed; tasks that never started are not reported.
std::vector<error> run_tasks(std::span<executor_task const> tasks, job_broker& broker,
    std::size_t most = std::numeric_limits<std::size_t>::max());

}
