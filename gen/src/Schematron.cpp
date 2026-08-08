// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Schematron.h"

#include "FileSystem.h"

#include <algorithm>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <utility>

namespace exyoki::generator
{
namespace
{
constexpr std::string_view kAttributeNamePattern = R"([A-Za-z_][A-Za-z0-9_.-]*(?::[A-Za-z_][A-Za-z0-9_.-]*)?)";

bool Match(const std::string& text, const std::regex& pattern, std::smatch& match)
{
    return std::regex_match(text, match, pattern);
}

std::vector<std::string> Captures(const std::smatch& match)
{
    std::vector<std::string> result;
    result.reserve(match.size() > 0 ? match.size() - 1 : 0);
    for (std::size_t i = 1; i < match.size(); ++i)
    {
        result.push_back(match[i].str());
    }
    return result;
}

bool IsAttributeName(std::string_view value)
{
    if (value.size() < 2 || value.front() != '@')
    {
        return false;
    }

    return value.find(' ') == std::string_view::npos && value.find('(') == std::string_view::npos && value.find(')') == std::string_view::npos;
}

std::string StripSchematronLiteral(std::string value)
{
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') || (value.front() == '"' && value.back() == '"')))
    {
        value.erase(value.size() - 1);
        value.erase(value.begin());
    }
    return value;
}

bool TryExtractAllowedValues(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex clause("^@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");

    std::string attributeName;
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= test.size())
    {
        const auto end = test.find(" or ", start);
        const auto item = test.substr(start, end == std::string::npos ? std::string::npos : end - start);

        std::smatch match;
        if (!Match(item, clause, match))
        {
            return false;
        }

        if (attributeName.empty())
        {
            attributeName = match[1].str();
        }
        else if (attributeName != match[1].str())
        {
            return false;
        }

        values.push_back(StripSchematronLiteral(match[2].str()));
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 4;
    }

    if (attributeName.empty() || values.size() < 2)
    {
        return false;
    }

    operands.clear();
    operands.push_back(std::move(attributeName));
    operands.insert(operands.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    return true;
}

/**
 * @brief Recognises the "these attributes exclude each other" rule family.
 *
 * The imported rules record the test with its enclosing `not(...)` stripped, so
 * what reaches here is the situation the rule forbids rather than the one it
 * requires: `(@x:auto and @x:indexed) or (@x:auto and @x:rgb) or ...` on
 * `x:tabColor` means at most one of the colour attributes may be written, and
 * the single-pair form `@x14:password and @x14:algorithmName` means the same
 * for a pair. Reading the pair form as "both are required" inverts the rule and
 * rejects, among others, every `x:c` a real calculation chain contains.
 *
 * A single pair therefore needs no parentheses; anything with alternatives does,
 * because that is what tells the alternatives apart.
 */
bool TryExtractMutuallyExclusiveAttributes(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex clause("^\\(@(" + std::string(kAttributeNamePattern) + ")\\s+and\\s+@(" + std::string(kAttributeNamePattern) + ")\\)$");
    static const std::regex bareClause("^@(" + std::string(kAttributeNamePattern) + ")\\s+and\\s+@(" + std::string(kAttributeNamePattern) + ")$");

    const bool single = test.find(" or ") == std::string::npos;

    std::vector<std::string> attributes;
    std::size_t start = 0;
    while (start <= test.size())
    {
        const auto end = test.find(" or ", start);
        const auto item = test.substr(start, end == std::string::npos ? std::string::npos : end - start);

        std::smatch match;
        if (!Match(item, clause, match) && !(single && Match(item, bareClause, match)))
        {
            return false;
        }

        for (std::size_t i = 1; i <= 2; ++i)
        {
            const auto attribute = match[i].str();
            if (std::find(attributes.begin(), attributes.end(), attribute) == attributes.end())
            {
                attributes.push_back(attribute);
            }
        }

        if (end == std::string::npos)
        {
            break;
        }
        start = end + 4;
    }

    if (attributes.size() < 2)
    {
        return false;
    }

    operands = std::move(attributes);
    return true;
}

bool TryExtractForbiddenValues(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex clause("^@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");

    std::string attributeName;
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= test.size())
    {
        const auto end = test.find(" and ", start);
        const auto item = test.substr(start, end == std::string::npos ? std::string::npos : end - start);

        std::smatch match;
        if (!Match(item, clause, match))
        {
            return false;
        }

        if (attributeName.empty())
        {
            attributeName = match[1].str();
        }
        else if (attributeName != match[1].str())
        {
            return false;
        }

        values.push_back(StripSchematronLiteral(match[2].str()));
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 5;
    }

