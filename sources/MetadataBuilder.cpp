// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/MetadataBuilder.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "OpenXmlDiagnosticNames.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"
#include "Utf8Text.hpp"

#include <algorithm>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>

namespace ExyokiOffice
{
using Detail::DescribeQualifiedName;

/// File-local helpers behind metadata construction.
class MetadataBuilderHelper
{
public:
    /**
     * @brief Checks whether the provided text conforms to the XML Schema @c token rules.
     *
     * The function ensures there are no leading or trailing spaces, no disallowed whitespace characters,
     * and no consecutive internal spaces.
     *
     * @param value Text to validate.
     * @return True if @p value represents a valid token; otherwise false.
     */
    static bool IsTokenString(std::string_view value) noexcept
    {
        if (value.empty())
        {
            return true;
        }

        if (value.front() == ' ' || value.back() == ' ')
        {
            return false;
        }

        bool lastWasSpace = false;
        for (char ch : value)
        {
            if (ch == '\r' || ch == '\n' || ch == '\t')
            {
                return false;
            }

            if (AsciiText::IsSpace(ch) && ch != ' ')
            {
                return false;
            }

            if (ch == ' ')
            {
                if (lastWasSpace)
                {
                    return false;
                }

                lastWasSpace = true;
            }
            else
            {
                lastWasSpace = false;
            }
        }

        return true;
    }

    /**
     * @brief Validates that the input matches the simplified NCName production.
     *
     * This check applies the XML 1.0 @c NCName production: a @c Name with no colon in it. The
     * production is written over code points rather than bytes, and answering it byte by byte would
     * be wrong in both directions - it would reject every name not spelled in ASCII, and it would
     * accept a code point such as @c U+00D7 that XML forbids, writing out a document no conforming
     * parser reads back. The text is therefore decoded as UTF-8 before it is classified.
     *
     * @param value Candidate NCName.
     * @return True if @p value represents a valid NCName; otherwise false.
     */
    static bool IsNcName(std::string_view value) noexcept
    {
        return Utf8Text::IsNcName(value);
    }

    /**
     * @brief Determines whether the supplied text is a qualified name (QName).
     *
     * The function validates optional prefixes as NCNames and enforces a single separating colon.
     *
     * @param value Candidate QName.
     * @return True if @p value is a valid QName; otherwise false.
     */
    static bool IsQName(std::string_view value) noexcept
    {
        if (value.empty())
        {
            return false;
        }

        const auto colon = value.find(':');
        if (colon == std::string_view::npos)
        {
            return IsNcName(value);
        }

        if (colon == 0 || colon == value.size() - 1)
        {
            return false;
        }

        return IsNcName(value.substr(0, colon)) && IsNcName(value.substr(colon + 1));
    }

