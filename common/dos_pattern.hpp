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

#pragma once

#include "types.hpp"

namespace hombre {

using i32 = __INT32_TYPE__;

namespace detail {

inline constexpr u32 kDosPatMaxStr = 256;
inline constexpr u32 kDosPatMaxPat = 256;
// Worst-case encoded size: 1 magic byte + up to 2 bytes per source byte + NUL.
inline constexpr u32 kDosPatMaxTokens = 2 * kDosPatMaxPat + 2;
inline constexpr u32 kDosPatMaxDepth = 32;
inline constexpr u32 kDosPatMaxSteps = 262144;

inline constexpr u8 kPatMagic = 0x01;
inline constexpr u8 kPatLit = 0x01;
inline constexpr u8 kPatAnyOne = 0x02;
inline constexpr u8 kPatAnyStr = 0x03;
inline constexpr u8 kPatRepBeg = 0x04;
inline constexpr u8 kPatRepEnd = 0x05;
inline constexpr u8 kPatNotBeg = 0x06;
inline constexpr u8 kPatNotEnd = 0x07;
inline constexpr u8 kPatOrBeg = 0x08;
inline constexpr u8 kPatOrNext = 0x09;
inline constexpr u8 kPatOrEnd = 0x0A;
inline constexpr u8 kPatEmpty = 0x0B;
inline constexpr u8 kPatClass = 0x0C;
inline constexpr u8 kPatNClass = 0x0D;
inline constexpr u8 kPatClassEnd = 0x0E;
inline constexpr u8 kPatClassRange = 0x0F;
inline constexpr u8 kPatClassLit = 0x01;

inline constexpr u8 dos_to_upper(u8 c) {
    return ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
               ? static_cast<u8>(c - 0x20)
               : c;
}

inline constexpr u8 dos_to_lower(u8 c) {
    return ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
               ? static_cast<u8>(c + 0x20)
               : c;
}

inline constexpr bool dos_chars_equal(u8 a, u8 b, bool no_case) {
    if (!no_case) {
        return a == b;
    }
    return dos_to_upper(a) == dos_to_upper(b);
}

inline constexpr bool dos_char_in_range(u8 c, u8 lo, u8 hi, bool no_case) {
    if (c >= lo && c <= hi) {
        return true;
    }
    if (!no_case) {
        return false;
    }
    const u8 up = dos_to_upper(c);
    if (up >= lo && up <= hi) {
        return true;
    }
    const u8 low = dos_to_lower(c);
    return low >= lo && low <= hi;
}

class DosPatternParser {
public:
    DosPatternParser(const u8* src, u32 src_len, u8* dst, u32 dst_size, bool no_case,
                     bool write, bool has_wild)
        : src_(src),
          src_len_(src_len),
          dst_(dst),
          dst_size_(dst_size),
          no_case_(no_case),
          write_(write),
          has_wild_(has_wild) {}

    bool parse() {
        if (write_ && has_wild_) {
            if (!put(kPatMagic)) {
                return false;
            }
        }
        if (!parse_seq(false, 0)) {
            return false;
        }
        if (src_pos_ != src_len_) {
            return false;
        }
        if (write_) {
            return put('\0');
        }
        return true;
    }

    bool has_wild() const {
        return has_wild_;
    }

private:
    bool put(u8 b) {
        if (out_pos_ >= dst_size_) {
            return false;
        }
        dst_[out_pos_++] = b;
        return true;
    }

    bool emit_op(u8 op) {
        if (!write_) {
            return true;
        }
        return put(op);
    }

    bool emit_raw(u8 b) {
        if (!write_) {
            return true;
        }
        return put(b);
    }

    bool emit_lit(u8 ch) {
        if (no_case_) {
            ch = dos_to_upper(ch);
        }
        if (!write_) {
            if (!has_wild_ && !saw_first_ && ch == kPatMagic) {
                has_wild_ = true;
            }
            saw_first_ = true;
            return true;
        }
        if (has_wild_ && ch < 0x10) {
            return put(kPatLit) && put(ch);
        }
        return put(ch);
    }

