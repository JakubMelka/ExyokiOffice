// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/TextPattern.hpp"

#include "TextPatternCompiler.hpp"

namespace ExyokiOffice
{

RegexPattern RegexPattern::Literal(std::string_view text, bool ignoreCase)
{
    RegexPattern pattern;
    pattern.Expression = Detail::EscapeRegexLiteral(text);
    pattern.IgnoreCase = ignoreCase;
    return pattern;
}

bool RegexPattern::IsValid(std::string* error) const
{
    return Detail::CompileRegex(*this, error).has_value();
}

} // namespace ExyokiOffice
