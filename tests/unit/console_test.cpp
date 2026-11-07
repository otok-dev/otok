// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The progress display (docs/PLAN.md decision 59): plain lines, and on a live terminal the tasks under
// way below them, drawn with a clock and a size the tests give.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "otok/core/console.hpp"

using namespace otok;
using namespace std::chrono_literals;

namespace {

struct fake_terminal
{
    progress_display::clock::time_point now{};
    std::pair<int, int> size{40, 24};

    progress_display::terminal make(bool utf8 = false)
    {
        progress_display::terminal t;
        t.utf8 = utf8;
        t.size = [this] { return size; };
        t.now = [this] { return now; };
        t.ticking = false;
        return t;
    }
};

}

TEST(console, writes_plain_lines_without_a_terminal)
{
    std::ostringstream out;
    {
        progress_display d{out, std::nullopt};
        EXPECT_FALSE(d.live());
        d.line("one");
        auto const task = d.begin("a task");
        d.draw();
        d.end(task);
        std::ostream s{&d.lines()};
        s << "two\nthr" << "ee\r\n" << "unfinished";
    }
    EXPECT_EQ(out.str(), "one\ntwo\nthree\nunfinished\n");
}

TEST(console, keeps_the_lines_of_each_thread_apart)
{
    std::ostringstream out;
    {
        progress_display d{out, std::nullopt};
        std::ostream s{&d.lines()};
        s << "main ";
        std::thread{[&] { s << "other\n"; }}.join();
        s << "line\n";
    }
    EXPECT_EQ(out.str(), "other\nmain line\n");
}

TEST(console, draws_the_tasks_below_the_lines)
{
    std::ostringstream out;
    fake_terminal t;
    {
        progress_display d{out, t.make()};
        EXPECT_TRUE(d.live());
        d.begin("boost 1.89");
        t.now += 1500ms;
        auto const lz4 = d.begin("lz4");
        t.now += 61s;
        d.draw();
        // the spinner by the tenths of a second each has run (62.5 s, 61 s), the labels aligned
        EXPECT_EQ(out.str(), "/ boost 1.89  1:02\n- lz4         1:01\n");

        // a line: the tasks erased, the line, the tasks again
        out.str({});
        d.end(lz4);
        d.line("built lz4");
        EXPECT_EQ(out.str(), "\x1b[2F\x1b[Jbuilt lz4\n/ boost 1.89  1:02\n");

        // a task waiting for a job (docs/PLAN.md decision 61)
        out.str({});
        auto const icu = d.begin("icu4c");
        d.waiting(icu, true);
        t.now += 5s;
        d.draw();
        // its time stands still meanwhile
        EXPECT_EQ(out.str(), "\x1b[1F\x1b[J. icu4c       0:00  queued\n\\ boost 1.89  1:07\n");
        out.str({});
        d.waiting(icu, false);
        t.now += 2s;
        d.draw();
        EXPECT_EQ(out.str(), "\x1b[2F\x1b[J\\ boost 1.89  1:09\n| icu4c       0:02\n");
        out.str({});
        d.end(icu);
        d.draw();
        out.str({});
    }
    // nothing left below the lines
    EXPECT_EQ(out.str(), "\x1b[1F\x1b[J");
}

TEST(console, shows_the_jobs_of_the_tasks)
{
    std::ostringstream out;
    fake_terminal t;
    {
        progress_display d{out, t.make(true)};
        std::int64_t boost_jobs = 12;
        auto const boost = d.begin("boost");
        d.count_jobs(boost, [&] { return boost_jobs; });
        auto const lz4 = d.begin("lz4");
        d.count_jobs(lz4, [] { return std::int64_t{1}; });
        auto const icu = d.begin("icu4c");
        d.draw();
        // right-aligned in one column; none for a task without a count
        EXPECT_EQ(out.str(), "⠋ boost  ×12  0:00\n⠋ lz4     ×1  0:00\n⠋ icu4c       0:00\n");

        // none while a task waits, nor below one
        out.str({});
        d.waiting(lz4, true);
        boost_jobs = 0;
        d.draw();
        // a short wait leaves it where it is
        EXPECT_EQ(out.str(), "\x1b[3F\x1b[J⠋ boost  0:00\n· lz4    0:00  queued\n⠋ icu4c  0:00\n");
        d.end(boost);
        d.end(lz4);
        d.end(icu);
        d.draw();
    }
}

TEST(console, shows_the_stages_of_the_tasks)
{
    std::ostringstream out;
    fake_terminal t;
    {
        progress_display d{out, t.make(true)};
        d.stage(d.begin("boost"), "b2 install");
        d.stage(d.begin("lz4"), "configure");
        d.begin("icu4c");
        d.draw();
        // in a column of their own
        EXPECT_EQ(out.str(), "⠋ boost  b2 install  0:00\n⠋ lz4    configure   0:00\n⠋ icu4c              0:00\n");

        // the stages give way first
        out.str({});
        t.size = {20, 24};
        d.draw();
        EXPECT_EQ(out.str(), "\x1b[3F\x1b[J⠋ boost  b2 …  0:00\n⠋ lz4    con…  0:00\n⠋ icu4c        0:00\n");
    }
}

