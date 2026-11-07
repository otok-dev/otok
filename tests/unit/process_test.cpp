// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// run_process, checked from the child's side: tests/child/child.cpp (otok-test-child) reports the
// arguments, environment and working directory it received.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <future>
#include <string>
#include <vector>

#include "otok/core/fs.hpp"
#include "otok/core/jobserver.hpp"
#include "otok/core/process.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

#ifdef _WIN32
constexpr char path_list_separator = ';';
char const* const child_file_name = "otok-child.exe";
#else
constexpr char path_list_separator = ':';
char const* const child_file_name = "otok-child";
#endif

std::filesystem::path child_path()
{
    return path_from_utf8(OTOK_TEST_CHILD);
}

process_spec child(std::filesystem::path const& cwd, std::vector<std::string> args)
{
    process_spec spec;
    spec.argv.push_back(path_to_utf8(child_path()));
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.cwd = cwd;
    spec.capture = true;
    return spec;
}

std::vector<std::string> lines(std::string const& text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t end; (end = text.find('\n', start)) != std::string::npos; start = end + 1) {
        out.push_back(text.substr(start, end - start));
    }
    return out;
}

// Copies the child into `dir` under a name that PATH lookups can find.
std::filesystem::path install_child(std::filesystem::path const& dir)
{
    std::filesystem::create_directories(dir);
    std::filesystem::path const target = dir / child_file_name;
    std::filesystem::copy_file(child_path(), target, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::permissions(target, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    return target;
}

}

TEST(process, arguments_arrive_unchanged)
{
    test::temp_dir dir;
    std::vector<std::string> const args{"plain", "", "two words", "tab\there", "quote\"inside", "\"quoted\"",
        "trailing\\", "trailing space\\", "back\\\\\"slashes", "\\\\server\\share", "*.c", "$HOME", "%PATH%",
        "a;b|c&d>e<f^g", "-DNAME=\"value with spaces\"", "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xE2\x82\xAC"};
    std::vector<std::string> command{"args"};
    command.insert(command.end(), args.begin(), args.end());
    auto e = run_process(child(dir.path(), command));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_TRUE(e->success()) << describe_exit(*e);
    EXPECT_EQ(lines(e->output), args);
}

TEST(process, windows_quoting)
{
    using detail::quote_windows_argument;
    EXPECT_EQ(quote_windows_argument("plain"), "plain");
    EXPECT_EQ(quote_windows_argument("C:\\dir\\file"), "C:\\dir\\file");  // backslashes alone stay as they are
    EXPECT_EQ(quote_windows_argument(""), "\"\"");
    EXPECT_EQ(quote_windows_argument("a b"), "\"a b\"");
    EXPECT_EQ(quote_windows_argument("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(quote_windows_argument("a b\\"), "\"a b\\\\\"");
    EXPECT_EQ(quote_windows_argument("a\\\"b"), "\"a\\\\\\\"b\"");
    EXPECT_EQ(quote_windows_argument("a\\\\b c"), "\"a\\\\b c\"");
}

TEST(process, environment_is_exactly_the_given_one)
{
    test::temp_dir dir;
    auto spec = child(dir.path(), {"env-all"});
    spec.env = {{"OTOK_A", "1"}, {"OTOK_B", "\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5"}, {"OTOK_EMPTY", ""}};
    auto e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    auto vars = lines(e->output);
    std::ranges::sort(vars);
    EXPECT_EQ(vars, (std::vector<std::string>{"OTOK_A=1", "OTOK_B=\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5", "OTOK_EMPTY="}));

    spec.env.clear();
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "");
}

TEST(process, working_directory)
{
    test::temp_dir dir;
    std::filesystem::path const cwd = dir.path() / path_from_utf8("work dir \xC3\xA9");
    std::filesystem::create_directories(cwd);
    auto e = run_process(child(cwd, {"cwd"}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    auto out = lines(e->output);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(std::filesystem::equivalent(path_from_utf8(out[0]), cwd)) << out[0];
}

TEST(process, output_goes_to_the_log)
{
    test::temp_dir dir;
    dir.write("build.log", "earlier\n");
    auto spec = child(dir.path(), {"stderr", "to stderr"});
    spec.capture = false;
    spec.log = dir.path() / "build.log";
    auto e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    spec.argv = {spec.argv[0], "args", "to stdout"};
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "");
    EXPECT_EQ(read_file(spec.log).value_or("?"), "earlier\nto stderr\nto stdout\n");

    // capture takes standard output; standard error still goes to the log
    spec.capture = true;
    spec.argv = {spec.argv[0], "args", "captured"};
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "captured\n");
    EXPECT_EQ(read_file(spec.log).value_or("?"), "earlier\nto stderr\nto stdout\n");

    // without a log the output is discarded
    spec.capture = false;
    spec.log.clear();
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_TRUE(e->success());
}

TEST(process, capture_limit)
{
    test::temp_dir dir;
    auto spec = child(dir.path(), {"bytes", "3000"});
    spec.capture_limit = 1000;
    auto e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, std::string(1000, 'x'));
    EXPECT_TRUE(e->output_truncated);
    EXPECT_TRUE(e->success());

    // more than any pipe buffer: the child is never blocked on a full pipe
    spec = child(dir.path(), {"bytes", "5000000"});
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output.size(), 1024u * 1024u);
    EXPECT_TRUE(e->output_truncated);
    EXPECT_TRUE(e->success());

    spec = child(dir.path(), {"bytes", "1000"});
    spec.capture_limit = 1000;
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_FALSE(e->output_truncated);
}