    /**
     * @brief Checks whether @p value is a plausible `xsd:anyURI`.
     *
     * `xsd:anyURI` is a URI *reference*, not an absolute URI: the empty string, a bare
     * fragment (`#ctx0`) and a relative path (`media/image1.png`) are all values of the
     * type. Requiring a scheme would reject exactly the form InkML uses for every
     * `contextRef`, `brushRef` and `traceFormatRef`, so this check only rules out text
     * that cannot be a URI reference at all: control characters and whitespace, the
     * double quote, a truncated percent-escape, and square brackets outside the
     * authority, where RFC 3986 reserves them for IPv6 literals. Everything else is
     * accepted, because the schemas do not make the validator an RFC parser.
     *
     * @param value Candidate URI reference.
     * @return True when the input can be a URI reference.
     */
    static bool IsUri(std::string_view value) noexcept
    {
        // The authority is what follows "//" up to the next path, query or fragment
        // delimiter. Only there may square brackets appear, and only as an IPv6 host.
        Size authorityBegin = std::string_view::npos;
        Size authorityEnd = std::string_view::npos;
        if (const auto slashes = value.find("//"); slashes != std::string_view::npos)
        {
            authorityBegin = slashes + 2;
            authorityEnd = value.find_first_of("/?#", authorityBegin);
            if (authorityEnd == std::string_view::npos)
            {
                authorityEnd = value.size();
            }
        }

        for (Size index = 0; index < value.size(); ++index)
        {
            const auto ch = static_cast<unsigned char>(value[index]);

            // Control characters, spaces and the quote cannot survive being written into
            // an XML attribute unescaped. Bytes above 0x7E are left alone: they carry
            // the UTF-8 of an internationalized reference.
            if (ch <= 0x20 || ch == 0x7F || ch == '"')
            {
                return false;
            }

            if (ch == '%')
            {
                if (index + 2 >= value.size() || !AsciiText::IsHexDigit(value[index + 1]) ||
                    !AsciiText::IsHexDigit(value[index + 2]))
                {
                    return false;
                }
            }

            if ((ch == '[' || ch == ']') && (index < authorityBegin || index >= authorityEnd))
            {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Reports whether the empty string is a value of the named simple type.
     *
     * A required attribute has to be *present*; the schemas say nothing about it being
     * non-empty. Whether an empty attribute still carries a value is a property of its
     * type. `xsd:string` and the binary types have the empty string in their lexical
     * space (`name=""` is a defined string, an empty hexBinary is zero octets); an
     * `xsd:list` spells zero items as the empty string, and where a list really must
     * carry something the schemas say so separately - `x:sqref` is required *and*
     * carries a `string-length(@x:sqref) >= 1` schematron rule. `ST_TrueFalseBlank`
     * names the blank in its own type name. Numbers, dates and enumerations have no
     * empty member.
     *
     * `OnOffValue` is deliberately absent even though `OnOffTextTraits` parses the
     * empty string: `ST_OnOff` is the union of `xsd:boolean` with `on`/`off`, and
     * neither branch admits a blank. The leniency belongs to the parser, not to the
     * schema, so the validator keeps reporting it.
     */
    static bool AllowsEmptyValue(std::string_view typeName) noexcept
    {
        return typeName == "StringValue" || typeName == "HexBinaryValue" || typeName == "Base64BinaryValue" ||
               typeName == "TrueFalseBlankValue" || typeName.starts_with("ListValue<");
    }

    /**
     * @brief Looks up the declared simple type of an attribute in its element's metadata.
     *
     * Returns an empty view when the element carries no metadata for the attribute, in
     * which case callers keep their conservative behavior: a constraint used on its own,
     * outside a generated element, cannot tell what the value should have been.
     */
    static std::string_view DeclaredAttributeType(const OpenXMLElement& element, const OpenXmlQualifiedName& name)
    {
        const auto* metaClass = element.ElementMetaClass();
        const auto metadata = metaClass ? metaClass->GetMetadata() : nullptr;
        if (!metadata)
        {
            return {};
        }

        for (const auto& attribute : metadata->Attributes())
        {
            if (attribute.Name == name)
            {
                return attribute.TypeName;
            }
        }

        return {};
    }

    /**
     * @brief Checks whether @p value is a well-formed `xsd:hexBinary` literal.
     *
     * Every octet is written as exactly two hexadecimal digits, so the literal has an
     * even length and contains nothing but hex digits. The empty literal denotes zero
     * octets and is valid.
     *
     * @param value Candidate hexBinary text.
     * @return True when the input encodes a whole number of octets.
     */
    static bool IsHexBinary(std::string_view value) noexcept
    {
        if (value.size() % 2 != 0)
        {
            return false;
        }

        return std::all_of(value.begin(), value.end(), [](char ch)
                           { return AsciiText::IsHexDigit(ch); });
    }

    /**
     * @brief Matches @p value against a regular expression pattern.
     * @param pattern ECMAScript regular expression.
     * @param value Text to validate.
     * @return True when the text matches the expression; false if it doesn't or the pattern is invalid.
     */
    static bool MatchPattern(const std::string& pattern, std::string_view value)
    {
        try
        {
            const std::regex expression(pattern, std::regex::ECMAScript);
            return std::regex_match(value.begin(), value.end(), expression);
        }
        catch (const std::regex_error&)
        {
            return false;
        }
    }

    static std::string_view GetLeafText(const OpenXMLElement& element) noexcept
    {
        const auto* leaf = dynamic_cast<const OpenXmlLeafTextElement*>(&element);
        return leaf ? leaf->GetText() : std::string_view{};
    }

    static bool IsAllowedEnumValue(const std::vector<MetadataEnumRule>& rules, std::string_view value)
    {
        return std::any_of(rules.begin(), rules.end(), [value](const MetadataEnumRule& rule)
                           { return std::any_of(rule.Values.begin(), rule.Values.end(), [value](const std::string& allowed)
                                                { return allowed == value; }); });
    }

    static bool ContainsStringValue(const std::vector<std::string>& values, std::string_view value)
    {
        return std::any_of(values.begin(), values.end(), [value](const std::string& item)
                           { return item == value; });
    }

    static std::string_view LocalTypeName(std::string_view qualifiedType) noexcept
    {
        const auto separator = qualifiedType.find_last_of(":/");
        return separator == std::string_view::npos ? qualifiedType : qualifiedType.substr(separator + 1);
    }

    template <typename TValue>
    static std::optional<Real> ParseNumericValue(std::string_view text)
    {
        const auto parsed = OpenXmlSimpleValueConvertor::FromString<TValue>(text);
        if (!parsed.IsDefined())
        {
            return std::nullopt;
        }
        return static_cast<Real>(parsed.Value());
    }

    static std::optional<Real> ParseNumberValidatorValue(std::string_view text, std::string_view qualifiedType)
    {
        const auto type = LocalTypeName(qualifiedType);
        if (type == "boolean")
        {
            return ParseNumericValue<BooleanValue>(text);
        }
        if (type == "byte")
        {
            return ParseNumericValue<SByteValue>(text);
        }
        if (type == "unsignedByte")
        {
            return ParseNumericValue<ByteValue>(text);
        }
        if (type == "short")
        {
            return ParseNumericValue<Int16Value>(text);
        }
        if (type == "unsignedShort")
        {
            return ParseNumericValue<UInt16Value>(text);
        }
        if (type == "int")
        {
            return ParseNumericValue<Int32Value>(text);
        }
        if (type == "unsignedInt")
        {
            return ParseNumericValue<UInt32Value>(text);
        }
        if (type == "long")
        {
            return ParseNumericValue<Int64Value>(text);
        }
        if (type == "unsignedLong" || type == "nonNegativeInteger" || type == "positiveInteger")
        {
            return ParseNumericValue<UInt64Value>(text);
        }
        if (type == "integer" || type == "nonPositiveInteger" || type == "negativeInteger")
        {
            return ParseNumericValue<IntegerValue>(text);
        }
        if (type == "float")
        {
            return ParseNumericValue<SingleValue>(text);
        }
        if (type == "decimal")
        {
            return ParseNumericValue<DecimalValue>(text);
        }
        return ParseNumericValue<DoubleValue>(text);
    }

    /**
     * @brief Reports whether a number validator's declared type has a numeric lexical space.
     *
     * The imported validators name their type as it appears in the schema, and not
     * every one of those is a number: `xne:ST_Ref` is a cell reference, `ST_Sqref`
     * a list of them, `xsd:base64Binary` and `xsd:dateTime` are not numbers either.
     * The types listed here are the ones @ref ParseNumberValidatorValue actually
     * maps; everything else reaches its `DoubleValue` fallback, which happens to
     * accept the numeric ones (`w:ST_TwipsMeasure`, `a:ST_Coordinate`) and rejects
     * the rest.
     */
    static bool IsNumericValidatorType(std::string_view qualifiedType) noexcept
    {
        static constexpr std::string_view kNumericTypes[] = {
            "boolean", "byte", "unsignedByte", "short", "unsignedShort", "int", "unsignedInt",
            "long", "unsignedLong", "nonNegativeInteger", "positiveInteger", "integer",
            "nonPositiveInteger", "negativeInteger", "float", "decimal", "double"};

        const auto type = LocalTypeName(qualifiedType);
        return std::any_of(std::begin(kNumericTypes), std::end(kNumericTypes),
                           [type](std::string_view candidate)
                           { return candidate == type; });
    }

    /**
     * @brief Reads a schematron operand in the radix the rule declared.
     *
     * A hexadecimal rule reads an `ST_LongHexNumber` value, which Word writes as
     * eight hexadecimal digits with no `0x` prefix; the prefix is accepted anyway
     * because the rule literals themselves carry it. Anything the radix does not
     * cover - a stray sign, trailing text, an empty value - stays unparsed, which
     * the callers report as a violation.
     */
    static std::optional<Real> ParseSchematronNumber(std::string_view text, MetadataSchematronNumberFormat format)
    {
        if (format != MetadataSchematronNumberFormat::Hexadecimal)
        {
            return ParseNumberValidatorValue(text, "DoubleValue");
        }

        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            text.remove_prefix(2);
        }
        if (text.empty() || text.size() > 16)
        {
            return std::nullopt;
        }

        UInt64 value = 0;
        for (const char digit : text)
        {
            const auto character = static_cast<unsigned char>(digit);
            UInt64 nibble = 0;
            if (character >= '0' && character <= '9')
            {
                nibble = static_cast<UInt64>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                nibble = static_cast<UInt64>(character - 'a') + 10;
            }
            else if (character >= 'A' && character <= 'F')
            {
                nibble = static_cast<UInt64>(character - 'A') + 10;
            }
            else
            {
                return std::nullopt;
            }
            value = (value << 4) | nibble;
        }
        return static_cast<Real>(value);
    }

    /** Compares an attribute value against a rule literal, honouring the rule's radix. */
    static bool MatchesSchematronValue(std::string_view value,
                                       const std::vector<std::string>& literals,
                                       MetadataSchematronNumberFormat format)
    {
        if (format != MetadataSchematronNumberFormat::Hexadecimal)
        {
            return ContainsStringValue(literals, value);
        }

        // `@w:val != 0x0040` states a number, not the spelling of one: the attribute
        // holds the same value written as unprefixed, zero-padded digits.
        const auto parsed = ParseSchematronNumber(value, format);
        if (!parsed)
        {
            return ContainsStringValue(literals, value);
        }
        return std::any_of(literals.begin(), literals.end(), [&](const std::string& literal)
                           {
                               const auto bound = ParseSchematronNumber(literal, format);
                               return bound ? *bound == *parsed : literal == value; });
    }

    template <typename TCallback>
    static void ForEachNumberToken(std::string_view text, bool isList, TCallback&& callback)
    {
        if (!isList)
        {
            callback(text);
            return;
        }

        Size start = 0;
        bool foundToken = false;
        while (start < text.size())
        {
            start = text.find_first_not_of(" \t\r\n", start);
            if (start == std::string_view::npos)
            {
                break;
            }
            const auto end = text.find_first_of(" \t\r\n", start);
            foundToken = true;
            callback(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        if (!foundToken)
        {
            callback(std::string_view{});
        }
    }
};

MetadataParticle::MetadataParticle(MetadataParticleKind kind,
                                   UInt32 minOccurs,
                                   std::optional<UInt32> maxOccurs,
                                   OpenXml::FileFormatVersions version,
                                   bool requireFilter) noexcept
    : kind_(kind), minOccurs_(minOccurs), maxOccurs_(maxOccurs), version_(version), requireFilter_(requireFilter)
{
}

MetadataParticleKind MetadataParticle::Kind() const noexcept
{
    return kind_;
}

UInt32 MetadataParticle::MinOccurs() const noexcept
{
    return minOccurs_;
}

std::optional<UInt32> MetadataParticle::MaxOccurs() const noexcept
{
    return maxOccurs_;
}

bool MetadataParticle::IsUnbounded() const noexcept
{
    return !maxOccurs_.has_value();
}

OpenXml::FileFormatVersions MetadataParticle::Version() const noexcept
{
    return version_;
}

bool MetadataParticle::RequiresFilter() const noexcept
{
    return requireFilter_;
}

MetadataCompositeParticle::MetadataCompositeParticle(MetadataParticleKind kind,
                                                     UInt32 minOccurs,
                                                     std::optional<UInt32> maxOccurs,
                                                     OpenXml::FileFormatVersions version,
                                                     bool requireFilter) noexcept
    : MetadataParticle(kind, minOccurs, maxOccurs, version, requireFilter)
{
}

void MetadataCompositeParticle::AddChild(const MetadataParticlePtr& particle)
{
    if (!particle)
    {
        return;
    }

    children_.push_back(particle);
}

const std::vector<MetadataParticlePtr>& MetadataCompositeParticle::Children() const noexcept
{
    return children_;
}

MetadataSequenceParticle::MetadataSequenceParticle(UInt32 minOccurs,
                                                   std::optional<UInt32> maxOccurs,
                                                   OpenXml::FileFormatVersions version,
                                                   bool requireFilter) noexcept
    : MetadataCompositeParticle(MetadataParticleKind::Sequence, minOccurs, maxOccurs, version, requireFilter)
{
}

MetadataChoiceParticle::MetadataChoiceParticle(UInt32 minOccurs,
                                               std::optional<UInt32> maxOccurs,
                                               OpenXml::FileFormatVersions version,
                                               bool requireFilter) noexcept
    : MetadataCompositeParticle(MetadataParticleKind::Choice, minOccurs, maxOccurs, version, requireFilter)
{
}

MetadataAllParticle::MetadataAllParticle(UInt32 minOccurs,
                                         std::optional<UInt32> maxOccurs,
                                         OpenXml::FileFormatVersions version,
                                         bool requireFilter) noexcept
    : MetadataCompositeParticle(MetadataParticleKind::All, minOccurs, maxOccurs, version, requireFilter)
{
}

MetadataGroupParticle::MetadataGroupParticle(UInt32 minOccurs,
                                             std::optional<UInt32> maxOccurs,
                                             OpenXml::FileFormatVersions version,
                                             bool requireFilter) noexcept
    : MetadataCompositeParticle(MetadataParticleKind::Group, minOccurs, maxOccurs, version, requireFilter)
{
}

MetadataElementParticle::MetadataElementParticle(OpenXmlQualifiedName element,
                                                 std::string elementType,
                                                 std::string propertyName,
                                                 UInt32 minOccurs,
                                                 std::optional<UInt32> maxOccurs,
                                                 OpenXml::FileFormatVersions version) noexcept
    : MetadataParticle(MetadataParticleKind::Element, minOccurs, maxOccurs, version), element_(element), elementType_(std::move(elementType)), propertyName_(std::move(propertyName))
{
}

const OpenXmlQualifiedName& MetadataElementParticle::Element() const noexcept
{
    return element_;
}

const std::string& MetadataElementParticle::ElementType() const noexcept
{
    return elementType_;
}

const std::string& MetadataElementParticle::PropertyName() const noexcept
{
    return propertyName_;
}

MetadataAnyParticle::MetadataAnyParticle(std::string wildcard,
                                         UInt32 minOccurs,
                                         std::optional<UInt32> maxOccurs,
                                         OpenXml::FileFormatVersions version) noexcept
    : MetadataParticle(MetadataParticleKind::Any, minOccurs, maxOccurs, version), wildcard_(std::move(wildcard))
{
}

const std::string& MetadataAnyParticle::Wildcard() const noexcept
{
    return wildcard_;
}

MetadataConstraint::MetadataConstraint(MetadataConstraintType type, std::string identifier) noexcept
    : type_(type), identifier_(std::move(identifier))
{
}

MetadataConstraintType MetadataConstraint::Type() const noexcept
{
    return type_;
}

const std::string& MetadataConstraint::Identifier() const noexcept
{
    return identifier_;
}

void MetadataConstraint::SetDescription(std::string description)
{
    description_ = std::move(description);
}

const std::string& MetadataConstraint::Description() const noexcept
{
    return description_;
}

void MetadataConstraint::AddAssociatedName(OpenXmlQualifiedName name)
{
    names_.push_back(name);
}

const std::vector<OpenXmlQualifiedName>& MetadataConstraint::AssociatedNames() const noexcept
{
    return names_;
}

MetadataUnionConstraint::MetadataUnionConstraint(MetadataConstraintType type, UInt32 unionId) noexcept
    : MetadataConstraint(type, "UnionValidator"), unionId_(unionId)
{
}

UInt32 MetadataUnionConstraint::UnionId() const noexcept
{
    return unionId_;
}

void MetadataUnionConstraint::AddAlternative(MetadataConstraintPtr alternative)
{
    if (alternative)
    {
        alternatives_.push_back(std::move(alternative));
    }
}

const std::vector<MetadataConstraintPtr>& MetadataUnionConstraint::Alternatives() const noexcept
{
    return alternatives_;
}

ValidationResult MetadataUnionConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult combined;
    if (alternatives_.empty())
    {
        combined.AddError(ValidationErrorId::Unknown,
                          "Union constraint does not contain any alternatives.",
                          locations.Location(element));
        return combined;
    }

    for (const auto& alternative : alternatives_)
    {
        const auto result = alternative->Validate(element, locations);
        if (result.IsValid())
        {
            return result;
        }
        combined.Merge(result);
    }
    return combined;
}

MetadataAttributeConstraint::MetadataAttributeConstraint(std::string identifier,
                                                         OpenXmlQualifiedName attributeName,
                                                         std::string propertyName)
    : MetadataConstraint(MetadataConstraintType::AttributeValue, std::move(identifier)), attributeName_(attributeName), propertyName_(std::move(propertyName))
{
}

const OpenXmlQualifiedName& MetadataAttributeConstraint::AttributeName() const noexcept
{
    return attributeName_;
}

const std::string& MetadataAttributeConstraint::PropertyName() const noexcept
{
    return propertyName_;
}

bool MetadataAttributeConstraint::TryGetAttribute(const OpenXMLElement& element, std::string_view& value) const
{
    return element.TryGetAttribute(attributeName_, value);
}

MetadataRequiredConstraint::MetadataRequiredConstraint(OpenXmlQualifiedName attributeName,
                                                       std::string propertyName,
                                                       bool required) noexcept
    : MetadataAttributeConstraint("RequiredValidator", attributeName, std::move(propertyName)), required_(required)
{
}

ValidationResult MetadataRequiredConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;

    if (!required_)
    {
        return result;
    }

    std::string_view value;
    if (!TryGetAttribute(element, value))
    {
        result.AddError(ValidationErrorId::MissingAttribute,
                        "Attribute '" + DescribeQualifiedName(AttributeName()) + "' is required.",
                        locations.Location(element, AttributeName()));
        return result;
    }

    // Being required means being present. An empty value is only an error when the
    // attribute's own type cannot represent it, which is exactly the distinction
    // the simple types draw between "set to the empty string" and "not set".
    if (value.empty() && !MetadataBuilderHelper::AllowsEmptyValue(MetadataBuilderHelper::DeclaredAttributeType(element, AttributeName())))
    {
        result.AddError(ValidationErrorId::EmptyAttribute,
                        "Attribute '" + DescribeQualifiedName(AttributeName()) + "' has no value.",
                        locations.Location(element, AttributeName()));
    }

    return result;
}

MetadataStringConstraint::MetadataStringConstraint(OpenXmlQualifiedName attributeName,
                                                   std::string propertyName,
                                                   std::optional<Size> minLength,
                                                   std::optional<Size> maxLength,
                                                   std::optional<Size> exactLength,
                                                   std::optional<std::string> pattern,
                                                   bool isToken,
                                                   bool isNcName,
                                                   bool isQName,
                                                   bool isId,
                                                   bool isUri,
                                                   bool isHexBinary) noexcept
    : MetadataAttributeConstraint("StringValidator", attributeName, std::move(propertyName)), minLength_(minLength), maxLength_(maxLength), exactLength_(exactLength), pattern_(std::move(pattern)), isToken_(isToken), isNcName_(isNcName), isQName_(isQName), isId_(isId), isUri_(isUri), isHexBinary_(isHexBinary)
{
}

ValidationResult MetadataStringConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;

    std::string_view value;
    if (!TryGetAttribute(element, value))
    {
        return result;
    }

    const auto attributeLabel = DescribeQualifiedName(AttributeName());

    // The location is only built where a facet actually fails. Building it walks
    // the ancestors and counts the same-name siblings, which is far more work
    // than the facet checks themselves, and almost every attribute passes.
    const auto appendError = [&](ValidationErrorId id, const std::string& message)
    {
        result.AddError(id, message, locations.Location(element, AttributeName()));
    };

    // Length facets of xsd:hexBinary count octets, and every octet is written as
    // two hex digits: length 3 is the six characters of a colour like C00000.
    const auto lengthUnit = isHexBinary_ ? std::string_view("octets") : std::string_view("characters");
    const Size measuredLength = isHexBinary_ ? value.size() / 2 : value.size();

    if (isHexBinary_ && !MetadataBuilderHelper::IsHexBinary(value))
    {
        appendError(ValidationErrorId::AttributePatternMismatch,
                    "Attribute '" + attributeLabel + "' must be an even-length sequence of hexadecimal digits.");
    }

    if (exactLength_.has_value() && measuredLength != *exactLength_)
    {
        appendError(ValidationErrorId::AttributeExactLengthMismatch,
                    "Attribute '" + attributeLabel + "' must have length " + std::to_string(*exactLength_) + " " +
                        std::string(lengthUnit) + ".");
    }

    if (minLength_.has_value() && measuredLength < *minLength_)
    {
        appendError(ValidationErrorId::AttributeMinLengthMismatch,
                    "Attribute '" + attributeLabel + "' must be at least " + std::to_string(*minLength_) + " " +
                        std::string(lengthUnit) + ".");
    }

    if (maxLength_.has_value() && measuredLength > *maxLength_)
    {
        appendError(ValidationErrorId::AttributeMaxLengthMismatch,
                    "Attribute '" + attributeLabel + "' must be at most " + std::to_string(*maxLength_) + " " +
                        std::string(lengthUnit) + ".");
    }

    if (pattern_.has_value() && !MetadataBuilderHelper::MatchPattern(*pattern_, value))
    {
        appendError(ValidationErrorId::AttributePatternMismatch,
                    "Attribute '" + attributeLabel + "' must match pattern '" + *pattern_ + "'.");
    }

    if (isToken_ && !MetadataBuilderHelper::IsTokenString(value))
    {
        appendError(ValidationErrorId::AttributeTokenMismatch,
                    "Attribute '" + attributeLabel + "' must be a valid XML token.");
    }

    if (isNcName_ && !MetadataBuilderHelper::IsNcName(value))
    {
        appendError(ValidationErrorId::AttributeNcNameViolation,
                    "Attribute '" + attributeLabel + "' must be a valid NCName.");
    }

    if (isQName_ && !MetadataBuilderHelper::IsQName(value))
    {
        appendError(ValidationErrorId::AttributeQNameViolation,
                    "Attribute '" + attributeLabel + "' must be a valid QName.");
    }

    if (isId_ && !MetadataBuilderHelper::IsNcName(value))
    {
        appendError(ValidationErrorId::AttributeIdViolation,
                    "Attribute '" + attributeLabel + "' must be a valid XML ID.");
    }

    if (isUri_ && !MetadataBuilderHelper::IsUri(value))
    {
        appendError(ValidationErrorId::AttributeUriViolation,
                    "Attribute '" + attributeLabel + "' must be a valid URI.");
    }

    return result;
}

MetadataNumberConstraint::MetadataNumberConstraint(OpenXmlQualifiedName attributeName,
                                                   std::string propertyName,
                                                   std::string valueType,
                                                   std::optional<Real> minInclusive,
                                                   std::optional<Real> maxInclusive,
                                                   std::optional<Real> minExclusive,
                                                   std::optional<Real> maxExclusive,
                                                   bool isPositive,
                                                   bool isNonNegative,
                                                   bool isList) noexcept
    : MetadataAttributeConstraint("NumberValidator", attributeName, std::move(propertyName)), valueType_(std::move(valueType)), minInclusive_(minInclusive), maxInclusive_(maxInclusive), minExclusive_(minExclusive), maxExclusive_(maxExclusive), isPositive_(isPositive), isNonNegative_(isNonNegative), isList_(isList)
{
}

bool MetadataNumberConstraint::HasEnforceableBounds() const noexcept
{
    return MetadataBuilderHelper::IsNumericValidatorType(valueType_) || minInclusive_ || maxInclusive_ || minExclusive_ ||
           maxExclusive_ || isPositive_ || isNonNegative_;
}

ValidationResult MetadataNumberConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;

