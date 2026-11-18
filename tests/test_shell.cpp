/*
 * The Hombre Operating System.
 * Copyright © 2026 Adam Waldenberg and Adeptum AB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Website: https://www.adeptum.se
 * Contact: info@adeptum.se
 */

#include "check.hpp"
#include "shell.hpp"

#include <string>
#include <vector>

using namespace hombre;

namespace {

struct Recorder {
    std::vector<std::string> calls;
    std::vector<std::string> paths;
    std::vector<std::string> args;
    std::vector<std::string> messages;
    std::string accept_builtin;
    bool run_result = true;
};

void host_echo(void* ctx, u8 ch) {
    Recorder& r = *static_cast<Recorder*>(ctx);
    r.calls.push_back(std::string("echo:") + static_cast<char>(ch));
}

void host_erase(void* ctx) { static_cast<Recorder*>(ctx)->calls.push_back("erase"); }

void host_end_line(void* ctx) { static_cast<Recorder*>(ctx)->calls.push_back("end_line"); }

bool host_run(void* ctx, const char* path, const char* args) {
    Recorder& r = *static_cast<Recorder*>(ctx);
    r.paths.push_back(path);
    r.args.push_back(args);
    r.calls.push_back(std::string("run:") + path + "|" + args);
    return r.run_result;
}

void host_message(void* ctx, const char* text) {
    Recorder& r = *static_cast<Recorder*>(ctx);
    r.messages.push_back(text);
    r.calls.push_back(std::string("message:") + text);
}

void host_prompt(void* ctx) { static_cast<Recorder*>(ctx)->calls.push_back("prompt"); }

void host_log_line(void* ctx, const char* text) {
    static_cast<Recorder*>(ctx)->calls.push_back(std::string("log:") + text);
}

bool host_builtin(void* ctx, const char* word, const char* args) {
    Recorder& r = *static_cast<Recorder*>(ctx);
    r.calls.push_back(std::string("builtin:") + word + "|" + args);
    return !r.accept_builtin.empty() && word == r.accept_builtin;
}

ShellHost make_host(Recorder& recorder) {
    ShellHost host{};
    host.echo = host_echo;
    host.erase = host_erase;
    host.end_line = host_end_line;
    host.run = host_run;
    host.message = host_message;
    host.prompt = host_prompt;
    host.log_line = host_log_line;
    host.ctx = &recorder;
    return host;
}

void type_text(Shell& shell, const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        shell.key(static_cast<u8>(*p));
    }
}

std::string echoed(const Recorder& r) {
    std::string out;
    for (const std::string& call : r.calls) {
        if (call.rfind("echo:", 0) == 0) {
            out.push_back(call[5]);
        }
    }
    return out;
}

std::vector<std::string> calls_with(const Recorder& r, const char* prefix) {
    std::vector<std::string> out;
    for (const std::string& call : r.calls) {
        if (call.rfind(prefix, 0) == 0) {
            out.push_back(call);
        }
    }
    return out;
}

void typing_echoes_each_character() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    shell.key('a');
    shell.key('b');
    shell.key('c');
    CHECK_EQ(r.calls.size(), 3u);
    CHECK(r.calls[0] == "echo:a");
    CHECK(r.calls[1] == "echo:b");
    CHECK(r.calls[2] == "echo:c");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 1u);
    CHECK(r.paths[0] == "C:abc");
    CHECK(r.args[0].empty());
}

void backspace_erases_and_empty_backspace_does_nothing() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    shell.key('\b');
    CHECK(r.calls.empty());
    shell.key('a');
    shell.key('b');
    shell.key('\b');
    CHECK_EQ(r.calls.size(), 3u);
    CHECK(r.calls[2] == "erase");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 1u);
    CHECK(r.paths[0] == "C:a");
}

void tab_pads_to_the_next_stop_of_8() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    shell.key('\t');
    CHECK(echoed(r) == std::string(8, ' '));
    shell.key('a');
    shell.key('\t');
    CHECK(echoed(r) == std::string(8, ' ') + "a" + std::string(7, ' '));
}

void enter_runs_c_dir_command_with_arguments() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    type_text(shell, "version  full ");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 1u);
    CHECK(r.paths[0] == "C:version");
    CHECK(r.args[0] == "full");
    const std::vector<std::string> logs = calls_with(r, "log:");
    CHECK_EQ(logs.size(), 1u);
    CHECK(logs[0] == "log:shell: line: version  full ");
    CHECK(r.calls.back() == "prompt");
}

void command_with_colon_or_slash_is_used_as_given() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    type_text(shell, "sys:c/version");
    shell.key('\n');
    type_text(shell, "ram:x");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 2u);
    CHECK(r.paths[0] == "sys:c/version");
    CHECK(r.args[0].empty());
    CHECK(r.paths[1] == "ram:x");
    CHECK(r.args[1].empty());
}