TEST(process, standard_input_is_empty)
{
    test::temp_dir dir;
    auto e = run_process(child(dir.path(), {"stdin"}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "0\n");
}

TEST(process, exit_status)
{
    test::temp_dir dir;
    auto e = run_process(child(dir.path(), {"exit", "3"}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->code, 3);
    EXPECT_EQ(e->signal, 0);
    EXPECT_FALSE(e->success());
    EXPECT_EQ(describe_exit(*e), "exit code 3");

#ifndef _WIN32
    e = run_process(child(dir.path(), {"abort"}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->signal, SIGABRT);
    EXPECT_FALSE(e->success());
    EXPECT_EQ(describe_exit(*e), "terminated by signal " + std::to_string(SIGABRT));
#endif

    process_exit status;
    status.code = static_cast<int>(0xC0000005u);
    EXPECT_EQ(describe_exit(status), "exit code 0xC0000005");
}

#ifdef _WIN32

// docs/PLAN.md decision 74: what is left of a build ends with otok, and a crash does not wait for a dialog
TEST(process, runs_in_a_job_that_ends_with_otok)
{
    test::temp_dir dir;
    auto e = run_process(child(dir.path(), {"job"}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "kill-on-close\ndie-on-exception\n");
}

#endif

TEST(process, program_lookup)
{
    test::temp_dir dir;
    std::filesystem::path const installed = install_child(dir.path() / "bin");
    std::string const path = path_to_utf8(dir.path() / "missing") + path_list_separator + path_list_separator
        + "relative" + path_list_separator + path_to_utf8(dir.path() / "bin");

    process_spec spec;
    spec.argv = {"otok-child", "args", "found"};
    spec.cwd = dir.path();
    spec.capture = true;
    spec.env = {{"PATH", path}};
    auto e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "found\n");

    auto found = find_program("otok-child", spec.env, dir.path());
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_TRUE(std::filesystem::equivalent(*found, installed));

    // a name with a separator is a path from the working directory
    spec.env.clear();
    spec.argv[0] = "bin/otok-child";
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "found\n");

    // otok's own PATH is never used
    spec.argv[0] = "otok-child";
    e = run_process(spec);
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().message, "cannot find 'otok-child' in PATH");

    spec.argv[0] = "./missing-program";
    e = run_process(spec);
    ASSERT_FALSE(e.has_value());
    EXPECT_NE(e.error().message.find("is not an executable file"), std::string::npos) << e.error().message;

#ifdef _WIN32
    // variable names are case-insensitive
    spec.argv[0] = "otok-child";
    spec.env = {{"Path", path}};
    e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->output, "found\n");
#else
    // a file without the execute permission is skipped
    dir.write("plain/otok-child", "not a program");
    spec.argv[0] = "otok-child";
    spec.env = {{"PATH", path_to_utf8(dir.path() / "plain") + ":" + path_to_utf8(dir.path() / "bin")}};
    found = find_program("otok-child", spec.env, dir.path());
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_TRUE(std::filesystem::equivalent(*found, installed));
#endif
}