    std::string_view text;
    if (!TryGetAttribute(element, text) || !HasEnforceableBounds())
    {
        return result;
    }

    const auto attributeLabel = DescribeQualifiedName(AttributeName());

    // The location is read only where a bound is actually broken: building one
    // walks the ancestors and scans the siblings, and most values are in range.
    MetadataBuilderHelper::ForEachNumberToken(text, isList_, [&](std::string_view token)
                                              {
        const auto parsedValue = MetadataBuilderHelper::ParseNumberValidatorValue(token, valueType_);
        if (!parsedValue)
        {
            result.AddError(ValidationErrorId::AttributeNumberParsingFailed,
                            "Attribute '" + attributeLabel + "' contains invalid value '" + std::string(token) + "'.",
                            locations.Location(element, AttributeName()));
            return;
        }

        const Real value = *parsedValue;
        if (minInclusive_ && value < *minInclusive_)
        {
            result.AddError(ValidationErrorId::AttributeMinInclusiveViolation, "Attribute '" + attributeLabel + "' is below its inclusive minimum.", locations.Location(element, AttributeName()));
        }
        if (maxInclusive_ && value > *maxInclusive_)
        {
            result.AddError(ValidationErrorId::AttributeMaxInclusiveViolation, "Attribute '" + attributeLabel + "' exceeds its inclusive maximum.", locations.Location(element, AttributeName()));
        }
        if (minExclusive_ && value <= *minExclusive_)
        {
            result.AddError(ValidationErrorId::AttributeMinExclusiveViolation, "Attribute '" + attributeLabel + "' is below its exclusive minimum.", locations.Location(element, AttributeName()));
        }
        if (maxExclusive_ && value >= *maxExclusive_)
        {
            result.AddError(ValidationErrorId::AttributeMaxExclusiveViolation, "Attribute '" + attributeLabel + "' exceeds its exclusive maximum.", locations.Location(element, AttributeName()));
        }
        if (isPositive_ && value <= 0.0)
        {
            result.AddError(ValidationErrorId::AttributePositiveViolation, "Attribute '" + attributeLabel + "' must be positive.", locations.Location(element, AttributeName()));
        }
        if (isNonNegative_ && value < 0.0)
        {
            result.AddError(ValidationErrorId::AttributeNonNegativeViolation, "Attribute '" + attributeLabel + "' must be non-negative.", locations.Location(element, AttributeName()));
        } });

