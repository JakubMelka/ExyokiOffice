// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXmlDomValidator.hpp"

#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/XmlLocationCache.hpp"
#include "MarkupCompatibilityInternal.hpp"
#include "OpenXmlContentModel.hpp"
#include "OpenXmlDiagnosticNames.hpp"
#include "OpenXmlVersionSupport.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

using Detail::SupportsOfficeVersion;

/**
 * @brief The state one validation run carries from element to element.
 *
 * Both members are caches whose entries are only sound for the tree and the
 * settings this run was started with, which is why they are built here rather
 * than held by the validator: a validator is a value the caller may keep, and a
 * cache keyed by node identity is not.
 */
struct OpenXmlDomValidationRun
{
    XmlLocationCache Locations;
    OpenXmlContentModelCache ContentModels;

    explicit OpenXmlDomValidationRun(const OpenXmlDomValidationSettings& settings)
        : ContentModels(settings.TargetVersion)
    {
    }
};

/** A content-model verdict, and whether the cross-check found the two matchers disagreeing. */
struct ContentModelVerdict
{
    ContentModelMatch Match;
    bool Mismatch = false;
};

class OpenXmlDomValidationHelpers
{
public:
    /**
     * @brief Reports whether the element is an extension payload holder.
     *
     * Every Open XML vocabulary carries the same extensibility idiom: an
     * `extLst` holding `ext` elements, each keyed by a GUID `@uri` and holding
     * whatever the producing application wanted to add. The schema declares that
     * payload as `xsd:any processContents="lax"`, so nothing about it is fixed.
     *
     * The imported metadata does model a handful of individual extensions as
     * their own types - `c:CT_DLblsExtension`, `c16r3:CT_DataDisplayOptions16` -
     * and the content model of an `extLst` names one of them. That is a typed
     * convenience for producing an extension, not a statement about which
     * extensions may be read: a single `extLst` holds whichever extensions the
     * writing application emitted, told apart by `@uri` rather than by position.
     * Checking the payload against whichever type the parent's model happened to
     * name rejects the chart, workbook and drawing extensions Excel writes.
     */
    static bool IsExtensionPayload(const OpenXMLElement& element)
    {
        if (element.QualifiedName().localName() != "ext")
        {
            return false;
        }

        const auto parent = element.Parent();
        const auto* parentClass = parent ? parent->ElementMetaClass() : nullptr;
        if (!parentClass)
        {
            return false;
        }

        // Bound to a local: a qualified name holds views, and reading one out of
        // a temporary would depend on where those views happen to point.
        const auto parentType = parentClass->TypeQualifiedName();
        return parentType.localName().find("ExtensionList") != std::string_view::npos;
    }

    /** Reports whether @p name appears anywhere in the content model. */
    static bool ParticleDeclares(const MetadataParticlePtr& particle, const OpenXmlQualifiedName& name)
    {
        if (!particle)
        {
            return false;
        }
        if (particle->Kind() == MetadataParticleKind::Element)
        {
            return static_cast<const MetadataElementParticle&>(*particle).Element() == name;
        }
        if (particle->Kind() == MetadataParticleKind::Any)
        {
            return false;
        }

        const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
        return std::any_of(composite.Children().begin(), composite.Children().end(),
                           [&name](const MetadataParticlePtr& child)
                           { return ParticleDeclares(child, name); });
    }

