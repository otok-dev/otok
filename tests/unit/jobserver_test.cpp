// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The jobserver and the executor (docs/PLAN.md decisions 10, 20).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "otok/core/executor.hpp"
#include "otok/core/file_lock.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/jobserver.hpp"
#include "otok/core/process.hpp"
#include "test_util.hpp"

using namespace otok;

using namespace std::chrono_literals;

TEST(jobserver, holds_jobs_minus_one_tokens)
{
    test::temp_dir dir;
    auto js = jobserver::create(3, dir.path());
    ASSERT_TRUE(js) << js.error().message;
    EXPECT_TRUE(js->try_acquire(10ms));
    EXPECT_TRUE(js->try_acquire(10ms));
    EXPECT_FALSE(js->try_acquire(10ms));
    js->release();
    EXPECT_TRUE(js->try_acquire(10ms));

    test::temp_dir other;
    auto single = jobserver::create(1, other.path());
    ASSERT_TRUE(single) << single.error().message;
    EXPECT_FALSE(single->try_acquire(1ms));
}

TEST(jobserver, describes_itself_in_makeflags)
{
    test::temp_dir dir;
    auto js = jobserver::create(4, dir.path());
    ASSERT_TRUE(js) << js.error().message;
#ifdef _WIN32
    EXPECT_EQ(js->makeflags().find("-j4 --jobserver-auth=otok-"), 0u) << js->makeflags();
#else
    EXPECT_EQ(js->makeflags(), "-j4 --jobserver-auth=fifo:" + (dir.path() / "jobserver").string());
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "jobserver"));
    test::temp_dir other;
    js = jobserver::create(1, other.path());  // the old one is gone with its fifo
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "jobserver"));
#endif
}

TEST(jobserver, launcher_runs_with_the_slot_or_a_token)
{
    test::temp_dir dir;
    auto js = jobserver::create(2, dir.path());  // one token in the pool
    ASSERT_TRUE(js) << js.error().message;
    std::string const auth = js->makeflags().substr(js->makeflags().find('=') + 1);
    std::filesystem::path const slot = dir.path() / "otok-job.lock";
    std::vector<std::string> const child{path_to_utf8(path_from_utf8(OTOK_TEST_CHILD)), "exit", "3"};

    // the slice's slot is free: it runs at once, with the program's exit
    auto ran = run_with_job(child, slot, auth, current_environment(), dir.path());
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_EQ(ran->code, 3);

    // the slot is busy: the pool's token, given back afterwards
    auto held = file_lock::acquire(slot);
    ASSERT_TRUE(held);
    ran = run_with_job(child, slot, auth, current_environment(), dir.path());
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_TRUE(js->try_acquire(10ms));

    // neither: it waits for one of them
    std::atomic<bool> done{false};
    std::thread waiter{[&] {
        auto r = run_with_job(child, slot, auth, current_environment(), dir.path());
        done = r && r->code == 3;
    }};
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(done);
    js->release();
    waiter.join();
    EXPECT_TRUE(done);

    // without a jobserver the slot alone
    held->release();
    ran = run_with_job(child, slot, "", current_environment(), dir.path());
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_EQ(ran->code, 3);
}

TEST(jobserver, knows_which_tools_understand_it)
{
    using enum jobserver_protocol;
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 4.4.1\nBuilt for x86_64-pc-linux-gnu\n", true), named);
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 5.0\n", false), named);
    // older ones through the descriptors they inherit, on POSIX
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 4.3\n", true), descriptors);
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 4.2.1\n", true), descriptors);
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 3.81\nCopyright (C) 2006\n", true), old_descriptors);
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 4.3\n", false), none);
    // the make of MSYS2 takes a fifo, not otok's named semaphore on Windows
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make 4.4.1\nBuilt for x86_64-pc-cygwin\n", false), none);
    EXPECT_EQ(detail::make_jobserver_protocol("bmake 20240711\n", true), none);
    EXPECT_EQ(detail::make_jobserver_protocol("GNU Make x\n", true), none);
    EXPECT_TRUE(detail::ninja_understands_jobserver("1.13.0\n"));
    EXPECT_TRUE(detail::ninja_understands_jobserver("1.13.1.git\n"));
    EXPECT_FALSE(detail::ninja_understands_jobserver("1.12.1\n"));
    EXPECT_FALSE(detail::ninja_understands_jobserver(""));
    EXPECT_EQ(jobserver_protocol_of("otok-no-such-tool", current_environment()), none);
}