    if (attributeName.empty() || values.size() < 2)
    {
        return false;
    }

    operands.clear();
    operands.push_back(std::move(attributeName));
    operands.insert(operands.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    return true;
}

bool TryExtractParenthesizedAllowedValues(std::string_view text,
                                          std::string& attributeName,
                                          std::vector<std::string>& values)
{
    if (text.size() < 2 || text.front() != '(' || text.back() != ')')
    {
        return false;
    }

    const std::string inner(text.substr(1, text.size() - 2));
    std::vector<std::string> operands;
    if (!TryExtractAllowedValues(inner, operands))
    {
        return false;
    }

    attributeName = std::move(operands.front());
    values.assign(std::make_move_iterator(operands.begin() + 1), std::make_move_iterator(operands.end()));
    return true;
}

bool TryExtractConditionalPresence(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex presence("^@(" + std::string(kAttributeNamePattern) + ")$");
    static const std::regex equality("^@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");
    static const std::regex inequality("^@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");

    const auto separator = test.find(") or ");
    if (separator == std::string::npos || test.empty() || test.front() != '(')
    {
        return false;
    }

    const auto left = test.substr(1, separator - 1);
    const auto right = test.substr(separator + 5);
    const auto andPos = left.find(" and ");
    if (andPos == std::string::npos)
    {
        return false;
    }

    std::smatch presenceMatch;
    std::smatch equalityMatch;
    std::smatch inequalityMatch;
    const auto leftPresence = left.substr(0, andPos);
    const auto leftEquality = left.substr(andPos + 5);
    if (!Match(leftPresence, presence, presenceMatch) || !Match(leftEquality, equality, equalityMatch) || !Match(right, inequality, inequalityMatch))
    {
        return false;
    }

    if (equalityMatch[1].str() != inequalityMatch[1].str() || StripSchematronLiteral(equalityMatch[2].str()) != StripSchematronLiteral(inequalityMatch[2].str()))
    {
        return false;
    }

    operands = {presenceMatch[1].str(), equalityMatch[1].str(), StripSchematronLiteral(equalityMatch[2].str())};
    return true;
}

bool TryExtractConditionalRequiredValue(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex equality("^@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");
    static const std::regex inequality("^@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");

    const auto separator = test.find(") or ");
    if (separator == std::string::npos || test.empty() || test.front() != '(')
    {
        return false;
    }

    const auto left = test.substr(1, separator - 1);
    const auto right = test.substr(separator + 5);
    const auto andPos = left.find(" and ");
    if (andPos == std::string::npos)
    {
        return false;
    }

    std::smatch targetMatch;
    std::smatch conditionMatch;
    std::smatch inequalityMatch;
    const auto targetClause = left.substr(0, andPos);
    const auto conditionClause = left.substr(andPos + 5);
    if (!Match(targetClause, equality, targetMatch) || !Match(conditionClause, equality, conditionMatch) || !Match(right, inequality, inequalityMatch))
    {
        return false;
    }

    if (conditionMatch[1].str() != inequalityMatch[1].str() || StripSchematronLiteral(conditionMatch[2].str()) != StripSchematronLiteral(inequalityMatch[2].str()))
    {
        return false;
    }

    operands = {targetMatch[1].str(), StripSchematronLiteral(targetMatch[2].str()), conditionMatch[1].str(),
                StripSchematronLiteral(conditionMatch[2].str())};
    return true;
}

bool TryExtractConditionalAllowedValues(const std::string& test, std::vector<std::string>& operands)
{
    const auto separator = test.find(") or ");
    if (separator == std::string::npos || test.empty() || test.front() != '(')
    {
        return false;
    }

    const auto first = test.substr(1, separator - 1);
    auto second = test.substr(separator + 5);
    if (second.size() >= 2 && second.front() == '(' && second.back() == ')')
    {
        second = second.substr(1, second.size() - 2);
    }

    std::string targetAttribute;
    std::vector<std::string> targetValues;
    std::string conditionAttribute;
    std::vector<std::string> conditionValues;

    const auto andPos = first.find(" and ");
    if (andPos == std::string::npos)
    {
        return false;
    }

    const auto firstLeft = first.substr(0, andPos);
    const auto firstRight = first.substr(andPos + 5);
    if (firstLeft.starts_with("("))
    {
        if (!TryExtractParenthesizedAllowedValues(firstLeft, targetAttribute, targetValues))
        {
            return false;
        }
    }
    else
    {
        static const std::regex equality("^@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");
        std::smatch match;
        if (!Match(firstLeft, equality, match))
        {
            return false;
        }
        targetAttribute = match[1].str();
        targetValues.push_back(StripSchematronLiteral(match[2].str()));
    }

    if (firstRight.starts_with("("))
    {
        if (!TryExtractParenthesizedAllowedValues(firstRight, conditionAttribute, conditionValues))
        {
            return false;
        }
    }
    else
    {
        static const std::regex equality("^@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");
        std::smatch match;
        if (!Match(firstRight, equality, match))
        {
            return false;
        }
        conditionAttribute = match[1].str();
        conditionValues.push_back(StripSchematronLiteral(match[2].str()));
    }

    std::vector<std::string> forbiddenCondition;
    if (!TryExtractForbiddenValues(second, forbiddenCondition))
    {
        static const std::regex inequality("^@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");
        std::smatch match;
        if (Match(second, inequality, match))
        {
            forbiddenCondition = {match[1].str(), StripSchematronLiteral(match[2].str())};
        }
    }

    if (forbiddenCondition.empty() || forbiddenCondition.front() != conditionAttribute || forbiddenCondition.size() != conditionValues.size() + 1)
    {
        return false;
    }

    for (const auto& value : conditionValues)
    {
        if (std::find(forbiddenCondition.begin() + 1, forbiddenCondition.end(), value) == forbiddenCondition.end())
        {
            return false;
        }
    }

    operands.clear();
    operands.push_back(std::move(targetAttribute));
    operands.push_back(std::to_string(targetValues.size()));
    operands.insert(operands.end(), std::make_move_iterator(targetValues.begin()), std::make_move_iterator(targetValues.end()));
    operands.push_back(std::move(conditionAttribute));
    operands.push_back(std::to_string(conditionValues.size()));
    operands.insert(operands.end(), std::make_move_iterator(conditionValues.begin()), std::make_move_iterator(conditionValues.end()));
    return true;
}

bool TryExtractConditionalForbiddenValues(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex firstClause("^@(" + std::string(kAttributeNamePattern) + ")$");
    static const std::regex inequality("^@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");

    const auto firstEnd = test.find(" and ");
    if (firstEnd == std::string::npos)
    {
        return false;
    }

    // The subject string must outlive the match: std::smatch stores iterators
    // into it, so matching against a temporary leaves every sub_match dangling
    // by the time it is read below.
    const std::string firstClauseText = test.substr(0, firstEnd);

    std::smatch match;
    if (!Match(firstClauseText, firstClause, match))
    {
        return false;
    }
    const auto trigger = match[1].str();

    std::string valueAttribute;
    std::vector<std::string> values;
    std::size_t start = firstEnd + 5;
    while (start <= test.size())
    {
        const auto end = test.find(" and ", start);
        const auto item = test.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!Match(item, inequality, match))
        {
            return false;
        }
        if (valueAttribute.empty())
        {
            valueAttribute = match[1].str();
        }
        else if (valueAttribute != match[1].str())
        {
            return false;
        }
        values.push_back(StripSchematronLiteral(match[2].str()));
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 5;
    }

