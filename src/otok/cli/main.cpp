// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "otok/commands/commands.hpp"
#include "otok/core/build_info.hpp"
#include "otok/core/command_line.hpp"
#include "otok/core/console.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/jobserver.hpp"
#include "otok/core/process.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"

namespace {

void print_version()
{
    std::string_view const version = otok::build_info::version;
    std::string_view const revision = otok::build_info::revision();
    std::printf("otok %.*s (%.*s)\n",
        static_cast<int>(version.size()), version.data(),
        static_cast<int>(revision.size()), revision.data());
    std::string_view const years = otok::build_info::copyright_years;
    std::printf("Copyright (C) %.*s Alexander A Pototskiy\n"
                "License GPLv3: GNU GPL version 3 <https://gnu.org/licenses/gpl-3.0.html>,\n"
                "with the Otok Output Exception: what otok generates is yours, under terms of your choice.\n"
                "This is free software: you are free to change and redistribute it.\n"
                "There is NO WARRANTY, to the extent permitted by law.\n"
                "Source: %.*s, tag v%.*s. Third-party licenses: otok licenses\n",
        static_cast<int>(years.size()), years.data(),
        static_cast<int>(otok::build_info::source_repository.size()), otok::build_info::source_repository.data(),
        static_cast<int>(version.size()), version.data());
}

// `otok jobserver-exec [--slot <file>] [--auth <jobserver>] -- <program> <args…>`: the launcher otok.b2 puts
// before the compiler (build-lua-api.md 6.2), not a command for people, so not in the help.
int jobserver_exec(std::vector<std::string> const& args)
{
    std::filesystem::path slot;
    std::string auth;
    std::size_t i = 1;
    for (; i < args.size() && args[i] != "--"; i += 2) {
        if (i + 1 >= args.size() || (args[i] != "--slot" && args[i] != "--auth")) {
            std::cerr << "otok jobserver-exec: usage: otok jobserver-exec [--slot <file>] [--auth <jobserver>] -- <program> <args...>\n";
            return 2;
        }
        if (args[i] == "--slot") {
            slot = otok::path_from_utf8(args[i + 1]);
        } else {
            auth = args[i + 1];
        }
    }
    if (i + 1 >= args.size()) {
        std::cerr << "otok jobserver-exec: no program to run\n";
        return 2;
    }
    std::error_code ec;
    auto ran = otok::run_with_job({args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end()}, slot, auth, otok::current_environment(),
        std::filesystem::current_path(ec));
    if (!ran) {
        std::cerr << "otok jobserver-exec: " << ran.error().message << "\n";
        return 127;
    }
    return ran->signal == 0 ? ran->code : 128 + ran->signal;
}

int run(std::vector<std::string> const& args)
{
    if (!args.empty() && args[0] == "jobserver-exec") return jobserver_exec(args);
    otok::use_utf8_console();
    // whole lines: a reader of both pipes (Gradle) would get a line of one inside a line of the other, written piece
    // by piece to the unbuffered standard error
    otok::line_writer out_lines{stdout};
    otok::line_writer err_lines{stderr};
    struct restore
    {
        std::ostream& stream;
        std::streambuf* buffer;
        ~restore()
        {
            stream.flush();
            stream.rdbuf(buffer);
        }
    } const restored_out{std::cout, std::cout.rdbuf(&out_lines)}, restored_err{std::cerr, std::cerr.rdbuf(&err_lines)};
    // a flush after every output would write a line's pieces again
    std::cerr.unsetf(std::ios::unitbuf);
    auto parsed = otok::parse_command_line(args, otok::global_options(), otok::command_specs());
    if (!parsed) {
        std::cerr << "error: " << parsed.error().message << "\n";
        return 2;
    }
    if (parsed->flag("version")) {
        print_version();
        return 0;
    }
    if (parsed->command.empty() || parsed->flag("help")) {
        if (parsed->command.empty() || parsed->command == "help") {
            std::cout << otok::general_usage("otok", otok::global_options(), otok::command_specs());
        } else {
            for (otok::command_spec const& c : otok::command_specs()) {
                if (c.name == parsed->command) std::cout << otok::command_usage("otok", c);
            }
        }
        return 0;
    }

    std::error_code ec;
    std::filesystem::path project_dir = std::filesystem::current_path(ec);
    if (auto dir = parsed->value("directory")) project_dir = std::filesystem::absolute(otok::path_from_utf8(*dir), ec);
    if (ec) {
        std::cerr << "error: cannot determine the working directory: " << ec.message() << "\n";
        return 1;
    }

    otok::target_info const* const targets[] = {&otok::target::apple::info(), &otok::target::android::info(), &otok::target::windows::info(),
        &otok::target::linux::info()};
    otok::command_context ctx{project_dir, targets, nullptr, std::cout, std::cerr};
    ctx.launcher = otok::current_executable();
    ctx.terminal = otok::error_terminal();
    ctx.make_build_targets = [](otok::machine_config const& config) {
        std::vector<std::unique_ptr<otok::build_target>> built;
        // every target, whose environments say on which machines they build (common 4.2)
        built.push_back(otok::target::android::make_build_target(config.android_ndk, otok::current_environment()));
        built.push_back(otok::target::windows::make_build_target(config.visual_studio, config.windows_sdk, config.llvm, otok::current_environment()));
        built.push_back(otok::target::linux::make_build_target(otok::current_environment()));
#ifdef __APPLE__
        built.push_back(otok::target::apple::make_build_target(config.xcode, otok::current_environment()));  // Apple builds run on a Mac
#endif
        return built;
    };
    return otok::run_command(*parsed, ctx);
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(otok::path_to_utf8(std::filesystem::path{argv[i]}));
    return run(args);
}
#else
int main(int argc, char* argv[])
{
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