TEST(jobserver, gives_older_make_its_makeflags)
{
    auto const auth = inherit_jobserver("-j10 --jobserver-auth=fifo:/tmp/a b/jobserver-1", jobserver_protocol::descriptors);
    ASSERT_TRUE(auth);
    EXPECT_EQ(auth->makeflags, "-j10 --jobserver-auth=3,4");
    EXPECT_EQ(auth->fifo, path_from_utf8("/tmp/a b/jobserver-1"));
    // a -j with a number would be forced on make 3.81, which would leave the jobserver
    auto const fds = inherit_jobserver("-j10 --jobserver-auth=fifo:/tmp/jobserver-1", jobserver_protocol::old_descriptors);
    ASSERT_TRUE(fds);
    EXPECT_EQ(fds->makeflags, "-j --jobserver-fds=3,4");
    EXPECT_FALSE(inherit_jobserver("-j10 --jobserver-auth=otok-1-jobserver", jobserver_protocol::descriptors));
}


TEST(executor, runs_tasks_after_those_they_need)
{
    std::mutex m;
    std::vector<int> order;
    auto const task = [&](int n, std::vector<std::size_t> after) {
        return executor_task{std::move(after), [&, n](std::size_t) -> result<void> {
            std::lock_guard lock{m};
            order.push_back(n);
            return {};
        }};
    };
    // 0 ← 1 ← 3, 0 ← 2 ← 3
    std::vector<executor_task> tasks{task(0, {}), task(1, {0}), task(2, {0}), task(3, {1, 2})};
    test::temp_dir dir;
    job_broker broker{4, dir.path()};
    EXPECT_TRUE(run_tasks(tasks, broker).empty());
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order.front(), 0);
    EXPECT_EQ(order.back(), 3);
    EXPECT_TRUE(run_tasks(tasks, broker, 1).empty());  // one at a time
    EXPECT_EQ(order.size(), 8u);
}

TEST(executor, runs_as_many_at_once_as_there_are_jobs)
{
    std::atomic<int> now{0}, most{0};
    std::atomic<std::size_t> most_running{0};
    std::vector<executor_task> tasks;
    for (int i = 0; i < 8; ++i) {
        tasks.push_back({{}, [&](std::size_t running) -> result<void> {
            int const n = ++now;
            for (int seen = most; n > seen && !most.compare_exchange_weak(seen, n);) {}
            for (std::size_t seen = most_running; running > seen && !most_running.compare_exchange_weak(seen, running);) {}
            std::this_thread::sleep_for(30ms);
            --now;
            return {};
        }});
    }
    test::temp_dir dir;
    job_broker broker{3, dir.path()};
    EXPECT_TRUE(run_tasks(tasks, broker).empty());
    EXPECT_GE(most.load(), 2);
    EXPECT_LE(most.load(), 3);
    EXPECT_LE(most_running.load(), 3u);
    EXPECT_EQ(broker.free_jobs(), 3);  // every job is back
}

TEST(executor, stops_starting_tasks_after_a_failure)
{
    std::atomic<bool> dependent_ran{false}, slow_started{false}, slow_finished{false};
    std::vector<executor_task> tasks{
        {{}, [&](std::size_t) -> result<void> {
             slow_started = true;
             std::this_thread::sleep_for(100ms);
             slow_finished = true;
             return {};
         }},
        // fails once the slow one runs: a failure before it started would keep it from starting
        {{}, [&](std::size_t) -> result<void> {
             for (int i = 0; i < 500 && !slow_started; ++i) std::this_thread::sleep_for(1ms);
             return fail("first failure");
         }},
        {{1}, [&](std::size_t) -> result<void> {
             dependent_ran = true;
             return {};
         }},
        {{}, [&](std::size_t) -> result<void> {
             for (int i = 0; i < 500 && !slow_started; ++i) std::this_thread::sleep_for(1ms);
             throw std::runtime_error{"thrown"};
         }},
    };
    test::temp_dir dir;
    job_broker broker{4, dir.path()};
    auto errors = run_tasks(tasks, broker);
    ASSERT_FALSE(errors.empty());
    EXPECT_FALSE(dependent_ran);
    EXPECT_TRUE(slow_finished);  // running tasks finish
    bool first = false;
    for (error const& e : errors) first = first || e.message == "first failure";
    EXPECT_TRUE(first);

    // one at a time: nothing starts after the failure
    std::vector<executor_task> serial{
        {{}, [](std::size_t) -> result<void> { return fail("a"); }},
        {{}, [](std::size_t) -> result<void> { return fail("b"); }},
    };
    errors = run_tasks(serial, broker, 1);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].message, "a");
}

