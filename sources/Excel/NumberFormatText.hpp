// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace ExyokiOffice::Excel
{

/**
 * @brief Formats values with a subset of Excel number-format codes.
 *
 * The subset understood by the TEXT worksheet function implementation covers
 * digit placeholders (`0`, `#`, `?`), decimal points, thousands separators and
 * trailing-comma scaling, percent, scientific notation (`E+00`), literal text
 * (quoted, escaped with `\`, or safe punctuation), the text placeholder `@`,
 * date and time tokens (`yyyy`, `yy`, `mmmm`, `mmm`, `mm`, `m`, `dddd`,
 * `ddd`, `dd`, `d`, `hh`, `h`, `ss`, `s`, `AM/PM`), the elapsed-time token
 * `[h]`, and up to four `;`-separated sections. `General` formats like the
 * cell display text.
 *
 * Unsupported codes - colors, conditions, fractions, and fill/skip tokens -
 * report std::nullopt so the caller can produce `#VALUE!`.
 */
class NumberFormatText final
{
public:
    NumberFormatText() = delete;

    /** @brief Formats a number, or std::nullopt for an unsupported code. */
    static std::optional<std::string> FormatNumber(Real value, std::string_view formatCode);
    /** @brief Formats text through the `@` placeholder section. */
    static std::optional<std::string> FormatText(std::string_view text, std::string_view formatCode);
};

} // namespace ExyokiOffice::Excel
