// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>

namespace ExyokiOffice
{

/**
 * @brief A regular expression, as the text of the expression plus its options.
 *
 * The API takes patterns in this form rather than as a compiled `std::regex` so
 * that `<regex>` stays out of the public headers. That is not only hygiene: a
 * compiled `std::regex` in a signature makes the standard library's regex
 * implementation part of this library's ABI, and it costs every translation
 * unit that includes a Word or Excel header the whole of `<regex>` whether it
 * searches anything or not.
 *
 * The syntax is ECMAScript, matched over bytes. UTF-8 goes through unharmed
 * because the library never splits a sequence, but the character classes are
 * ASCII: `.` matches one *byte*, so it takes three of them to match `字`, and
 * @ref IgnoreCase folds `a`/`A` and nothing else. Anchor and quantify accented
 * text accordingly, or match it literally with @ref Literal.
 */
struct EXYOKIOFFICE_EXPORT RegexPattern
{
    /** @brief The ECMAScript expression. */
    std::string Expression;
    /** @brief Fold ASCII letter case while matching. */
    bool IgnoreCase = false;

    /**
     * @brief Longest text this library will run a regular expression over.
     *
     * Backtracking regular expression engines recurse, and the standard
     * libraries this builds against differ in how much: the Microsoft
     * implementation descends per input character for a repeated group, so a
     * long enough subject exhausts the thread stack. A stack overflow is not an
     * exception - it cannot be caught, and the process is gone - which makes it
     * the one failure a library used behind an MCP server, on documents the
     * operator did not write, must not be able to reach.
     *
     * Text longer than this is therefore not searched, and the call reports
     * nothing found rather than crashing. 32768 bytes is far past any real
     * paragraph or cell; a document that exceeds it is being used as a payload,
     * not as prose.
     */
    static constexpr Size MaximumSubjectLength = 32768;

    /**
     * @brief A pattern that matches @p text as itself, metacharacters included.
     *
     * Use it when the needle comes from a user or a document rather than from a
     * programmer: `C++ (v2.0)` compiled as an expression is a syntax error, and
     * `a.b` would otherwise match `axb`.
     */
    [[nodiscard]] static RegexPattern Literal(std::string_view text, bool ignoreCase = false);

    /**
     * @brief Whether the expression compiles.
     *
     * @param error Optional destination for the compiler's explanation.
     * @return True when the expression is usable; false leaves every search
     *         that uses it empty rather than throwing.
     */
    [[nodiscard]] bool IsValid(std::string* error = nullptr) const;
};

} // namespace ExyokiOffice
