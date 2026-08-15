// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice
{

class OpenXMLElement;
class OpenXmlPackage;
class OpenXmlPackagePart;
class OpenXmlPartRootElement;
class OpenXMLElementClass;
struct OpenXmlPartContainerImpl;
struct OpenXmlPackagePartImpl;
struct OpenXmlPackageImpl;

/**
 * @brief Represents a relationship entry inside a .rels XML part.
 *
 * Open XML packages never connect parts directly. Instead, the package stores a list of
 * relationships for each part (and for the package root) inside the `_rels` folder.
 * Each relationship has a unique `Id`, a semantic `Type` URI, and a `Target`
 * pointing either to another part or to an external resource.
 *
 * Relationships are automatically created and destroyed when you call the helper
 * methods on OpenXmlPartContainer / OpenXmlPackagePart. Advanced users can persist
 * custom relationships by constructing an OpenXmlRelationship and registering it through
 * the container API.
 */
struct EXYOKIOFFICE_EXPORT OpenXmlRelationship
{
    /// Unique identifier referenced from XML markup (for example r:id="rId1").
    std::string Id;
    /// Fully qualified relationship type URI (e.g. http://schemas.../officeDocument).
    std::string Type;
    /// Target URI relative to the relationship source part.
    std::string Target;
    /// Raw TargetMode attribute value read from the .rels file, if present.
    std::string TargetMode;
    /// Indicates whether the target references an external resource instead of a package part.
    bool IsExternal = false;
};

/**
 * @brief Describes one incoming relationship edge pointing at a package part.
 *
 * OPC parts are graph nodes and relationships are directional edges. A single part can
 * be targeted by multiple source containers, so this structure records the source
 * container URI and the relationship metadata for each incoming edge independently.
 */
struct EXYOKIOFFICE_EXPORT OpenXmlIncomingRelationship
{
    /// URI of the source part, or "/" for the package-level relationship container.
    std::string SourceUri;
    /// Relationship Id in the source container.
    std::string Id;
    /// Relationship type URI in the source container.
    std::string Type;
    /// Raw target value as stored in the source .rels file.
    std::string Target;
    /// Raw TargetMode attribute value, if present.
    std::string TargetMode;
    /// Indicates whether this edge targets an external resource.
    bool IsExternal = false;
};

enum class OpenXmlPartKind
{
    Xml,
    Binary
};

/**
 * @brief The Office application family a package belongs to.
 *
 * Almost everything in the packaging layer is family-neutral. The exception is
 * that a few parts are stored in a different folder depending on the family, so
 * a package has to be able to say which one it is. A plain OpenXmlPackage, or
 * any package whose family is not known, reports Unknown and uses the
 * family-neutral defaults.
 */
enum class OpenXmlDocumentFamily
{
    /// Not a Word, Excel or PowerPoint package, or the family is unknown.
    Unknown,
    /// WordprocessingML (.docx and friends).
    Word,
    /// SpreadsheetML (.xlsx and friends).
    Excel,
    /// PresentationML (.pptx and friends).
    PowerPoint
};

/**
 * @brief Describes how a part should be materialized inside the package.
 *
 * A descriptor is the metadata counterpart to the strongly typed part class. It stores
 * the fixed relationship type URI, the default MIME content type, and the file naming
 * convention used when a part is attached to a container.
 *
 * All textual fields are `std::string_view` values. The descriptor does not own
 * those strings and does not copy them. The referenced character ranges must remain
 * valid for the lifetime of the descriptor and for as long as any package part stores
 * a copy of the descriptor. Generated descriptors satisfy this requirement by using
 * string literals and other static-storage metadata.
 *
 * Generated part classes expose a static Descriptor() method that returns a descriptor
 * tailored to the specific strongly typed part. Custom parts can be created by filling
 * out this struct manually, but custom descriptors should use string literals or other
 * long-lived storage for every string_view field. Do not initialize descriptor fields
 * from temporary `std::string` objects, local buffers, or strings that may be modified
 * or destroyed while the descriptor or its associated part remains alive.
 */
struct OpenXmlPartDescriptor
{
    /// Human-readable name of the part class (e.g. "MainDocumentPart"). Non-owning.
    std::string_view Name{};
    /// Relationship type URI used when the part is attached to another part or the package. Non-owning.
    std::string_view RelationshipType{};
    /// Default MIME content type persisted in [Content_Types].xml. Non-owning.
    std::string_view ContentType{};
    /// Logical file name prefix (e.g. "document", "worksheet"). Non-owning.
    std::string_view Target{};
    /// Optional file extension override (otherwise inferred from Kind/content type). Non-owning.
    std::string_view TargetExtension{};
    /// Relative folder under the parent where the part should be stored (e.g. "word", "xl/worksheets"). Non-owning.
    std::string_view DefaultPath{};
    /// Indicates whether this part stores XML markup or arbitrary binary data.
    OpenXmlPartKind Kind = OpenXmlPartKind::Xml;
    /// Minimum file format version that introduced this part.
    ExyokiOffice::OpenXml::FileFormatVersions Version = ExyokiOffice::OpenXml::FileFormatVersions::Office2007;
    /// Folder used inside a WordprocessingML package, when it differs. Non-owning.
    std::string_view WordPath{};
    /// Folder used inside a SpreadsheetML package, when it differs. Non-owning.
    std::string_view ExcelPath{};
    /// Folder used inside a PresentationML package, when it differs. Non-owning.
    std::string_view PowerPointPath{};

    /**
     * @brief Returns the folder to use inside a package of the given family.
     *
     * A few parts live in a different folder depending on which application
     * owns the package: the web extension task panes part is stored under
     * `word/webextensions` in a document, `xl/webextensions` in a workbook and
     * `ppt/webextensions` in a presentation. Every other part uses DefaultPath,
     * which is also the fallback when a family has no folder of its own.
     */
    [[nodiscard]] constexpr std::string_view PathFor(OpenXmlDocumentFamily family) const noexcept
    {
        switch (family)
        {
            case OpenXmlDocumentFamily::Word:
                return WordPath.empty() ? DefaultPath : WordPath;
            case OpenXmlDocumentFamily::Excel:
                return ExcelPath.empty() ? DefaultPath : ExcelPath;
            case OpenXmlDocumentFamily::PowerPoint:
                return PowerPointPath.empty() ? DefaultPath : PowerPointPath;
            case OpenXmlDocumentFamily::Unknown:
                break;
        }
        return DefaultPath;
    }
};

/**
 * @brief Base class that manages the relationship graph for packages and parts.
 *
 * OpenXmlPartContainer centralizes the logic that every package node needs: it owns the
 * children attached to the container, issues new relationship identifiers, and exposes
 * helper methods for derived classes.
 *
 * Relationships in Open XML are directional edges that connect one part with another
 * part (or an external resource) while tagging the edge with a `RelationshipType`. This
 * indirection allows different package flavors (Word, Excel, PowerPoint, custom
 * solutions) to use the same file structure.
 *
 * Typical workflow:
 * @code
 * // Given a package or part derived from OpenXmlPartContainer...
 * auto styles = std::make_shared<Packaging::StylesWithEffectsPart>();
 * container.AttachChildPart(styles, Packaging::StylesWithEffectsPart::Descriptor(), false);
 *
 * for (const auto& rel : container.Relationships())
 * {
 *     std::cout << rel.Id << " -> " << rel.Target << " (" << rel.Type << ")\n";
 * }
 * @endcode
 *
 * The container automatically updates `.rels` files when the package is saved.
 */
class EXYOKIOFFICE_EXPORT OpenXmlPartContainer
{
public:
    virtual ~OpenXmlPartContainer();

    /// Returns the collection of child parts attached to this container.
    const std::vector<std::shared_ptr<OpenXmlPackagePart>>& Parts() const noexcept;

    /// Returns the raw relationship entries originating from this container.
    const std::vector<OpenXmlRelationship>& Relationships() const noexcept;

    /**
     * @brief Deep-copies a package part and its relationship graph.
     *
     * The copied root receives a new relationship from this container and a new
     * package URI. Descendant XML and binary payloads are copied, outgoing
     * relationship identifiers are preserved (so relationship references in
     * the copied XML remain valid), and cycles or multiply referenced parts are
     * handled without duplicating the same graph node more than once.
     *
     * Relationship types listed in @p sharedRelationshipTypes are attached to
     * the original target instead of being cloned. This is intended for
     * immutable, presentation-wide resources such as slide layouts and media.
     * External relationships are always copied verbatim.
     *
     * @param source Part to copy. It must belong to the same package as this
     *               container; it need not be a child of this container.
     * @param sharedRelationshipTypes Relationship type URIs whose internal
     *               targets may be shared by the new graph.
     * @return The independently owned copied root part, or `nullptr` if the
     *         operation cannot be completed. On failure, the copied root and
     *         every newly reachable descendant are removed from the package.
     * @note The operation is package-local. It does not copy parts between
     *       different Open XML packages.
     */
    std::shared_ptr<OpenXmlPackagePart> ClonePartGraph(
        const std::shared_ptr<OpenXmlPackagePart>& source,
        const std::vector<std::string_view>& sharedRelationshipTypes = {});

    /**
     * @brief Imports a complete part graph from another Open XML package.
     *
     * Every internal part is recreated in this container's package. XML and
     * binary payloads, content types, external relationships, and relationship
     * identifiers are preserved, while relationship targets are rewritten to
     * the newly allocated destination URIs. Cycles and multiply referenced
     * nodes are retained without creating duplicate destination parts.
     *
     * @param source Root of the graph to import. The part may belong to any
     *               package, but this container must belong to a package.
     * @return The imported root, or `nullptr` if the graph cannot be imported.
     * @note No source part is shared with the destination package.
     */
    std::shared_ptr<OpenXmlPackagePart> ImportPartGraph(
        const std::shared_ptr<OpenXmlPackagePart>& source);

    /**
     * @brief Adds a relationship to a part that already belongs to this package.
     *
     * Unlike generated `Add...Part` methods, this operation does not allocate a
     * new URI or transfer ownership of the target. It is intended for OPC graph
     * edges whose target is shared, such as a slide-to-layout relationship or a
     * layout-to-master back-reference.
     *
     * @param part Existing target part in the same package.
     * @param relationshipType Relationship type URI for the new edge.
     * @return The allocated relationship identifier, or an empty string when
     *         the target is null, belongs to another package, or attachment fails.
     */
    std::string AddPartReference(const std::shared_ptr<OpenXmlPackagePart>& part,
                                 std::string_view relationshipType);

    /**
     * @brief Attaches a newly constructed custom part using the supplied descriptor.
     *
     * This is the public counterpart of generated `Add...Part` methods for opaque
     * extension payloads that have no generated part class. The descriptor controls
     * the allocated URI and relationship type.
     *
     * @param part Unattached part constructed with the same descriptor.
     * @param descriptor Stable metadata describing its URI, relationship, and storage kind.
     * @param allowMultiple Allocate a numeric suffix when sibling parts share a target name.
     * @return `true` when the part was registered and related to this container.
     */
    bool AttachCustomPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                          const OpenXmlPartDescriptor& descriptor,
                          bool allowMultiple = true);

    /**
     * @brief Removes this container's relationship to an existing part.
     * @param part Target part whose incoming edge from this container is removed.
     * @return `true` when an edge was removed; otherwise `false`.
     * @note The target remains in the package while reachable through another edge.
     */
    bool RemovePartReference(const std::shared_ptr<OpenXmlPackagePart>& part);

    /**
     * @brief Redirects one internal relationship to a different part of the same package.
     *
     * The relationship keeps its identifier, type, and target mode, so XML
     * markup referencing the id (`r:id`, `r:embed`, ...) stays valid; only the
     * target URI changes. When the previous target becomes unreachable from
     * the package root, it is removed together with its exclusive descendants,
     * exactly like RemovePartReference().
     *
     * @param relationshipId Identifier of an existing internal relationship of
     *        this container.
     * @param newTarget Existing part of the same package that becomes the new
     *        relationship target.
     * @return `true` when the relationship now points at @p newTarget. False
     *         when the id is unknown or external, or the target is invalid; the
     *         relationship is left unchanged in that case.
     */
    bool RetargetRelationship(std::string_view relationshipId,
                              const std::shared_ptr<OpenXmlPackagePart>& newTarget);

    std::vector<OpenXmlRelationship> RelationshipsByType(std::string_view relationshipType) const;
    std::string AddDataPartReferenceRelationship(std::string_view relationshipType, std::string targetUri);
    bool RemoveDataPartReferenceRelationship(std::string_view relationshipType,
                                             std::string_view relationshipId);

    [[nodiscard]] bool HasPackage() const noexcept;
    OpenXmlPackage* Package() noexcept;
    const OpenXmlPackage* Package() const noexcept;

    virtual std::filesystem::path ContainerPath() const = 0;
    virtual std::string ContainerUri() const = 0;

    /**
     * @brief Adds a relationship whose target is an external resource instead of a package part.
     * @param relationshipType Relationship type URI (for example http://.../attachedTemplate).
     * @param targetUri Absolute or relative URI pointing to the external resource.
     * @return The generated relationship Id (rIdX) that can be referenced from DOM elements, or empty on failure.
     */
    std::string AddExternalRelationship(std::string_view relationshipType, std::string targetUri);

    /// Removes an externally targeted relationship by Id (silently ignores missing entries).
    void RemoveExternalRelationship(const std::string& relationshipId);

protected:
    friend struct OpenXmlPackageImpl;
    explicit OpenXmlPartContainer(OpenXmlPackage* package = nullptr) noexcept;

    void SetPackage(OpenXmlPackage* package);
    void ResetContainerState();

    /**
     * @brief Drops the owning references to the child parts selected by @p shouldRelease.
     *
     * The Open XML relationship graph legitimately contains cycles: a slide layout points
     * back at its slide master, a notes slide back at the slide it annotates. Every
     * relationship edge is an owning `std::shared_ptr`, so no part in such a cycle ever
     * reaches a zero use count on its own and the whole component would stay alive after
     * the package stops tracking it. Parts leaving a package therefore have their edges
     * cut explicitly.
     *
     * Only the ownership edge goes away. The relationship entries of this container are
     * left untouched, while a released child loses the incoming relationships recorded
     * from this container so that it cannot outlive the container holding a dangling
     * source pointer.
     */
    void ReleaseChildParts(const std::function<bool(const OpenXmlPackagePart&)>& shouldRelease);

    /**
     * @brief Adds a child part and automatically creates the corresponding relationship entry.
     *
     * @param part        Optional pre-constructed part instance. When null, callers typically
     *                    instantiate the part immediately before calling this method.
     * @param descriptor  Metadata describing the part (relationship type, content type, etc.).
     * @param allowMultiple  When true, the generated URI is suffixed with an index to keep
     *                       multiple siblings separate (e.g. /word/document1.xml, /word/document2.xml).
     */
    bool AttachChildPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                         const OpenXmlPartDescriptor& descriptor,
                         bool allowMultiple);

    /**
     * @brief Removes a child part and deletes the associated relationship entry.
     * @return true when the part was attached and was successfully detached.
     */
    bool DetachChildPart(const std::shared_ptr<OpenXmlPackagePart>& part);
    bool AttachExistingChildPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                                 std::string relationshipId,
                                 std::string target,
                                 std::string relationshipType,
                                 bool isExternal,
                                 std::string targetMode = {});

    /// Generates a unique identifier (rIdX) suitable for a new relationship entry.
    std::string AllocateRelationshipId();
    /// Registers a pre-constructed relationship entry (advanced scenarios).
    void RegisterRelationship(OpenXmlRelationship relationship);
    /// Removes the relationship with the specified Id if it exists.
    void RemoveRelationship(const std::string& id);
    std::optional<OpenXmlRelationship> FindRelationship(const std::string& id) const;
    /// Visits every descendant part (depth-first) and executes the provided callback.
    void ForEachPart(const std::function<void(const std::shared_ptr<OpenXmlPackagePart>&)>& visitor) const;

    template <typename TPart>
    std::vector<std::shared_ptr<TPart>> GetPartsOfType() const;

    template <typename TPart>
    std::shared_ptr<TPart> GetPartOfType() const;