    return result;
}

MetadataEnumConstraint::MetadataEnumConstraint(OpenXmlQualifiedName attributeName,
                                               std::string propertyName,
                                               ValidatorFunction validator) noexcept
    : MetadataAttributeConstraint("EnumValidator", attributeName, std::move(propertyName)), validator_(std::move(validator))
{
}

ValidationResult MetadataEnumConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;

    if (!validator_)
    {
        return result;
    }

    std::string_view value;
    if (!TryGetAttribute(element, value) || value.empty())
    {
        return result;
    }

    if (!validator_(value))
    {
        const auto location = locations.Location(element, AttributeName());
        const auto attributeLabel = DescribeQualifiedName(AttributeName());
        result.AddError(ValidationErrorId::AttributeEnumViolation,
                        "Attribute '" + attributeLabel + "' has invalid value '" + std::string(value) + "'.",
                        location);
    }

    return result;
}

MetadataAttributeEnumUnionConstraint::MetadataAttributeEnumUnionConstraint(
    OpenXmlQualifiedName attributeName,
    std::string propertyName,
    std::vector<MetadataEnumRule> rules) noexcept
    : MetadataAttributeConstraint("EnumValidator", std::move(attributeName), std::move(propertyName)), rules_(std::move(rules))
{
}

const std::vector<MetadataEnumRule>& MetadataAttributeEnumUnionConstraint::Rules() const noexcept
{
    return rules_;
}

ValidationResult MetadataAttributeEnumUnionConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (!TryGetAttribute(element, value) || value.empty() || MetadataBuilderHelper::IsAllowedEnumValue(rules_, value))
    {
        return result;
    }

    result.AddError(ValidationErrorId::AttributeEnumViolation,
                    "Attribute '" + DescribeQualifiedName(AttributeName()) + "' has invalid value '" + std::string(value) + "'.",
                    locations.Location(element, AttributeName()));
    return result;
}

MetadataTextStringConstraint::MetadataTextStringConstraint(std::optional<Size> minLength,
                                                           std::optional<Size> maxLength,
                                                           std::optional<Size> exactLength,
                                                           std::optional<std::string> pattern,
                                                           bool isToken,
                                                           bool isNcName,
                                                           bool isQName,
                                                           bool isId,
                                                           bool isUri) noexcept
    : MetadataConstraint(MetadataConstraintType::TextValue, "StringValidator"), minLength_(minLength), maxLength_(maxLength), exactLength_(exactLength), pattern_(std::move(pattern)), isToken_(isToken), isNcName_(isNcName), isQName_(isQName), isId_(isId), isUri_(isUri)
{
}

ValidationResult MetadataTextStringConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    const auto value = MetadataBuilderHelper::GetLeafText(element);

    // Deliberately inside the callback: this validator runs on every element that
    // carries text, and building a location up front would pay for the ancestor
    // walk and the sibling scan on all of them to serve the few that fail.
    const auto add = [&](ValidationErrorId id, std::string message)
    {
        result.AddError(id, std::move(message), locations.Location(element));
    };

    if (exactLength_ && value.size() != *exactLength_)
    {
        add(ValidationErrorId::TextExactLengthMismatch, "Element text must have length " + std::to_string(*exactLength_) + ".");
    }
    if (minLength_ && value.size() < *minLength_)
    {
        add(ValidationErrorId::TextMinLengthMismatch, "Element text is shorter than " + std::to_string(*minLength_) + " characters.");
    }
    if (maxLength_ && value.size() > *maxLength_)
    {
        add(ValidationErrorId::TextMaxLengthMismatch, "Element text is longer than " + std::to_string(*maxLength_) + " characters.");
    }
    if (pattern_ && !MetadataBuilderHelper::MatchPattern(*pattern_, value))
    {
        add(ValidationErrorId::TextPatternMismatch, "Element text must match pattern '" + *pattern_ + "'.");
    }
    if (isToken_ && !MetadataBuilderHelper::IsTokenString(value))
    {
        add(ValidationErrorId::TextTokenMismatch, "Element text must be a valid XML token.");
    }
    if (isNcName_ && !MetadataBuilderHelper::IsNcName(value))
    {
        add(ValidationErrorId::TextNcNameViolation, "Element text must be a valid NCName.");
    }
    if (isQName_ && !MetadataBuilderHelper::IsQName(value))
    {
        add(ValidationErrorId::TextQNameViolation, "Element text must be a valid QName.");
    }
    if (isId_ && !MetadataBuilderHelper::IsNcName(value))
    {
        add(ValidationErrorId::TextIdViolation, "Element text must be a valid XML ID.");
    }
    if (isUri_ && !MetadataBuilderHelper::IsUri(value))
    {
        add(ValidationErrorId::TextUriViolation, "Element text must be a valid URI.");
    }
    return result;
}

MetadataTextNumberConstraint::MetadataTextNumberConstraint(std::string valueType,
                                                           std::optional<Real> minInclusive,
                                                           std::optional<Real> maxInclusive,
                                                           std::optional<Real> minExclusive,
                                                           std::optional<Real> maxExclusive,
                                                           bool isPositive,
                                                           bool isNonNegative,
                                                           bool isList) noexcept
    : MetadataConstraint(MetadataConstraintType::TextValue, "NumberValidator"), valueType_(std::move(valueType)), minInclusive_(minInclusive), maxInclusive_(maxInclusive), minExclusive_(minExclusive), maxExclusive_(maxExclusive), isPositive_(isPositive), isNonNegative_(isNonNegative), isList_(isList)
{
}

bool MetadataTextNumberConstraint::HasEnforceableBounds() const noexcept
{
    return MetadataBuilderHelper::IsNumericValidatorType(valueType_) || minInclusive_ || maxInclusive_ || minExclusive_ ||
           maxExclusive_ || isPositive_ || isNonNegative_;
}

ValidationResult MetadataTextNumberConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    if (!HasEnforceableBounds())
    {
        return result;
    }

    // Read only where a number is out of range, for the same reason as in the text
    // string validator above.
    const auto validateOne = [&](std::string_view text)
    {
        const auto parsed = MetadataBuilderHelper::ParseNumberValidatorValue(text, valueType_);
        if (!parsed)
        {
            result.AddError(ValidationErrorId::TextNumberParsingFailed,
                            "Element text contains an invalid number '" + std::string(text) + "'.", locations.Location(element));
            return;
        }
        const Real value = *parsed;
        if (minInclusive_ && value < *minInclusive_)
        {
            result.AddError(ValidationErrorId::TextMinInclusiveViolation, "Element text number is below its inclusive minimum.", locations.Location(element));
        }
        if (maxInclusive_ && value > *maxInclusive_)
        {
            result.AddError(ValidationErrorId::TextMaxInclusiveViolation, "Element text number exceeds its inclusive maximum.", locations.Location(element));
        }
        if (minExclusive_ && value <= *minExclusive_)
        {
            result.AddError(ValidationErrorId::TextMinExclusiveViolation, "Element text number is below its exclusive minimum.", locations.Location(element));
        }
        if (maxExclusive_ && value >= *maxExclusive_)
        {
            result.AddError(ValidationErrorId::TextMaxExclusiveViolation, "Element text number exceeds its exclusive maximum.", locations.Location(element));
        }
        if (isPositive_ && value <= 0.0)
        {
            result.AddError(ValidationErrorId::TextPositiveViolation, "Element text number must be positive.", locations.Location(element));
        }
        if (isNonNegative_ && value < 0.0)
        {
            result.AddError(ValidationErrorId::TextNonNegativeViolation, "Element text number must be non-negative.", locations.Location(element));
        }
    };

    MetadataBuilderHelper::ForEachNumberToken(MetadataBuilderHelper::GetLeafText(element), isList_, validateOne);
    return result;
}