    /**
     * @brief Retries the content model with ignorable, unrecognized children removed.
     *
     * ECMA-376 Part 3 lets a producer mark whole namespaces `mc:Ignorable`: a
     * consumer that does not understand one drops its elements and carries on, so
     * they cannot make the surrounding content invalid. Every workbook Excel
     * writes relies on this - the revision namespaces are ignorable, and
     * `xr:revisionPtr` sits between elements the workbook's own content model
     * declares.
     *
     * "Understood" here means "named by this content model": a child the model
     * declares is kept and checked in place even when its namespace is ignorable,
     * so marking a namespace ignorable cannot silence a real ordering error. The
     * retry runs only after the plain match has already failed, which keeps the
     * ancestor walk for `mc:Ignorable` off the common path.
     */
    static bool MatchesWithoutIgnorableContent(const OpenXMLElement& element,
                                               const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                               const MetadataDefinition& metadata,
                                               const OpenXmlDomValidationSettings& settings,
                                               OpenXmlDomValidationRun& run)
    {
        const auto ignorable = CollectIgnorableNamespaces(element);
        if (ignorable.empty())
        {
            return false;
        }

        std::vector<std::shared_ptr<OpenXMLElement>> retained;
        retained.reserve(children.size());
        for (const auto& child : children)
        {
            const auto& name = child->QualifiedName();
            const bool drop = std::find(ignorable.begin(), ignorable.end(), name.namespaceUri()) != ignorable.end() &&
                              !ParticleDeclares(metadata.ParticleTree(), name);
            if (!drop)
            {
                retained.push_back(child);
            }
        }

        if (retained.size() == children.size())
        {
            return false;
        }

        return MatchContentModel(metadata.ParticleTree(), retained,
                                 element.QualifiedName().namespaceUri(), settings, run)
            .Match.Accepted;
    }

    /**
     * @brief Matches @p children against @p particle, by whichever matcher can answer.
     *
     * The automaton answers unless it declined to compile the model, which it
     * does rather than approximate one - see @ref OpenXmlContentModelAutomaton.
     * The reference matcher is exact for every model, so falling back to it
     * costs time and nothing else, and the shapes that get declined (a single
     * element repeated up to a large numeric bound) are the ones it handles
     * quickly anyway.
     */
    static ContentModelVerdict MatchContentModel(const MetadataParticlePtr& particle,
                                                 const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                                 std::string_view targetNamespace,
                                                 const OpenXmlDomValidationSettings& settings,
                                                 OpenXmlDomValidationRun& run)
    {
        const OpenXmlContentModelReference reference(settings.TargetVersion);
        const auto& automaton = run.ContentModels.Get(particle, children.size());
        if (!automaton.IsSupported())
        {
            return ContentModelVerdict{reference.Match(particle, children, targetNamespace), false};
        }

        ContentModelVerdict verdict{automaton.Match(children, targetNamespace), false};
        if (settings.CrossCheckContentModel)
        {
            verdict.Mismatch =
                reference.Match(particle, children, targetNamespace).Accepted != verdict.Match.Accepted;
        }
        return verdict;
    }

