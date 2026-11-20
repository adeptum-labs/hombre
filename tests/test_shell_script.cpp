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
#include "shell_script.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace hombre;

namespace {

// A host that records the lines it runs and answers from tables.
struct FakeHost {
    std::vector<std::string> lines;
    std::vector<std::string> messages;
    std::map<std::string, u32> rcs;          // return code by line
    std::set<std::string> files;
    std::map<std::string, std::string> vars;
    std::map<std::string, std::string> outputs;  // capture output by line
    u32 fail = 10;
    u32 break_after = 0xFFFFFFFF;  // lines run before Ctrl+D comes

    static FakeHost& of(void* ctx) { return *static_cast<FakeHost*>(ctx); }

    ScriptHost host() {
        ScriptHost h{};
        h.run_line = [](void* ctx, const char* line) -> u32 {
            FakeHost& f = of(ctx);
            f.lines.push_back(line);
            const auto it = f.rcs.find(line);
            return it == f.rcs.end() ? 0 : it->second;
        };
        h.exists = [](void* ctx, const char* path) { return of(ctx).files.count(path) != 0; };
        h.get_var = [](void* ctx, const char* name, char* out, u32 size) {
            const auto& vars = of(ctx).vars;
            const auto it = vars.find(name);
            if (it == vars.end()) {
                return false;
            }
            std::snprintf(out, size, "%s", it->second.c_str());
            return true;
        };
        h.capture = [](void* ctx, const char* line, char* out, u32 size) {
            const auto& outputs = of(ctx).outputs;
            const auto it = outputs.find(line);
            std::snprintf(out, size, "%s", it == outputs.end() ? "" : it->second.c_str());
            return it != outputs.end();
        };
        h.break_requested = [](void* ctx) { return of(ctx).lines.size() >= of(ctx).break_after; };
        h.fail_level = [](void* ctx) { return of(ctx).fail; };
        h.message = [](void* ctx, const char* text) { of(ctx).messages.push_back(text); };
        h.ctx = this;
        return h;
    }

    u32 run(const std::string& text, ScriptRunner::Stop* stop = nullptr) {
        ScriptRunner runner(host());
        const u32 rc = runner.run(text.c_str(), static_cast<u32>(text.size()));
        if (stop != nullptr) {
            *stop = runner.stopped();
        }
        return rc;
    }
};

using Lines = std::vector<std::string>;

void comments_and_blank_lines_are_skipped() {
    FakeHost f;
    f.run("; a comment\n\nC:Version >NIL: ; trailing\n  Echo \"a;b\"\n");
    CHECK(f.lines == (Lines{"C:Version >NIL:", "Echo \"a;b\""}));
}

void if_else_endif_picks_one_branch() {
    FakeHost f;
    f.files.insert("SYS:L");
    f.run("IF EXISTS SYS:L\n  one\nELSE\n  two\nENDIF\nIF NOT EXISTS SYS:L\n  three\n"
          "ELSE\n  four\nEndIF\n");
    CHECK(f.lines == (Lines{"one", "four"}));
}

void nested_ifs_inside_a_branch_not_taken_are_skipped_whole() {
    FakeHost f;
    f.run("IF EXISTS nothing\n IF EXISTS nothing\n  a\n ELSE\n  b\n ENDIF\n c\nELSE\n d\n"
          " IF NOT EXISTS nothing\n  e\n ENDIF\nENDIF\nf\n");
    CHECK(f.lines == (Lines{"d", "e", "f"}));
}

void eq_compares_strings_without_case() {
    FakeHost f;
    f.vars["Language"] = "English";
    f.run("IF NOT $Language EQ \"english\"\n  other\nENDIF\nIF $Language EQ english\n  same\n"
          "ENDIF\n");
    CHECK(f.lines == (Lines{"same"}));
}

void ge_with_val_compares_numbers_from_backquotes() {
    FakeHost f;
    f.outputs["C:Avail TOTAL"] = "2000000\n";
    f.run("IF \"`C:Avail TOTAL`\" GE \"1250000\" VAL\n  big\nElse\n  small\nEndIF\n"
          "IF \"900\" GE \"1250000\" VAL\n  wrong\nEndIF\nIF \"900\" GE \"1250000\"\n  text\nEndIF\n");
    CHECK(f.lines == (Lines{"big", "text"}));
}

void warn_error_and_fail_test_the_last_return_code() {
    FakeHost f;
    f.fail = 21;
    f.rcs["bad"] = 10;
    f.run("bad\nIF WARN\n w\nENDIF\nIF ERROR\n e\nENDIF\nbad\nIF FAIL\n f\nENDIF\n");
    CHECK(f.lines == (Lines{"bad", "w", "bad"}));
}

void a_return_code_at_the_fail_level_stops_the_script() {
    FakeHost f;
    f.rcs["bad"] = 10;
    ScriptRunner::Stop stop{};
    CHECK_EQ(f.run("one\nbad\ntwo\n", &stop), 10u);
    CHECK(stop == ScriptRunner::Stop::Failed);
    CHECK(f.lines == (Lines{"one", "bad"}));
    FakeHost g;
    g.fail = 21;
    g.rcs["bad"] = 20;
    g.run("bad\ntwo\n");
    CHECK(g.lines == (Lines{"bad", "two"}));
}

void skip_jumps_to_its_label_and_quit_ends() {
    FakeHost f;
    ScriptRunner::Stop stop{};
    CHECK_EQ(f.run("a\nSKIP there\nb\nLAB there\nc\nQUIT 5\nd\n", &stop), 5u);
    CHECK(stop == ScriptRunner::Stop::Quit);
    CHECK(f.lines == (Lines{"a", "c"}));
    FakeHost g;
    g.run("SKIP nowhere\nb\n", &stop);
    CHECK(stop == ScriptRunner::Stop::Failed);
    CHECK(g.lines.empty());
}

void ctrl_d_stops_the_script() {
    FakeHost f;
    f.break_after = 2;
    ScriptRunner::Stop stop{};
    f.run("a\nb\nc\nd\n", &stop);
    CHECK(stop == ScriptRunner::Stop::Break);
    CHECK(f.lines == (Lines{"a", "b"}));
    CHECK(f.messages == (Lines{"***Break\n"}));
}

void variables_and_backquotes_expand() {
    FakeHost f;
    f.vars["Workbench"] = "40.42";
    f.outputs["date"] = "today\n";
    f.run("SetEnv Workbench $Workbench\necho ${Workbench}x\necho `date` $unset $\n");
    CHECK(f.lines == (Lines{"SetEnv Workbench 40.42", "echo 40.42x", "echo today $unset $"}));
}

void dot_lines_are_ignored() {
    FakeHost f;
    f.run(".KEY name\n.BRA {\necho hi\n");
    CHECK(f.lines == (Lines{"echo hi"}));
}

}

int main() {
    comments_and_blank_lines_are_skipped();
    if_else_endif_picks_one_branch();
    nested_ifs_inside_a_branch_not_taken_are_skipped_whole();
    eq_compares_strings_without_case();
    ge_with_val_compares_numbers_from_backquotes();
    warn_error_and_fail_test_the_last_return_code();
    a_return_code_at_the_fail_level_stops_the_script();
    skip_jumps_to_its_label_and_quit_ends();
    ctrl_d_stops_the_script();
    variables_and_backquotes_expand();
    dot_lines_are_ignored();
    return check::finish("test_shell_script");
}