private:
    std::unique_ptr<OpenXmlPartContainerImpl> m_impl;
};

template <typename TPart>
std::vector<std::shared_ptr<TPart>> OpenXmlPartContainer::GetPartsOfType() const
{
    std::vector<std::shared_ptr<TPart>> result;
    const auto& parts = Parts();
    result.reserve(parts.size());
    for (const auto& part : parts)
    {
        if (auto typed = std::dynamic_pointer_cast<TPart>(part))
        {
            result.push_back(typed);
        }
    }
    return result;
}

template <typename TPart>
std::shared_ptr<TPart> OpenXmlPartContainer::GetPartOfType() const
{
    for (const auto& part : Parts())
    {
        if (auto typed = std::dynamic_pointer_cast<TPart>(part))
        {
            return typed;
        }
    }
    return nullptr;
}

/**
 * @brief Shared infrastructure for all concrete part types.
 *
 * This is the bridge between the packaging runtime and the strongly typed DOM.
 * OpenXmlPackagePart tracks the part URI, incoming relationship edges, and the
 * underlying XML/binary payload. Generator-produced classes simply inherit from this
 * base and expose typed accessors. The ContentType() returned by the part is written
 * to `[Content_Types].xml`, while the relationship metadata flows into the appropriate
 * `.rels` file.
 *
 * @code
 * auto header = std::make_shared<Packaging::HeaderPart>();
 * mainDocumentPart->AttachChildPart(header, Packaging::HeaderPart::Descriptor(), true);
 * header->SetXmlString("<w:hdr xmlns:w=\"...\">...</w:hdr>");
 * std::cout << header->ContentType(); // application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml
 * @endcode
 */