    static void ValidateOne(const OpenXMLElement& element,
                            const OpenXmlDomValidationSettings& settings,
                            OpenXmlDomValidationRun& run,
                            DiagnosticSink& sink)
    {
        auto& locations = run.Locations;
        const auto* metaClass = element.ElementMetaClass();
        if (!metaClass)
        {
            return;
        }

        const auto metadata = metaClass->GetMetadata();
        if (!metadata)
        {
            return;
        }

        if (!SupportsOfficeVersion(settings.TargetVersion, metadata->Summary().Availability))
        {
            ValidationIssue issue;
            issue.Severity = ValidationSeverity::Error;
            issue.Domain = ValidationDomain::Schema;
            issue.Id = ValidationErrorId::ElementVersionViolation;
            issue.Location = locations.Location(element);
            issue.Message = "Element '" + issue.Location.ElementName +
                            "' is not available in the requested Office version.";
            sink.Report(std::move(issue));
        }

        const auto validateConstraint = [&](const MetadataConstraintPtr& constraint)
        {
            if (!constraint)
            {
                return;
            }
            if (const auto versionConstraint =
                    std::dynamic_pointer_cast<MetadataOfficeVersionConstraint>(constraint))
            {
                if (!element.HasAttribute(versionConstraint->AttributeName()) ||
                    SupportsOfficeVersion(settings.TargetVersion,
                                          versionConstraint->Version()))
                {
                    return;
                }

                ValidationIssue issue;
                issue.Severity = ValidationSeverity::Error;
                issue.Domain = ValidationDomain::Schema;
                issue.Id = ValidationErrorId::AttributeVersionViolation;
                issue.Location = locations.Location(element, versionConstraint->AttributeName());
                issue.ConstraintId = versionConstraint->Identifier();
                issue.Message = "Attribute '" + issue.Location.AttributeName + "' of element '" +
                                issue.Location.ElementName +
                                "' is not available in the requested Office version.";
                sink.Report(std::move(issue));
                return;
            }

            auto result = constraint->Validate(element, locations);
            for (auto issue : result.Issues())
            {
                issue.Domain = ValidationDomain::Schema;
                if (issue.ConstraintId.empty())
                {
                    issue.ConstraintId = constraint->Identifier();
                }
                sink.Report(std::move(issue));
            }
        };

        for (const auto& attribute : metadata->Attributes())
        {
            // An attribute written as `attr=""` whose type has no empty member is one
            // defect, so it gets one diagnostic. Reporting it here rather than from
            // the required validator covers optional attributes too - the generator
            // emits a required validator only where the schema says required - and
            // stops the value facets from adding a second verdict about the same
            // empty text.
            std::string_view value;
            if (element.TryGetAttribute(attribute.Name, value) && value.empty() && !attribute.AllowsEmptyValue())
            {
                ValidationIssue issue;
                issue.Severity = ValidationSeverity::Error;
                issue.Domain = ValidationDomain::Schema;
                issue.Id = ValidationErrorId::EmptyAttribute;
                issue.Location = locations.Location(element, attribute.Name);
                issue.ConstraintId = "RequiredValidator";
                issue.Message = "Attribute '" + Detail::DescribeQualifiedName(attribute.Name) + "' has no value.";
                sink.Report(std::move(issue));
                continue;
            }

            for (const auto& validator : attribute.Validators)
            {
                validateConstraint(validator);
            }
        }

        for (const auto& constraint : metadata->Constraints())
        {
            validateConstraint(constraint);
        }

        if (metadata->ParticleTree() && !IsExtensionPayload(element))
        {
            // Type the children from this element's own content model: several
            // Open XML names are declared by more than one class, and the
            // element-name registry cannot tell them apart.
            const auto children = element.ChildrenInContentModel();
            const auto verdict = MatchContentModel(metadata->ParticleTree(), children,
                                                   element.QualifiedName().namespaceUri(), settings, run);

            if (verdict.Mismatch)
            {
                // A defect in this library, not in the document, and reported as
                // one: the two matchers were asked the same question and did not
                // give the same answer.
                ValidationIssue issue;
                issue.Severity = ValidationSeverity::Error;
                issue.Domain = ValidationDomain::Schema;
                issue.Id = ValidationErrorId::ContentModelCrossCheckMismatch;
                issue.Location = locations.Location(element);
                issue.Message = "Content model cross-check failed for element '" +
                                issue.Location.ElementName + "': the compiled automaton " +
                                (verdict.Match.Accepted ? "accepted" : "rejected") +
                                " content the reference matcher did not.";
                sink.Report(std::move(issue));
            }

            if (!verdict.Match.Accepted &&
                !MatchesWithoutIgnorableContent(element, children, *metadata, settings, run))
            {
                ValidationIssue issue;
                issue.Severity = ValidationSeverity::Error;
                issue.Domain = ValidationDomain::Schema;
                issue.Id = ValidationErrorId::ParticleConstraintViolation;
                issue.Message = DescribeParticleFailure(element, children, verdict.Match,
                                                        locations, issue.Location);
                sink.Report(std::move(issue));
            }
        }
    }

    /**
     * @brief Turns a content-model failure into a message that names the offender.
     *
     * The location is moved onto the offending child where there is one, because
     * that is the element the author has to look at; the message still names the
     * parent whose content model was violated.
     */
    static std::string DescribeParticleFailure(const OpenXMLElement& element,
                                               const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                               const ContentModelMatch& failure,
                                               XmlLocationCache& locations,
                                               XmlLocation& location)
    {
        location = locations.Location(element);
        const auto parentName = location.ElementName;

        std::string message = "Content of element '" + parentName + "' does not satisfy its schema particle: ";
        if (failure.ChildIndex < children.size() && children[failure.ChildIndex])
        {
            const auto& child = children[failure.ChildIndex];
            auto childLocation = locations.Location(*child);
            message += "child " + std::to_string(failure.ChildIndex + 1) + " is '" + childLocation.ElementName +
                       "', " + DescribeExpectation(failure) + ".";
            location = std::move(childLocation);
        }
        else
        {
            message += "content ends after " + std::to_string(children.size()) + " child element" +
                       (children.size() == 1 ? "" : "s") + ", " + DescribeExpectation(failure) + ".";
        }

        return message;
    }

