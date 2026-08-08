// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Flags.h"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "FileFormatVersions.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ExyokiOffice
{

namespace Pugi
{
struct xml_node_struct;
}

class OpenXMLElement;
class OpenXMLElementClass;

/**
 * @internal
 * @brief Opaque handle to the XML node backing an element.
 *
 * A trivially copyable, pointer-sized value. It owns nothing and does not keep
 * the underlying document alive: the XML memory belongs to the package part
 * that parsed it. Only the DOM implementation can turn the handle back into a
 * live XML node.
 */
class OpenXmlNodeHandle
{
public:
    OpenXmlNodeHandle() noexcept = default;
    explicit OpenXmlNodeHandle(Pugi::xml_node_struct* node) noexcept
        : m_node(node)
    {
    }

    Pugi::xml_node_struct* Get() const noexcept { return m_node; }

    explicit operator bool() const noexcept { return m_node != nullptr; }

    friend bool operator==(OpenXmlNodeHandle left, OpenXmlNodeHandle right) noexcept
    {
        return left.m_node == right.m_node;
    }

private:
    Pugi::xml_node_struct* m_node = nullptr;
};

enum class OpenXmlMetaEnumFlag : UInt32
{
    None = 0,
};

using OpenXmlMetaEnumFlags = Flags<OpenXmlMetaEnumFlag>;

class OpenXmlMetaEnum
{
public:
    virtual ~OpenXmlMetaEnum() = default;

    virtual OpenXmlQualifiedName TypeQualifiedName() const noexcept = 0;
    virtual OpenXml::FileFormatVersions GetVersion() const noexcept;
    virtual OpenXml::FileFormatVersions GetVersionForEnumValue(UInt32 rawValue) const noexcept;
    virtual OpenXmlMetaEnumFlags GetFlags() const noexcept;
    virtual std::string_view ToString(UInt32 rawValue) const = 0;
    virtual UInt32 FromString(std::string_view value) const = 0;
};

enum class OpenXmlElementClassFlag : UInt32
{
    None = 0,
    IsAbstract = 0x1,
    IsDerived = 0x2,
    IsLeafElement = 0x4,
    IsLeafText = 0x8,
};

using OpenXmlElementClassFlags = Flags<OpenXmlElementClassFlag>;

/** Selects whether a copy operation duplicates only one element or its whole subtree. */
enum class OpenXmlCloneDepth
{
    /** Copies the element name and its attributes, but none of its content. */
    Shallow,
    /** Copies the element together with every descendant node. */
    Deep,
};

/**
 * @brief Selects how an existing subtree is positioned inside its new parent.
 *
 * `Exact` places the subtree exactly where the caller asked, which may violate
 * the parent's content model. `SchemaOrdered` treats the requested position as a
 * preference and moves it to the closest position the parent's schema particle
 * admits — the same rule typed child creation applies.
 */
enum class OpenXmlPlacement
{
    /** Honors the requested physical position without consulting schema metadata. */
    Exact,
    /** Corrects the requested position against the parent's content model. */
    SchemaOrdered,
};

/**
 * @brief Base class for all strongly typed Open XML DOM elements.
 *
 * The class exposes metadata queries, attribute helpers, and structural navigation that
 * transparently return the correct derived type by routing requests through the generated
 * OpenXmlElementFactory.
 */
class EXYOKIOFFICE_EXPORT OpenXMLElement
{
public:
    using Ptr = std::shared_ptr<OpenXMLElement>;

    virtual ~OpenXMLElement() = default;
    OpenXMLElement(const OpenXMLElement&) = delete;
    OpenXMLElement& operator=(const OpenXMLElement&) = delete;
    OpenXMLElement(OpenXMLElement&&) = delete;
    OpenXMLElement& operator=(OpenXMLElement&&) = delete;

    /**
     * @brief Returns the meta-class that describes this instance.
     */
    virtual const OpenXMLElementClass* ElementMetaClass() const noexcept = 0;

    /**
     * @brief Gets the qualified element name defined by the meta-class.
     */
    OpenXmlQualifiedName QualifiedName() const;

    /**
     * @brief Gets the strongly typed Open XML schema type name for this element.
     */
    OpenXmlQualifiedName TypeQualifiedName() const;

    /**
     * @brief Returns the XML location of this element for diagnostics.
     *
     * The path carries positional predicates (`/w:document/w:body/w:p[3]`), so it
     * addresses this element and not merely every element of its name.
     */
    XmlLocation GetXmlLocation() const noexcept;

    /**
     * @brief Returns this element's location, pointing at one of its attributes.
     *
     * The attribute does not have to exist: a missing required attribute is one of
     * the things a diagnostic needs to point at. A present attribute is reported
     * as the document spells it, an absent one using the prefix its namespace is
     * bound to.
     */
    XmlLocation GetXmlLocation(const OpenXmlQualifiedName& attribute) const noexcept;

    /**
     * @brief Indicates whether this wrapper references a valid XML node.
     */
    [[nodiscard]] bool IsNull() const noexcept;

    /**
     * @brief Checks whether two wrappers reference the same underlying XML node.
     *
     * OpenXMLElement wrappers are lightweight views and the same XML node may be
     * materialized as multiple distinct shared_ptr instances. Use this method
     * when node identity matters.
     */
    [[nodiscard]] bool IsSameNode(const OpenXMLElement& other) const noexcept;

    /**
     * @brief Copies this element as a new child of `targetParent`.
     *
     * `targetParent` may belong to a completely different document, part, or
     * package than this element — this is the primitive cross-document
     * "clone a subtree" operation. The copy shares no XML node with the
     * original: mutating one never affects the other. Every namespace
     * prefix used anywhere in the copied content (on element names or attribute
     * names), and the default namespace binding when the content relies on one,
     * is resolved in this element's own context before copying and, if the
     * destination does not already resolve it to the same URI, is redeclared
     * directly on the copied root. This keeps the copy self-contained and valid
     * regardless of what namespaces the destination document happens to already
     * declare.
     *
     * Namespace prefixes appearing inside *attribute values* (`xsi:type` and
     * other QName-valued content) are not rewritten; a subtree that depends on
     * them must be copied into a destination that declares the same prefixes.
     *
     * Wrapper invalidation: this operation destroys nothing, so every existing
     * wrapper stays valid.
     *
     * @param targetParent Parent element to receive the copy.
     * @param before Existing child of `targetParent` before which the copy
     * is inserted; when null, the copy is appended as the last child.
     * @param depth Whether descendants are copied as well.
     * @param placement Whether `before` is honored exactly or corrected against
     * the content model of `targetParent`.
     * @return Wrapper for the newly created copy, or nullptr when this
     * element or `targetParent` does not reference a valid node.
     */
    std::shared_ptr<OpenXMLElement> CopyInto(const std::shared_ptr<OpenXMLElement>& targetParent,
                                             const std::shared_ptr<OpenXMLElement>& before = nullptr,
                                             OpenXmlCloneDepth depth = OpenXmlCloneDepth::Deep,
                                             OpenXmlPlacement placement = OpenXmlPlacement::Exact) const;

    /**
     * @brief Copies this element directly after an existing element.
     *
     * Equivalent to @ref CopyInto() with `after`'s parent as the target parent
     * and `after`'s next sibling as the insertion anchor.
     *
     * @param after Element the copy is placed behind; must have a parent.
     * @param depth Whether descendants are copied as well.
     * @return Wrapper for the newly created copy, or nullptr when either
     * element does not reference a valid node or `after` has no parent.
     */
    std::shared_ptr<OpenXMLElement> CopyAfter(const std::shared_ptr<OpenXMLElement>& after,
                                              OpenXmlCloneDepth depth = OpenXmlCloneDepth::Deep) const;

    /**
     * @brief Moves this element and its whole subtree under `targetParent`.
     *
     * Within one document the XML node is relinked rather than recreated, so the
     * node keeps its identity and **every** wrapper that views this element or
     * any of its descendants stays valid — including this one. Moving into a
     * different document is not a relink: the subtree is copied with the same
     * namespace rules as @ref CopyInto() and the original is destroyed, so this
     * wrapper is reset to null and all other wrappers into the moved subtree
     * become stale. Use the returned wrapper afterwards in both cases.
     *
     * The move is rejected (nullptr, tree unchanged) when `targetParent` is this
     * element or one of its descendants.
     *
     * @param targetParent Parent element to receive this element.
     * @param before Existing child of `targetParent` before which this element
     * is placed; when null, it is appended as the last child.
     * @param placement Whether `before` is honored exactly or corrected against
     * the content model of `targetParent`.
     * @return Wrapper for this element at its new position, or nullptr when the
     * move could not be performed.
     */
    std::shared_ptr<OpenXMLElement> MoveInto(const std::shared_ptr<OpenXMLElement>& targetParent,
                                             const std::shared_ptr<OpenXMLElement>& before = nullptr,
                                             OpenXmlPlacement placement = OpenXmlPlacement::Exact);

    /**
     * @brief Moves this element directly after an existing element.
     *
     * Shares the semantics and the wrapper rules of @ref MoveInto().
     *
     * @param after Element this one is placed behind; must have a parent.
     * @return Wrapper for this element at its new position, or nullptr when the
     * move could not be performed.
     */
    std::shared_ptr<OpenXMLElement> MoveAfter(const std::shared_ptr<OpenXMLElement>& after);

    /**
     * @brief Puts `replacement` at this element's exact position and destroys this element.
     *
     * `replacement` is moved when it lives in the same document and copied when
     * it does not, following @ref MoveInto() and @ref CopyInto(). Because the
     * replaced element defines the position, schema ordering does not apply.
     *
     * Wrapper invalidation: this element is destroyed, so this wrapper is reset
     * to null and every other wrapper into its former subtree becomes stale.
     *
     * @param replacement Element to put in this element's place; must not be
     * this element or one of its ancestors.
     * @return Wrapper for `replacement` at its new position, or nullptr when the
     * replacement could not be performed. Nothing is destroyed on failure.
     */
    std::shared_ptr<OpenXMLElement> ReplaceWith(const std::shared_ptr<OpenXMLElement>& replacement);

    /**
     * @brief Replaces this element with its own content, keeping document order.
     *
     * Every child node — elements as well as text — is moved into this element's
     * position in order, and this element is then destroyed. This is the
     * "unwrap" primitive: it is what promoting the content of a chosen markup
     * compatibility branch, or dissolving a container element, needs. The moved
     * children keep their node identity, so wrappers viewing them stay valid.
     *
     * Wrapper invalidation: this element is destroyed, so this wrapper is reset
     * to null. Wrappers into the promoted children remain valid.
     *
     * @return Wrappers for the promoted element children in document order, or
     * an empty vector when this element has no valid node or no parent.
     */
    std::vector<std::shared_ptr<OpenXMLElement>> ReplaceWithChildren();

    /**
     * @brief Detaches this element from its parent and destroys its subtree.
     *
     * Wrapper invalidation: this wrapper is reset to null, so @ref IsNull()
     * starts reporting true for it. Every other wrapper that views this element
     * or one of its descendants becomes stale and must not be used again —
     * detecting that is the caller's responsibility.
     *
     * @return true when the element was removed.
     */
    bool Remove();

    /**
     * @brief Destroys every child node of this element, text content included.
     *
     * Wrapper invalidation: wrappers into the removed children become stale.
     * This wrapper stays valid and now views an empty element.
     *
     * @return true when this element references a valid node.
     */
    bool RemoveAllChildren();

    /**
     * @brief Returns the parent element or nullptr if the current node is the root.
     */
    std::shared_ptr<OpenXMLElement> Parent() const;

    /**
     * @brief Materializes all element children in document order.
     *
     * Children are typed from their element name alone. Around 190 Open XML
     * element names are declared by more than one class — `w:rPr` is run
     * properties in a run but style run properties in a style, `x:r` is a pivot
     * cache record under `x:pivotCacheRecords` but a rich text run under
     * `x:si`. For those names this method returns the class the element-name
     * registry maps to, which need not be the one the schema expects at this
     * position. Use @ref ChildrenInContentModel() when the surrounding content
     * model must decide, or the typed @ref Elements() and
     * @ref GetFirstChildOfType() when the caller already knows the type it wants.
     */
    std::vector<std::shared_ptr<OpenXMLElement>> Children() const;

    /**
     * @brief Materializes all element children, typed by this element's content model.
     *
     * Identical to @ref Children() except that a child whose element name is
     * declared by more than one class is typed from this element's particle
     * metadata, which records the schema type expected at each child position.
     * Children that the content model does not declare, and elements whose name
     * is unambiguous, fall back to @ref Children()'s name-based resolution.
     *
     * This is what schema validation needs: it is the schema's own view of the
     * tree rather than the registry's best guess. Ordinary editing code should
     * keep using @ref Children() or the typed accessors, whose behavior this
     * method deliberately does not change.
     */
    std::vector<std::shared_ptr<OpenXMLElement>> ChildrenInContentModel() const;

    /**
     * @brief Finds the first child with the specified qualified name.
     */
    std::shared_ptr<OpenXMLElement> GetChild(const OpenXmlQualifiedName& name) const;

    /**
     * @brief Returns the first element sibling under the same parent.
     */
    std::shared_ptr<OpenXMLElement> FirstSibling() const;

    /**
     * @brief Returns the previous element sibling, if any.
     */
    std::shared_ptr<OpenXMLElement> PreviousSibling() const;

    /**
     * @brief Returns the next element sibling, if any.
     */
    std::shared_ptr<OpenXMLElement> NextSibling() const;

    /**
     * @brief Returns the last element sibling that shares the same parent.
     */
    std::shared_ptr<OpenXMLElement> LastSibling() const;

    template <typename TElement>
    std::shared_ptr<TElement> AppendChild();

    template <typename TElement>
    std::shared_ptr<TElement> InsertChild(const std::shared_ptr<OpenXMLElement>& before = nullptr);

    /**
     * @brief Appends a child without consulting schema particle metadata.
     *
     * This advanced operation may create schema-invalid markup. Prefer AppendChild()
     * unless exact physical placement is required, and validate the resulting subtree.
     */
    template <typename TElement>
    std::shared_ptr<TElement> AppendChildRaw();

    /**
     * @brief Inserts a child at an exact physical position without schema ordering.
     *
     * A null or foreign `before` anchor appends the child. This operation may violate
     * the parent's content model and is intentionally separated from InsertChild().
     */
    template <typename TElement>
    std::shared_ptr<TElement> InsertChildRaw(const std::shared_ptr<OpenXMLElement>& before = nullptr);

    /**
     * @brief Creates a new child directly behind an existing child.
     *
     * The counterpart of InsertChild(), which inserts in front of its anchor. The
     * requested position is a preference: it is corrected when the parent's
     * content model does not admit it. A null or foreign `after` anchor appends
     * the child.
     */
    template <typename TElement>
    std::shared_ptr<TElement> InsertChildAfter(const std::shared_ptr<OpenXMLElement>& after);

    /**
     * @brief Creates a new child directly behind an existing child, ignoring schema ordering.
     *
     * This advanced operation may create schema-invalid markup. Prefer
     * InsertChildAfter() unless exact physical placement is required.
     */
    template <typename TElement>
    std::shared_ptr<TElement> InsertChildAfterRaw(const std::shared_ptr<OpenXMLElement>& after);

    /**
     * @brief Removes a direct child and destroys its subtree.
     *
     * Wrapper invalidation: the `child` wrapper is reset to null. Every other
     * wrapper into the removed subtree becomes stale and must not be used again.
     */
    template <typename TElement>
    bool RemoveChild(const std::shared_ptr<TElement>& child);

    template <typename TElement>
    std::vector<std::shared_ptr<TElement>> Elements() const;

    template <typename TElement>
    std::vector<std::shared_ptr<TElement>> Descendants() const;

    template <typename TElement>
    std::shared_ptr<TElement> GetFirstChildOfType() const;

    template <typename TElement>
    std::shared_ptr<TElement> GetNextSiblingOfType() const;

    template <typename TElement>
    std::shared_ptr<TElement> GetPreviousSiblingOfType() const;

    /**
     * @brief Checks whether an attribute with the given qualified name exists.
     */
    [[nodiscard]] bool HasAttribute(const OpenXmlQualifiedName& name) const;

    /**
     * @brief Gets the attribute value as a string view (empty view when missing).
     */
    std::string_view GetAttribute(const OpenXmlQualifiedName& name) const;

    /**
     * @brief Retrieves the attribute value if present.
     *
     * @param name Attribute name to read.
     * @param value Receives the attribute content when available.
     * @return true when the attribute exists.
     */
    [[nodiscard]] bool TryGetAttribute(const OpenXmlQualifiedName& name, std::string_view& value) const;

    /**
     * @brief Sets or replaces the attribute value using raw text.
     */
    void SetAttribute(const OpenXmlQualifiedName& name, std::string_view value);

    /**
     * @brief Removes the attribute with the specified qualified name.
     */
    void RemoveAttribute(const OpenXmlQualifiedName& name);

    template <typename TValue>
    [[nodiscard]] bool TryGetAttributeValue(const OpenXmlQualifiedName& attribute, TValue& value) const
    {
        value = TValue{};

        std::string_view text;
        if (TryGetAttribute(attribute, text))
        {
            value = OpenXmlSimpleValueConvertor::FromString<TValue>(text);
            return value.IsDefined();
        }

        return false;
    }

    template <typename TValue>
    TValue GetAttributeValue(const OpenXmlQualifiedName& attribute) const
    {
        TValue value{};
        static_cast<void>(TryGetAttributeValue(attribute, value));
        return value;
    }

    template <typename TValue>
    void SetAttributeValue(const OpenXmlQualifiedName& attribute, const TValue& value)
    {
        if (!value.IsDefined())
        {
            RemoveAttribute(attribute);
            return;
        }

        const auto text = OpenXmlSimpleValueConvertor::ToString(value);
        SetAttribute(attribute, text);
    }

    /**
     * @internal
     * @brief Binds this wrapper to an XML node.
     */
    void SetNodeHandle(OpenXmlNodeHandle node) noexcept;

    /**
     * @internal
     * @brief Returns the handle of the XML node backing this element.
     *
     * Exposed only for internal/package-layer code that needs the live XML
     * node behind a wrapper (for example to deep-copy a subtree across
     * documents with pugixml's own node-copy operations). Application code
     * should never need this; the handle is opaque outside the DOM
     * implementation.
     */
    OpenXmlNodeHandle GetNodeHandle() const noexcept;

    /** @return `true` when both wrappers refer to the same underlying XML node. */
    [[nodiscard]] bool IsSameNode(const std::shared_ptr<OpenXMLElement>& other) const noexcept;

private:
    std::shared_ptr<OpenXMLElement> AppendChildInternal(const OpenXMLElementClass* metaClass,
                                                        bool schemaAware);
    std::shared_ptr<OpenXMLElement> InsertChildInternal(const OpenXMLElementClass* metaClass,
                                                        const std::shared_ptr<OpenXMLElement>& before,
                                                        bool schemaAware);
    std::shared_ptr<OpenXMLElement> InsertChildAfterInternal(const OpenXMLElementClass* metaClass,
                                                             const std::shared_ptr<OpenXMLElement>& after,
                                                             bool schemaAware);
    bool RemoveChildInternal(const std::shared_ptr<OpenXMLElement>& child);
    std::vector<std::shared_ptr<OpenXMLElement>> ElementsInternal(const OpenXMLElementClass* metaClass) const;
    std::vector<std::shared_ptr<OpenXMLElement>> DescendantsInternal(const OpenXMLElementClass* metaClass) const;
    std::shared_ptr<OpenXMLElement> GetFirstChildOfTypeInternal(const OpenXMLElementClass* metaClass) const;
    std::shared_ptr<OpenXMLElement> GetNextSiblingOfTypeInternal(const OpenXMLElementClass* metaClass) const;
    std::shared_ptr<OpenXMLElement> GetPreviousSiblingOfTypeInternal(const OpenXMLElementClass* metaClass) const;

protected:
    OpenXMLElement() = default;

    OpenXmlNodeHandle m_node;
};

class EXYOKIOFFICE_EXPORT OpenXmlCompositeElement : public OpenXMLElement
{
public:
    using Ptr = std::shared_ptr<OpenXmlCompositeElement>;

    virtual ~OpenXmlCompositeElement() = default;

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

/** Generic wrapper used to preserve and traverse XML elements unknown to the generated schema. */
class EXYOKIOFFICE_EXPORT GenericOpenXmlElement final : public OpenXMLElement
{
public:
    explicit GenericOpenXmlElement(OpenXmlQualifiedName name);
    ~GenericOpenXmlElement() override;

    const OpenXMLElementClass* ElementMetaClass() const noexcept override;

private:
    std::string namespaceUri_;
    std::string localName_;
    std::unique_ptr<OpenXMLElementClass> metaClass_;
};

class EXYOKIOFFICE_EXPORT OpenXmlPartRootElement : public OpenXmlCompositeElement
{
public:
    using Ptr = std::shared_ptr<OpenXmlPartRootElement>;

    virtual ~OpenXmlPartRootElement() = default;

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

class EXYOKIOFFICE_EXPORT OpenXmlLeafElement : public OpenXMLElement
{
public:
    using Ptr = std::shared_ptr<OpenXmlLeafElement>;

    virtual ~OpenXmlLeafElement() = default;

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

class EXYOKIOFFICE_EXPORT OpenXmlLeafTextElement : public OpenXmlLeafElement
{
public:
    using Ptr = std::shared_ptr<OpenXmlLeafTextElement>;

    virtual ~OpenXmlLeafTextElement() = default;

    std::string_view GetText() const;
    void SetText(std::string_view text);

    static const OpenXMLElementClass* StaticMetaClass() noexcept;
    const OpenXMLElementClass* ElementMetaClass() const noexcept override;
};

class EXYOKIOFFICE_EXPORT OpenXMLElementClass
{
public:
    OpenXMLElementClass() noexcept;

    virtual ~OpenXMLElementClass() = default;

    virtual const OpenXMLElementClass* GetBaseMetaClass() const noexcept = 0;
    virtual OpenXmlQualifiedName QualifiedName() const noexcept = 0;
    virtual OpenXmlQualifiedName TypeQualifiedName() const noexcept = 0;
    virtual OpenXml::FileFormatVersions GetVersion() const noexcept = 0;
    virtual OpenXmlElementClassFlags GetFlags() const noexcept;
    virtual std::span<const OpenXmlAttribute> GetAttributes() const noexcept;

    std::shared_ptr<MetadataDefinition> GetMetadata() const;

    [[nodiscard]] virtual bool Inherits(const OpenXMLElementClass* base) const noexcept;
    virtual std::shared_ptr<OpenXMLElement> Create() const = 0;

protected:
    virtual void ConfigureMetadata(MetadataBuilder& builder) const;

private:
    mutable std::once_flag metadataOnce_;
    mutable std::shared_ptr<MetadataDefinition> metadata_;
};

template <typename T>
const OpenXMLElementClass* openxmlelement_class()
{
    return T::StaticMetaClass();
}

template <typename T>
std::shared_ptr<T> openxmlelement_cast(const std::shared_ptr<OpenXMLElement>& object)
{
    if (!object)
    {
        return nullptr;
    }

    const auto* actual = object->ElementMetaClass();
    const auto* target = openxmlelement_class<T>();
    if (!actual->Inherits(target))
    {
        return nullptr;
    }

    return std::static_pointer_cast<T>(object);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::AppendChild()
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = AppendChildInternal(TElement::StaticMetaClass(), true);
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::InsertChild(const std::shared_ptr<OpenXMLElement>& before)
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = InsertChildInternal(TElement::StaticMetaClass(), before, true);
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::AppendChildRaw()
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = AppendChildInternal(TElement::StaticMetaClass(), false);
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::InsertChildRaw(const std::shared_ptr<OpenXMLElement>& before)
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = InsertChildInternal(TElement::StaticMetaClass(), before, false);
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::InsertChildAfter(const std::shared_ptr<OpenXMLElement>& after)
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = InsertChildAfterInternal(TElement::StaticMetaClass(), after, true);
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::InsertChildAfterRaw(const std::shared_ptr<OpenXMLElement>& after)
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = InsertChildAfterInternal(TElement::StaticMetaClass(), after, false);
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
bool OpenXMLElement::RemoveChild(const std::shared_ptr<TElement>& child)
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    return RemoveChildInternal(child);
}

template <typename TElement>
std::vector<std::shared_ptr<TElement>> OpenXMLElement::Elements() const
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    std::vector<std::shared_ptr<TElement>> result;
    for (auto& element : ElementsInternal(TElement::StaticMetaClass()))
    {
        result.push_back(std::static_pointer_cast<TElement>(std::move(element)));
    }
    return result;
}

template <typename TElement>
std::vector<std::shared_ptr<TElement>> OpenXMLElement::Descendants() const
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    std::vector<std::shared_ptr<TElement>> result;
    for (auto& element : DescendantsInternal(TElement::StaticMetaClass()))
    {
        result.push_back(std::static_pointer_cast<TElement>(std::move(element)));
    }
    return result;
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::GetFirstChildOfType() const
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = GetFirstChildOfTypeInternal(TElement::StaticMetaClass());
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::GetNextSiblingOfType() const
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = GetNextSiblingOfTypeInternal(TElement::StaticMetaClass());
    return std::static_pointer_cast<TElement>(element);
}

template <typename TElement>
std::shared_ptr<TElement> OpenXMLElement::GetPreviousSiblingOfType() const
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>,
                  "TElement must derive from OpenXMLElement.");
    auto element = GetPreviousSiblingOfTypeInternal(TElement::StaticMetaClass());
    return std::static_pointer_cast<TElement>(element);
}

} // namespace ExyokiOffice
