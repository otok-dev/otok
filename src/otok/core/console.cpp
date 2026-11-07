// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/console.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <span>

#ifdef _WIN32
#   include <windows.h>
#else
#   include <sys/ioctl.h>
#   include <unistd.h>
#endif

namespace otok {

void use_utf8_console() noexcept
{
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif
}

line_writer::int_type line_writer::overflow(int_type c)
{
    if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
    char const ch = traits_type::to_char_type(c);
    xsputn(&ch, 1);
    return c;
}

std::streamsize line_writer::xsputn(char const* s, std::streamsize n)
{
    std::lock_guard lock{m_};
    std::string& pending = pending_[std::this_thread::get_id()];
    pending.append(s, static_cast<std::size_t>(n));
    // up to the last line break: one write, the lines of other threads before or after it
    if (std::size_t const end = pending.find_last_of('\n'); end != std::string::npos) {
        std::fwrite(pending.data(), 1, end + 1, file_);
        std::fflush(file_);
        pending.erase(0, end + 1);
    }
    if (pending.empty()) pending_.erase(std::this_thread::get_id());
    return n;
}

int line_writer::sync()
{
    std::lock_guard lock{m_};
    auto const it = pending_.find(std::this_thread::get_id());
    if (it != pending_.end()) {
        std::fwrite(it->second.data(), 1, it->second.size(), file_);
        pending_.erase(it);
    }
    return std::fflush(file_) == 0 ? 0 : -1;
}

terminal_caps error_terminal() noexcept
{
#ifdef _WIN32
    HANDLE const h = ::GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !::GetConsoleMode(h, &mode)) return {};
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) && !::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return {};
    return {true, ::GetConsoleOutputCP() == CP_UTF8};
#else
    if (!::isatty(STDERR_FILENO)) return {};
    char const* term = std::getenv("TERM");
    if (!term || !*term || std::string_view{term} == "dumb") return {};
    // the locale's charset: the first of these that is set
    std::string charset;
    for (char const* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        char const* value = std::getenv(name);
        if (value && *value) {
            charset = value;
            break;
        }
    }
    std::ranges::transform(charset, charset.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return {true, charset.find("utf-8") != std::string::npos || charset.find("utf8") != std::string::npos};
#endif
}

std::pair<int, int> terminal_size() noexcept
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!::GetConsoleScreenBufferInfo(::GetStdHandle(STD_ERROR_HANDLE), &info)) return {0, 0};
    return {info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1};
#else
    winsize ws{};
    if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != 0) return {0, 0};
    return {ws.ws_col, ws.ws_row};
#endif
}

namespace detail {

std::size_t text_columns(std::string_view text)
{
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string fit_text(std::string_view text, std::size_t columns, std::string_view ellipsis)
{
    if (text_columns(text) <= columns) return std::string{text};
    std::size_t const keep = columns > text_columns(ellipsis) ? columns - text_columns(ellipsis) : 0;
    std::size_t n = 0, at = 0;
    for (; at < text.size(); ++at) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && n++ == keep) break;
    }
    return std::string{text.substr(0, at)} + (columns >= text_columns(ellipsis) ? std::string{ellipsis} : std::string{});
}

std::string elapsed_text(std::chrono::steady_clock::duration d)
{
    auto const total = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    long long const s = total % 60, m = total / 60 % 60, h = total / 3600;
    char text[32];
    if (h > 0) {
        std::snprintf(text, sizeof text, "%lld:%02lld:%02lld", h, m, s);
    } else {
        std::snprintf(text, sizeof text, "%lld:%02lld", m, s);
    }
    return text;
}

}

progress_display::progress_display(std::ostream& out, std::optional<terminal> live)
    : out_{out}, live_{std::move(live)}, buffer_{*this}
{
    if (live_ && live_->ticking) {
        ticker_ = std::thread{[this] {
            std::unique_lock lock{m_};
            while (!stop_changed_.wait_for(lock, std::chrono::milliseconds{100}, [this] { return stop_; })) {
                lock.unlock();
                draw();
                lock.lock();
            }
        }};
    }
}