MetadataTextEnumConstraint::MetadataTextEnumConstraint(std::vector<MetadataEnumRule> rules) noexcept
    : MetadataConstraint(MetadataConstraintType::TextValue, "EnumValidator"), rules_(std::move(rules))
{
}

const std::vector<MetadataEnumRule>& MetadataTextEnumConstraint::Rules() const noexcept
{
    return rules_;
}

ValidationResult MetadataTextEnumConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    const auto value = MetadataBuilderHelper::GetLeafText(element);
    if (!value.empty() && !MetadataBuilderHelper::IsAllowedEnumValue(rules_, value))
    {
        result.AddError(ValidationErrorId::TextEnumViolation,
                        "Element text has invalid enumeration value '" + std::string(value) + "'.",
                        locations.Location(element));
    }
    return result;
}

MetadataOfficeVersionConstraint::MetadataOfficeVersionConstraint(OpenXmlQualifiedName attributeName,
                                                                 std::string propertyName,
                                                                 OpenXml::FileFormatVersions version) noexcept
    : MetadataAttributeConstraint("OfficeVersionValidator", attributeName, std::move(propertyName)), version_(version)
{
}

ValidationResult MetadataOfficeVersionConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;

    const auto* metaClass = element.ElementMetaClass();
    if (!metaClass)
    {
        return result;
    }

    if (!static_cast<bool>(metaClass->GetVersion() & version_))
    {
        const auto location = locations.Location(element);
        result.AddError(ValidationErrorId::AttributeVersionViolation,
                        "Element '" + DescribeQualifiedName(metaClass->QualifiedName()) + "' is not available in the requested Office version.",
                        location);
    }

    return result;
}

OpenXml::FileFormatVersions MetadataOfficeVersionConstraint::Version() const noexcept
{
    return version_;
}

bool EvaluateComparison(Real lhs, MetadataSchematronComparisonOperator comparison, Real rhs) noexcept
{
    switch (comparison)
    {
        case MetadataSchematronComparisonOperator::LessThan:
            return lhs < rhs;
        case MetadataSchematronComparisonOperator::LessThanOrEqual:
            return lhs <= rhs;
        case MetadataSchematronComparisonOperator::GreaterThan:
            return lhs > rhs;
        case MetadataSchematronComparisonOperator::GreaterThanOrEqual:
            return lhs >= rhs;
    }
    return false;
}

MetadataSchematronAttributeConstraint::MetadataSchematronAttributeConstraint(OpenXmlQualifiedName attributeName,
                                                                             std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronValidator", std::move(attributeName), std::move(testExpression))
{
}

MetadataSchematronAttributeConstraint::MetadataSchematronAttributeConstraint(std::string identifier,
                                                                             OpenXmlQualifiedName attributeName,
                                                                             std::string testExpression) noexcept
    : MetadataAttributeConstraint(std::move(identifier), std::move(attributeName), {}), testExpression_(std::move(testExpression))
{
}

const std::string& MetadataSchematronAttributeConstraint::TestExpression() const noexcept
{
    return testExpression_;
}

void MetadataSchematronAttributeConstraint::AddSchematronViolation(ValidationResult& result,
                                                                   const OpenXMLElement& element,
                                                                   XmlLocationCache& locations) const
{
    result.AddError(ValidationErrorId::SchematronConstraintViolation,
                    "Schematron rule '" + testExpression_ + "' failed for attribute '" + DescribeQualifiedName(AttributeName()) + "'.",
                    locations.Location(element, AttributeName()));
}

MetadataSchematronAttributePresenceConstraint::MetadataSchematronAttributePresenceConstraint(
    OpenXmlQualifiedName attributeName,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributePresence", std::move(attributeName), std::move(testExpression))
{
}

ValidationResult MetadataSchematronAttributePresenceConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (!TryGetAttribute(element, value))
    {
        result.AddError(ValidationErrorId::MissingAttribute,
                        "Attribute '" + DescribeQualifiedName(AttributeName()) + "' is required by schematron rule '" + TestExpression() + "'.",
                        locations.Location(element, AttributeName()));
        return result;
    }
    return result;
}

struct MetadataSchematronAttributeRegexConstraint::RegexState
{
    explicit RegexState(std::string pattern)
        : Pattern(std::move(pattern))
    {
        try
        {
            Expression.emplace(Pattern, std::regex::ECMAScript);
        }
        catch (const std::regex_error& error)
        {
            CompileError = error.what();
        }
    }

    std::string Pattern;
    std::optional<std::regex> Expression;
    std::string CompileError;
};

MetadataSchematronAttributeRegexConstraint::MetadataSchematronAttributeRegexConstraint(
    OpenXmlQualifiedName attributeName,
    std::string pattern,
    std::string testExpression)
    : MetadataSchematronAttributeConstraint("SchematronAttributeRegex", std::move(attributeName), std::move(testExpression)), regex_(std::make_shared<RegexState>(std::move(pattern)))
{
}

ValidationResult MetadataSchematronAttributeRegexConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (TryGetAttribute(element, value) && (!regex_ || !regex_->Expression || !std::regex_match(value.begin(), value.end(), *regex_->Expression)))
    {
        if (regex_ && !regex_->Expression)
        {
            result.AddError(ValidationErrorId::SchematronRuleNotEvaluable,
                            "Schematron rule '" + TestExpression() + "' for attribute '" + DescribeQualifiedName(AttributeName()) + "' could not be evaluated: pattern '" + regex_->Pattern + "' failed to compile (" + regex_->CompileError + ").",
                            locations.Location(element, AttributeName()));
        }
        else
        {
            AddSchematronViolation(result, element, locations);
        }
    }
    return result;
}

