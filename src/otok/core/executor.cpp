// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/executor.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <string>

namespace otok {

namespace {

using namespace std::chrono_literals;

// How long tokens lie unused in a slice's jobserver before they go back, and how long the slice is then not
// offered another.
constexpr auto idle_grace = 200ms;
constexpr auto probe_pause = 1s;

// The task running on this thread: its broker, its priority, whether it holds its job.
struct running_task
{
    job_broker* broker = nullptr;
    job_priority priority;
    bool holding = false;
    std::function<void(bool, std::string const&)> notice;   // on_job_wait
};

thread_local running_task* current_task = nullptr;

// Takes a job, however long it waits, telling that it does when one is not free at once.
void take_again(running_task& t)
{
    if (!t.broker->take(t.priority, 0ms)) {
        if (t.notice) t.notice(true, {});
        while (!t.broker->take(t.priority, 50ms)) {}
        if (t.notice) t.notice(false, {});
    }
    t.holding = true;
}

}

job_broker::job_broker(std::int64_t jobs, std::filesystem::path dir, bool ticking)
    : jobs_{std::max<std::int64_t>(jobs, 1)}, dir_{std::move(dir)}, free_{jobs_}
{
    if (!ticking) return;
    ticker_ = std::thread{[this] {
        std::unique_lock lock{m_};
        while (!changed_.wait_for(lock, 50ms, [this] { return stop_; })) {
            lock.unlock();
            tick();
            lock.lock();
        }
    }};
}

job_broker::~job_broker()
{
    {
        std::lock_guard lock{m_};
        stop_ = true;
    }
    changed_.notify_all();
    if (ticker_.joinable()) ticker_.join();
}

bool job_broker::waiter_first(job_priority p) const
{
    return std::ranges::none_of(waiting_, [&](job_priority const& w) { return w.before(p); });
}

bool job_broker::wanted_before_locked(job_priority p) const
{
    return !waiter_first(p) || std::ranges::any_of(leases_, [&](lease const* l) { return l->wants && l->priority_.before(p); });
}

bool job_broker::wanted_before(job_priority p)
{
    std::lock_guard lock{m_};
    return wanted_before_locked(p);
}

bool job_broker::take(job_priority p, std::chrono::milliseconds timeout)
{
    std::unique_lock lock{m_};
    waiting_.push_back(p);
    bool const got = changed_.wait_for(lock, timeout, [&] {
        // the first of the waiting tasks, and no slice before it using all its tokens
        return free_ > 0 && waiter_first(p)
            && std::ranges::none_of(leases_, [&](lease const* l) { return l->wants && l->priority_.before(p); });
    });
    waiting_.erase(std::ranges::find_if(waiting_, [&](job_priority const& w) { return w.weight == p.weight && w.order == p.order; }));
    if (got) --free_;
    lock.unlock();
    changed_.notify_all();  // the next waiting task may be first now
    return got;
}

void job_broker::give()
{
    {
        std::lock_guard lock{m_};
        ++free_;
    }
    changed_.notify_all();
}

std::uint64_t job_broker::next_order()
{
    std::lock_guard lock{m_};
    return ++orders_;
}

std::int64_t job_broker::free_jobs()
{
    std::lock_guard lock{m_};
    return free_;
}

result<std::unique_ptr<job_broker::lease>> job_broker::attach(job_priority p)
{
    std::string name;
    {
        std::lock_guard lock{m_};
        name = "jobserver-" + std::to_string(++names_);
    }
    // clients may run every job; the broker decides how many they get
    auto server = jobserver::create(jobs_, 0, dir_, name);
    if (!server) return std::unexpected{server.error()};
    std::unique_ptr<lease> l{new lease{*this, std::move(*server), p}};
    std::lock_guard lock{m_};
    leases_.push_back(l.get());
    return l;
}

void job_broker::detach(lease& l)
{
    {
        std::lock_guard lock{m_};
        // its clients are gone: whatever it was given is free again
        free_ += l.given;
        std::erase(leases_, &l);
    }
    changed_.notify_all();
}

job_broker::lease::~lease()
{
    broker_.detach(*this);
}

std::int64_t job_broker::lease::hold(std::int64_t most)
{
    std::lock_guard lock{broker_.m_};
    std::int64_t n = 0;
    while (n < most && broker_.free_ > 0 && !broker_.wanted_before_locked(priority_)) {
        --broker_.free_;
        ++n;
    }
    given += n;
    held += n;
    return n;
}

void job_broker::lease::put_back(std::int64_t n)
{
    {
        std::lock_guard lock{broker_.m_};
        broker_.free_ += n;
        given -= n;
        held -= n;
    }
    broker_.changed_.notify_all();
}

void job_broker::lease::set_running(bool now, bool with_clients)
{
    std::lock_guard lock{broker_.m_};
    running = now;
    clients = now && with_clients;
    if (now) return;
    // its clients are gone: the tokens they gave back are free at once
    wants = false;
    while (server_.try_acquire(std::chrono::milliseconds{0})) {
        --given;
        ++broker_.free_;
    }
    idle_since = {};
    broker_.changed_.notify_all();
}

std::int64_t job_broker::lease::in_use()
{
    std::lock_guard lock{broker_.m_};
    return given - idle;
}

void job_broker::tick()
{
    {
        std::lock_guard lock{m_};
        auto const now = std::chrono::steady_clock::now();
        for (lease* l : leases_) {
            // the tokens lying in its jobserver; its clients cannot take them meanwhile, they wait
            l->idle = 0;
            while (l->server_.try_acquire(0ms)) ++l->idle;
            if (l->idle == 0) {
                l->idle_since = {};
            } else if (l->idle_since == std::chrono::steady_clock::time_point{}) {
                l->idle_since = now;
            }
            if (l->idle > 0 && now - l->idle_since >= idle_grace) {
                free_ += l->idle;
                l->given -= l->idle;
                l->idle = 0;
                l->idle_since = {};
                l->probe_after = now + probe_pause;
            }
            l->wants = l->running && l->clients && l->idle == 0 && l->held == 0 && now >= l->probe_after;
        }
        std::vector<lease*> order = leases_;
        std::ranges::sort(order, [](lease const* a, lease const* b) { return a->priority_.before(b->priority_); });
        // a token from those after it, which they gave back as a compilation ended
        auto const from_after = [&](job_priority p) -> lease* {
            for (auto it = order.rbegin(); it != order.rend() && p.before((*it)->priority_); ++it) {
                if ((*it)->idle > 0) return *it;
            }
            return nullptr;
        };
        for (lease* l : order) {
            if (!l->wants || !waiter_first(l->priority_)) continue;  // a task before it takes the free job in take()
            if (free_ > 0) {
                --free_;
            } else if (lease* other = from_after(l->priority_)) {
                --other->idle;
                --other->given;
            } else {
                continue;
            }
            ++l->given;
            ++l->idle;
        }
        // a task waiting for a job while none is free: the next token of a slice after it
        if (free_ == 0 && !waiting_.empty()) {
            job_priority first = waiting_.front();
            for (job_priority const& w : waiting_) {
                if (w.before(first)) first = w;
            }
            if (lease* other = from_after(first)) {
                --other->idle;
                --other->given;
                ++free_;
            }
        }
        for (lease* l : leases_) {
            for (std::int64_t i = 0; i < l->idle; ++i) l->server_.release();
        }
    }
    changed_.notify_all();
}

lent_job::lent_job(std::string const& what)
{
    if (!current_task || !current_task->holding) return;
    current_task->broker->give();
    current_task->holding = false;
    lent_ = true;
    if (current_task->notice) current_task->notice(true, what);
}

lent_job::~lent_job()
{
    if (!lent_) return;
    if (current_task->notice) current_task->notice(false, {});
    take_again(*current_task);
}

void job_boundary()
{
    running_task* t = current_task;
    if (!t || !t->holding || !t->broker->wanted_before(t->priority)) return;
    t->broker->give();
    t->holding = false;
    take_again(*t);
}

void on_job_wait(std::function<void(bool waits, std::string const& what)> notice)
{
    if (current_task) current_task->notice = std::move(notice);
}

job_broker* current_broker() noexcept
{
    return current_task ? current_task->broker : nullptr;
}

job_priority current_priority() noexcept
{
    return current_task ? current_task->priority : job_priority{};
}

std::vector<error> run_tasks(std::span<executor_task const> tasks, job_broker& broker, std::size_t most)
{
    std::mutex m;
    std::condition_variable changed;
    std::vector<std::size_t> waiting(tasks.size());     // unfinished tasks each one needs
    std::vector<std::vector<std::size_t>> dependents(tasks.size());
    std::vector<job_priority> priority(tasks.size());
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        waiting[i] = tasks[i].after.size();
        for (std::size_t a : tasks[i].after) dependents[a].push_back(i);
    }
    // the weight of a task: every task after it, however far
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        std::set<std::size_t> after;
        std::vector<std::size_t> next = dependents[i];
        while (!next.empty()) {
            std::size_t const d = next.back();
            next.pop_back();
            if (after.insert(d).second) next.insert(next.end(), dependents[d].begin(), dependents[d].end());
        }
        priority[i].weight = after.size();
    }
    // with `m` held
    auto const make_ready = [&](std::size_t i) {
        priority[i].order = broker.next_order();
        ready.push_back(i);
    };
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (waiting[i] == 0) make_ready(i);
    }
    std::size_t running = 0;
    bool failed = false;
    std::vector<error> errors;
    std::vector<std::thread> threads;

    // with `m` held
    auto const start = [&](std::size_t i) {
        std::erase(ready, i);
        std::size_t const now = ++running;
        threads.emplace_back([&, i, now] {
            running_task task{&broker, priority[i], true, {}};
            current_task = &task;
            result<void> r;
            try {
                r = tasks[i].run(now);
            } catch (std::exception const& e) {
                r = fail(e.what());
            }
            current_task = nullptr;
            {
                std::lock_guard lock{m};
                --running;
                if (!r) {
                    failed = true;
                    errors.push_back(r.error());
                } else {
                    for (std::size_t d : dependents[i]) {
                        if (--waiting[d] == 0) make_ready(d);
                    }
                }
                changed.notify_all();
            }
            // after the task is counted out, so that the next one cannot start before it seems to end
            if (task.holding) broker.give();
        });
    };

    std::unique_lock lock{m};
    for (;;) {
        if (failed || ready.empty() || running >= most) {
            if (running == 0) break;
            changed.wait(lock);
            continue;
        }
        std::size_t const first = *std::ranges::min_element(ready, [&](std::size_t a, std::size_t b) { return priority[a].before(priority[b]); });
        // a job, or a running task ending meanwhile
        lock.unlock();
        bool const got = broker.take(priority[first], 50ms);
        lock.lock();
        if (!got) continue;
        if (failed) {
            broker.give();
            continue;
        }
        start(first);
    }
    lock.unlock();
    for (std::thread& t : threads) t.join();
    return errors;
}

}