progress_display::~progress_display()
{
    {
        std::lock_guard lock{m_};
        stop_ = true;
    }
    stop_changed_.notify_all();
    if (ticker_.joinable()) ticker_.join();
    buffer_.finish();
    std::lock_guard lock{m_};
    if (live_) out_ << erased() << std::flush;
}

void progress_display::line(std::string_view text)
{
    std::lock_guard lock{m_};
    if (!live_) {
        out_ << text << '\n' << std::flush;
        return;
    }
    std::string const removed = erased();
    out_ << removed << text << '\n' << drawn() << std::flush;
}

std::uint64_t progress_display::begin(std::string label, job_priority priority)
{
    std::lock_guard lock{m_};
    std::uint64_t const id = ++next_;
    if (live_) {
        task& t = tasks_.emplace_back();
        t.id = id;
        t.label = std::move(label);
        t.started = live_->now();
        t.priority = priority;
    }
    return id;
}

void progress_display::end(std::uint64_t id)
{
    std::lock_guard lock{m_};
    std::erase_if(tasks_, [&](task const& t) { return t.id == id; });
}

void progress_display::waiting(std::uint64_t id, bool waits, std::string what)
{
    std::lock_guard lock{m_};
    if (!live_) return;
    clock::time_point const now = live_->now();
    for (task& t : tasks_) {
        if (t.id != id) continue;
        t.waits_for = waits ? std::move(what) : std::string{};
        if (t.waits == waits) continue;
        if (waits) {
            t.waits_since = now;
        } else {
            t.waited += now - t.waits_since;
        }
        t.waits = waits;
    }
}

void progress_display::count_jobs(std::uint64_t id, std::function<std::int64_t()> count)
{
    std::lock_guard lock{m_};
    for (task& t : tasks_) {
        if (t.id == id) t.jobs = std::move(count);
    }
}

void progress_display::stage(std::uint64_t id, std::string text)
{
    std::lock_guard lock{m_};
    for (task& t : tasks_) {
        if (t.id == id) t.stage = std::move(text);
    }
}

void progress_display::draw()
{
    std::lock_guard lock{m_};
    if (!live_ || (drawn_ == 0 && tasks_.empty())) return;
    std::string const removed = erased();
    out_ << removed << drawn() << std::flush;
}

std::string progress_display::erased()
{
    if (drawn_ == 0) return {};
    // to the start of the first line of tasks, then everything below it
    std::string const text = "\x1b[" + std::to_string(drawn_) + "F\x1b[J";
    drawn_ = 0;
    return text;
}