MetadataSchematronAttributeStringLengthConstraint::MetadataSchematronAttributeStringLengthConstraint(
    OpenXmlQualifiedName attributeName,
    Size minLength,
    Size maxLength,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeStringLength", std::move(attributeName), std::move(testExpression)), minLength_(minLength), maxLength_(maxLength)
{
}

MetadataSchematronAttributeStringLengthConstraint::MetadataSchematronAttributeStringLengthConstraint(
    OpenXmlQualifiedName attributeName,
    MetadataSchematronComparisonOperator comparison,
    Size limit,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeStringLength", std::move(attributeName), std::move(testExpression)), comparison_(comparison), limit_(limit)
{
}

ValidationResult MetadataSchematronAttributeStringLengthConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (!TryGetAttribute(element, value))
    {
        return result;
    }

    bool ok = true;
    if (comparison_ && limit_)
    {
        ok = EvaluateComparison(static_cast<Real>(value.size()), *comparison_, static_cast<Real>(*limit_));
    }
    else if (minLength_ && maxLength_)
    {
        ok = value.size() >= *minLength_ && value.size() <= *maxLength_;
    }

    if (!ok)
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeNumericRangeConstraint::MetadataSchematronAttributeNumericRangeConstraint(
    OpenXmlQualifiedName attributeName,
    Real minInclusive,
    Real maxInclusive,
    std::string testExpression,
    MetadataSchematronNumberFormat numberFormat) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeNumericRange", std::move(attributeName), std::move(testExpression)), minInclusive_(minInclusive), maxInclusive_(maxInclusive), numberFormat_(numberFormat)
{
}

MetadataSchematronAttributeNumericRangeConstraint::MetadataSchematronAttributeNumericRangeConstraint(
    OpenXmlQualifiedName attributeName,
    MetadataSchematronComparisonOperator lowerComparison,
    Real lowerValue,
    MetadataSchematronComparisonOperator upperComparison,
    Real upperValue,
    std::string testExpression,
    MetadataSchematronNumberFormat numberFormat) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeNumericRange", std::move(attributeName), std::move(testExpression)), minInclusive_(lowerValue), maxInclusive_(upperValue), lowerComparison_(lowerComparison), lowerValue_(lowerValue), upperComparison_(upperComparison), upperValue_(upperValue), numberFormat_(numberFormat)
{
}

ValidationResult MetadataSchematronAttributeNumericRangeConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (TryGetAttribute(element, value))
    {
        const auto parsed = MetadataBuilderHelper::ParseSchematronNumber(value, numberFormat_);
        bool ok = false;
        if (parsed)
        {
            if (lowerComparison_ && lowerValue_ && upperComparison_ && upperValue_)
            {
                ok = EvaluateComparison(*parsed, *lowerComparison_, *lowerValue_) && EvaluateComparison(*parsed, *upperComparison_, *upperValue_);
            }
            else
            {
                ok = *parsed >= minInclusive_ && *parsed <= maxInclusive_;
            }
        }

        if (!ok)
        {
            AddSchematronViolation(result, element, locations);
        }
    }
    return result;
}

MetadataSchematronAttributeNumericComparisonConstraint::MetadataSchematronAttributeNumericComparisonConstraint(
    OpenXmlQualifiedName attributeName,
    MetadataSchematronComparisonOperator comparison,
    Real value,
    std::string testExpression,
    MetadataSchematronNumberFormat numberFormat) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeNumericComparison", std::move(attributeName), std::move(testExpression)), comparison_(comparison), value_(value), numberFormat_(numberFormat)
{
}

ValidationResult MetadataSchematronAttributeNumericComparisonConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (TryGetAttribute(element, value))
    {
        const auto parsed = MetadataBuilderHelper::ParseSchematronNumber(value, numberFormat_);
        if (!parsed || !EvaluateComparison(*parsed, comparison_, value_))
        {
            AddSchematronViolation(result, element, locations);
        }
    }
    return result;
}