    if (valueAttribute.empty() || values.empty())
    {
        return false;
    }

    operands = {trigger, valueAttribute};
    operands.insert(operands.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    return true;
}

bool TryExtractTriggerAllowedValues(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex triggerPattern("^@(" + std::string(kAttributeNamePattern) + ")$");

    const auto andPos = test.find(" and ");
    if (andPos == std::string::npos)
    {
        return false;
    }

    // Named local for the same reason as in TryExtractConditionalForbiddenValues:
    // std::smatch would otherwise hold iterators into a destroyed temporary.
    const std::string triggerText = test.substr(0, andPos);

    std::smatch match;
    if (!Match(triggerText, triggerPattern, match))
    {
        return false;
    }
    const auto triggerAttribute = match[1].str();

    std::string valueAttribute;
    std::vector<std::string> values;
    if (!TryExtractParenthesizedAllowedValues(test.substr(andPos + 5), valueAttribute, values))
    {
        return false;
    }

    operands = {triggerAttribute, valueAttribute};
    operands.insert(operands.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    return true;
}

SchematronPatternKind ClassifyAttributeBooleanExpression(const std::string& test, std::vector<std::string>& operands)
{
    static const std::regex equality("^@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");
    static const std::regex inequality("^@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");
    static const std::regex presence("^@(" + std::string(kAttributeNamePattern) + ")$");
    static const std::regex implication("^@(" + std::string(kAttributeNamePattern) + ")\\s+and\\s+@(" + std::string(kAttributeNamePattern) + R"()\s*=\s*([^ ]+)$)");
    static const std::regex implicationWithInequality("^@(" + std::string(kAttributeNamePattern) + ")\\s+and\\s+@(" + std::string(kAttributeNamePattern) + R"()\s*!=\s*([^ ]+)$)");

    std::smatch match;
    if (TryExtractAllowedValues(test, operands))
    {
        return SchematronPatternKind::AttributeAllowedValues;
    }
    if (TryExtractForbiddenValues(test, operands))
    {
        return SchematronPatternKind::AttributeForbiddenValues;
    }
    if (TryExtractConditionalRequiredValue(test, operands))
    {
        return SchematronPatternKind::AttributeConditionalRequiredValue;
    }
    if (TryExtractConditionalAllowedValues(test, operands))
    {
        return SchematronPatternKind::AttributeConditionalAllowedValues;
    }
    if (TryExtractConditionalPresence(test, operands))
    {
        return SchematronPatternKind::AttributeConditionalPresence;
    }
    if (Match(test, equality, match))
    {
        operands = Captures(match);
        return SchematronPatternKind::AttributeEquality;
    }
    if (Match(test, inequality, match))
    {
        operands = Captures(match);
        return SchematronPatternKind::AttributeInequality;
    }
    if (Match(test, presence, match))
    {
        operands = Captures(match);
        return SchematronPatternKind::AttributePresence;
    }
    if (Match(test, implication, match))
    {
        operands = {match[1].str(), match[2].str(), "=", StripSchematronLiteral(match[3].str())};
        return SchematronPatternKind::AttributeImplication;
    }
    if (Match(test, implicationWithInequality, match))
    {
        operands = {match[1].str(), match[2].str(), "!=", StripSchematronLiteral(match[3].str())};
        return SchematronPatternKind::AttributeImplication;
    }
    if (TryExtractConditionalForbiddenValues(test, operands))
    {
        return SchematronPatternKind::AttributeConditionalForbiddenValues;
    }
    if (TryExtractTriggerAllowedValues(test, operands))
    {
        return SchematronPatternKind::AttributeConditionalPresenceAllowedValues;
    }
    if (TryExtractMutuallyExclusiveAttributes(test, operands))
    {
        return SchematronPatternKind::AttributeMutualExclusion;
    }
    return SchematronPatternKind::Unsupported;
}
} // namespace

std::string_view ToString(SchematronPatternKind kind) noexcept
{
    switch (kind)
    {
        case SchematronPatternKind::RelationshipExists:
            return "relationship-exists";
        case SchematronPatternKind::RelationshipType:
            return "relationship-type";
        case SchematronPatternKind::PartReferenceExists:
            return "part-reference-exists";
        case SchematronPatternKind::PartCountComparison:
            return "part-count-comparison";
        case SchematronPatternKind::UniqueValues:
            return "unique-values";
        case SchematronPatternKind::AncestorUniqueValues:
            return "ancestor-unique-values";
        case SchematronPatternKind::AttributeRegex:
            return "attribute-regex";
        case SchematronPatternKind::AttributeStringLength:
            return "attribute-string-length";
        case SchematronPatternKind::AttributeNumericComparison:
            return "attribute-numeric-comparison";
        case SchematronPatternKind::AttributeNumericRange:
            return "attribute-numeric-range";
        case SchematronPatternKind::AttributeEquality:
            return "attribute-equality";
        case SchematronPatternKind::AttributeInequality:
            return "attribute-inequality";
        case SchematronPatternKind::AttributeAllowedValues:
            return "attribute-allowed-values";
        case SchematronPatternKind::AttributePresence:
            return "attribute-presence";
        case SchematronPatternKind::AttributeImplication:
            return "attribute-implication";
        case SchematronPatternKind::AttributeMutualExclusion:
            return "attribute-mutual-exclusion";
        case SchematronPatternKind::AttributeForbiddenValues:
            return "attribute-forbidden-values";
        case SchematronPatternKind::AttributeNumericAttributeComparison:
            return "attribute-numeric-attribute-comparison";
        case SchematronPatternKind::AttributeConditionalPresence:
            return "attribute-conditional-presence";
        case SchematronPatternKind::AttributeConditionalRequiredValue:
            return "attribute-conditional-required-value";
        case SchematronPatternKind::AttributeConditionalAllowedValues:
            return "attribute-conditional-allowed-values";
        case SchematronPatternKind::AttributeConditionalForbiddenValues:
            return "attribute-conditional-forbidden-values";
        case SchematronPatternKind::AttributeConditionalPresenceAllowedValues:
            return "attribute-conditional-presence-allowed-values";
        case SchematronPatternKind::Unsupported:
            return "unsupported";
    }

    return "unsupported";
}

SchematronRule ClassifySchematronRule(std::string context,
                                      std::string test,
                                      std::string app,
                                      std::string sourceFile)
{
    SchematronRule rule{std::move(context), std::move(test), std::move(app), std::move(sourceFile)};
    std::smatch match;

    static const std::regex relationshipType("^document\\(rels\\)//r:Relationship\\[@Id = current\\(\\)/@(" + std::string(kAttributeNamePattern) + R"()\]/@Type = '([^']+)'$)");
    static const std::regex relationshipExists("^document\\(rels\\)//r:Relationship\\[@Id = current\\(\\)/@(" + std::string(kAttributeNamePattern) + R"()\]$)");
    static const std::regex partReference("^Index-of\\(document\\('([^']+)'\\)//([^,]+),\\s*@(" + std::string(kAttributeNamePattern) + ")\\)$");
    static const std::regex partCountComparison("^@(" + std::string(kAttributeNamePattern) + ")\\s*([<>]=?)\\s*count\\(document\\('([^']+)'\\)//([^)]+)\\)\\s*\\+\\s*(-?[0-9]+)$");
    static const std::regex uniqueValues(
        R"(^count\(distinct-values\((.+)\)\)\s*=\s*count\((.+)\)$)");
    static const std::regex regexMatch(
        "^matches\\(@(" + std::string(kAttributeNamePattern) + "),\\s*\"([^\"]+)\"\\)$");
    static const std::regex stringLengthRange("^string-length\\(@(" + std::string(kAttributeNamePattern) + ")\\)\\s*>=\\s*([0-9]+)\\s+and\\s+string-length\\(@\\1\\)\\s*<=\\s*([0-9]+)$");
    static const std::regex stringLengthCompare("^string-length\\(@(" + std::string(kAttributeNamePattern) + ")\\)\\s*([<>]=?)\\s*([0-9]+)$");
    const std::string numericLiteral = R"(-?(?:0x[0-9A-Fa-f]+|[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?f?))";
    const std::regex numericRange("^@(" + std::string(kAttributeNamePattern) + ")\\s*([<>]=?)\\s*(" + numericLiteral + ")\\s+and\\s+@\\1\\s*([<>]=?)\\s*(" + numericLiteral + ")$");
    const std::regex numericCompare("^@(" + std::string(kAttributeNamePattern) + ")\\s*([<>]=?)\\s*(" + numericLiteral + ")$");
    const std::regex numericAttributeCompare("^@(" + std::string(kAttributeNamePattern) + ")\\s*([<>]=?)\\s*@(" + std::string(kAttributeNamePattern) + ")$");

    if (Match(rule.test, relationshipType, match))
    {
        rule.kind = SchematronPatternKind::RelationshipType;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, relationshipExists, match))
    {
        rule.kind = SchematronPatternKind::RelationshipExists;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, partReference, match))
    {
        rule.kind = SchematronPatternKind::PartReferenceExists;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, partCountComparison, match))
    {
        rule.kind = SchematronPatternKind::PartCountComparison;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, uniqueValues, match))
    {
        rule.operands = Captures(match);
        rule.kind = rule.test.find("ancestor::") == std::string::npos
                        ? SchematronPatternKind::UniqueValues
                        : SchematronPatternKind::AncestorUniqueValues;
    }
    else if (Match(rule.test, regexMatch, match))
    {
        rule.kind = SchematronPatternKind::AttributeRegex;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, stringLengthRange, match) || Match(rule.test, stringLengthCompare, match))
    {
        rule.kind = SchematronPatternKind::AttributeStringLength;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, numericRange, match))
    {
        rule.kind = SchematronPatternKind::AttributeNumericRange;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, numericCompare, match))
    {
        rule.kind = SchematronPatternKind::AttributeNumericComparison;
        rule.operands = Captures(match);
    }
    else if (Match(rule.test, numericAttributeCompare, match))
    {
        rule.kind = SchematronPatternKind::AttributeNumericAttributeComparison;
        rule.operands = Captures(match);
    }
    else if (IsAttributeName(rule.test))
    {
        rule.kind = SchematronPatternKind::AttributePresence;
        rule.operands.push_back(rule.test.substr(1));
    }
    else
    {
        rule.kind = ClassifyAttributeBooleanExpression(rule.test, rule.operands);
    }

    return rule;
}

SchematronRule ParseSchematronRule(const JsonValue& value, std::string sourceFile)
{
    if (!value.is_object())
    {
        throw std::runtime_error("Schematron rule entry is not an object.");
    }

    return ClassifySchematronRule(value.at("Context").as_string(),
                                  value.at("Test").as_string(),
                                  value.try_get_string("App").value_or("All"),
                                  std::move(sourceFile));
}

std::vector<SchematronRule> LoadSchematronRules(const std::filesystem::path& path)
{
    const auto json = JsonValue::Parse(ReadFileText(path));
    if (!json.is_array())
    {
        throw std::runtime_error("Schematron file is expected to be an array: " + path.string());
    }

    std::vector<SchematronRule> result;
    const auto sourceFile = path.generic_string();
    for (const auto& item : json.as_array())
    {
        result.push_back(ParseSchematronRule(item, sourceFile));
    }
    return result;
}
} // namespace exyoki::generator