// docs/PLAN.md decision 61: what more tasks wait for first.
TEST(executor, starts_first_what_more_tasks_wait_for)
{
    std::mutex m;
    std::vector<int> order;
    auto const task = [&](int n, std::vector<std::size_t> after) {
        return executor_task{std::move(after), [&, n](std::size_t) -> result<void> {
            std::lock_guard lock{m};
            order.push_back(n);
            return {};
        }};
    };
    // 2 needs 1, so 1 comes before 0, which nothing needs
    std::vector<executor_task> tasks{task(0, {}), task(1, {}), task(2, {1})};
    test::temp_dir dir;
    job_broker broker{1, dir.path()};
    EXPECT_TRUE(run_tasks(tasks, broker).empty());
    EXPECT_EQ(order, (std::vector<int>{1, 0, 2}));
}

// docs/PLAN.md decision 60: executors sharing their jobs run as many tasks together as one would.
TEST(executor, shares_jobs_between_executors)
{
    std::atomic<int> now{0}, most{0};
    auto const tasks = [&] {
        std::vector<executor_task> list;
        for (int i = 0; i < 6; ++i) {
            list.push_back({{}, [&](std::size_t) -> result<void> {
                int const n = ++now;
                for (int seen = most; n > seen && !most.compare_exchange_weak(seen, n);) {}
                std::this_thread::sleep_for(30ms);
                --now;
                return {};
            }});
        }
        return list;
    };
    test::temp_dir dir;
    job_broker broker{3, dir.path()};
    std::vector<executor_task> const a = tasks(), b = tasks();
    std::thread other{[&] { EXPECT_TRUE(run_tasks(a, broker).empty()); }};
    EXPECT_TRUE(run_tasks(b, broker).empty());
    other.join();
    EXPECT_GE(most.load(), 2);
    EXPECT_LE(most.load(), 3);
    EXPECT_EQ(broker.free_jobs(), 3);
}

// A task waiting for another lends its job, so that the other can run although the jobs are all taken.
TEST(executor, lends_the_job_of_a_waiting_task)
{
    std::atomic<bool> done{false};
    std::vector<executor_task> tasks{
        {{}, [&](std::size_t) -> result<void> {
             std::vector<std::string> waits;
             on_job_wait([&](bool w, std::string const& what) { waits.push_back(w ? "waits for " + what : "goes on"); });
             {
                 lent_job const lent{"another build"};
                 for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(10ms);
             }
             // to show what it waits for (then perhaps for its job, while the other one still holds it)
             if (waits.size() < 2 || waits[0] != "waits for another build" || waits[1] != "goes on") return fail("not told what it waits for");
             if (!done) return fail("the other task never ran");
             return {};
         }},
        {{}, [&](std::size_t) -> result<void> {
             done = true;
             return {};
         }},
    };
    test::temp_dir dir;
    job_broker broker{1, dir.path()};
    auto const errors = run_tasks(tasks, broker);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front().message);
    lent_job const outside{"nothing"};  // outside a task: nothing
    job_boundary();
}