std::string progress_display::drawn()
{
    auto [columns, rows] = live_->size();
    // short of the last column, where a terminal may wrap
    std::size_t const width = columns > 1 ? static_cast<std::size_t>(columns - 1) : 79;
    std::size_t const most = rows > 0 ? static_cast<std::size_t>(std::max(1, rows / 2 - 1)) : 10;
    std::string_view const ellipsis = live_->utf8 ? "…" : "...";
    static constexpr std::string_view utf8_frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    static constexpr std::string_view ascii_frames[] = {"|", "/", "-", "\\"};
    std::span<std::string_view const> const frames = live_->utf8 ? std::span<std::string_view const>{utf8_frames}
                                                                 : std::span<std::string_view const>{ascii_frames};

    clock::time_point const now = live_->now();
    // what waits (docs/PLAN.md decision 61) above what runs, each part from the last to have the jobs to the first, so
    // that the first runs at the bottom and the next to run is right above what runs; those of one priority in the
    // order they began. Only waits of half a second move a task: short ones between two commands leave it where it is.
    auto const waits_long = [&](task const& t) { return t.waits && now - t.waits_since >= std::chrono::milliseconds{500}; };
    std::vector<task const*> sorted;
    for (task const& t : tasks_) sorted.push_back(&t);
    std::ranges::stable_sort(sorted, [&](task const* a, task const* b) {
        bool const a_waits = waits_long(*a), b_waits = waits_long(*b);
        return a_waits != b_waits ? a_waits : b->priority.before(a->priority);
    });
    // too many: those at the top go, what waits first, then what runs last
    std::size_t const hidden = sorted.size() > most ? sorted.size() - (most - 1) : 0;
    sorted.erase(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(hidden));
    std::size_t const shown = sorted.size();
    // a queued task's stage is the command it waits to start
    std::string_view const arrow = live_->utf8 ? "→ " : "-> ";
    auto const stage_of = [&](task const& t) { return t.waits && t.waits_for.empty() && !t.stage.empty() ? std::string{arrow} + t.stage : t.stage; };
    std::size_t label_width = 0, stage_width = 0, jobs_width = 0;
    std::vector<std::string> jobs(shown);
    for (std::size_t i = 0; i < shown; ++i) {
        label_width = std::max(label_width, detail::text_columns(sorted[i]->label));
        stage_width = std::max(stage_width, detail::text_columns(stage_of(*sorted[i])));
        std::int64_t const n = sorted[i]->jobs && !sorted[i]->waits ? sorted[i]->jobs() : 0;
        if (n > 0) jobs[i] = (live_->utf8 ? "×" : "x") + std::to_string(n);
        jobs_width = std::max(jobs_width, detail::text_columns(jobs[i]));
    }
    std::vector<std::string> lines;
    if (hidden > 0) lines.push_back(detail::fit_text("  and " + std::to_string(hidden) + " more", width, ellipsis));
    for (std::size_t i = 0; i < shown; ++i) {
        task const& t = *sorted[i];
        auto const ran = (t.waits ? t.waits_since : now) - t.started - t.waited;
        auto const frame = static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(ran).count() / 100) % frames.size();
        // the jobs, right-aligned, and the time, all in one column
        std::string time = jobs_width > 0 ? std::string(jobs_width - detail::text_columns(jobs[i]), ' ') + jobs[i] + "  " : std::string{};
        time += detail::elapsed_text(ran);
        if (t.waits) time += t.waits_for.empty() ? "  queued" : "  waiting for " + t.waits_for;
        // the spinner, the label, the stage, the jobs and the time, two spaces apart; the stage gives way to the width
        // first, then the label
        std::size_t const tail = detail::text_columns(time) + 4;
        std::size_t const room = width > tail ? width - tail : 0;
        std::size_t const stage_room = stage_width > 0 && room > label_width + 2 ? std::min(stage_width, room - label_width - 2) : 0;
        std::size_t const label_room = stage_room > 0 ? room - stage_room - 2 : room;
        std::string label = detail::fit_text(t.label, label_room, ellipsis);
        label += std::string(std::min(label_width, label_room) - std::min(detail::text_columns(label), std::min(label_width, label_room)), ' ');
        if (stage_room > 0) {
            std::string const stage = detail::fit_text(stage_of(t), stage_room, ellipsis);
            label += "  " + stage + std::string(stage_room - detail::text_columns(stage), ' ');
        }
        std::string_view const mark = t.waits ? (live_->utf8 ? "·" : ".") : frames[frame];
        lines.push_back(std::string{mark} + " " + label + "  " + time);
    }
    std::string text;
    for (std::string const& l : lines) text += l + "\n";
    drawn_ = lines.size();
    return text;
}

void progress_display::line_buffer::finish()
{
    std::map<std::thread::id, std::string> rest;
    {
        std::lock_guard lock{m_};
        rest.swap(pending_);
    }
    for (auto const& [thread, text] : rest) {
        if (!text.empty()) display_.line(text);
    }
}

progress_display::line_buffer::int_type progress_display::line_buffer::overflow(int_type c)
{
    if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
    char const ch = traits_type::to_char_type(c);
    xsputn(&ch, 1);
    return c;
}

std::streamsize progress_display::line_buffer::xsputn(char const* s, std::streamsize n)
{
    std::vector<std::string> complete;
    {
        std::lock_guard lock{m_};
        std::string& pending = pending_[std::this_thread::get_id()];
        for (std::streamsize i = 0; i < n; ++i) {
            if (s[i] != '\n') {
                pending += s[i];
                continue;
            }
            if (!pending.empty() && pending.back() == '\r') pending.pop_back();
            complete.push_back(std::move(pending));
            pending.clear();
        }
        if (pending.empty()) pending_.erase(std::this_thread::get_id());
    }
    for (std::string const& l : complete) display_.line(l);
    return n;
}

}