    bool emit_class_lit(u8 ch) {
        if (!write_) {
            return true;
        }
        if (ch < 0x10) {
            return put(kPatClassLit) && put(ch);
        }
        return put(ch);
    }

    bool read_class_char(u8& out) {
        if (src_pos_ >= src_len_) {
            return false;
        }
        if (src_[src_pos_] == '\'') {
            ++src_pos_;
            if (src_pos_ >= src_len_) {
                return false;
            }
        }
        out = src_[src_pos_++];
        return true;
    }

    bool parse_class() {
        ++src_pos_;
        if (src_pos_ >= src_len_) {
            return false;
        }
        if (src_[src_pos_] == '~') {
            ++src_pos_;
            if (!emit_op(kPatNClass)) {
                return false;
            }
        } else {
            if (!emit_op(kPatClass)) {
                return false;
            }
        }
        if (src_pos_ >= src_len_ || src_[src_pos_] == ']') {
            return false;
        }
        while (src_pos_ < src_len_ && src_[src_pos_] != ']') {
            u8 lo = 0;
            if (!read_class_char(lo)) {
                return false;
            }
            if (src_pos_ + 1 < src_len_ && src_[src_pos_] == '-' &&
                src_[src_pos_ + 1] != ']') {
                ++src_pos_;
                u8 hi = 0;
                if (!read_class_char(hi)) {
                    return false;
                }
                if (lo > hi) {
                    return false;
                }
                if (!emit_op(kPatClassRange) || !emit_raw(lo) || !emit_raw(hi)) {
                    return false;
                }
            } else {
                if (!emit_class_lit(lo)) {
                    return false;
                }
            }
        }
        if (src_pos_ >= src_len_ || src_[src_pos_] != ']') {
            return false;
        }
        ++src_pos_;
        return emit_op(kPatClassEnd);
    }

    bool parse_group(u32 depth) {
        if (depth >= kDosPatMaxDepth) {
            return false;
        }
        ++src_pos_;
        if (!emit_op(kPatOrBeg)) {
            return false;
        }
        for (;;) {
            if (!parse_seq(true, depth + 1)) {
                return false;
            }
            if (src_pos_ >= src_len_) {
                return false;
            }
            if (src_[src_pos_] == '|') {
                ++src_pos_;
                if (!emit_op(kPatOrNext)) {
                    return false;
                }
            } else if (src_[src_pos_] == ')') {
                ++src_pos_;
                return emit_op(kPatOrEnd);
            } else {
                return false;
            }
        }
    }

    bool parse_atom(u32 depth) {
        if (src_pos_ >= src_len_) {
            return false;
        }
        const u8 c = src_[src_pos_];
        switch (c) {
        case '?':
            has_wild_ = true;
            saw_first_ = true;
            ++src_pos_;
            return emit_op(kPatAnyOne);
        case '%':
            has_wild_ = true;
            saw_first_ = true;
            ++src_pos_;
            return emit_op(kPatEmpty);
        case '(':
            has_wild_ = true;
            saw_first_ = true;
            return parse_group(depth);
        case '[':
            has_wild_ = true;
            saw_first_ = true;
            return parse_class();
        case '#':
        case '~':
        case '|':
        case ')':
            return false;
        case '\'':
            ++src_pos_;
            if (src_pos_ >= src_len_) {
                return false;
            }
            return emit_lit(src_[src_pos_++]);
        default:
            return emit_lit(src_[src_pos_++]);
        }
    }