// The jobserver of a slice (docs/PLAN.md decision 61): filled a token at a time while its clients use every
// token, emptied of the tokens they leave unused.
TEST(job_broker, fills_and_empties_the_jobservers_of_slices)
{
    test::temp_dir dir;
    job_broker broker{4, dir.path(), false};
    auto made = broker.attach({0, 1});
    ASSERT_TRUE(made) << made.error().message;
    auto& slice = **made;
    broker.tick();
    EXPECT_EQ(broker.free_jobs(), 4);  // no command runs: it wants nothing
    slice.set_running(true);
    broker.tick();
    EXPECT_EQ(broker.free_jobs(), 3);
    EXPECT_EQ(slice.in_use(), 0);
    ASSERT_TRUE(slice.server().try_acquire(0ms));  // its make takes it
    broker.tick();
    EXPECT_EQ(broker.free_jobs(), 2);
    EXPECT_EQ(slice.in_use(), 1);  // the one its make took; the next lies in its jobserver
    // the one it does not take lies unused, and goes back after a while
    broker.tick();
    EXPECT_EQ(broker.free_jobs(), 2);
    std::this_thread::sleep_for(250ms);
    broker.tick();
    EXPECT_EQ(broker.free_jobs(), 3);
    EXPECT_EQ(slice.in_use(), 1);
    // the command ends: what it took is back at once
    slice.server().release();
    slice.set_running(false);
    EXPECT_EQ(broker.free_jobs(), 4);
    EXPECT_EQ(slice.in_use(), 0);
}

TEST(job_broker, gives_the_tokens_of_slices_after_to_the_one_before)
{
    test::temp_dir dir;
    job_broker broker{2, dir.path(), false};
    auto late = broker.attach({0, 2});
    auto early = broker.attach({0, 1});
    ASSERT_TRUE(late && early);
    (*late)->set_running(true);
    broker.tick();
    ASSERT_TRUE((*late)->server().try_acquire(0ms));
    broker.tick();
    ASSERT_TRUE((*late)->server().try_acquire(0ms));
    EXPECT_EQ(broker.free_jobs(), 0);
    // the early slice wants one; the late one gives one back as a compilation ends: it goes to the early one
    (*early)->set_running(true);
    broker.tick();
    (*late)->server().release();
    broker.tick();
    EXPECT_FALSE((*late)->server().try_acquire(0ms));
    EXPECT_TRUE((*early)->server().try_acquire(0ms));

    // a tool without the jobserver gets nothing while a slice before it wants
    EXPECT_EQ((*late)->hold(4), 0);
    (*early)->server().release();
    (*early)->set_running(false);
    (*late)->server().release();
    (*late)->set_running(false);
    EXPECT_EQ(broker.free_jobs(), 2);
    EXPECT_EQ((*late)->hold(4), 2);
    EXPECT_EQ(broker.free_jobs(), 0);
    (*late)->put_back(2);
    EXPECT_EQ(broker.free_jobs(), 2);
}

TEST(job_broker, a_command_without_the_jobserver_wants_nothing)
{
    test::temp_dir dir;
    job_broker broker{2, dir.path(), false};
    auto early = broker.attach({0, 1});
    ASSERT_TRUE(early);
    // make 3.81 with a fixed -j: the tokens would only lie in its jobserver, and keep tasks after it waiting
    (*early)->set_running(true, false);
    broker.tick();
    EXPECT_EQ(broker.free_jobs(), 2);
    EXPECT_FALSE(broker.wanted_before({0, 2}));
    EXPECT_TRUE(broker.take({0, 2}, 0ms));
    broker.give();
    (*early)->set_running(false);
}

// Between two commands a task gives its job to a slice before it that wants one, and waits for it again.
TEST(job_broker, takes_the_job_of_a_task_between_its_commands)
{
    test::temp_dir dir;
    job_broker broker{2, dir.path(), false};
    std::vector<executor_task> tasks{{{}, [&](std::size_t) -> result<void> {
        auto made = broker.attach({10, 0});
        if (!made) return std::unexpected{made.error()};
        auto& first = **made;
        first.set_running(true);
        broker.tick();  // the other job goes to it; it wants more
        EXPECT_EQ(broker.free_jobs(), 0);
        std::thread ends{[&] {
            std::this_thread::sleep_for(100ms);
            EXPECT_EQ(broker.free_jobs(), 1);  // the task's job, given back
            first.set_running(false);
        }};
        std::vector<bool> waits;
        on_job_wait([&](bool w, std::string const& what) {
            EXPECT_EQ(what, "");  // a job
            waits.push_back(w);
        });
        auto const started = std::chrono::steady_clock::now();
        job_boundary();
        EXPECT_GE(std::chrono::steady_clock::now() - started, 90ms);
        EXPECT_EQ(waits, (std::vector<bool>{true, false}));  // to show that it waits
        ends.join();
        return {};
    }}};
    EXPECT_TRUE(run_tasks(tasks, broker).empty());
    EXPECT_EQ(broker.free_jobs(), 2);
}