class EXYOKIOFFICE_EXPORT OpenXmlPackagePart : public OpenXmlPartContainer,
                                               public std::enable_shared_from_this<OpenXmlPackagePart>
{
public:
    using Ptr = std::shared_ptr<OpenXmlPackagePart>;

    explicit OpenXmlPackagePart(const OpenXmlPartDescriptor& descriptor, OpenXmlPackage* package = nullptr);
    virtual ~OpenXmlPackagePart();

    /// Returns the descriptor used to create the part (always valid).
    const OpenXmlPartDescriptor& Descriptor() const noexcept;

    /// Absolute URI of the part (for example /word/document.xml).
    const std::string& Uri() const noexcept;
    /// Relationship type URI advertised to parents.
    std::string_view RelationshipType() const noexcept;
    /// MIME content type persisted in [Content_Types].xml.
    std::string_view ContentType() const noexcept;
    /// Relationship identifier assigned by the only incoming container, or empty when ambiguous/unattached.
    const std::string& RelationshipId() const noexcept;
    /// Returns all relationship edges that currently target this part.
    const std::vector<OpenXmlIncomingRelationship>& IncomingRelationships() const noexcept;

    /// Convenience helper for `Descriptor().Kind == OpenXmlPartKind::Xml`.
    [[nodiscard]] bool IsXmlPart() const noexcept;
    /// Convenience helper for `Descriptor().Kind == OpenXmlPartKind::Binary`.
    [[nodiscard]] bool IsBinaryPart() const noexcept;

    /// Serializes the cached XML DOM tree into a string (XML parts only).
    std::string GetXmlString() const;
    /// Parses and replaces the cached XML DOM tree (XML parts only).
    void SetXmlString(const std::string& xml);
    /// Overrides the default content type (useful for custom parts).
    void SetContentType(std::string contentType);

    /// Returns the binary buffer stored in the part (binary parts only).
    std::vector<Byte> GetBinaryData() const;
    /// Replaces the binary buffer stored in the part (binary parts only).
    void SetBinaryData(std::vector<Byte> data);

    /**
     * @brief Returns true when the bytes read from the package file are still kept.
     *
     * Retention is controlled by OpenXmlPackage::SetPartByteRetention, applies to
     * XML parts, and is off for parts that were created in memory rather than
     * loaded. A binary part needs no retention: its loaded bytes are exactly what
     * GetBinaryData() returns until you replace them.
     */
    [[nodiscard]] bool HasOriginalBytes() const noexcept;

    /**
     * @brief Returns the part exactly as it was stored in the loaded package.
     *
     * A digital signature digests the stored byte stream, not the parsed tree.
     * Because saving re-serializes XML parts, those bytes usually differ from
     * what GetXmlString() produces; this accessor is what makes it possible to
     * verify a signature created by another application.
     *
     * @return The original bytes, or an empty vector when they were not retained.
     */
    [[nodiscard]] std::vector<Byte> GetOriginalBytes() const;

    /// Wraps the document element inside an OpenXmlPartRootElement-derived instance.
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetRootElement() const;

    /// Returns the parent folder for the part (e.g. /word or /xl/worksheets).
    std::filesystem::path ContainerPath() const override;
    /// Same as ContainerPath() but as a normalized URI string.
    std::string ContainerUri() const override;

protected:
    /// Internal helper invoked by containers to update legacy parent references.
    void SetParent(OpenXmlPartContainer* parent, std::string relationshipId);
    /// Assigns the canonical URI once the part is attached to a package.
    void SetUri(std::string uri);
    /// Initializes an empty XML part with the specified root element.
    void InitializeRootElement(const OpenXMLElementClass* metaClass);
    void InitializeRootElement(OpenXmlQualifiedName name);

    /// Stores the bytes read from the package file (called by the OPC loader).
    void SetOriginalBytes(std::vector<Byte> data);

private:
    friend class OpenXmlPackage;
    friend struct OpenXmlPackageImpl;
    friend class OpenXmlPartContainer;

    void AddIncomingRelationship(OpenXmlPartContainer* source, const OpenXmlRelationship& relationship);
    void RemoveIncomingRelationshipsFrom(const OpenXmlPartContainer* source);
    void RemoveIncomingRelationship(const OpenXmlPartContainer* source, std::string_view relationshipId);
    /// Non-throwing teardown path used by ~OpenXmlPartContainer(): drops edges from `source` and
    /// clears a dangling Parent() reference without rebuilding the (allocating) relationship view cache.
    void DetachIncomingRelationshipsFrom(const OpenXmlPartContainer* source) noexcept;
    void ClearIncomingRelationships();
    void RefreshPrimaryRelationship();

    std::unique_ptr<OpenXmlPackagePartImpl> m_impl;
};

/** Preserves package parts for which no generated strongly typed class exists. */
class EXYOKIOFFICE_EXPORT OpaquePackagePart final : public OpenXmlPackagePart
{
public:
    OpaquePackagePart(std::string relationshipType,
                      std::string contentType,
                      OpenXmlPartKind kind,
                      OpenXmlPackage* package = nullptr);
};

} // namespace ExyokiOffice