TEST(console, shows_the_tasks_by_priority)
{
    std::ostringstream out;
    fake_terminal t;
    {
        progress_display d{out, t.make()};
        d.begin("late", {0, 3});
        d.begin("none");
        d.begin("early", {0, 1});
        d.begin("weighty", {2, 5});
        d.draw();
        // the first to have the jobs at the bottom
        EXPECT_EQ(out.str(), "| none     0:00\n| late     0:00\n| early    0:00\n| weighty  0:00\n");
    }
}

TEST(console, shows_what_waits_above_what_runs)
{
    std::ostringstream out;
    fake_terminal t;
    t.size = {80, 24};
    {
        progress_display d{out, t.make()};
        auto const late = d.begin("late", {0, 3});
        auto const none = d.begin("none");
        auto const early = d.begin("early", {0, 1});
        auto const weighty = d.begin("weighty", {2, 5});
        d.stage(late, "make");
        d.stage(early, "perl Configure");
        d.stage(weighty, "build");
        t.now += 2s;
        d.waiting(late, true);
        d.waiting(early, true);
        d.waiting(weighty, true, "another build");
        t.now += 1s;
        d.draw();
        // the next to have a job right above what runs; a queued one's stage is the command it waits to start
        EXPECT_EQ(out.str(),
            ". late     -> make            0:02  queued\n"
            ". early    -> perl Configure  0:02  queued\n"
            ". weighty  build              0:02  waiting for another build\n"
            "- none                        0:03\n");

        // too many: those that wait go first, from the top
        out.str({});
        t.size = {80, 6};
        d.draw();
        EXPECT_EQ(out.str(), "\x1b[4F\x1b[J  and 3 more\n- none  0:03\n");
        d.end(late);
        d.end(none);
        d.end(early);
        d.end(weighty);
        d.draw();
    }
}

TEST(console, fits_the_tasks_to_the_terminal)
{
    std::ostringstream out;
    fake_terminal t;
    t.size = {16, 24};
    {
        progress_display d{out, t.make()};
        d.begin("boost 1.89");
        t.now += 62s;
        d.draw();
        // short of the last column
        EXPECT_EQ(out.str(), "| boos...  1:02\n");

        // at most half the rows, less one
        out.str({});
        t.size = {80, 6};
        d.begin("lz4");
        d.begin("icu4c");
        d.draw();
        EXPECT_EQ(out.str(), "\x1b[1F\x1b[J  and 2 more\n| icu4c  0:00\n");
    }
}

TEST(console, spins_in_utf8)
{
    std::ostringstream out;
    fake_terminal t;
    t.size = {12, 24};
    {
        progress_display d{out, t.make(true)};
        d.begin("библиотека");
        t.now += 200ms;
        d.draw();
        EXPECT_EQ(out.str(), "⠹ би…  0:00\n");
    }
}

TEST(console, texts)
{
    EXPECT_EQ(detail::elapsed_text(0s), "0:00");
    EXPECT_EQ(detail::elapsed_text(754s), "12:34");
    EXPECT_EQ(detail::elapsed_text(3723s), "1:02:03");
    EXPECT_EQ(detail::text_columns("абв"), 3u);
    EXPECT_EQ(detail::fit_text("абвгд", 4, "…"), "абв…");
    EXPECT_EQ(detail::fit_text("abc", 3, "..."), "abc");
    EXPECT_EQ(detail::fit_text("abcdef", 2, "..."), "");
}

namespace {

std::string file_text(std::FILE* file)
{
    std::rewind(file);
    std::string text;
    char buffer[256];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, file)) > 0;) text.append(buffer, n);
    std::fseek(file, 0, SEEK_END);  // a write after a read needs a seek between them
    return text;
}

}

// A line goes out whole: the piece of an unfinished one waits for its line break, while the lines of another thread
// go before it.
TEST(console, writes_whole_lines_to_a_file)
{
    std::FILE* const file = std::tmpfile();
    ASSERT_NE(file, nullptr);
    {
        line_writer buffer{file};
        std::ostream out{&buffer};
        out << "toolchain of '";
        std::thread{[&] { std::ostream{&buffer} << "inputs in the cache: 12\n"; }}.join();
        EXPECT_EQ(file_text(file), "inputs in the cache: 12\n");
        out << "android': NDK\n" << "first" << "\nsecond\nthi";
        EXPECT_EQ(file_text(file), "inputs in the cache: 12\ntoolchain of 'android': NDK\nfirst\nsecond\n");
        out << std::flush;
        EXPECT_EQ(file_text(file), "inputs in the cache: 12\ntoolchain of 'android': NDK\nfirst\nsecond\nthi");
    }
    std::fclose(file);
}