TEST(process, rejects_bad_specs)
{
    test::temp_dir dir;
    process_spec spec;
    spec.cwd = dir.path();
    EXPECT_EQ(run_process(spec).error().message, "no program to run");

    spec = child(dir.path(), {"args"});
    spec.cwd = dir.path() / "missing";
    EXPECT_EQ(run_process(spec).error().message, "the working directory '" + path_to_utf8(spec.cwd) + "' does not exist");

    spec = child(dir.path(), {"args"});
    spec.env = {{"A=B", "1"}};
    EXPECT_EQ(run_process(spec).error().message, "'A=B' is not a valid environment variable name");

    spec = child(dir.path(), {"args", std::string{"a\0b", 3}});
    EXPECT_EQ(run_process(spec).error().message, "an argument of '" + spec.argv[0] + "' contains a NUL character");

    spec = child(dir.path(), {"args"});
    spec.log = dir.path() / "missing" / "build.log";
    auto e = run_process(spec);
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().message.find("cannot open the log '" + path_to_utf8(spec.log) + "'"), 0u) << e.error().message;

#ifdef _WIN32
    spec = child(dir.path(), {"args"});
    spec.env = {{"PATH", "a"}, {"Path", "b"}};
    EXPECT_EQ(run_process(spec).error().message, "the environment variables 'PATH' and 'Path' differ only in case");
#endif
}

TEST(process, concurrent_runs)
{
    // Each child must see only its own pipe: a pipe inherited by an unrelated child would stay open
    // and its reader would wait for that child too.
    test::temp_dir dir;
    std::vector<std::future<result<process_exit>>> runs;
    for (int i = 0; i < 8; ++i) {
        runs.push_back(std::async(std::launch::async, [&dir, i] {
            return run_process(child(dir.path(), {"bytes", std::to_string(100000 + i)}));
        }));
    }
    for (int i = 0; i < 8; ++i) {
        auto e = runs[static_cast<std::size_t>(i)].get();
        ASSERT_TRUE(e.has_value()) << e.error().message;
        EXPECT_EQ(e->output.size(), static_cast<std::size_t>(100000 + i));
    }
}

#ifndef _WIN32
TEST(process, passes_a_jobserver_fifo_as_descriptors)
{
    // GNU make before 4.4 takes the tokens from descriptors 3 and 4 (docs/PLAN.md decision 61)
    test::temp_dir dir;
    auto server = jobserver::create(2, 1, dir.path(), "jobserver");
    ASSERT_TRUE(server) << server.error().message;
    process_spec spec = child(dir.path(), {"jobserver"});
    spec.jobserver_fifo = server->fifo();
    auto e = run_process(spec);
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->code, 0);
    EXPECT_EQ(e->output, "+\n");
    // the token is back
    EXPECT_TRUE(server->try_acquire(std::chrono::milliseconds{0}));
    EXPECT_FALSE(server->try_acquire(std::chrono::milliseconds{0}));

    spec.jobserver_fifo = dir.path() / "none";
    EXPECT_EQ(run_process(spec).error().message.find("cannot open the jobserver fifo '"), 0u);
}
#endif
