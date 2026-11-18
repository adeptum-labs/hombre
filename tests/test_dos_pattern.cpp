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
#include "dos_pattern.hpp"

#include <cstring>

using namespace hombre;

namespace {

i32 parse(const char* pat, u8* dst, u32 dst_size, bool no_case = false) {
    const u32 len = static_cast<u32>(std::strlen(pat));
    return dos_parse_pattern(reinterpret_cast<const u8*>(pat), len, dst, dst_size, no_case);
}

bool match(const u8* tok, const char* str, bool no_case = false) {
    const u32 len = static_cast<u32>(std::strlen(str));
    return dos_match_pattern(tok, reinterpret_cast<const u8*>(str), len, no_case);
}

bool parse_and_match(const char* pat, const char* str, bool no_case = false) {
    u8 tok[516]{};
    const i32 rc = parse(pat, tok, sizeof(tok), no_case);
    CHECK(rc >= 0);
    return match(tok, str, no_case);
}

void plain_name_has_no_wildcards() {
    u8 tok[64]{};
    CHECK_EQ(parse("Dir", tok, sizeof(tok), false), 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(tok), "Dir") == 0);
    CHECK(match(tok, "Dir", false));
    CHECK(!match(tok, "dir", false));
    CHECK(!match(tok, "Dir2", false));
    CHECK(!match(tok, "Di", false));

    u8 tok_nc[64]{};
    CHECK_EQ(parse("Dir", tok_nc, sizeof(tok_nc), true), 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(tok_nc), "DIR") == 0);
    CHECK(match(tok_nc, "Dir", true));
    CHECK(match(tok_nc, "dir", true));
    CHECK(match(tok_nc, "DIR", true));
    CHECK(!match(tok_nc, "dirs", true));

    // Empty pattern and literal '*'.
    u8 empty_tok[16]{};
    CHECK_EQ(parse("", empty_tok, sizeof(empty_tok), false), 0);
    CHECK(match(empty_tok, "", false));
    CHECK(!match(empty_tok, "a", false));

    u8 star_tok[32]{};
    CHECK_EQ(parse("a*b", star_tok, sizeof(star_tok), false), 0);
    CHECK(match(star_tok, "a*b", false));
    CHECK(!match(star_tok, "axb", false));
}

void question_mark_and_hash() {
    u8 tok[64]{};
    CHECK_EQ(parse("#?.info", tok, sizeof(tok), false), 1);
    CHECK(match(tok, ".info", false));
    CHECK(match(tok, "Disk.info", false));
    CHECK(!match(tok, "Disk.inf", false));
    CHECK(!match(tok, "Disk.info.bak", false));

    CHECK(parse_and_match("a?c", "abc"));
    CHECK(!parse_and_match("a?c", "ac"));
    CHECK(!parse_and_match("a?c", "abbc"));

    CHECK(parse_and_match("#a", ""));
    CHECK(parse_and_match("#a", "a"));
    CHECK(parse_and_match("#a", "aaaa"));
    CHECK(!parse_and_match("#a", "ab"));
}

void alternation_and_groups() {
    u8 tok[64]{};
    CHECK_EQ(parse("(Dir|List)", tok, sizeof(tok), false), 1);
    CHECK(match(tok, "Dir", false));
    CHECK(match(tok, "List", false));
    CHECK(!match(tok, "Type", false));
    CHECK(!match(tok, "DirList", false));

    CHECK(parse_and_match("#(ab)", ""));
    CHECK(parse_and_match("#(ab)", "ab"));
    CHECK(parse_and_match("#(ab)", "abab"));
    CHECK(!parse_and_match("#(ab)", "aba"));

    // Nested groups and empty alternative.
    CHECK(parse_and_match("((a|b)|c)d", "ad"));
    CHECK(parse_and_match("((a|b)|c)d", "bd"));
    CHECK(parse_and_match("((a|b)|c)d", "cd"));
    CHECK(!parse_and_match("((a|b)|c)d", "dd"));
    CHECK(parse_and_match("a(|b)", "a"));
    CHECK(parse_and_match("a(|b)", "ab"));
    CHECK(!parse_and_match("a(|b)", "ac"));
    CHECK(parse_and_match("#(a|%)", "aaa"));
    CHECK(parse_and_match("#(a|%)", ""));
}

void negation() {
    u8 tok[64]{};
    CHECK_EQ(parse("~(#?.info)", tok, sizeof(tok), false), 1);
    CHECK(match(tok, "Dir", false));
    CHECK(match(tok, "", false));
    CHECK(!match(tok, "Dir.info", false));
    CHECK(!match(tok, ".info", false));

    CHECK_EQ(parse("~a", tok, sizeof(tok), false), 1);
    CHECK(match(tok, "b", false));
    CHECK(match(tok, "", false));
    CHECK(match(tok, "ab", false));
    CHECK(!match(tok, "a", false));

    CHECK(parse_and_match("~(ab)c", "xyc"));
    CHECK(parse_and_match("~(ab)c", "c"));
    CHECK(!parse_and_match("~(ab)c", "abc"));

    CHECK(parse_and_match("~(~a)", "a"));
    CHECK(!parse_and_match("~(~a)", "b"));
    CHECK(!parse_and_match("~(~a)", ""));
}

