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

// IoErr() codes
constexpr u32 ERROR_NO_FREE_STORE = 103;
constexpr u32 ERROR_TASK_TABLE_FULL = 105;
constexpr u32 ERROR_BAD_TEMPLATE = 114;
constexpr u32 ERROR_BAD_NUMBER = 115;
constexpr u32 ERROR_REQUIRED_ARG_MISSING = 116;
constexpr u32 ERROR_KEY_NEEDS_ARG = 117;
constexpr u32 ERROR_TOO_MANY_ARGS = 118;
constexpr u32 ERROR_UNMATCHED_QUOTES = 119;
constexpr u32 ERROR_LINE_TOO_LONG = 120;
constexpr u32 ERROR_FILE_NOT_OBJECT = 121;
constexpr u32 ERROR_INVALID_RESIDENT_LIBRARY = 122;
constexpr u32 ERROR_NO_DEFAULT_DIR = 201;
constexpr u32 ERROR_OBJECT_IN_USE = 202;
constexpr u32 ERROR_OBJECT_EXISTS = 203;
constexpr u32 ERROR_DIR_NOT_FOUND = 204;
constexpr u32 ERROR_OBJECT_NOT_FOUND = 205;
constexpr u32 ERROR_BAD_STREAM_NAME = 206;
constexpr u32 ERROR_OBJECT_TOO_LARGE = 207;
constexpr u32 ERROR_ACTION_NOT_KNOWN = 209;
constexpr u32 ERROR_INVALID_COMPONENT_NAME = 210;
constexpr u32 ERROR_INVALID_LOCK = 211;
constexpr u32 ERROR_OBJECT_WRONG_TYPE = 212;
constexpr u32 ERROR_DISK_NOT_VALIDATED = 213;
constexpr u32 ERROR_DISK_WRITE_PROTECTED = 214;
constexpr u32 ERROR_RENAME_ACROSS_DEVICES = 215;
constexpr u32 ERROR_DIRECTORY_NOT_EMPTY = 216;
constexpr u32 ERROR_TOO_MANY_LEVELS = 217;
constexpr u32 ERROR_DEVICE_NOT_MOUNTED = 218;
constexpr u32 ERROR_SEEK_ERROR = 219;
constexpr u32 ERROR_COMMENT_TOO_BIG = 220;
constexpr u32 ERROR_DISK_FULL = 221;
constexpr u32 ERROR_DELETE_PROTECTED = 222;
constexpr u32 ERROR_WRITE_PROTECTED = 223;
constexpr u32 ERROR_READ_PROTECTED = 224;
constexpr u32 ERROR_NOT_A_DOS_DISK = 225;
constexpr u32 ERROR_NO_DISK = 226;
constexpr u32 ERROR_NO_MORE_ENTRIES = 232;
constexpr u32 ERROR_IS_SOFT_LINK = 233;
constexpr u32 ERROR_OBJECT_LINKED = 234;
constexpr u32 ERROR_BAD_HUNK = 235;
constexpr u32 ERROR_NOT_IMPLEMENTED = 236;
constexpr u32 ERROR_RECORD_NOT_LOCKED = 240;
constexpr u32 ERROR_LOCK_COLLISION = 241;
constexpr u32 ERROR_LOCK_TIMEOUT = 242;
constexpr u32 ERROR_UNLOCK_ERROR = 243;
constexpr u32 ERROR_BUFFER_OVERFLOW = 303;
constexpr u32 ERROR_BREAK = 304;
constexpr u32 ERROR_NOT_EXECUTABLE = 305;

}  // namespace hombre