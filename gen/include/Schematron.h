// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Json.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace exyoki::generator
{
enum class SchematronPatternKind
{
    Unsupported,
    RelationshipExists,
    RelationshipType,
    PartReferenceExists,
    PartCountComparison,
    UniqueValues,
    AncestorUniqueValues,
    AttributeRegex,
    AttributeStringLength,
    AttributeNumericComparison,
    AttributeNumericRange,
    AttributeEquality,
    AttributeInequality,
    AttributeAllowedValues,
    AttributePresence,
    AttributeImplication,
    AttributeMutualExclusion,
    AttributeForbiddenValues,
    AttributeNumericAttributeComparison,
    AttributeConditionalPresence,
    AttributeConditionalRequiredValue,
    AttributeConditionalAllowedValues,
    AttributeConditionalForbiddenValues,
    AttributeConditionalPresenceAllowedValues,
};

struct SchematronRule
{
    std::string context;
    std::string test;
    std::string app;
    std::string sourceFile;
    SchematronPatternKind kind = SchematronPatternKind::Unsupported;
    std::vector<std::string> operands;
};

std::string_view ToString(SchematronPatternKind kind) noexcept;
SchematronRule ParseSchematronRule(const JsonValue& value, std::string sourceFile);
SchematronRule ClassifySchematronRule(std::string context,
                                      std::string test,
                                      std::string app,
                                      std::string sourceFile);
std::vector<SchematronRule> LoadSchematronRules(const std::filesystem::path& path);
} // namespace exyoki::generator