    bool parse_item(u32 depth) {
        if (src_pos_ >= src_len_) {
            return false;
        }
        const u8 c = src_[src_pos_];
        if (c == '#') {
            if (depth >= kDosPatMaxDepth) {
                return false;
            }
            has_wild_ = true;
            saw_first_ = true;
            ++src_pos_;
            if (src_pos_ >= src_len_) {
                return false;
            }
            if (src_[src_pos_] == '?') {
                ++src_pos_;
                return emit_op(kPatAnyStr);
            }
            if (!emit_op(kPatRepBeg)) {
                return false;
            }
            if (!parse_atom(depth + 1)) {
                return false;
            }
            return emit_op(kPatRepEnd);
        }
        if (c == '~') {
            if (depth >= kDosPatMaxDepth) {
                return false;
            }
            has_wild_ = true;
            saw_first_ = true;
            ++src_pos_;
            if (src_pos_ >= src_len_ || src_[src_pos_] == '|' || src_[src_pos_] == ')') {
                return false;
            }
            if (!emit_op(kPatNotBeg)) {
                return false;
            }
            if (!parse_item(depth + 1)) {
                return false;
            }
            return emit_op(kPatNotEnd);
        }
        return parse_atom(depth);
    }

    bool parse_seq(bool in_group, u32 depth) {
        while (src_pos_ < src_len_) {
            const u8 c = src_[src_pos_];
            if (in_group && (c == '|' || c == ')')) {
                break;
            }
            if (!in_group && c == ')') {
                return false;
            }
            if (!in_group && c == '|') {
                if (!emit_lit(src_[src_pos_++])) {
                    return false;
                }
                continue;
            }
            if (!parse_item(depth)) {
                return false;
            }
        }
        return true;
    }

