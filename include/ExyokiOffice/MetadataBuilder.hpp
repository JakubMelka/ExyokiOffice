// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "ExyokiOffice/XmlLocationCache.hpp"
#include "FileFormatVersions.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice
{

class OpenXMLElement;
class MetadataConstraint;
class MetadataParticle;
class MetadataCompositeParticle;
class MetadataSequenceParticle;
class MetadataChoiceParticle;
class MetadataAllParticle;
class MetadataElementParticle;
class MetadataAnyParticle;
class MetadataGroupParticle;
class MetadataBuilder;
class MetadataDefinition;
class MetadataAttributeConstraint;
class MetadataRequiredConstraint;
class MetadataStringConstraint;
class MetadataNumberConstraint;
class MetadataEnumConstraint;
class MetadataOfficeVersionConstraint;
class MetadataTextStringConstraint;
class MetadataTextNumberConstraint;
class MetadataTextEnumConstraint;
class MetadataSchematronAttributeConstraint;

using MetadataConstraintPtr = std::shared_ptr<MetadataConstraint>;
using MetadataParticlePtr = std::shared_ptr<MetadataParticle>;

enum class MetadataParticleKind
{
    Element,
    Sequence,
    Choice,
    All,
    Group,
    Any
};

class EXYOKIOFFICE_EXPORT MetadataParticle
{
public:
    MetadataParticle(MetadataParticleKind kind,
                     UInt32 minOccurs,
                     std::optional<UInt32> maxOccurs,
                     OpenXml::FileFormatVersions version,
                     bool requireFilter = false) noexcept;
    virtual ~MetadataParticle() = default;

    MetadataParticleKind Kind() const noexcept;
    UInt32 MinOccurs() const noexcept;
    std::optional<UInt32> MaxOccurs() const noexcept;
    [[nodiscard]] bool IsUnbounded() const noexcept;
    OpenXml::FileFormatVersions Version() const noexcept;
    [[nodiscard]] bool RequiresFilter() const noexcept;

protected:
    MetadataParticleKind kind_;
    UInt32 minOccurs_;
    std::optional<UInt32> maxOccurs_;
    OpenXml::FileFormatVersions version_;
    bool requireFilter_;
};

class EXYOKIOFFICE_EXPORT MetadataCompositeParticle : public MetadataParticle
{
public:
    MetadataCompositeParticle(MetadataParticleKind kind,
                              UInt32 minOccurs,
                              std::optional<UInt32> maxOccurs,
                              OpenXml::FileFormatVersions version,
                              bool requireFilter = false) noexcept;

    void AddChild(const MetadataParticlePtr& particle);
    const std::vector<MetadataParticlePtr>& Children() const noexcept;

protected:
    std::vector<MetadataParticlePtr> children_;
};

class EXYOKIOFFICE_EXPORT MetadataSequenceParticle : public MetadataCompositeParticle
{
public:
    MetadataSequenceParticle(UInt32 minOccurs,
                             std::optional<UInt32> maxOccurs,
                             OpenXml::FileFormatVersions version,
                             bool requireFilter = false) noexcept;
};

class EXYOKIOFFICE_EXPORT MetadataChoiceParticle : public MetadataCompositeParticle
{
public:
    MetadataChoiceParticle(UInt32 minOccurs,
                           std::optional<UInt32> maxOccurs,
                           OpenXml::FileFormatVersions version,
                           bool requireFilter = false) noexcept;
};

class EXYOKIOFFICE_EXPORT MetadataAllParticle : public MetadataCompositeParticle
{
public:
    MetadataAllParticle(UInt32 minOccurs,
                        std::optional<UInt32> maxOccurs,
                        OpenXml::FileFormatVersions version,
                        bool requireFilter = false) noexcept;
};

class EXYOKIOFFICE_EXPORT MetadataGroupParticle : public MetadataCompositeParticle
{
public:
    MetadataGroupParticle(UInt32 minOccurs,
                          std::optional<UInt32> maxOccurs,
                          OpenXml::FileFormatVersions version,
                          bool requireFilter = false) noexcept;
};

class EXYOKIOFFICE_EXPORT MetadataElementParticle : public MetadataParticle
{
public:
    MetadataElementParticle(OpenXmlQualifiedName element,
                            std::string elementType,
                            std::string propertyName,
                            UInt32 minOccurs,
                            std::optional<UInt32> maxOccurs,
                            OpenXml::FileFormatVersions version) noexcept;

    const OpenXmlQualifiedName& Element() const noexcept;
    const std::string& ElementType() const noexcept;
    const std::string& PropertyName() const noexcept;

private:
    OpenXmlQualifiedName element_;
    std::string elementType_;
    std::string propertyName_;
};

class EXYOKIOFFICE_EXPORT MetadataAnyParticle : public MetadataParticle
{
public:
    MetadataAnyParticle(std::string wildcard,
                        UInt32 minOccurs,
                        std::optional<UInt32> maxOccurs,
                        OpenXml::FileFormatVersions version) noexcept;

    const std::string& Wildcard() const noexcept;

private:
    std::string wildcard_;
};

enum class MetadataConstraintType
{
    Unknown,
    AttributeValue,
    TextValue,
    Particle,
    Uniqueness,
    Custom
};

class EXYOKIOFFICE_EXPORT MetadataConstraint
{
public:
    MetadataConstraint(MetadataConstraintType type, std::string identifier) noexcept;
    virtual ~MetadataConstraint() = default;

    MetadataConstraintType Type() const noexcept;
    const std::string& Identifier() const noexcept;

    void SetDescription(std::string description);
    const std::string& Description() const noexcept;

    void AddAssociatedName(OpenXmlQualifiedName name);
    const std::vector<OpenXmlQualifiedName>& AssociatedNames() const noexcept;

    virtual ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const = 0;

private:
    MetadataConstraintType type_;
    std::string identifier_;
    std::string description_;
    std::vector<OpenXmlQualifiedName> names_;
};

class EXYOKIOFFICE_EXPORT MetadataUnionConstraint final : public MetadataConstraint
{
public:
    MetadataUnionConstraint(MetadataConstraintType type, UInt32 unionId) noexcept;

    UInt32 UnionId() const noexcept;
    void AddAlternative(MetadataConstraintPtr alternative);
    const std::vector<MetadataConstraintPtr>& Alternatives() const noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    UInt32 unionId_;
    std::vector<MetadataConstraintPtr> alternatives_;
};

class EXYOKIOFFICE_EXPORT MetadataAttributeConstraint : public MetadataConstraint
{
public:
    MetadataAttributeConstraint(std::string identifier,
                                OpenXmlQualifiedName attributeName,
                                std::string propertyName);
    const OpenXmlQualifiedName& AttributeName() const noexcept;
    const std::string& PropertyName() const noexcept;

protected:
    [[nodiscard]] bool TryGetAttribute(const OpenXMLElement& element, std::string_view& value) const;

private:
    OpenXmlQualifiedName attributeName_;
    std::string propertyName_;
};

class EXYOKIOFFICE_EXPORT MetadataRequiredConstraint final : public MetadataAttributeConstraint
{
public:
    MetadataRequiredConstraint(OpenXmlQualifiedName attributeName,
                               std::string propertyName,
                               bool required) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    bool required_;
};

class EXYOKIOFFICE_EXPORT MetadataStringConstraint final : public MetadataAttributeConstraint
{
public:
    /**
     * @param isHexBinary The attribute holds `xsd:hexBinary`, so its length facets
     * count octets rather than characters: a `Length` of 3 admits the six hex
     * digits of `C00000`. The value must also consist of hex digit pairs.
     */
    MetadataStringConstraint(OpenXmlQualifiedName attributeName,
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
                             bool isHexBinary = false) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::optional<Size> minLength_;
    std::optional<Size> maxLength_;
    std::optional<Size> exactLength_;
    std::optional<std::string> pattern_;
    bool isToken_;
    bool isNcName_;
    bool isQName_;
    bool isId_;
    bool isUri_;
    bool isHexBinary_;
};

class EXYOKIOFFICE_EXPORT MetadataNumberConstraint final : public MetadataAttributeConstraint
{
public:
    MetadataNumberConstraint(OpenXmlQualifiedName attributeName,
                             std::string propertyName,
                             std::string valueType,
                             std::optional<Real> minInclusive,
                             std::optional<Real> maxInclusive,
                             std::optional<Real> minExclusive,
                             std::optional<Real> maxExclusive,
                             bool isPositive,
                             bool isNonNegative,
                             bool isList) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    /**
     * @brief Reports whether this validator can say anything about its value.
     *
     * A number validator whose declared type has no numeric lexical space can
     * only be enforced through an explicit bound: the bound is the only
     * evidence that the value was meant to be read as a number. Without one
     * there is nothing to check, and demanding a number rejects the cell
     * references, timestamps and base64 payloads such validators sit on.
     */
    [[nodiscard]] bool HasEnforceableBounds() const noexcept;

    std::string valueType_;
    std::optional<Real> minInclusive_;
    std::optional<Real> maxInclusive_;
    std::optional<Real> minExclusive_;
    std::optional<Real> maxExclusive_;
    bool isPositive_;
    bool isNonNegative_;
    bool isList_;
};

class EXYOKIOFFICE_EXPORT MetadataEnumConstraint final : public MetadataAttributeConstraint
{
public:
    using ValidatorFunction = std::function<bool(std::string_view)>;

    MetadataEnumConstraint(OpenXmlQualifiedName attributeName,
                           std::string propertyName,
                           ValidatorFunction validator) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

    template <typename TValue>
    static std::shared_ptr<MetadataEnumConstraint> Create(OpenXmlQualifiedName attributeName,
                                                          std::string propertyName)
    {
        ValidatorFunction validator = [](std::string_view text)
        {
            TValue value;
            return value.AssignFromString(text) && value.IsDefined();
        };

        return std::make_shared<MetadataEnumConstraint>(std::move(attributeName),
                                                        std::move(propertyName),
                                                        std::move(validator));
    }

private:
    ValidatorFunction validator_;
};

struct EXYOKIOFFICE_EXPORT MetadataEnumRule
{
    std::vector<std::string> Values;
    OpenXml::FileFormatVersions Version{OpenXml::FileFormatVersions::Office2007};
    UInt32 UnionId{0};
    bool IsInitialVersion{false};
};

class EXYOKIOFFICE_EXPORT MetadataAttributeEnumUnionConstraint final : public MetadataAttributeConstraint
{
public:
    MetadataAttributeEnumUnionConstraint(OpenXmlQualifiedName attributeName,
                                         std::string propertyName,
                                         std::vector<MetadataEnumRule> rules) noexcept;

    const std::vector<MetadataEnumRule>& Rules() const noexcept;
    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::vector<MetadataEnumRule> rules_;
};

class EXYOKIOFFICE_EXPORT MetadataTextStringConstraint final : public MetadataConstraint
{
public:
    MetadataTextStringConstraint(std::optional<Size> minLength,
                                 std::optional<Size> maxLength,
                                 std::optional<Size> exactLength,
                                 std::optional<std::string> pattern,
                                 bool isToken,
                                 bool isNcName,
                                 bool isQName,
                                 bool isId,
                                 bool isUri) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::optional<Size> minLength_;
    std::optional<Size> maxLength_;
    std::optional<Size> exactLength_;
    std::optional<std::string> pattern_;
    bool isToken_;
    bool isNcName_;
    bool isQName_;
    bool isId_;
    bool isUri_;
};

class EXYOKIOFFICE_EXPORT MetadataTextNumberConstraint final : public MetadataConstraint
{
public:
    MetadataTextNumberConstraint(std::string valueType,
                                 std::optional<Real> minInclusive,
                                 std::optional<Real> maxInclusive,
                                 std::optional<Real> minExclusive,
                                 std::optional<Real> maxExclusive,
                                 bool isPositive,
                                 bool isNonNegative,
                                 bool isList) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    /**
     * @brief Reports whether this validator can say anything about its value.
     *
     * A number validator whose declared type has no numeric lexical space can
     * only be enforced through an explicit bound: the bound is the only
     * evidence that the value was meant to be read as a number. Without one
     * there is nothing to check, and demanding a number rejects the cell
     * references, timestamps and base64 payloads such validators sit on.
     */
    [[nodiscard]] bool HasEnforceableBounds() const noexcept;

    std::string valueType_;
    std::optional<Real> minInclusive_;
    std::optional<Real> maxInclusive_;
    std::optional<Real> minExclusive_;
    std::optional<Real> maxExclusive_;
    bool isPositive_;
    bool isNonNegative_;
    bool isList_;
};

class EXYOKIOFFICE_EXPORT MetadataTextEnumConstraint final : public MetadataConstraint
{
public:
    explicit MetadataTextEnumConstraint(std::vector<MetadataEnumRule> rules) noexcept;

    const std::vector<MetadataEnumRule>& Rules() const noexcept;
    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::vector<MetadataEnumRule> rules_;
};

class EXYOKIOFFICE_EXPORT MetadataOfficeVersionConstraint final : public MetadataAttributeConstraint
{
public:
    MetadataOfficeVersionConstraint(OpenXmlQualifiedName attributeName,
                                    std::string propertyName,
                                    OpenXml::FileFormatVersions version) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;
    OpenXml::FileFormatVersions Version() const noexcept;

private:
    OpenXml::FileFormatVersions version_;
};

enum class MetadataSchematronComparisonOperator
{
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
};

/**
 * @brief The radix a schematron rule reads its attribute in.
 *
 * Most rules bound a decimal attribute, but a handful bound one whose lexical
 * space is hexadecimal - `ST_LongHexNumber` values such as `w14:paraId`,
 * `w14:textId` and `w14:docId/@w14:val`, written as eight unprefixed
 * hexadecimal digits. The rules record the radix only in their own literals
 * (`@w14:paraId < 0x80000000`), so the generator carries it over to the
 * constraint. Reading such a value as decimal yields 0 for anything starting
 * with a letter and rejects nearly every paragraph Word writes.
 */
enum class MetadataSchematronNumberFormat
{
    Decimal,
    Hexadecimal,
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeConstraint : public MetadataAttributeConstraint
{
public:
    MetadataSchematronAttributeConstraint(OpenXmlQualifiedName attributeName,
                                          std::string testExpression) noexcept;

    const std::string& TestExpression() const noexcept;

protected:
    /**
     * @brief Lets a derived rule name its own kind.
     *
     * Every schematron diagnostic carries `ValidationErrorId::SchematronConstraintViolation`,
     * so the kind of rule that fired lives in `Identifier()`: without it a caller
     * cannot tell an allowed-values rejection from a numeric range one.
     */
    MetadataSchematronAttributeConstraint(std::string identifier,
                                          OpenXmlQualifiedName attributeName,
                                          std::string testExpression) noexcept;

    void AddSchematronViolation(ValidationResult& result,
                                const OpenXMLElement& element,
                                XmlLocationCache& locations) const;

private:
    std::string testExpression_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributePresenceConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributePresenceConstraint(OpenXmlQualifiedName attributeName,
                                                  std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeRegexConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeRegexConstraint(OpenXmlQualifiedName attributeName,
                                               std::string pattern,
                                               std::string testExpression);

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    struct RegexState;
    std::shared_ptr<const RegexState> regex_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeStringLengthConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeStringLengthConstraint(OpenXmlQualifiedName attributeName,
                                                      Size minLength,
                                                      Size maxLength,
                                                      std::string testExpression) noexcept;
    MetadataSchematronAttributeStringLengthConstraint(OpenXmlQualifiedName attributeName,
                                                      MetadataSchematronComparisonOperator comparison,
                                                      Size limit,
                                                      std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::optional<Size> minLength_;
    std::optional<Size> maxLength_;
    std::optional<MetadataSchematronComparisonOperator> comparison_;
    std::optional<Size> limit_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeNumericRangeConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeNumericRangeConstraint(
        OpenXmlQualifiedName attributeName,
        Real minInclusive,
        Real maxInclusive,
        std::string testExpression,
        MetadataSchematronNumberFormat numberFormat = MetadataSchematronNumberFormat::Decimal) noexcept;
    MetadataSchematronAttributeNumericRangeConstraint(
        OpenXmlQualifiedName attributeName,
        MetadataSchematronComparisonOperator lowerComparison,
        Real lowerValue,
        MetadataSchematronComparisonOperator upperComparison,
        Real upperValue,
        std::string testExpression,
        MetadataSchematronNumberFormat numberFormat = MetadataSchematronNumberFormat::Decimal) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    Real minInclusive_;
    Real maxInclusive_;
    std::optional<MetadataSchematronComparisonOperator> lowerComparison_;
    std::optional<Real> lowerValue_;
    std::optional<MetadataSchematronComparisonOperator> upperComparison_;
    std::optional<Real> upperValue_;
    MetadataSchematronNumberFormat numberFormat_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeNumericComparisonConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeNumericComparisonConstraint(
        OpenXmlQualifiedName attributeName,
        MetadataSchematronComparisonOperator comparison,
        Real value,
        std::string testExpression,
        MetadataSchematronNumberFormat numberFormat = MetadataSchematronNumberFormat::Decimal) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    MetadataSchematronComparisonOperator comparison_;
    Real value_;
    MetadataSchematronNumberFormat numberFormat_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeNumericAttributeComparisonConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeNumericAttributeComparisonConstraint(OpenXmlQualifiedName leftAttributeName,
                                                                    MetadataSchematronComparisonOperator comparison,
                                                                    OpenXmlQualifiedName rightAttributeName,
                                                                    std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    MetadataSchematronComparisonOperator comparison_;
    OpenXmlQualifiedName rightAttributeName_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeEqualityConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeEqualityConstraint(OpenXmlQualifiedName attributeName,
                                                  std::string expectedValue,
                                                  std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::string expectedValue_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeInequalityConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeInequalityConstraint(OpenXmlQualifiedName attributeName,
                                                    std::string disallowedValue,
                                                    std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::string disallowedValue_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeAllowedValuesConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeAllowedValuesConstraint(
        OpenXmlQualifiedName attributeName,
        std::vector<std::string> allowedValues,
        std::string testExpression,
        MetadataSchematronNumberFormat numberFormat = MetadataSchematronNumberFormat::Decimal) noexcept;

    const std::vector<std::string>& AllowedValues() const noexcept;
    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::vector<std::string> allowedValues_;
    MetadataSchematronNumberFormat numberFormat_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeForbiddenValuesConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeForbiddenValuesConstraint(
        OpenXmlQualifiedName attributeName,
        std::vector<std::string> disallowedValues,
        std::string testExpression,
        MetadataSchematronNumberFormat numberFormat = MetadataSchematronNumberFormat::Decimal) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::vector<std::string> disallowedValues_;
    MetadataSchematronNumberFormat numberFormat_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeRequiredValueConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeRequiredValueConstraint(OpenXmlQualifiedName triggerAttributeName,
                                                       OpenXmlQualifiedName valueAttributeName,
                                                       std::string expectedValue,
                                                       std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    OpenXmlQualifiedName valueAttributeName_;
    std::string expectedValue_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeForbiddenValueConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeForbiddenValueConstraint(OpenXmlQualifiedName triggerAttributeName,
                                                        OpenXmlQualifiedName valueAttributeName,
                                                        std::string disallowedValue,
                                                        std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    OpenXmlQualifiedName valueAttributeName_;
    std::string disallowedValue_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeMutualExclusionConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeMutualExclusionConstraint(std::vector<OpenXmlQualifiedName> attributeNames,
                                                         std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::vector<OpenXmlQualifiedName> attributeNames_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeConditionalPresenceConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeConditionalPresenceConstraint(OpenXmlQualifiedName requiredAttributeName,
                                                             OpenXmlQualifiedName conditionAttributeName,
                                                             std::string conditionValue,
                                                             std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    OpenXmlQualifiedName conditionAttributeName_;
    std::string conditionValue_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeConditionalRequiredValueConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeConditionalRequiredValueConstraint(OpenXmlQualifiedName requiredAttributeName,
                                                                  std::string requiredValue,
                                                                  OpenXmlQualifiedName conditionAttributeName,
                                                                  std::string conditionValue,
                                                                  std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::string requiredValue_;
    OpenXmlQualifiedName conditionAttributeName_;
    std::string conditionValue_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeConditionalAllowedValuesConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeConditionalAllowedValuesConstraint(OpenXmlQualifiedName valueAttributeName,
                                                                  std::vector<std::string> allowedValues,
                                                                  OpenXmlQualifiedName conditionAttributeName,
                                                                  std::vector<std::string> conditionValues,
                                                                  std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    std::vector<std::string> allowedValues_;
    OpenXmlQualifiedName conditionAttributeName_;
    std::vector<std::string> conditionValues_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeConditionalForbiddenValuesConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeConditionalForbiddenValuesConstraint(OpenXmlQualifiedName triggerAttributeName,
                                                                    OpenXmlQualifiedName valueAttributeName,
                                                                    std::vector<std::string> disallowedValues,
                                                                    std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    OpenXmlQualifiedName valueAttributeName_;
    std::vector<std::string> disallowedValues_;
};

class EXYOKIOFFICE_EXPORT MetadataSchematronAttributeConditionalPresenceAllowedValuesConstraint final
    : public MetadataSchematronAttributeConstraint
{
public:
    MetadataSchematronAttributeConditionalPresenceAllowedValuesConstraint(OpenXmlQualifiedName triggerAttributeName,
                                                                          OpenXmlQualifiedName valueAttributeName,
                                                                          std::vector<std::string> allowedValues,
                                                                          std::string testExpression) noexcept;

    ValidationResult Validate(const OpenXMLElement& element, XmlLocationCache& locations) const override;

private:
    OpenXmlQualifiedName valueAttributeName_;
    std::vector<std::string> allowedValues_;
};

struct EXYOKIOFFICE_EXPORT MetadataAttributeInfo
{
    OpenXmlQualifiedName Name;
    std::string PropertyName;
    std::string TypeName;
    OpenXml::FileFormatVersions Version{OpenXml::FileFormatVersions::Office2007};
    std::string Description;
    std::vector<MetadataConstraintPtr> Validators;

    /**
     * @brief Reports whether `TypeName` can represent the empty string.
     *
     * Being written as `attr=""` is not the same as being absent, and which of the
     * two an empty value means is a property of the declared type: a string, a
     * binary, a list or `ST_TrueFalseBlank` all have the empty string in their
     * lexical space, while a number, a date or an enumeration do not.
     */
    [[nodiscard]] bool AllowsEmptyValue() const noexcept;
};

struct MetadataAdditionalElementInfo
{
    OpenXmlQualifiedName Name;
    std::string TypeName;
};

struct MetadataSummary
{
    OpenXmlQualifiedName ElementName;
    OpenXmlQualifiedName TypeName;
    std::string SchemaName;
    OpenXml::FileFormatVersions Availability{OpenXml::FileFormatVersions::Office2007};
};

class EXYOKIOFFICE_EXPORT MetadataDefinition
{
public:
    const MetadataSummary& Summary() const noexcept;
    const std::vector<MetadataAttributeInfo>& Attributes() const noexcept;
    const std::vector<MetadataAdditionalElementInfo>& AdditionalElements() const noexcept;
    const std::vector<MetadataConstraintPtr>& Constraints() const noexcept;
    const MetadataParticlePtr& ParticleTree() const noexcept;

private:
    friend class MetadataBuilder;

    MetadataSummary summary_;
    std::vector<MetadataAttributeInfo> attributes_;
    std::vector<MetadataAdditionalElementInfo> additionalElements_;
    std::vector<MetadataConstraintPtr> constraints_;
    MetadataParticlePtr particleTree_;
};

class EXYOKIOFFICE_EXPORT MetadataBuilder
{
public:
    MetadataBuilder();
    ~MetadataBuilder();

    MetadataBuilder(const MetadataBuilder&) = delete;
    MetadataBuilder& operator=(const MetadataBuilder&) = delete;

    MetadataBuilder& SetElementName(OpenXmlQualifiedName name);
    MetadataBuilder& SetTypeName(OpenXmlQualifiedName name);
    MetadataBuilder& SetSchemaName(std::string schema);
    MetadataBuilder& SetAvailability(OpenXml::FileFormatVersions version);

    MetadataAttributeInfo& AddAttribute(OpenXmlQualifiedName name,
                                        std::string propertyName,
                                        std::string typeName,
                                        OpenXml::FileFormatVersions version,
                                        std::string description);

    void AddAdditionalElement(OpenXmlQualifiedName name, std::string typeName);

    void AddConstraint(MetadataConstraintPtr constraint);
    void SetParticleTree(MetadataParticlePtr particle);

    const MetadataSummary& Summary() const noexcept;
    std::shared_ptr<MetadataDefinition> Build() const;

private:
    std::shared_ptr<MetadataDefinition> definition_;
};

} // namespace ExyokiOffice