void empty_and_blank_lines_only_prompt() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    shell.key('\n');
    CHECK_EQ(r.calls.size(), 3u);
    CHECK(r.calls[0] == "end_line");
    CHECK(r.calls[1] == "log:shell: line: ");
    CHECK(r.calls[2] == "prompt");
    type_text(shell, "   ");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 0u);
    CHECK_EQ(r.messages.size(), 0u);
    CHECK(r.calls.back() == "prompt");
    CHECK(r.calls[r.calls.size() - 2] == "log:shell: line: " + std::string(3, ' '));
}

void unknown_command_message_and_log_line() {
    Recorder r;
    r.run_result = false;
    Shell shell;
    shell.init(make_host(r));
    type_text(shell, "bogus 1");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 1u);
    CHECK(r.paths[0] == "C:bogus");
    CHECK(r.args[0] == "1");
    CHECK_EQ(r.messages.size(), 1u);
    CHECK(r.messages[0] == "Unknown command bogus");
    const std::vector<std::string> logs = calls_with(r, "log:");
    CHECK_EQ(logs.size(), 2u);
    CHECK(logs[0] == "log:shell: line: bogus 1");
    CHECK(logs[1] == "log:shell: unknown command bogus");
    CHECK(r.calls.back() == "prompt");
}

void line_log_escapes_latin1() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    shell.key(static_cast<u8>(0xe5));
    shell.key(static_cast<u8>(0xe4));
    shell.key(static_cast<u8>(0xf6));
    shell.key('\n');
    const std::vector<std::string> logs = calls_with(r, "log:");
    CHECK_EQ(logs.size(), 1u);
    CHECK(logs[0] == "log:shell: line: \\xe5\\xe4\\xf6");
}

void line_is_limited_to_512_bytes() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    for (u32 i = 0; i < Shell::kMaxLine + 40; ++i) {
        shell.key('x');
    }
    CHECK_EQ(echoed(r).size(), static_cast<usize>(Shell::kMaxLine));
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 1u);
    CHECK_EQ(r.paths[0].size(), static_cast<usize>(Shell::kMaxLine) + 2);
    CHECK(r.args[0].empty());
}

void second_command_after_unknown_one_works() {
    Recorder r;
    r.run_result = false;
    Shell shell;
    shell.init(make_host(r));
    type_text(shell, "nope");
    shell.key('\n');
    r.run_result = true;
    type_text(shell, "version");
    shell.key('\n');
    CHECK_EQ(r.paths.size(), 2u);
    CHECK(r.paths[1] == "C:version");
    CHECK_EQ(r.messages.size(), 1u);
    CHECK(r.calls.back() == "prompt");
}

void control_bytes_are_ignored() {
    Recorder r;
    Shell shell;
    shell.init(make_host(r));
    const u8 ignored[] = {0x01, 0x07, 0x0d, 0x1b, 0x7f, 0x80, 0x9f};
    for (u8 c : ignored) {
        shell.key(c);
    }
    CHECK(r.calls.empty());
}

void builtin_is_tried_first() {
    Recorder r;
    r.accept_builtin = "cd";
    ShellHost host = make_host(r);
    host.builtin = host_builtin;
    Shell shell;
    shell.init(host);

    type_text(shell, "cd c");
    shell.key('\n');
    CHECK(r.paths.empty());
    CHECK(r.messages.empty());
    CHECK_EQ(calls_with(r, "builtin:").size(), 1u);
    CHECK(calls_with(r, "builtin:")[0] == "builtin:cd|c");
    CHECK(r.calls.back() == "prompt");

    type_text(shell, "version 1");
    shell.key('\n');
    CHECK_EQ(calls_with(r, "builtin:").size(), 2u);
    CHECK(calls_with(r, "builtin:")[1] == "builtin:version|1");
    CHECK_EQ(r.paths.size(), 1u);
    CHECK(r.paths[0] == "C:version");
    CHECK(r.args[0] == "1");
    CHECK(r.calls.back() == "prompt");
}

}

int main() {
    typing_echoes_each_character();
    backspace_erases_and_empty_backspace_does_nothing();
    tab_pads_to_the_next_stop_of_8();
    enter_runs_c_dir_command_with_arguments();
    command_with_colon_or_slash_is_used_as_given();
    empty_and_blank_lines_only_prompt();
    unknown_command_message_and_log_line();
    line_log_escapes_latin1();
    line_is_limited_to_512_bytes();
    second_command_after_unknown_one_works();
    control_bytes_are_ignored();
    builtin_is_tried_first();
    return check::finish("test_shell");
}