    const u8* src_;
    u32 src_len_;
    u8* dst_;
    u32 dst_size_;
    bool no_case_;
    bool write_;
    bool has_wild_;
    bool saw_first_ = false;
    u32 src_pos_ = 0;
    u32 out_pos_ = 0;
};

struct PosSet {
    static constexpr u32 kWords = 5;
    u64 w[kWords]{};
};

inline void set_pos(PosSet& s, u32 p) {
    s.w[p >> 6] |= (u64{1} << (p & 63));
}

inline bool has_pos(const PosSet& s, u32 p) {
    return (s.w[p >> 6] & (u64{1} << (p & 63))) != 0;
}

inline bool pos_empty(const PosSet& s) {
    return (s.w[0] | s.w[1] | s.w[2] | s.w[3] | s.w[4]) == 0;
}

inline void pos_or(PosSet& dst, const PosSet& src) {
    for (u32 k = 0; k < PosSet::kWords; ++k) {
        dst.w[k] |= src.w[k];
    }
}

inline void mask_to_len(PosSet& s, u32 str_len) {
    const u32 n_bits = str_len + 1;
    const u32 full = n_bits >> 6;
    const u32 rem = n_bits & 63;
    if (full < PosSet::kWords) {
        if (rem > 0) {
            s.w[full] &= (u64{1} << rem) - 1;
            for (u32 k = full + 1; k < PosSet::kWords; ++k) {
                s.w[k] = 0;
            }
        } else {
            for (u32 k = full; k < PosSet::kWords; ++k) {
                s.w[k] = 0;
            }
        }
    }
}

inline PosSet range_from(u32 start_p, u32 str_len) {
    PosSet out{};
    if (start_p > str_len) {
        return out;
    }
    const u32 wk = start_p >> 6;
    out.w[wk] = ~u64{0} << (start_p & 63);
    for (u32 k = wk + 1; k < PosSet::kWords; ++k) {
        out.w[k] = ~u64{0};
    }
    mask_to_len(out, str_len);
    return out;
}

inline PosSet shift_left_1(const PosSet& src, u32 str_len) {
    PosSet out{};
    u64 carry = 0;
    for (u32 k = 0; k < PosSet::kWords; ++k) {
        const u64 v = src.w[k];
        out.w[k] = (v << 1) | carry;
        carry = v >> 63;
    }
    mask_to_len(out, str_len);
    return out;
}

inline PosSet fill_to_end(const PosSet& src, u32 str_len) {
    for (u32 k = 0; k < PosSet::kWords; ++k) {
        if (src.w[k] != 0) {
            const u32 bit = static_cast<u32>(__builtin_ctzll(src.w[k]));
            return range_from(k * 64 + bit, str_len);
        }
    }
    return PosSet{};
}

inline bool match_class_at(const u8* tok, u32 beg, u32 end, bool negated, u8 c, bool no_case) {
    bool hit = false;
    u32 p = beg;
    while (p < end) {
        if (tok[p] == kPatClassLit) {
            if (p + 1 >= end) {
                break;
            }
            if (dos_chars_equal(c, tok[p + 1], no_case)) {
                hit = true;
                break;
            }
            p += 2;
        } else if (tok[p] == kPatClassRange) {
            if (p + 2 >= end) {
                break;
            }
            if (dos_char_in_range(c, tok[p + 1], tok[p + 2], no_case)) {
                hit = true;
                break;
            }
            p += 3;
        } else {
            if (dos_chars_equal(c, tok[p], no_case)) {
                hit = true;
                break;
            }
            ++p;
        }
    }
    return negated ? !hit : hit;
}

inline u32 skip_token_step(const u8* tok, u32 p, u32 limit, u32& depth) {
    if (p >= limit) {
        return limit;
    }
    const u8 op = tok[p];
    if (op == kPatLit) {
        return (p + 2 <= limit) ? (p + 2) : limit;
    }
    if (op == kPatClass || op == kPatNClass) {
        ++p;
        while (p < limit && tok[p] != kPatClassEnd) {
            if (tok[p] == kPatClassLit) {
                p = (p + 2 <= limit) ? (p + 2) : limit;
            } else if (tok[p] == kPatClassRange) {
                p = (p + 3 <= limit) ? (p + 3) : limit;
            } else {
                ++p;
            }
        }
        return (p < limit) ? (p + 1) : limit;
    }
    if (op == kPatOrBeg || op == kPatRepBeg || op == kPatNotBeg) {
        ++depth;
    } else if (op == kPatOrEnd || op == kPatRepEnd || op == kPatNotEnd) {
        if (depth > 0) {
            --depth;
        }
    }
    return p + 1;
}

inline u32 find_matching_end(const u8* tok, u32 start, u32 limit, u8 end_tok, bool& found) {
    u32 p = start + 1;
    u32 depth = 0;
    while (p < limit) {
        if (depth == 0 && tok[p] == end_tok) {
            found = true;
            return p;
        }
        p = skip_token_step(tok, p, limit, depth);
    }
    found = false;
    return limit;
}

class DosPatternMatcher {
public:
    DosPatternMatcher(const u8* tok, const u8* str, u32 str_len, bool no_case)
        : tok_(tok), str_(str), str_len_(str_len), no_case_(no_case) {}

