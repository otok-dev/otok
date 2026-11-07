// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// otok-test-child: the program run_process starts in tests/unit/process_test.cpp. It reports what it
// received, so the tests see the argument vector, environment, working directory and streams from
// the child's side. Everything is written as UTF-8 bytes, one item per line.
//
//   args A...     prints each argument
//   env NAME...   prints the value of each variable, or <unset>
//   env-all       prints every NAME=value of the environment
//   cwd           prints the working directory
//   stdin         prints the number of bytes on standard input
//   stderr TEXT   writes TEXT to standard error
//   bytes N       writes N bytes 'x' to standard output
//   exit N        exits with code N
//   abort         terminates abnormally (a signal on POSIX)
//   job           Windows: prints the limits otok sets of the job it runs in (kill-on-close, die-on-exception), or none
//   jobserver     POSIX: takes a token from descriptor 3, gives it back to descriptor 4 and prints it
//
// A first argument naming an existing file is a script it was started for as an interpreter (tests/unit/builder_test.cpp):
// it prints `script <file name>` and runs the command that follows.

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#   include <fcntl.h>
#   include <io.h>
#   include <windows.h>
#else
#   include <unistd.h>
extern char** environ;
#endif

namespace {

std::string utf8(std::filesystem::path const& p)
{
    std::u8string const s = p.u8string();
    return {reinterpret_cast<char const*>(s.data()), s.size()};
}

#ifdef _WIN32
std::string utf8(std::wstring const& w)
{
    return utf8(std::filesystem::path{w});
}
#endif

std::vector<std::string> environment()
{
    std::vector<std::string> vars;
#ifdef _WIN32
    wchar_t* block = ::GetEnvironmentStringsW();
    for (wchar_t const* p = block; *p; p += std::wcslen(p) + 1) {
        if (*p != L'=') vars.push_back(utf8(std::wstring{p}));  // skip the hidden "=C:=C:\..." entries
    }
    ::FreeEnvironmentStringsW(block);
#else
    for (char** p = environ; *p; ++p) vars.emplace_back(*p);
#endif
    return vars;
}

int run(std::vector<std::string> args)
{
    if (args.empty()) return 100;
    std::error_code ec;
    if (std::filesystem::is_regular_file(std::filesystem::path{std::u8string{args[0].begin(), args[0].end()}}, ec)) {
        std::cout << "script " << utf8(std::filesystem::path{std::u8string{args[0].begin(), args[0].end()}}.filename()) << '\n';
        args.erase(args.begin());
        if (args.empty()) return 0;
    }
    std::string const& command = args[0];
    if (command == "args") {
        for (std::size_t i = 1; i < args.size(); ++i) std::cout << args[i] << '\n';
    } else if (command == "env") {
        auto const vars = environment();
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string value = "<unset>";
            for (std::string const& v : vars) {
                if (v.size() > args[i].size() && v.compare(0, args[i].size(), args[i]) == 0 && v[args[i].size()] == '=') {
                    value = v.substr(args[i].size() + 1);
                }
            }
            std::cout << value << '\n';
        }
    } else if (command == "env-all") {
        for (std::string const& v : environment()) std::cout << v << '\n';
    } else if (command == "cwd") {
        std::cout << utf8(std::filesystem::current_path()) << '\n';
    } else if (command == "stdin") {
        std::size_t count = 0;
        while (std::cin.get() != std::char_traits<char>::eof()) ++count;
        std::cout << count << '\n';
    } else if (command == "stderr" && args.size() == 2) {
        std::cerr << args[1] << '\n';
    } else if (command == "bytes" && args.size() == 2) {
        std::cout << std::string(std::stoul(args[1]), 'x');
    } else if (command == "exit" && args.size() == 2) {
        return std::stoi(args[1]);
    } else if (command == "abort") {
        std::abort();
#ifdef _WIN32
    } else if (command == "job") {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        BOOL in_job = FALSE;
        if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) || !in_job
            || !::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof limits, nullptr)) {
            std::cout << "none\n";
            return 0;
        }
        DWORD const flags = limits.BasicLimitInformation.LimitFlags;
        if (flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) std::cout << "kill-on-close\n";
        if (flags & JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION) std::cout << "die-on-exception\n";
#endif
#ifndef _WIN32
    } else if (command == "jobserver") {
        char token = 0;
        if (::read(3, &token, 1) != 1 || ::write(4, &token, 1) != 1) return 101;
        std::cout << token << '\n';
#endif
    } else {
        return 100;
    }
    return 0;
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(utf8(std::wstring{argv[i]}));
    return run(args);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
