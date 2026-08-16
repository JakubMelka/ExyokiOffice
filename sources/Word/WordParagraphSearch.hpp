// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Word/WordDocument.hpp"

#include <regex>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Detail
{

/**
 * @file
 * @brief Paragraph regular-expression search over an already compiled expression.
 *
 * The public API takes a @ref ExyokiOffice::RegexPattern rather than a compiled
 * `std::regex`, which is what keeps `<regex>` out of the installed headers. The
 * cost is that `Word::Paragraph::FindAllRegex` has to compile its argument, and
 * that is the right trade for one paragraph and the wrong one for a document: a
 * search across N paragraphs would build the same automaton N times, and
 * building it is the expensive half of a short match.
 *
 * Callers inside this library that walk a whole document therefore compile once
 * with @ref CompileRegex and come through here. The public methods are the
 * compile once, match once wrappers over exactly these two functions, so what
 * the public API promises - the paragraph text model, the subject-length limit
 * - is implemented once and holds for both.
 */

/// @brief As `Word::Paragraph::FindAllRegex`, over the already compiled @p expression.
[[nodiscard]] std::vector<Word::ContentRange> FindAllRegexCompiled(const Word::Paragraph& paragraph,
                                                                   const std::regex& expression);

/// @brief As `Word::Paragraph::ReplaceAllRegex`, over the already compiled @p expression.
Size ReplaceAllRegexCompiled(Word::Paragraph& paragraph,
                             const std::regex& expression,
                             std::string_view replacementFormat);

} // namespace ExyokiOffice::Detail