    PosSet eval_seq(u32 beg, u32 end, PosSet cur, u32 depth) {
        if (aborted_ || depth > kDosPatMaxDepth) {
            aborted_ = true;
            return PosSet{};
        }
        u32 pos = beg;
        while (pos < end && !pos_empty(cur)) {
            if (++steps_ > kDosPatMaxSteps) {
                aborted_ = true;
                return PosSet{};
            }
            const u8 op = tok_[pos];
            if (op == kPatAnyOne) {
                cur = shift_left_1(cur, str_len_);
                ++pos;
            } else if (op == kPatAnyStr) {
                cur = fill_to_end(cur, str_len_);
                ++pos;
            } else if (op == kPatEmpty) {
                ++pos;
            } else if (op == kPatLit) {
                if (pos + 1 >= end) {
                    aborted_ = true;
                    return PosSet{};
                }
                cur = advance_literal(cur, tok_[pos + 1]);
                pos += 2;
            } else if (op == kPatClass || op == kPatNClass) {
                const bool neg = (op == kPatNClass);
                const u32 cbeg = pos + 1;
                u32 dummy = 0;
                const u32 after = skip_token_step(tok_, pos, end, dummy);
                if (after <= cbeg || tok_[after - 1] != kPatClassEnd) {
                    aborted_ = true;
                    return PosSet{};
                }
                cur = advance_class(cur, cbeg, after - 1, neg);
                pos = after;
            } else if (op == kPatOrBeg) {
                bool found = false;
                const u32 or_end = find_matching_end(tok_, pos, end, kPatOrEnd, found);
                if (!found) {
                    aborted_ = true;
                    return PosSet{};
                }
                cur = eval_or(pos, or_end, cur, depth);
                pos = or_end + 1;
            } else if (op == kPatRepBeg) {
                bool found = false;
                const u32 rep_end = find_matching_end(tok_, pos, end, kPatRepEnd, found);
                if (!found) {
                    aborted_ = true;
                    return PosSet{};
                }
                cur = eval_rep(pos + 1, rep_end, cur, depth);
                pos = rep_end + 1;
            } else if (op == kPatNotBeg) {
                bool found = false;
                const u32 not_end = find_matching_end(tok_, pos, end, kPatNotEnd, found);
                if (!found) {
                    aborted_ = true;
                    return PosSet{};
                }
                cur = eval_not(pos + 1, not_end, cur, depth);
                pos = not_end + 1;
            } else if (op < 0x10) {
                aborted_ = true;
                return PosSet{};
            } else {
                cur = advance_literal(cur, op);
                ++pos;
            }
        }
        return aborted_ ? PosSet{} : cur;
    }

    bool aborted() const {
        return aborted_;
    }

private:
    PosSet advance_literal(const PosSet& cur, u8 ch) const {
        PosSet next{};
        for (u32 k = 0; k < PosSet::kWords; ++k) {
            u64 m = cur.w[k];
            while (m != 0) {
                const u32 bit = static_cast<u32>(__builtin_ctzll(m));
                m &= m - 1;
                const u32 p = k * 64 + bit;
                if (p < str_len_ && dos_chars_equal(str_[p], ch, no_case_)) {
                    set_pos(next, p + 1);
                }
            }
        }
        return next;
    }

    PosSet advance_class(const PosSet& cur, u32 cbeg, u32 cend, bool neg) const {
        PosSet next{};
        for (u32 k = 0; k < PosSet::kWords; ++k) {
            u64 m = cur.w[k];
            while (m != 0) {
                const u32 bit = static_cast<u32>(__builtin_ctzll(m));
                m &= m - 1;
                const u32 p = k * 64 + bit;
                if (p < str_len_ && match_class_at(tok_, cbeg, cend, neg, str_[p], no_case_)) {
                    set_pos(next, p + 1);
                }
            }
        }
        return next;
    }

    PosSet eval_or(u32 or_beg, u32 or_end, const PosSet& cur, u32 depth) {
        PosSet combined{};
        u32 b_start = or_beg + 1;
        u32 b_pos = b_start;
        u32 sub_depth = 0;
        while (b_pos <= or_end) {
            if (b_pos == or_end || (sub_depth == 0 && tok_[b_pos] == kPatOrNext)) {
                const PosSet branch = eval_seq(b_start, b_pos, cur, depth + 1);
                if (aborted_) {
                    return PosSet{};
                }
                pos_or(combined, branch);
                b_start = b_pos + 1;
                ++b_pos;
                continue;
            }
            b_pos = skip_token_step(tok_, b_pos, or_end, sub_depth);
        }
        return combined;
    }

    PosSet eval_rep(u32 beg, u32 end, const PosSet& cur, u32 depth) {
        PosSet accum = cur;
        PosSet frontier = cur;
        while (!pos_empty(frontier)) {
            if (++steps_ > kDosPatMaxSteps) {
                aborted_ = true;
                return PosSet{};
            }
            const PosSet next = eval_seq(beg, end, frontier, depth + 1);
            if (aborted_) {
                return PosSet{};
            }
            PosSet added{};
            for (u32 k = 0; k < PosSet::kWords; ++k) {
                added.w[k] = next.w[k] & ~accum.w[k];
                accum.w[k] |= added.w[k];
            }
            frontier = added;
        }
        return accum;
    }