    /** Renders the admissible names as a message fragment. */
    static std::string DescribeExpectation(const ContentModelMatch& failure)
    {
        const auto& expected = failure.Expected;
        if (expected.empty())
        {
            return failure.ExpectedWildcard
                       ? "where the content model admits only elements of another namespace"
                       : "which the content model does not allow here";
        }

        // A long list stops being useful; the first few names already point the
        // reader at the right part of the schema.
        constexpr Size kMaxNames = 6;
        std::string message =
            expected.size() == 1 && !failure.ExpectedWildcard ? "expected '" : "expected one of '";
        for (Size index = 0; index < expected.size() && index < kMaxNames; ++index)
        {
            if (index != 0)
            {
                message += "', '";
            }
            message += Detail::DescribeQualifiedName(expected[index]);
        }
        message.push_back('\'');
        if (expected.size() > kMaxNames)
        {
            message += " (and " + std::to_string(expected.size() - kMaxNames) + " more)";
        }
        if (failure.ExpectedWildcard)
        {
            message += " or an element the model admits through a wildcard";
        }
        return message;
    }

    /**
     * @brief Validates @p element and everything below it, in document order.
     *
     * The walk keeps its own stack rather than recursing per level. Nesting
     * depth here is whatever the part being validated contains, and a part can
     * come from a package the caller did not write: descending one call frame
     * per level would let a document nested a few thousand elements deep
     * overflow the stack instead of producing diagnostics. Children are pushed
     * in reverse so that diagnostics still arrive in document order.
     */
    static void ValidateTree(const OpenXMLElement& element,
                             const OpenXmlDomValidationSettings& settings,
                             OpenXmlDomValidationRun& run,
                             DiagnosticSink& sink)
    {
        std::vector<std::shared_ptr<OpenXMLElement>> pending;
        ValidateOne(element, settings, run, sink);

        const auto pushChildren = [&pending](const OpenXMLElement& parent)
        {
            const auto children = parent.ChildrenInContentModel();
            for (auto child = children.rbegin(); child != children.rend(); ++child)
            {
                if (*child)
                {
                    pending.push_back(*child);
                }
            }
        };

        pushChildren(element);
        while (!pending.empty())
        {
            const std::shared_ptr<OpenXMLElement> current = std::move(pending.back());
            pending.pop_back();

            ValidateOne(*current, settings, run, sink);
            pushChildren(*current);
        }
    }
};

OpenXmlDomValidator::OpenXmlDomValidator(OpenXmlDomValidationSettings settings) noexcept
    : settings_(settings)
{
}

ValidationResult OpenXmlDomValidator::Validate(const OpenXMLElement& element) const
{
    ValidationResult result;
    Validate(element, result);
    return result;
}

void OpenXmlDomValidator::Validate(const OpenXMLElement& element, DiagnosticSink& sink) const
{
    // One set of caches per run. The tree cannot change while it is being
    // validated, so a whole document's element paths cost a single pass instead
    // of one ancestor walk and sibling scan per diagnostic, and each content
    // model is compiled into an automaton once instead of once per element.
    OpenXmlDomValidationRun run(settings_);
    OpenXmlDomValidationHelpers::ValidateTree(element, settings_, run, sink);
}

ValidationResult OpenXmlDomValidator::ValidateElement(const OpenXMLElement& element) const
{
    ValidationResult result;
    ValidateElement(element, result);
    return result;
}

void OpenXmlDomValidator::ValidateElement(const OpenXMLElement& element, DiagnosticSink& sink) const
{
    OpenXmlDomValidationRun run(settings_);
    OpenXmlDomValidationHelpers::ValidateOne(element, settings_, run, sink);
}

} // namespace ExyokiOffice
