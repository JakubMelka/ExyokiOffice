// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice
{

/**
 * @brief The markup compatibility namespace defined by ECMA-376 Part 3.
 *
 * Unlike the format vocabularies, `mc:` describes how a consumer should treat
 * markup it does not understand, so it can appear in any part of any package.
 */
inline constexpr std::string_view kMarkupCompatibilityNamespace =
    "http://schemas.openxmlformats.org/markup-compatibility/2006";

/**
 * @brief Names of the markup compatibility elements and attributes.
 *
 * These are hand-written rather than generated: `mc:` is the extensibility
 * framework of ECMA-376 Part 3, not a content-model vocabulary of Part 1, so the
 * imported schema metadata has no description of it to generate from.
 */
struct EXYOKIOFFICE_EXPORT MarkupCompatibilityNames
{
    /** `mc:AlternateContent` - a set of mutually exclusive renderings of one thing. */
    static OpenXmlQualifiedName AlternateContent() noexcept;
    /** `mc:Choice` - a branch guarded by the namespaces it requires. */
    static OpenXmlQualifiedName Choice() noexcept;
    /** `mc:Fallback` - the branch used when no choice can be taken. */
    static OpenXmlQualifiedName Fallback() noexcept;

    /** `@Requires` on a choice: prefixes whose namespaces it needs. */
    static OpenXmlQualifiedName Requires() noexcept;
    /** `@mc:Ignorable`: prefixes whose elements and attributes may be dropped. */
    static OpenXmlQualifiedName Ignorable() noexcept;
    /** `@mc:ProcessContent`: ignorable elements whose children must be kept. */
    static OpenXmlQualifiedName ProcessContent() noexcept;
    /** `@mc:MustUnderstand`: prefixes a consumer has to understand to proceed. */
    static OpenXmlQualifiedName MustUnderstand() noexcept;
    /** `@mc:PreserveElements`: ignorable elements that must survive anyway. */
    static OpenXmlQualifiedName PreserveElements() noexcept;
    /** `@mc:PreserveAttributes`: ignorable attributes that must survive anyway. */
    static OpenXmlQualifiedName PreserveAttributes() noexcept;
};

class AlternateContentChoice;
class AlternateContentFallback;

/**
 * @brief `mc:AlternateContent` - a set of alternative renderings of one piece of content.
 *
 * A consumer takes the first @ref AlternateContentChoice whose required
 * namespaces it understands, and the @ref AlternateContentFallback when it can
 * take none of them. The element carries no content model of its own here: its
 * children are whole subtrees of any vocabulary, so validation deliberately has
 * nothing to say about them.
 */
class EXYOKIOFFICE_EXPORT AlternateContent : public OpenXmlCompositeElement
{
public:
    using Ptr = std::shared_ptr<AlternateContent>;

    AlternateContent() = default;
    ~AlternateContent() override = default;

    /** Returns the choices in declaration order, which is the order of preference. */
    std::vector<std::shared_ptr<AlternateContentChoice>> Choices() const;

    /** Returns the fallback branch, when the element declares one. */
    std::shared_ptr<AlternateContentFallback> Fallback() const;

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

/**
 * @brief `mc:Choice` - one branch of an @ref AlternateContent.
 *
 * The branch may be taken only when every prefix listed in `@Requires` resolves,
 * in this element's scope, to a namespace the consumer understands.
 */
class EXYOKIOFFICE_EXPORT AlternateContentChoice : public OpenXmlCompositeElement
{
public:
    using Ptr = std::shared_ptr<AlternateContentChoice>;

    AlternateContentChoice() = default;
    ~AlternateContentChoice() override = default;

    /** Raw `@Requires` value: a whitespace-separated list of namespace prefixes. */
    StringValue GetRequires() const;
    void SetRequires(const StringValue& value);

    /** The `@Requires` prefixes, split into individual entries. */
    std::vector<std::string> RequiredPrefixes() const;

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

/**
 * @brief `mc:Fallback` - the branch taken when no choice applies.
 */
class EXYOKIOFFICE_EXPORT AlternateContentFallback : public OpenXmlCompositeElement
{
public:
    using Ptr = std::shared_ptr<AlternateContentFallback>;

    AlternateContentFallback() = default;
    ~AlternateContentFallback() override = default;

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

/**
 * @brief The markup compatibility attributes declared on a single element.
 *
 * All of them are prefix or name lists that apply to the element and, for
 * `Ignorable`, to everything below it. Reading them never fails: an element
 * without markup compatibility attributes simply yields empty lists.
 */
struct EXYOKIOFFICE_EXPORT MarkupCompatibilityAttributes
{
    /** Prefixes whose content may be dropped when it is not understood. */
    std::vector<std::string> Ignorable;
    /** Ignorable element names whose *children* are processed instead of dropped. */
    std::vector<std::string> ProcessContent;
    /** Prefixes that must be understood; a consumer that cannot must stop. */
    std::vector<std::string> MustUnderstand;
    /** Ignorable element names to keep verbatim, or `*` for all of them. */
    std::vector<std::string> PreserveElements;
    /** Ignorable attribute names to keep verbatim, or `*` for all of them. */
    std::vector<std::string> PreserveAttributes;

    /** Reads the attributes declared on @p element. */
    [[nodiscard]] static MarkupCompatibilityAttributes Read(const OpenXMLElement& element);

    /** Reports whether the element declares no markup compatibility attributes at all. */
    [[nodiscard]] bool IsEmpty() const noexcept;
};

/**
 * @brief Controls how aggressively markup compatibility (`mc:`) elements are processed.
 *
 * Some documents carry different content for different Office versions. This mode
 * decides whether and where those branches are resolved while loading.
 */
enum class MarkupCompatibilityProcessMode
{
    /**
     * @brief Do not interpret markup compatibility constructs at all.
     *
     * The document is loaded exactly as it appears on disk and `mc:AlternateContent`
     * is left untouched. This is the only mode that keeps a save byte-identical to
     * what was loaded.
     */
    NoProcess = 0,
    /**
     * @brief Resolve markup compatibility in the main part and the parts it references.
     *
     * A middle ground for documents where only the primary content matters.
     */
    ProcessLoadedPartsOnly,
    /**
     * @brief Resolve markup compatibility in every part reachable in the package.
     */
    ProcessAllParts
};

/**
 * @brief Bundles the options that describe how markup compatibility is processed.
 *
 * Processing rewrites the loaded tree: chosen branches replace their
 * `mc:AlternateContent`, and ignorable content that no rule preserves is dropped.
 * That is not reversible, so any mode other than
 * @ref MarkupCompatibilityProcessMode::NoProcess makes a save lossy with respect
 * to the file that was opened. The default therefore processes nothing.
 */
struct EXYOKIOFFICE_EXPORT MarkupCompatibilityProcessSettings
{
    /** Selected processing mode. Defaults to leaving the markup untouched. */
    MarkupCompatibilityProcessMode ProcessMode = MarkupCompatibilityProcessMode::NoProcess;

    /**
     * @brief The Office generation whose vocabularies count as understood.
     *
     * A choice is taken when every namespace it requires belongs to this generation
     * or an earlier one. Ignored when the mode is `NoProcess`.
     */
    OpenXml::FileFormatVersions TargetFileFormatVersions = OpenXml::FileFormatVersions::Office2007;
};

/**
 * @brief Resolves markup compatibility markup into the content a consumer sees.
 *
 * The processor rewrites the tree in place, following ECMA-376 Part 3:
 *
 * - every `mc:AlternateContent` is replaced by the content of the first
 *   `mc:Choice` whose required namespaces are all understood, or by the
 *   `mc:Fallback` when no choice qualifies, or by nothing at all;
 * - elements and attributes in a namespace listed by `mc:Ignorable`, which the
 *   target generation does not understand, are dropped - unless `mc:ProcessContent`
 *   asks for their children to be kept in their place, or `mc:PreserveElements` /
 *   `mc:PreserveAttributes` protects them;
 * - `mc:MustUnderstand` naming a namespace outside the target generation stops
 *   processing, because the document has declared that it cannot be read correctly
 *   without it;
 * - the markup compatibility attributes the processor has consumed are removed, so
 *   the result no longer describes compatibility rules that have already been
 *   applied.
 *
 * Which namespaces count as understood follows from
 * @ref MarkupCompatibilityProcessSettings::TargetFileFormatVersions: a namespace is
 * understood when it is one of the well-known Open XML namespaces introduced by
 * that generation or an earlier one.
 *
 * Processing is destructive: the removed markup is gone from the tree, so saving
 * afterwards does not reproduce the file that was loaded.
 */
class EXYOKIOFFICE_EXPORT MarkupCompatibilityProcessor
{
public:
    /**
     * @param settings Target generation and mode. The mode itself is not consulted
     * here - callers decide which trees to run the processor over - but the target
     * generation is.
     * @param diagnostics Optional sink for the problems found while processing.
     */
    explicit MarkupCompatibilityProcessor(MarkupCompatibilityProcessSettings settings,
                                          DiagnosticSink* diagnostics = nullptr) noexcept;

    /**
     * @brief Processes @p element and everything below it.
     *
     * @return false when the content declares, through `mc:MustUnderstand`, that it
     * needs a namespace the target generation does not cover. The tree may already
     * have been partially rewritten in that case.
     */
    bool Process(const std::shared_ptr<OpenXMLElement>& element);

    /** Reports whether a namespace URI belongs to the configured target generation. */
    [[nodiscard]] bool IsUnderstoodNamespace(std::string_view namespaceUri) const;

private:
    MarkupCompatibilityProcessSettings settings_;
    DiagnosticSink* diagnostics_;
};

class OpenXmlPackage;
class OpenXmlPackagePart;

/**
 * @brief Applies markup compatibility processing to the XML parts of a package.
 *
 * Which parts are visited follows @ref MarkupCompatibilityProcessSettings::ProcessMode:
 * `NoProcess` visits none, `ProcessLoadedPartsOnly` visits @p mainPart and the parts
 * it references directly, and `ProcessAllParts` visits every part reachable through
 * the relationship graph. Non-XML parts are left alone.
 *
 * @param package Package whose parts are processed.
 * @param mainPart The document's main part; only consulted by `ProcessLoadedPartsOnly`.
 * @param settings Mode and target Office generation.
 * @param diagnostics Optional sink for the problems found along the way.
 * @return false when a part declares, through `mc:MustUnderstand`, that it needs a
 * namespace the target generation does not cover.
 */
EXYOKIOFFICE_EXPORT bool ProcessMarkupCompatibility(OpenXmlPackage& package,
                                                    const std::shared_ptr<OpenXmlPackagePart>& mainPart,
                                                    const MarkupCompatibilityProcessSettings& settings,
                                                    DiagnosticSink* diagnostics = nullptr);

} // namespace ExyokiOffice
