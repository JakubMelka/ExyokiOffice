// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"

#include <string>
#include <vector>

namespace ExyokiOffice
{

enum class ValidationSeverity
{
    Error,
    Warning
};

enum class ValidationDomain
{
    General,
    Opc,
    Xml,
    Dom,
    Schema,
    Packaging,
    Security,
    Io
};

enum class ValidationErrorId
{
    Unknown,
    MissingAttribute,
    EmptyAttribute,
    AttributeExactLengthMismatch,
    AttributeMinLengthMismatch,
    AttributeMaxLengthMismatch,
    AttributePatternMismatch,
    AttributeTokenMismatch,
    AttributeNcNameViolation,
    AttributeQNameViolation,
    AttributeIdViolation,
    AttributeUriViolation,
    AttributeNumberParsingFailed,
    AttributeMinInclusiveViolation,
    AttributeMaxInclusiveViolation,
    AttributeMinExclusiveViolation,
    AttributeMaxExclusiveViolation,
    AttributePositiveViolation,
    AttributeNonNegativeViolation,
    AttributeEnumViolation,
    AttributeVersionViolation,
    TextExactLengthMismatch,
    TextMinLengthMismatch,
    TextMaxLengthMismatch,
    TextPatternMismatch,
    TextTokenMismatch,
    TextNcNameViolation,
    TextQNameViolation,
    TextIdViolation,
    TextUriViolation,
    TextNumberParsingFailed,
    TextMinInclusiveViolation,
    TextMaxInclusiveViolation,
    TextMinExclusiveViolation,
    TextMaxExclusiveViolation,
    TextPositiveViolation,
    TextNonNegativeViolation,
    TextEnumViolation,
    ParticleConstraintViolation,
    ElementVersionViolation,
    PartVersionViolation,
    PackageMissingMainPart,
    PackageMultipleMainParts,
    /** The package is ISO 29500 Strict; only the Transitional class is implemented. */
    PackageStrictConformanceUnsupported,
    PackageRelationshipTypeMismatch,
    PackageContentTypeMismatch,
    OpcDuplicateRelationshipId,
    OpcEmptyRelationshipId,
    OpcEmptyRelationshipType,
    OpcEmptyRelationshipTarget,
    OpcInvalidRelationshipTargetMode,
    OpcDanglingRelationshipTarget,
    OpcDuplicatePartUri,
    /** An XML part is not a well-formed XML document (no root element, or more than one). */
    OpcMalformedPartXml,
    OpcLimitExceeded,
    XmlLimitExceeded,
    /** `mc:MustUnderstand` names a namespace the target Office generation does not cover. */
    MarkupCompatibilityMustUnderstandUnsupported,
    /** An `mc:AlternateContent` does not have the shape ECMA-376 Part 3 prescribes. */
    MarkupCompatibilityMalformedAlternateContent,
    /** A markup compatibility attribute names a prefix that is not declared in scope. */
    MarkupCompatibilityUnresolvablePrefix,
    /** A generated schematron rule rejected the element. `ConstraintId` names the rule kind. */
    SchematronConstraintViolation,
    /** A schematron rule could not be evaluated at all, so the element was not checked against it. */
    SchematronRuleNotEvaluable,
    /** A digital signature part is not well formed or does not follow the package signature shape. */
    SignatureMalformed,
    /** A digest stored in a signature does not match the part or object it covers. */
    SignatureDigestMismatch,
    /** A signature covers a part that is no longer in the package. */
    SignaturePartMissing,
    /** A signature names a canonicalization, digest, or signature algorithm this library cannot apply. */
    SignatureUnsupportedAlgorithm,
    /** The signature value did not verify against the certificate in the signature. */
    SignatureValueInvalid,
    /** The signature value was not checked because no crypto provider was supplied. */
    SignatureNotVerified,
    /** Saving the package in its current state invalidates a signature it already carries. */
    SignatureInvalidatedBySave,
    /** An external resource was requested, but no resolver was supplied; nothing was accessed. */
    ExternalResourceNoResolver,
    /** The external resource policy refused the target, so the resolver was never asked for it. */
    ExternalResourceDenied,
    /** The resolver returned more data than the policy allows; the content was discarded. */
    ExternalResourceTooLarge,
    /** The per-package request count or byte budget for external resources is exhausted. */
    ExternalResourceBudgetExceeded,
    /** The resolver was asked but could not deliver the resource. */
    ExternalResourceUnavailable,
    /**
     * The content-model automaton and the reference matcher disagreed about an
     * element, which is a defect in this library rather than in the document.
     * Only ever reported when `OpenXmlDomValidationSettings::CrossCheckContentModel`
     * asked for both verdicts.
     */
    ContentModelCrossCheckMismatch
};

/**
 * @brief Points a diagnostic at a concrete place in a part's XML.
 *
 * `Path` is an XPath-like location with positional predicates, so it addresses
 * one element rather than every element of that name: `/w:document/w:body/w:p[3]`.
 * Prefixes are normalized to the well-known Open XML ones where the namespace is
 * known, which makes paths comparable across documents that chose their own.
 *
 * There is deliberately no line or column. Locations have to stay meaningful for
 * elements the caller has just built in memory, where no source text exists, and
 * pugixml's parse offsets stop being valid as soon as the tree is edited.
 */
struct EXYOKIOFFICE_EXPORT XmlLocation
{
    /** Positional XPath-like path of the element the diagnostic is about. */
    std::string Path;
    /** Qualified name of that element, in `prefix:localName` form. */
    std::string ElementName;
    /** Qualified name of the offending attribute, when the issue is about one. */
    std::string AttributeName;

    [[nodiscard]] bool IsValid() const noexcept { return !Path.empty(); }
};

struct EXYOKIOFFICE_EXPORT ValidationIssue
{
    ValidationSeverity Severity{ValidationSeverity::Error};
    ValidationDomain Domain{ValidationDomain::General};
    ValidationErrorId Id{ValidationErrorId::Unknown};
    std::string Message;
    XmlLocation Location{};
    /** Identifier of the schema or schematron rule that produced the issue, when it has one. */
    std::string ConstraintId;
    std::string PartUri;
    std::string RelationshipSourceUri;
    std::string RelationshipId;
    std::string RelationshipType;
    std::string TargetUri;
    std::string TargetMode;
};

class EXYOKIOFFICE_EXPORT DiagnosticSink
{
public:
    virtual ~DiagnosticSink() = default;
    virtual void Report(ValidationIssue issue) = 0;
};

class EXYOKIOFFICE_EXPORT ValidationResult
    : public DiagnosticSink
{
public:
    ValidationResult() = default;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] bool HasErrors() const noexcept;
    [[nodiscard]] bool HasWarnings() const noexcept;
    [[nodiscard]] const std::vector<ValidationIssue>& Issues() const noexcept;

    void AddIssue(ValidationIssue issue);
    void Report(ValidationIssue issue) override;
    void AddError(ValidationErrorId id, std::string message, XmlLocation location);
    void AddWarning(ValidationErrorId id, std::string message, XmlLocation location);
    void Merge(const ValidationResult& other);

private:
    std::vector<ValidationIssue> issues_;
};

} // namespace ExyokiOffice