MetadataSchematronAttributeNumericAttributeComparisonConstraint::
    MetadataSchematronAttributeNumericAttributeComparisonConstraint(OpenXmlQualifiedName leftAttributeName,
                                                                    MetadataSchematronComparisonOperator comparison,
                                                                    OpenXmlQualifiedName rightAttributeName,
                                                                    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeNumericAttributeComparison", std::move(leftAttributeName), std::move(testExpression)), comparison_(comparison), rightAttributeName_(std::move(rightAttributeName))
{
}

ValidationResult MetadataSchematronAttributeNumericAttributeComparisonConstraint::Validate(
    const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view leftValue;
    std::string_view rightValue;
    if (!TryGetAttribute(element, leftValue) || !element.TryGetAttribute(rightAttributeName_, rightValue))
    {
        return result;
    }

    const auto left = MetadataBuilderHelper::ParseNumberValidatorValue(leftValue, "DoubleValue");
    const auto right = MetadataBuilderHelper::ParseNumberValidatorValue(rightValue, "DoubleValue");
    if (!left || !right || !EvaluateComparison(*left, comparison_, *right))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeEqualityConstraint::MetadataSchematronAttributeEqualityConstraint(
    OpenXmlQualifiedName attributeName,
    std::string expectedValue,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeEquality", std::move(attributeName), std::move(testExpression)), expectedValue_(std::move(expectedValue))
{
}

ValidationResult MetadataSchematronAttributeEqualityConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (TryGetAttribute(element, value) && value != expectedValue_)
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeInequalityConstraint::MetadataSchematronAttributeInequalityConstraint(
    OpenXmlQualifiedName attributeName,
    std::string disallowedValue,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeInequality", std::move(attributeName), std::move(testExpression)), disallowedValue_(std::move(disallowedValue))
{
}

ValidationResult MetadataSchematronAttributeInequalityConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (TryGetAttribute(element, value) && value == disallowedValue_)
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeAllowedValuesConstraint::MetadataSchematronAttributeAllowedValuesConstraint(
    OpenXmlQualifiedName attributeName,
    std::vector<std::string> allowedValues,
    std::string testExpression,
    MetadataSchematronNumberFormat numberFormat) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeAllowedValues", std::move(attributeName), std::move(testExpression)), allowedValues_(std::move(allowedValues)), numberFormat_(numberFormat)
{
}

const std::vector<std::string>& MetadataSchematronAttributeAllowedValuesConstraint::AllowedValues() const noexcept
{
    return allowedValues_;
}

ValidationResult MetadataSchematronAttributeAllowedValuesConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (!TryGetAttribute(element, value))
    {
        return result;
    }

    if (!MetadataBuilderHelper::MatchesSchematronValue(value, allowedValues_, numberFormat_))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeForbiddenValuesConstraint::MetadataSchematronAttributeForbiddenValuesConstraint(
    OpenXmlQualifiedName attributeName,
    std::vector<std::string> disallowedValues,
    std::string testExpression,
    MetadataSchematronNumberFormat numberFormat) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeForbiddenValues", std::move(attributeName), std::move(testExpression)), disallowedValues_(std::move(disallowedValues)), numberFormat_(numberFormat)
{
}

ValidationResult MetadataSchematronAttributeForbiddenValuesConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view value;
    if (TryGetAttribute(element, value) && MetadataBuilderHelper::MatchesSchematronValue(value, disallowedValues_, numberFormat_))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeRequiredValueConstraint::MetadataSchematronAttributeRequiredValueConstraint(
    OpenXmlQualifiedName triggerAttributeName,
    OpenXmlQualifiedName valueAttributeName,
    std::string expectedValue,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeRequiredValue", std::move(triggerAttributeName), std::move(testExpression)), valueAttributeName_(std::move(valueAttributeName)), expectedValue_(std::move(expectedValue))
{
}

ValidationResult MetadataSchematronAttributeRequiredValueConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view triggerValue;
    if (!TryGetAttribute(element, triggerValue))
    {
        return result;
    }

    std::string_view value;
    if (!element.TryGetAttribute(valueAttributeName_, value) || value != expectedValue_)
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeForbiddenValueConstraint::MetadataSchematronAttributeForbiddenValueConstraint(
    OpenXmlQualifiedName triggerAttributeName,
    OpenXmlQualifiedName valueAttributeName,
    std::string disallowedValue,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeForbiddenValue", std::move(triggerAttributeName), std::move(testExpression)), valueAttributeName_(std::move(valueAttributeName)), disallowedValue_(std::move(disallowedValue))
{
}

ValidationResult MetadataSchematronAttributeForbiddenValueConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view triggerValue;
    if (!TryGetAttribute(element, triggerValue))
    {
        return result;
    }

    std::string_view value;
    if (element.TryGetAttribute(valueAttributeName_, value) && value == disallowedValue_)
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeMutualExclusionConstraint::MetadataSchematronAttributeMutualExclusionConstraint(
    std::vector<OpenXmlQualifiedName> attributeNames,
    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeMutualExclusion",
                                            attributeNames.empty() ? OpenXmlQualifiedName{} : attributeNames.front(),
                                            std::move(testExpression)),
      attributeNames_(std::move(attributeNames))
{
}

ValidationResult MetadataSchematronAttributeMutualExclusionConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    Size presentCount = 0;
    for (const auto& attributeName : attributeNames_)
    {
        std::string_view value;
        if (element.TryGetAttribute(attributeName, value))
        {
            ++presentCount;
            if (presentCount > 1)
            {
                AddSchematronViolation(result, element, locations);
                break;
            }
        }
    }
    return result;
}

MetadataSchematronAttributeConditionalPresenceConstraint::
    MetadataSchematronAttributeConditionalPresenceConstraint(OpenXmlQualifiedName requiredAttributeName,
                                                             OpenXmlQualifiedName conditionAttributeName,
                                                             std::string conditionValue,
                                                             std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeConditionalPresence", std::move(requiredAttributeName), std::move(testExpression)), conditionAttributeName_(std::move(conditionAttributeName)), conditionValue_(std::move(conditionValue))
{
}

ValidationResult MetadataSchematronAttributeConditionalPresenceConstraint::Validate(const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view condition;
    if (!element.TryGetAttribute(conditionAttributeName_, condition) || condition != conditionValue_)
    {
        return result;
    }

    std::string_view required;
    if (!TryGetAttribute(element, required))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeConditionalRequiredValueConstraint::
    MetadataSchematronAttributeConditionalRequiredValueConstraint(OpenXmlQualifiedName requiredAttributeName,
                                                                  std::string requiredValue,
                                                                  OpenXmlQualifiedName conditionAttributeName,
                                                                  std::string conditionValue,
                                                                  std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeConditionalRequiredValue", std::move(requiredAttributeName), std::move(testExpression)), requiredValue_(std::move(requiredValue)), conditionAttributeName_(std::move(conditionAttributeName)), conditionValue_(std::move(conditionValue))
{
}

ValidationResult MetadataSchematronAttributeConditionalRequiredValueConstraint::Validate(
    const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view condition;
    if (!element.TryGetAttribute(conditionAttributeName_, condition) || condition != conditionValue_)
    {
        return result;
    }

    std::string_view required;
    if (!TryGetAttribute(element, required) || required != requiredValue_)
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeConditionalAllowedValuesConstraint::
    MetadataSchematronAttributeConditionalAllowedValuesConstraint(OpenXmlQualifiedName valueAttributeName,
                                                                  std::vector<std::string> allowedValues,
                                                                  OpenXmlQualifiedName conditionAttributeName,
                                                                  std::vector<std::string> conditionValues,
                                                                  std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeConditionalAllowedValues", std::move(valueAttributeName), std::move(testExpression)), allowedValues_(std::move(allowedValues)), conditionAttributeName_(std::move(conditionAttributeName)), conditionValues_(std::move(conditionValues))
{
}

ValidationResult MetadataSchematronAttributeConditionalAllowedValuesConstraint::Validate(
    const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view condition;
    if (!element.TryGetAttribute(conditionAttributeName_, condition) || !MetadataBuilderHelper::ContainsStringValue(conditionValues_, condition))
    {
        return result;
    }

    std::string_view value;
    if (!TryGetAttribute(element, value) || !MetadataBuilderHelper::ContainsStringValue(allowedValues_, value))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeConditionalForbiddenValuesConstraint::
    MetadataSchematronAttributeConditionalForbiddenValuesConstraint(OpenXmlQualifiedName triggerAttributeName,
                                                                    OpenXmlQualifiedName valueAttributeName,
                                                                    std::vector<std::string> disallowedValues,
                                                                    std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeConditionalForbiddenValues", std::move(triggerAttributeName), std::move(testExpression)), valueAttributeName_(std::move(valueAttributeName)), disallowedValues_(std::move(disallowedValues))
{
}

ValidationResult MetadataSchematronAttributeConditionalForbiddenValuesConstraint::Validate(
    const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view trigger;
    if (!TryGetAttribute(element, trigger))
    {
        return result;
    }

    std::string_view value;
    if (element.TryGetAttribute(valueAttributeName_, value) && MetadataBuilderHelper::ContainsStringValue(disallowedValues_, value))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

MetadataSchematronAttributeConditionalPresenceAllowedValuesConstraint::
    MetadataSchematronAttributeConditionalPresenceAllowedValuesConstraint(OpenXmlQualifiedName triggerAttributeName,
                                                                          OpenXmlQualifiedName valueAttributeName,
                                                                          std::vector<std::string> allowedValues,
                                                                          std::string testExpression) noexcept
    : MetadataSchematronAttributeConstraint("SchematronAttributeConditionalPresenceAllowedValues", std::move(triggerAttributeName), std::move(testExpression)), valueAttributeName_(std::move(valueAttributeName)), allowedValues_(std::move(allowedValues))
{
}

ValidationResult MetadataSchematronAttributeConditionalPresenceAllowedValuesConstraint::Validate(
    const OpenXMLElement& element, XmlLocationCache& locations) const
{
    ValidationResult result;
    std::string_view trigger;
    if (!TryGetAttribute(element, trigger))
    {
        return result;
    }

    std::string_view value;
    if (!element.TryGetAttribute(valueAttributeName_, value) || !MetadataBuilderHelper::ContainsStringValue(allowedValues_, value))
    {
        AddSchematronViolation(result, element, locations);
    }
    return result;
}

bool MetadataAttributeInfo::AllowsEmptyValue() const noexcept
{
    return MetadataBuilderHelper::AllowsEmptyValue(TypeName);
}

const MetadataSummary& MetadataDefinition::Summary() const noexcept
{
    return summary_;
}

const std::vector<MetadataAttributeInfo>& MetadataDefinition::Attributes() const noexcept
{
    return attributes_;
}

const std::vector<MetadataAdditionalElementInfo>& MetadataDefinition::AdditionalElements() const noexcept
{
    return additionalElements_;
}

const std::vector<MetadataConstraintPtr>& MetadataDefinition::Constraints() const noexcept
{
    return constraints_;
}

const MetadataParticlePtr& MetadataDefinition::ParticleTree() const noexcept
{
    return particleTree_;
}

MetadataBuilder::MetadataBuilder()
    : definition_(std::make_shared<MetadataDefinition>())
{
}

MetadataBuilder::~MetadataBuilder() = default;

MetadataBuilder& MetadataBuilder::SetElementName(OpenXmlQualifiedName name)
{
    definition_->summary_.ElementName = name;
    return *this;
}

MetadataBuilder& MetadataBuilder::SetTypeName(OpenXmlQualifiedName name)
{
    definition_->summary_.TypeName = name;
    return *this;
}

MetadataBuilder& MetadataBuilder::SetSchemaName(std::string schema)
{
    definition_->summary_.SchemaName = std::move(schema);
    return *this;
}

MetadataBuilder& MetadataBuilder::SetAvailability(OpenXml::FileFormatVersions version)
{
    definition_->summary_.Availability = version;
    return *this;
}

MetadataAttributeInfo& MetadataBuilder::AddAttribute(OpenXmlQualifiedName name,
                                                     std::string propertyName,
                                                     std::string typeName,
                                                     OpenXml::FileFormatVersions version,
                                                     std::string description)
{
    MetadataAttributeInfo attribute;
    attribute.Name = name;
    attribute.PropertyName = std::move(propertyName);
    attribute.TypeName = std::move(typeName);
    attribute.Version = version;
    attribute.Description = std::move(description);

    definition_->attributes_.push_back(std::move(attribute));
    return definition_->attributes_.back();
}

void MetadataBuilder::AddAdditionalElement(OpenXmlQualifiedName name, std::string typeName)
{
    definition_->additionalElements_.push_back({std::move(name), std::move(typeName)});
}

void MetadataBuilder::AddConstraint(MetadataConstraintPtr constraint)
{
    if (!constraint)
    {
        return;
    }

    definition_->constraints_.push_back(std::move(constraint));
}

void MetadataBuilder::SetParticleTree(MetadataParticlePtr particle)
{
    definition_->particleTree_ = std::move(particle);
}

const MetadataSummary& MetadataBuilder::Summary() const noexcept
{
    return definition_->summary_;
}

std::shared_ptr<MetadataDefinition> MetadataBuilder::Build() const
{
    return std::make_shared<MetadataDefinition>(*definition_);
}

} // namespace ExyokiOffice
