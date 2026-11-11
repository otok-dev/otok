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
//   semaphore N   Windows: opens the semaphore named N and prints opened, or the error
//   traverse D    Windows: opens the directory D to pass through it and prints opened, or the error
//   list D        Windows: opens the directory D to list it and prints opened, or the error
//   connect       Windows: connects to 192.0.2.1:80 (TEST-NET, never answers) and prints the Winsock error, or
//                 timeout after 2 s (an AppContainer without capabilities is refused at once: 10013)
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
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <fcntl.h>
#   include <io.h>
#   include <windows.h>
#   pragma comment(lib, "ws2_32.lib")
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
    } else if ((command == "traverse" || command == "list") && args.size() == 2) {
        std::filesystem::path const dir{std::u8string{args[1].begin(), args[1].end()}};
        HANDLE const h = ::CreateFileW(dir.c_str(), command == "traverse" ? SYNCHRONIZE | FILE_TRAVERSE : SYNCHRONIZE | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        std::cout << (h != INVALID_HANDLE_VALUE ? std::string{"opened"} : std::to_string(::GetLastError())) << '\n';
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    } else if (command == "semaphore" && args.size() == 2) {
        HANDLE const s = ::OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, std::filesystem::path{std::u8string{args[1].begin(), args[1].end()}}.c_str());
        std::cout << (s ? std::string{"opened"} : std::to_string(::GetLastError())) << '\n';
    } else if (command == "connect") {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        SOCKET const s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        u_long nonblocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonblocking);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = ::htons(80);
        ::inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
            std::cout << "connected\n";
            return 0;
        }
        int error = ::WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval timeout{2, 0};
            if (::select(0, nullptr, &writable, &failed, &timeout) == 0) {
                std::cout << "timeout\n";
                return 0;
            }
            int length = sizeof error;
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
        }
        std::cout << error << '\n';
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
