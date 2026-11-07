// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "otok/core/executor.hpp"

// The terminal otok writes to.
namespace otok {

// otok writes UTF-8. A Windows console shows it as such only when told so; elsewhere nothing to do.
void use_utf8_console() noexcept;

// What the terminal of otok's standard error can show (docs/PLAN.md decision 59).
struct terminal_caps
{
    bool live = false;  // a terminal that takes escape sequences, so lines can be redrawn
    bool utf8 = false;  // it shows UTF-8
};

// Standard error's terminal; on a Windows console this turns escape sequences on. Not live for a pipe, a
// file or TERM=dumb.
terminal_caps error_terminal() noexcept;

// The columns and rows of standard error's terminal; {0, 0} when unknown.
std::pair<int, int> terminal_size() noexcept;

// A stream buffer over a C stream that writes every complete line with one write and flushes it, each thread's lines
// apart, so that a reader of both pipes of otok (Gradle, an IDE) never gets a line in pieces with the other stream's
// between them. sync() writes the calling thread's unfinished line too (the escape sequences of a display, a prompt).
class line_writer : public std::streambuf
{
public:
    explicit line_writer(std::FILE* file) noexcept : file_{file} {}

protected:
    int_type overflow(int_type c) override;
    std::streamsize xsputn(char const* s, std::streamsize n) override;
    int sync() override;

private:
    std::FILE* file_;
    std::mutex m_;
    std::map<std::thread::id, std::string> pending_;
};

// Progress: lines written whole, and on a live terminal the tasks under way kept below them, each with
// a spinner and the time it has run (docs/PLAN.md decision 59). Every function may be called from any
// thread.
class progress_display
{
public:
    using clock = std::chrono::steady_clock;

    struct terminal
    {
        bool utf8 = false;
        std::function<std::pair<int, int>()> size = terminal_size;
        std::function<clock::time_point()> now = clock::now;
        bool ticking = true;    // a thread redraws ten times a second; tests call draw()
    };

    // Without a terminal: plain lines, and tasks are not shown.
    progress_display(std::ostream& out, std::optional<terminal> live);
    ~progress_display();
    progress_display(progress_display const&) = delete;
    progress_display& operator=(progress_display const&) = delete;

    bool live() const noexcept { return live_.has_value(); }

    // A line above the tasks.
    void line(std::string_view text);

    // A task under way, shown until end(). The tasks that wait are above those that run, each part from the last of
    // their priority to the first: the first that runs is at the bottom.
    std::uint64_t begin(std::string label, job_priority priority = {});
    void end(std::uint64_t task);

    // Whether a task waits (docs/PLAN.md decision 61): for a job, "queued" and an arrow before its stage, the command it
    // waits to start; for `what`, "waiting for <what>". Shown without a spinner, and its time, the time it has run,
    // stands still meanwhile.
    void waiting(std::uint64_t task, bool waits, std::string what = {});

    // How many jobs a task runs, asked at each drawing (docs/PLAN.md decision 61): shown as "×3" before the time
    // while it does not wait; none, or a count below one: nothing.
    void count_jobs(std::uint64_t task, std::function<std::int64_t()> count);

    // What a task does now ("configure", "make install"), shown after its label.
    void stage(std::uint64_t task, std::string text);

    // Redraws the tasks.
    void draw();

    // A stream buffer that gives line() every complete line written to it, each thread's apart.
    std::streambuf& lines() noexcept { return buffer_; }

private:
    class line_buffer : public std::streambuf
    {
    public:
        explicit line_buffer(progress_display& d) : display_{d} {}
        void finish();      // the unfinished lines, as lines

    protected:
        int_type overflow(int_type c) override;
        std::streamsize xsputn(char const* s, std::streamsize n) override;

    private:
        progress_display& display_;
        std::mutex m_;
        std::map<std::thread::id, std::string> pending_;
    };

    struct task
    {
        std::uint64_t id = 0;
        std::string label;
        std::string stage;
        clock::time_point started;
        job_priority priority;
        bool waits = false;
        clock::time_point waits_since;
        std::string waits_for;      // empty: a job
        clock::duration waited{};   // in the waits that ended
        std::function<std::int64_t()> jobs;
    };

    // With m_ held: the escape sequences that remove the drawn tasks, and those that draw them.
    std::string erased();
    std::string drawn();

    std::ostream& out_;
    std::optional<terminal> live_;
    std::mutex m_;
    std::vector<task> tasks_;
    std::uint64_t next_ = 0;
    std::size_t drawn_ = 0;     // lines of tasks on the screen
    line_buffer buffer_;
    std::condition_variable stop_changed_;
    bool stop_ = false;
    std::thread ticker_;
};

namespace detail {

// Columns of a UTF-8 text, a code point each.
std::size_t text_columns(std::string_view text);

// The text cut to `columns`, the cut marked by `ellipsis`.
std::string fit_text(std::string_view text, std::size_t columns, std::string_view ellipsis);

// "0:07", "12:34", "1:02:03".
std::string elapsed_text(std::chrono::steady_clock::duration d);

}

}