    PosSet eval_not(u32 beg, u32 end, const PosSet& cur, u32 depth) {
        PosSet out{};
        for (u32 k = 0; k < PosSet::kWords; ++k) {
            u64 m = cur.w[k];
            while (m != 0) {
                if (++steps_ > kDosPatMaxSteps) {
                    aborted_ = true;
                    return PosSet{};
                }
                const u32 bit = static_cast<u32>(__builtin_ctzll(m));
                m &= m - 1;
                const u32 p = k * 64 + bit;
                if (p > str_len_) {
                    continue;
                }
                PosSet single{};
                set_pos(single, p);
                const PosSet matched = eval_seq(beg, end, single, depth + 1);
                if (aborted_) {
                    return PosSet{};
                }
                const PosSet valid = range_from(p, str_len_);
                for (u32 j = 0; j < PosSet::kWords; ++j) {
                    out.w[j] |= valid.w[j] & ~matched.w[j];
                }
            }
        }
        return out;
    }

    const u8* tok_;
    const u8* str_;
    u32 str_len_;
    bool no_case_;
    u32 steps_ = 0;
    bool aborted_ = false;
};

} // namespace detail

// Tokenises an AmigaDOS 3.1 pattern into dst (NUL-terminated). Returns 1 if
// the pattern contains wildcards, 0 if it is a plain name (upper-cased when
// no_case is set), or -1 on syntax error or insufficient dst_size. A buffer
// of at least 2 * src_len + 2 bytes is always sufficient for any valid pattern.
inline i32 dos_parse_pattern(const u8* src, u32 src_len, u8* dst, u32 dst_size, bool no_case) {
    if (dst == nullptr || dst_size == 0) {
        return -1;
    }
    if (src == nullptr && src_len > 0) {
        return -1;
    }
    for (u32 i = 0; i < src_len; ++i) {
        if (src[i] == '\0') {
            return -1;
        }
    }

    detail::DosPatternParser dry(src, src_len, nullptr, 0, no_case, false, false);
    if (!dry.parse()) {
        return -1;
    }
    const bool has_wild = dry.has_wild();

    detail::DosPatternParser emit(src, src_len, dst, dst_size, no_case, true, has_wild);
    if (!emit.parse()) {
        return -1;
    }
    return has_wild ? 1 : 0;
}

// Matches a parsed pattern against the whole string str[0..str_len). Uses
// Latin-1 case folding when no_case is true. Returns false on mismatch or
// over-long input (> 256 chars / > 514 token bytes).
inline bool dos_match_pattern(const u8* token, const u8* str, u32 str_len, bool no_case) {
    if (token == nullptr) {
        return false;
    }
    if (str == nullptr && str_len > 0) {
        return false;
    }
    if (str_len > detail::kDosPatMaxStr) {
        return false;
    }

    u32 tok_len = 0;
    while (tok_len <= detail::kDosPatMaxTokens && token[tok_len] != '\0') {
        ++tok_len;
    }
    if (tok_len > detail::kDosPatMaxTokens) {
        return false;
    }

    if (token[0] != detail::kPatMagic) {
        if (tok_len != str_len) {
            return false;
        }
        for (u32 i = 0; i < str_len; ++i) {
            if (!detail::dos_chars_equal(str[i], token[i], no_case)) {
                return false;
            }
        }
        return true;
    }

    detail::PosSet start{};
    detail::set_pos(start, 0);
    detail::DosPatternMatcher matcher(token, str, str_len, no_case);
    const detail::PosSet end = matcher.eval_seq(1, tok_len, start, 0);
    if (matcher.aborted()) {
        return false;
    }
    return detail::has_pos(end, str_len);
}

} // namespace hombre