void classes() {
    u8 tok[64]{};
    CHECK_EQ(parse("[a-c]x", tok, sizeof(tok), false), 1);
    CHECK(match(tok, "ax", false));
    CHECK(match(tok, "bx", false));
    CHECK(match(tok, "cx", false));
    CHECK(!match(tok, "dx", false));
    CHECK(!match(tok, "x", false));

    CHECK_EQ(parse("[~a-c]x", tok, sizeof(tok), false), 1);
    CHECK(!match(tok, "ax", false));
    CHECK(!match(tok, "bx", false));
    CHECK(!match(tok, "cx", false));
    CHECK(match(tok, "dx", false));
    CHECK(!match(tok, "x", false));

    CHECK(parse_and_match("[abc]", "b"));
    CHECK(!parse_and_match("[abc]", "d"));
    CHECK(parse_and_match("[a-]", "-"));
    CHECK(parse_and_match("[a-]", "a"));
    CHECK(parse_and_match("[']]", "]"));
    CHECK(parse_and_match("#[0-9]", ""));
    CHECK(parse_and_match("#[0-9]", "42"));
    CHECK(!parse_and_match("#[0-9]", "4a2"));
}

void percent_and_escape() {
    u8 tok[64]{};
    CHECK_EQ(parse("a%b", tok, sizeof(tok), false), 1);
    CHECK(match(tok, "ab", false));
    CHECK(!match(tok, "a%b", false));

    CHECK(parse_and_match("'?", "?"));
    CHECK(!parse_and_match("'?", "a"));
    CHECK(!parse_and_match("'?", "'?"));

    CHECK(parse_and_match("'#?", "#?"));
    CHECK(!parse_and_match("'#?", "foo"));
    CHECK(!parse_and_match("'#?", "?"));

    CHECK(parse_and_match("''", "'"));
    CHECK(parse_and_match("#'?", ""));
    CHECK(parse_and_match("#'?", "???"));
    CHECK(!parse_and_match("#'?", "a"));
}

void no_case_compares() {
    u8 tok[64]{};
    CHECK_EQ(parse("#?.INFO", tok, sizeof(tok), true), 1);
    CHECK(match(tok, "disk.info", true));
    CHECK(match(tok, "DISK.INFO", true));
    CHECK(!match(tok, "disk.inf", true));

    // Latin-1 bytes: 0xE4 ('ä') <-> 0xC4 ('Ä').
    CHECK_EQ(parse("C(\xE4)", tok, sizeof(tok), true), 1);
    CHECK(match(tok, "c\xC4", true));
    CHECK(match(tok, "c\xE4", true));
    CHECK(!match(tok, "c\xC4", false));

    // 0xF7 (division sign) must not fold to 0xD7 (multiplication sign).
    CHECK_EQ(parse("\xF7", tok, sizeof(tok), true), 0);
    CHECK(match(tok, "\xF7", true));
    CHECK(!match(tok, "\xD7", true));

    // Class ranges with case folding.
    CHECK(parse_and_match("[a-c]x", "BX", true));
    CHECK(parse_and_match("[A-C]x", "bx", true));
    CHECK(parse_and_match("[\xE0-\xE6]", "\xC4", true));
}

void syntax_errors_and_overflow() {
    u8 tok[64]{};
    CHECK_EQ(parse("(ab", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("ab)", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("[abc", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("[]", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("[~]", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("[z-a]", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("foo#", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("foo~", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("foo'", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("##a", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("(#)", tok, sizeof(tok), false), -1);
    CHECK_EQ(parse("(~)", tok, sizeof(tok), false), -1);

    // Destination buffer too small.
    CHECK_EQ(parse("Dir", tok, 0, false), -1);
    CHECK_EQ(parse("Dir", tok, 3, false), -1);
    CHECK_EQ(parse("Dir", tok, 4, false), 0);
    CHECK_EQ(parse("~a", tok, 4, false), -1);
    CHECK_EQ(parse("~a", tok, 2 * 2 + 2, false), 1);

    // 2 * len + 2 is always sufficient, even for repeated negations/control bytes.
    const u8 ctrl_pat[] = {'~', '~', 0x01, 0x02};
    u8 exact[2 * sizeof(ctrl_pat) + 2]{};
    CHECK_EQ(dos_parse_pattern(ctrl_pat, sizeof(ctrl_pat), exact, sizeof(exact), false), 1);
    const u8 ctrl_str[] = {0x01, 0x02};
    CHECK(dos_match_pattern(exact, ctrl_str, sizeof(ctrl_str), false));

    // Over-long string (300 chars) returns false without crashing.
    u8 long_str[300]{};
    std::memset(long_str, 'a', sizeof(long_str));
    CHECK_EQ(parse("#?", tok, sizeof(tok), false), 1);
    CHECK(!dos_match_pattern(tok, long_str, sizeof(long_str), false));
    CHECK(dos_match_pattern(tok, long_str, 256, false));

    // Over-long pattern (> 256 chars) also fails safely.
    u8 long_pat[300]{};
    std::memset(long_pat, 'a', sizeof(long_pat));
    u8 long_tok[2 * sizeof(long_pat) + 2]{};
    CHECK_EQ(dos_parse_pattern(long_pat, sizeof(long_pat), long_tok, sizeof(long_tok), false), 0);
    CHECK(!dos_match_pattern(long_tok, long_str, 256, false));
}

} // namespace

int main() {
    plain_name_has_no_wildcards();
    question_mark_and_hash();
    alternation_and_groups();
    negation();
    classes();
    percent_and_escape();
    no_case_compares();
    syntax_errors_and_overflow();
    return check::finish("test_dos_pattern");
}
