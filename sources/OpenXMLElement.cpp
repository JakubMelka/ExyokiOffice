// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "ExyokiOffice/DOM/OpenXmlElementFactory.hpp"
#include "ExyokiOffice/XmlLocationCache.hpp"
#include "MarkupCompatibilityInternal.hpp"
#include "OpenXmlContentModel.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

std::string_view OpenXmlQualifiedName::namespaceUri() const noexcept
{
    return m_namespaceUri;
}

std::string_view OpenXmlQualifiedName::localName() const noexcept
{
    return m_localName;
}

bool OpenXmlQualifiedName::operator==(const OpenXmlQualifiedName& other) const noexcept
{
    return m_namespaceUri == other.m_namespaceUri && m_localName == other.m_localName;
}

bool OpenXmlQualifiedName::operator!=(const OpenXmlQualifiedName& other) const noexcept
{
    return !(*this == other);
}

bool OpenXmlQualifiedName::operator<(const OpenXmlQualifiedName& other) const noexcept
{
    if (m_namespaceUri == other.m_namespaceUri)
    {
        return m_localName < other.m_localName;
    }

    return m_namespaceUri < other.m_namespaceUri;
}

/// File-local qualified-name parsing helpers.
class OpenXmlElementNameHelper
{
public:
    struct ParsedName
    {
        std::string_view prefix;
        std::string_view localName;
    };

    static ParsedName SplitName(std::string_view qualifiedName)
    {
        const auto colon = qualifiedName.find(':');
        if (colon == std::string_view::npos)
        {
            return {std::string_view{}, qualifiedName};
        }
        return {qualifiedName.substr(0, colon), qualifiedName.substr(colon + 1)};
    }

    static Pugi::xml_attribute FindAttribute(const ExyokiOffice::Pugi::xml_node& node,
                                             const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        const auto targetLocal = name.localName();
        const auto targetNs = name.namespaceUri();

        for (const auto& attr : node.attributes())
        {
            const auto parsed = SplitName(std::string_view(attr.name()));
            if (parsed.localName != targetLocal)
            {
                continue;
            }

            std::optional<std::string> attrNamespace;
            if (parsed.prefix.empty())
            {
                attrNamespace = std::string{};
            }
            else
            {
                attrNamespace = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(node, parsed.prefix);
            }
            if (!attrNamespace)
            {
                attrNamespace = std::string{};
            }

            if (*attrNamespace == targetNs)
            {
                return attr;
            }
        }

        return {};
    }

    // The namespace bindings a piece of copied content depends on: the prefixes its
    // element and attribute names use, plus whether any element name is unprefixed
    // and therefore bound by the default namespace declaration in scope.
    struct NamespaceUsage
    {
        std::set<std::string> Prefixes;
        bool UsesDefaultNamespace = false;
    };

    // Collects the namespace bindings referenced by element and attribute names in
    // `node`, optionally including its whole subtree (used by the copy helpers to
    // work out which declarations a copy has to carry with it).
    static void CollectNamespaceUsage(const ExyokiOffice::Pugi::xml_node& node, NamespaceUsage& usage, bool recurse)
    {
        // An explicit stack rather than a call per level: the subtree being
        // copied can be as deeply nested as the document it came from, and the
        // result only accumulates into a set, so visiting order does not matter.
        std::vector<ExyokiOffice::Pugi::xml_node> pending;
        pending.push_back(node);

        while (!pending.empty())
        {
            const ExyokiOffice::Pugi::xml_node current = pending.back();
            pending.pop_back();

            if (!current || current.type() != Pugi::node_element)
            {
                continue;
            }

            const auto elementPrefix =
                SplitName(current.name() ? std::string_view(current.name()) : std::string_view{}).prefix;
            if (elementPrefix.empty())
            {
                usage.UsesDefaultNamespace = true;
            }
            else
            {
                usage.Prefixes.emplace(elementPrefix);
            }

            for (const auto& attribute : current.attributes())
            {
                const auto attributePrefix =
                    SplitName(attribute.name() ? std::string_view(attribute.name()) : std::string_view{}).prefix;
                if (!attributePrefix.empty() && attributePrefix != "xmlns")
                {
                    usage.Prefixes.emplace(attributePrefix);
                }
            }

            if (!recurse)
            {
                continue;
            }

            for (auto child : current.children())
            {
                pending.push_back(child);
            }
        }
    }

    /**
     * @brief Resolves the qualified name of an XML node without materializing it.
     */
    static std::optional<OpenXmlQualifiedName> NodeQualifiedName(const ExyokiOffice::Pugi::xml_node& node,
                                                                 std::string& namespaceStorage)
    {
        if (!node || node.type() != Pugi::node_element)
        {
            return std::nullopt;
        }
        const char* nameText = node.name();
        const std::string_view rawName = nameText ? std::string_view(nameText) : std::string_view{};
        if (rawName.empty())
        {
            return std::nullopt;
        }
        const auto parts = SplitName(rawName);
        if (const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(node, parts.prefix))
        {
            namespaceStorage = *uri;
            return OpenXmlQualifiedName(std::string_view(namespaceStorage), parts.localName);
        }
        return OpenXmlQualifiedName(std::string_view{}, parts.localName);
    }

    /**
     * @brief Finds the element particle that declares @p name inside a content model.
     */
    static const MetadataElementParticle* FindElementParticle(const MetadataParticlePtr& particle,
                                                              const OpenXmlQualifiedName& name)
    {
        if (!particle)
        {
            return nullptr;
        }
        if (particle->Kind() == MetadataParticleKind::Element)
        {
            const auto& element = static_cast<const MetadataElementParticle&>(*particle);
            return element.Element() == name ? &element : nullptr;
        }
        if (particle->Kind() == MetadataParticleKind::Any)
        {
            return nullptr;
        }
        const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
        for (const auto& child : composite.Children())
        {
            if (const auto* found = FindElementParticle(child, name))
            {
                return found;
            }
        }
        return nullptr;
    }
};

namespace Detail
{

std::shared_ptr<OpenXMLElement> CreateOpenXmlElementFromNode(
    const ExyokiOffice::Pugi::xml_node& node,
    const ExyokiOffice::OpenXMLElementClass* preferredClass)
{
    if (!node || node.type() != Pugi::node_element)
    {
        return nullptr;
    }

    const char* nameText = node.name();
    const std::string_view rawName = nameText ? std::string_view(nameText) : std::string_view{};
    if (rawName.empty())
    {
        return nullptr;
    }

    const auto parts = OpenXmlElementNameHelper::SplitName(rawName);
    std::string namespaceBuffer;
    std::string_view namespaceView;
    if (const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(node, parts.prefix))
    {
        namespaceBuffer = *uri;
        namespaceView = namespaceBuffer;
    }
    else
    {
        namespaceView = std::string_view{};
    }

    const OpenXmlQualifiedName qualifiedName(namespaceView, parts.localName);
    std::shared_ptr<OpenXMLElement> element;
    if (preferredClass && preferredClass->QualifiedName() == qualifiedName)
    {
        element = preferredClass->Create();
    }
    if (!element)
    {
        element = ExyokiOffice::Generated::OpenXmlElementFactory::Create(qualifiedName);
    }
    if (!element)
    {
        // The generated factory only knows the schema vocabularies. Markup
        // compatibility is defined outside them and is hand-written, so it gets a
        // chance before an element falls back to the untyped wrapper.
        if (const auto* metaClass = ResolveMarkupCompatibilityClass(qualifiedName))
        {
            element = metaClass->Create();
        }
    }
    if (!element)
    {
        element = std::make_shared<GenericOpenXmlElement>(qualifiedName);
    }

    element->SetNodeHandle(Detail::ToHandle(node));
    return element;
}

std::shared_ptr<OpenXMLElement> CreateOpenXmlElementFromNode(const ExyokiOffice::Pugi::xml_node& node)
{
    return CreateOpenXmlElementFromNode(node, nullptr);
}

} // namespace Detail

class OpenXmlSchemaInsertionHelpers
{
public:
    static bool CanInsert(const OpenXMLElement& parent,
                          const OpenXmlQualifiedName& childName)
    {
        const auto* parentClass = parent.ElementMetaClass();
        const auto metadata = parentClass ? parentClass->GetMetadata() : nullptr;
        if (!metadata || !metadata->ParticleTree())
        {
            return true;
        }

        const auto capacity = MaxOccurrences(metadata->ParticleTree(),
                                             childName,
                                             parent.QualifiedName().namespaceUri());
        if (capacity && *capacity == 0)
        {
            // Keep typed insertion forward-compatible with extension elements and
            // incomplete imported metadata. The validator remains authoritative
            // for reporting a child that is not admitted by the particle.
            return true;
        }
        if (!capacity)
        {
            return true;
        }

        const auto children = parent.Children();
        const auto existingCount = std::count_if(
            children.begin(), children.end(), [&](const auto& child)
            { return child && child->QualifiedName() == childName; });
        return static_cast<Size>(existingCount) < *capacity;
    }

    static std::shared_ptr<OpenXMLElement> FindInsertionAnchor(
        const OpenXMLElement& parent,
        const OpenXmlQualifiedName& childName,
        const std::shared_ptr<OpenXMLElement>& preferred = nullptr)
    {
        const auto* parentClass = parent.ElementMetaClass();
        if (!parentClass)
        {
            return nullptr;
        }

        const auto metadata = parentClass->GetMetadata();
        if (!metadata || !metadata->ParticleTree())
        {
            return nullptr;
        }

        std::vector<std::pair<OpenXmlQualifiedName, OpenXmlQualifiedName>> precedence;
        CollectPrecedence(metadata->ParticleTree(), precedence);

        const auto children = parent.Children();
        Size lowerBound = 0;
        Size upperBound = children.size();
        std::optional<Size> preferredIndex;
        for (Size index = 0; index < children.size(); ++index)
        {
            const auto existingName = children[index]->QualifiedName();
            if (preferred && children[index]->IsSameNode(preferred))
            {
                preferredIndex = index;
            }

            const auto newBeforeExisting = std::find_if(
                precedence.begin(), precedence.end(), [&](const auto& rule)
                { return rule.first == childName && rule.second == existingName; });
            if (newBeforeExisting != precedence.end())
            {
                upperBound = std::min(upperBound, index);
            }

            const auto existingBeforeNew = std::find_if(
                precedence.begin(), precedence.end(), [&](const auto& rule)
                { return rule.first == existingName && rule.second == childName; });
            if (existingBeforeNew != precedence.end())
            {
                lowerBound = std::max(lowerBound, index + 1);
            }
        }

        Size target = upperBound;
        if (preferredIndex && *preferredIndex >= lowerBound && *preferredIndex <= upperBound)
        {
            target = *preferredIndex;
        }
        if (target < children.size())
        {
            return children[target];
        }
        return nullptr;
    }

private:
    static std::optional<Size> MultiplyCapacity(std::optional<Size> value,
                                                const MetadataParticle& particle)
    {
        if (value && *value == 0)
        {
            return 0;
        }
        if (!value || particle.IsUnbounded())
        {
            return std::nullopt;
        }
        return *value * static_cast<Size>(*particle.MaxOccurs());
    }

    static std::optional<Size> MaxOccurrences(const MetadataParticlePtr& particle,
                                              const OpenXmlQualifiedName& childName,
                                              std::string_view targetNamespace)
    {
        if (!particle)
        {
            return 0;
        }
        if (particle->Kind() == MetadataParticleKind::Element)
        {
            if (static_cast<const MetadataElementParticle&>(*particle).Element() != childName)
            {
                return 0;
            }
            return particle->IsUnbounded()
                       ? std::optional<Size>{}
                       : std::optional<Size>{*particle->MaxOccurs()};
        }
        if (particle->Kind() == MetadataParticleKind::Any)
        {
            if (!ContentModelWildcardMatches(
                    static_cast<const MetadataAnyParticle&>(*particle).Wildcard(),
                    childName.namespaceUri(), targetNamespace))
            {
                return 0;
            }
            return particle->IsUnbounded()
                       ? std::optional<Size>{}
                       : std::optional<Size>{*particle->MaxOccurs()};
        }

        const auto composite = std::dynamic_pointer_cast<MetadataCompositeParticle>(particle);
        if (!composite)
        {
            return 0;
        }

        std::optional<Size> capacity{0};
        for (const auto& child : composite->Children())
        {
            const auto childCapacity = MaxOccurrences(child, childName, targetNamespace);
            if (!childCapacity)
            {
                capacity = std::nullopt;
                break;
            }
            if (particle->Kind() == MetadataParticleKind::Choice)
            {
                capacity = std::max(*capacity, *childCapacity);
            }
            else
            {
                capacity = *capacity + *childCapacity;
            }
        }
        return MultiplyCapacity(capacity, *particle);
    }

    static void CollectNames(const MetadataParticlePtr& particle,
                             std::vector<OpenXmlQualifiedName>& names)
    {
        if (!particle)
        {
            return;
        }
        if (particle->Kind() == MetadataParticleKind::Element)
        {
            names.push_back(static_cast<const MetadataElementParticle&>(*particle).Element());
            return;
        }
        const auto composite = std::dynamic_pointer_cast<MetadataCompositeParticle>(particle);
        if (!composite)
        {
            return;
        }
        for (const auto& child : composite->Children())
        {
            CollectNames(child, names);
        }
    }

    static void CollectPrecedence(
        const MetadataParticlePtr& particle,
        std::vector<std::pair<OpenXmlQualifiedName, OpenXmlQualifiedName>>& precedence)
    {
        if (!particle)
        {
            return;
        }
        const auto composite = std::dynamic_pointer_cast<MetadataCompositeParticle>(particle);
        if (!composite)
        {
            return;
        }

        if (particle->Kind() == MetadataParticleKind::Sequence)
        {
            std::vector<OpenXmlQualifiedName> previousNames;
            for (const auto& child : composite->Children())
            {
                std::vector<OpenXmlQualifiedName> currentNames;
                CollectNames(child, currentNames);
                for (const auto& previous : previousNames)
                {
                    for (const auto& current : currentNames)
                    {
                        precedence.emplace_back(previous, current);
                    }
                }
                previousNames.insert(previousNames.end(), currentNames.begin(), currentNames.end());
                CollectPrecedence(child, precedence);
            }
            return;
        }

        for (const auto& child : composite->Children())
        {
            CollectPrecedence(child, precedence);
        }
    }
};

class GenericElementMetaClass final : public OpenXMLElementClass
{
public:
    explicit GenericElementMetaClass(OpenXmlQualifiedName name) : name_(name) {}

    const OpenXMLElementClass* GetBaseMetaClass() const noexcept override { return nullptr; }
    OpenXmlQualifiedName QualifiedName() const noexcept override { return name_; }
    OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return name_; }
    OpenXml::FileFormatVersions GetVersion() const noexcept override
    {
        return OpenXml::FileFormatVersions::None;
    }
    std::shared_ptr<OpenXMLElement> Create() const override { return nullptr; }

private:
    OpenXmlQualifiedName name_;
};

GenericOpenXmlElement::GenericOpenXmlElement(OpenXmlQualifiedName name)
    : namespaceUri_(name.namespaceUri()), localName_(name.localName()), metaClass_(std::make_unique<GenericElementMetaClass>(OpenXmlQualifiedName(namespaceUri_, localName_)))
{
}

GenericOpenXmlElement::~GenericOpenXmlElement() = default;

const OpenXMLElementClass* GenericOpenXmlElement::ElementMetaClass() const noexcept
{
    return metaClass_.get();
}

/// File-local pugixml navigation helpers for element serialization.
class OpenXmlElementXmlHelper
{
public:
    static ExyokiOffice::Pugi::xml_node FirstElementChild(const ExyokiOffice::Pugi::xml_node& node)
    {
        auto child = node.first_child();
        while (child && child.type() != Pugi::node_element)
        {
            child = child.next_sibling();
        }
        return child;
    }

    static ExyokiOffice::Pugi::xml_node LastElementChild(const ExyokiOffice::Pugi::xml_node& node)
    {
        auto child = node.last_child();
        while (child && child.type() != Pugi::node_element)
        {
            child = child.previous_sibling();
        }
        return child;
    }

    static ExyokiOffice::Pugi::xml_node NextElementSibling(const ExyokiOffice::Pugi::xml_node& node)
    {
        auto sibling = node.next_sibling();
        while (sibling && sibling.type() != Pugi::node_element)
        {
            sibling = sibling.next_sibling();
        }
        return sibling;
    }

    static ExyokiOffice::Pugi::xml_node PreviousElementSibling(const ExyokiOffice::Pugi::xml_node& node)
    {
        auto sibling = node.previous_sibling();
        while (sibling && sibling.type() != Pugi::node_element)
        {
            sibling = sibling.previous_sibling();
        }
        return sibling;
    }

    /**
     * @brief Formats a node's name for diagnostics, normalizing the prefix.
     *
     * A document may bind any prefix to a namespace, so the prefix it happens to use
     * is not a stable way to name an element in a diagnostic. Where the namespace is
     * a known Open XML one, its canonical prefix is used instead; otherwise the name
     * is reported exactly as written.
     */
    static std::string DisplayQualifiedName(std::string_view rawName,
                                            std::string_view localName,
                                            std::string_view namespaceUri)
    {
        if (rawName.empty())
        {
            return {};
        }

        if (!namespaceUri.empty())
        {
            if (const auto wellKnown =
                    ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(namespaceUri))
            {
                std::string result;
                result.reserve(wellKnown->size() + 1 + localName.size());
                result.append(*wellKnown);
                result.push_back(':');
                result.append(localName);
                return result;
            }
        }

        return std::string(rawName);
    }

    static std::string DisplayQualifiedName(const ExyokiOffice::Pugi::xml_node& node)
    {
        const char* nameText = node.name();
        const std::string_view rawName = nameText ? std::string_view(nameText) : std::string_view{};
        if (rawName.empty())
        {
            return {};
        }

        const auto parts = OpenXmlElementNameHelper::SplitName(rawName);
        const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(node, parts.prefix);
        return DisplayQualifiedName(rawName, parts.localName, uri ? std::string_view(*uri) : std::string_view{});
    }

    /**
     * @brief Formats an attribute name for diagnostics when the attribute is absent.
     *
     * Used for "attribute is required" style messages, where there is no attribute in
     * the document to read a prefix from. The prefix the element's scope already binds
     * to the namespace wins, so the message matches how the document would spell it.
     */
    static std::string DescribeQualifiedAttributeName(const ExyokiOffice::Pugi::xml_node& node,
                                                      const OpenXmlQualifiedName& name)
    {
        if (name.namespaceUri().empty())
        {
            return std::string(name.localName());
        }

        std::string prefix;
        if (const auto inScope = ExyokiOffice::Xml::NamespaceResolver::LookupPrefixForUri(node, name.namespaceUri());
            inScope && !inScope->empty())
        {
            prefix = *inScope;
        }
        else if (const auto wellKnown =
                     ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(name.namespaceUri()))
        {
            prefix = *wellKnown;
        }
        else
        {
            return std::string(name.localName());
        }

        std::string result;
        result.reserve(prefix.size() + 1 + name.localName().size());
        result.append(prefix);
        result.push_back(':');
        result.append(name.localName());
        return result;
    }

    static bool NodeMatchesQualifiedName(const ExyokiOffice::Pugi::xml_node& node, const OpenXmlQualifiedName& name)
    {
        const char* nameText = node.name();
        const std::string_view rawName = nameText ? std::string_view(nameText) : std::string_view{};
        if (rawName.empty())
        {
            return false;
        }

        const auto parts = OpenXmlElementNameHelper::SplitName(rawName);
        std::string namespaceBuffer;
        std::string_view namespaceView;
        if (const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(node, parts.prefix))
        {
            namespaceBuffer = *uri;
            namespaceView = namespaceBuffer;
        }
        else
        {
            namespaceView = std::string_view{};
        }

        return namespaceView == name.namespaceUri() && parts.localName == name.localName();
    }

    static std::string BuildElementQualifiedName(ExyokiOffice::Pugi::xml_node& node, const OpenXmlQualifiedName& name)
    {
        std::string prefix;
        if (!name.namespaceUri().empty())
        {
            auto existingPrefix = Xml::NamespaceResolver::LookupPrefixForUri(node, name.namespaceUri());
            if (!existingPrefix.has_value())
            {
                if (auto suggested =
                        ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(name.namespaceUri()))
                {
                    existingPrefix = *suggested;
                }
            }

            prefix = Xml::NamespaceResolver::EnsurePrefix(node, name.namespaceUri(), existingPrefix);
        }

        std::string qualifiedName;
        if (prefix.empty())
        {
            qualifiedName.assign(name.localName());
        }
        else
        {
            qualifiedName.reserve(prefix.size() + 1 + name.localName().size());
            qualifiedName.append(prefix);
            qualifiedName.push_back(':');
            qualifiedName.append(name.localName());
        }

        return qualifiedName;
    }

    /**
     * @brief Resolves the anchor an existing subtree should be placed in front of.
     *
     * For exact placement the caller's anchor is used verbatim. For schema-ordered
     * placement it becomes a preference that the parent's content model may override.
     */
    static std::shared_ptr<OpenXMLElement> ResolvePlacementAnchor(const OpenXMLElement& targetParent,
                                                                  const OpenXmlQualifiedName& childName,
                                                                  const std::shared_ptr<OpenXMLElement>& before,
                                                                  OpenXmlPlacement placement)
    {
        if (placement != OpenXmlPlacement::SchemaOrdered)
        {
            return before;
        }

        return OpenXmlSchemaInsertionHelpers::FindInsertionAnchor(targetParent, childName, before);
    }

    /**
     * @brief Copies `source` under `targetNode` and repairs its namespace bindings.
     *
     * The copy is inserted in front of `beforeNode`, or appended when that node is
     * empty. Every namespace binding the copied content depends on but the
     * destination does not already provide identically is redeclared on the copied
     * root, so the result stays self-contained.
     */
    static Pugi::xml_node CopyNodeInto(const ExyokiOffice::Pugi::xml_node& source,
                                       ExyokiOffice::Pugi::xml_node& targetNode,
                                       const ExyokiOffice::Pugi::xml_node& beforeNode,
                                       OpenXmlCloneDepth depth)
    {
        // Resolve the bindings in the *source* context before copying: the
        // destination tree may not declare (or may declare differently) some of them.
        OpenXmlElementNameHelper::NamespaceUsage usage;
        OpenXmlElementNameHelper::CollectNamespaceUsage(source, usage, depth == OpenXmlCloneDepth::Deep);

        std::vector<std::pair<std::string, std::string>> requiredDeclarations;
        for (const auto& prefix : usage.Prefixes)
        {
            if (auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(source, prefix))
            {
                requiredDeclarations.emplace_back(prefix, std::string(*uri));
            }
        }
        if (usage.UsesDefaultNamespace)
        {
            // An unprefixed element name binds to whatever xmlns="..." is in scope.
            // The empty binding matters just as much as a real one: without it the
            // copy would silently adopt the destination's default namespace.
            const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(source, std::string_view{});
            requiredDeclarations.emplace_back(std::string{}, uri ? std::string(*uri) : std::string{});
        }

        Pugi::xml_node copiedNode;
        if (depth == OpenXmlCloneDepth::Deep)
        {
            copiedNode =
                beforeNode ? targetNode.insert_copy_before(source, beforeNode) : targetNode.append_copy(source);
        }
        else
        {
            copiedNode = beforeNode ? targetNode.insert_child_before(source.type(), beforeNode)
                                    : targetNode.append_child(source.type());
            if (copiedNode)
            {
                copiedNode.set_name(source.name());
                for (const auto& attribute : source.attributes())
                {
                    copiedNode.append_attribute(attribute.name()).set_value(attribute.value());
                }
            }
        }
        if (!copiedNode)
        {
            return {};
        }

        for (const auto& [prefix, uri] : requiredDeclarations)
        {
            const auto existing = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(copiedNode, prefix);
            const std::string_view resolved = existing ? *existing : std::string_view{};
            if (resolved == uri)
            {
                continue;
            }

            const std::string attributeName = prefix.empty() ? std::string("xmlns") : "xmlns:" + prefix;
            auto attribute = copiedNode.attribute(attributeName.c_str());
            if (!attribute)
            {
                attribute = copiedNode.append_attribute(attributeName.c_str());
            }
            attribute.set_value(uri.c_str());
        }

        return copiedNode;
    }
};

OpenXmlQualifiedName OpenXMLElement::QualifiedName() const
{
    const auto* elementClass = ElementMetaClass();
    if (elementClass == nullptr)
    {
        return {};
    }

    return elementClass->QualifiedName();
}

OpenXmlQualifiedName OpenXMLElement::TypeQualifiedName() const
{
    const auto* elementClass = ElementMetaClass();
    if (elementClass == nullptr)
    {
        return {};
    }

    return elementClass->TypeQualifiedName();
}

/**
 * @internal
 * @brief Holds the recorded paths and the traversal that fills them in.
 *
 * Paths are keyed by the identity of the underlying XML node, because an
 * OpenXMLElement is a view that the DOM recreates on every navigation call and
 * two views of the same node are distinct objects.
 */
struct XmlLocationCache::Impl
{
    struct Entry
    {
        std::string Path;
        std::string ElementName;
    };

    /**
     * @brief Returns the entry of @p node, recording whatever is still missing.
     *
     * Walks up to the nearest ancestor that is already recorded - or to the
     * document - and then fills in one generation at a time on the way back
     * down, so every level below an already-known one costs a single pass.
     */
    const Entry& Resolve(const Pugi::xml_node& node)
    {
        if (const auto found = Entries.find(node.internal_object()); found != Entries.end())
        {
            return found->second;
        }

        std::vector<Pugi::xml_node> parents;
        for (auto parent = node.parent(); parent; parent = parent.parent())
        {
            parents.push_back(parent);
            if (parent.type() != Pugi::node_element ||
                Entries.find(parent.internal_object()) != Entries.end())
            {
                break;
            }
        }

        for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent)
        {
            PopulateChildren(*parent);
        }

        if (const auto found = Entries.find(node.internal_object()); found != Entries.end())
        {
            return found->second;
        }

        // A node with no parent at all: nothing to be positioned among, and no
        // ancestry to prefix it with. Its own name is the whole path.
        auto name = OpenXmlElementXmlHelper::DisplayQualifiedName(node);
        auto path = "/" + name;
        return Entries.emplace(node.internal_object(), Entry{std::move(path), std::move(name)}).first->second;
    }

    /**
     * @brief Records the path of every element child of @p parent in one pass.
     *
     * @p parent is either the document or an element whose own path is already
     * recorded. Counting how many children share a name is what the position
     * predicate needs, and that count is only known once all of them have been
     * seen - so by the time one child's path can be written, they all can.
     */
    void PopulateChildren(const Pugi::xml_node& parent)
    {
        std::string_view parentPath;
        if (parent.type() == Pugi::node_element)
        {
            const auto found = Entries.find(parent.internal_object());
            if (found == Entries.end())
            {
                return;
            }

            // References into an unordered_map survive its rehashing, so the view
            // stays valid while the children below are inserted.
            parentPath = found->second.Path;
        }

        struct ChildName
        {
            Pugi::xml_node Node;
            std::string Display;
            std::string Key;
        };

        std::vector<ChildName> children;
        std::unordered_map<std::string, Size> totals;
        for (auto child = OpenXmlElementXmlHelper::FirstElementChild(parent); child; child = OpenXmlElementXmlHelper::NextElementSibling(child))
        {
            const char* nameText = child.name();
            const std::string_view rawName = nameText ? std::string_view(nameText) : std::string_view{};
            const auto parts = OpenXmlElementNameHelper::SplitName(rawName);
            const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(child, parts.prefix);
            const std::string_view namespaceUri = uri ? std::string_view(*uri) : std::string_view{};

            // A newline separates the two halves of the key because neither a
            // namespace URI nor a local name may contain one.
            std::string key(namespaceUri);
            key.push_back('\n');
            key.append(parts.localName);

            ++totals[key];
            children.push_back(
                {child, OpenXmlElementXmlHelper::DisplayQualifiedName(rawName, parts.localName, namespaceUri), std::move(key)});
        }

        std::unordered_map<std::string, Size> positions;
        for (auto& child : children)
        {
            const Size position = ++positions[child.Key];
            std::string path;
            path.reserve(parentPath.size() + child.Display.size() + 8);
            path.append(parentPath);
            path.push_back('/');
            path.append(child.Display);
            if (totals[child.Key] > 1)
            {
                path.push_back('[');
                path.append(std::to_string(position));
                path.push_back(']');
            }

            Entries.emplace(child.Node.internal_object(), Entry{std::move(path), std::move(child.Display)});
        }
    }

    std::unordered_map<const Pugi::xml_node_struct*, Entry> Entries;
};

XmlLocationCache::XmlLocationCache()
    : impl_(std::make_unique<Impl>())
{
}

XmlLocationCache::~XmlLocationCache() = default;

XmlLocationCache::XmlLocationCache(XmlLocationCache&&) noexcept = default;

XmlLocationCache& XmlLocationCache::operator=(XmlLocationCache&&) noexcept = default;

XmlLocation XmlLocationCache::Location(const OpenXMLElement& element)
{
    const auto node = Detail::ToNode(element.GetNodeHandle());
    if (!node || node.type() != Pugi::node_element)
    {
        return {};
    }

    const auto& entry = impl_->Resolve(node);
    XmlLocation location;
    location.Path = entry.Path;
    location.ElementName = entry.ElementName;
    return location;
}

XmlLocation XmlLocationCache::Location(const OpenXMLElement& element, const OpenXmlQualifiedName& attribute)
{
    auto location = Location(element);
    if (!location.IsValid())
    {
        return location;
    }

    const auto node = Detail::ToNode(element.GetNodeHandle());
    if (const auto existing = OpenXmlElementNameHelper::FindAttribute(node, attribute))
    {
        // Report the attribute exactly as the document spells it when it is
        // actually present, so the location can be matched against the markup.
        location.AttributeName = existing.name();
        return location;
    }

    location.AttributeName = OpenXmlElementXmlHelper::DescribeQualifiedAttributeName(node, attribute);
    return location;
}

void XmlLocationCache::Clear() noexcept
{
    impl_->Entries.clear();
}

Size XmlLocationCache::MemoizedElementCount() const noexcept
{
    return impl_->Entries.size();
}

XmlLocation OpenXMLElement::GetXmlLocation() const noexcept
{
    XmlLocationCache cache;
    return cache.Location(*this);
}

XmlLocation OpenXMLElement::GetXmlLocation(const OpenXmlQualifiedName& attribute) const noexcept
{
    XmlLocationCache cache;
    return cache.Location(*this, attribute);
}

bool OpenXMLElement::IsNull() const noexcept
{
    return !m_node;
}

bool OpenXMLElement::IsSameNode(const OpenXMLElement& other) const noexcept
{
    return m_node && m_node == other.m_node;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::CopyInto(const std::shared_ptr<OpenXMLElement>& targetParent,
                                                         const std::shared_ptr<OpenXMLElement>& before,
                                                         OpenXmlCloneDepth depth,
                                                         OpenXmlPlacement placement) const
{
    const auto node = Detail::ToNode(m_node);
    auto targetNode = Detail::NodeOf(targetParent);
    if (!node || !targetNode)
    {
        return nullptr;
    }

    const auto anchor = OpenXmlElementXmlHelper::ResolvePlacementAnchor(*targetParent, QualifiedName(), before, placement);
    const Pugi::xml_node anchorNode = Detail::NodeOf(anchor);
    const Pugi::xml_node beforeNode = anchorNode.parent() == targetNode ? anchorNode : Pugi::xml_node{};

    const auto copiedNode = OpenXmlElementXmlHelper::CopyNodeInto(node, targetNode, beforeNode, depth);
    if (!copiedNode)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(copiedNode);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::CopyAfter(const std::shared_ptr<OpenXMLElement>& after,
                                                          OpenXmlCloneDepth depth) const
{
    const auto anchorNode = Detail::NodeOf(after);
    if (!m_node || !anchorNode)
    {
        return nullptr;
    }

    auto parentNode = anchorNode.parent();
    if (!parentNode || parentNode.type() != Pugi::node_element)
    {
        return nullptr;
    }

    const auto copiedNode = OpenXmlElementXmlHelper::CopyNodeInto(Detail::ToNode(m_node), parentNode, anchorNode.next_sibling(), depth);
    if (!copiedNode)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(copiedNode);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::MoveInto(const std::shared_ptr<OpenXMLElement>& targetParent,
                                                         const std::shared_ptr<OpenXMLElement>& before,
                                                         OpenXmlPlacement placement)
{
    auto node = Detail::ToNode(m_node);
    auto targetNode = Detail::NodeOf(targetParent);
    if (!node || !targetNode)
    {
        return nullptr;
    }

    const bool alreadyChild = node.parent() == targetNode;
    if (placement == OpenXmlPlacement::SchemaOrdered && !alreadyChild && !OpenXmlSchemaInsertionHelpers::CanInsert(*targetParent, QualifiedName()))
    {
        return nullptr;
    }

    const auto anchor = OpenXmlElementXmlHelper::ResolvePlacementAnchor(*targetParent, QualifiedName(), before, placement);
    auto anchorNode = Detail::NodeOf(anchor);
    if (anchorNode.parent() != targetNode)
    {
        anchorNode = Pugi::xml_node{};
    }
    if (anchorNode == node)
    {
        // Already exactly where it was asked to go.
        return Detail::CreateOpenXmlElementFromNode(node);
    }

    if (node.root() != targetNode.root())
    {
        // pugixml cannot relink across documents, so a cross-document move is a
        // copy plus destruction of the source. Node identity is lost and every
        // wrapper into the old subtree goes stale, this one included.
        const auto copiedNode = OpenXmlElementXmlHelper::CopyNodeInto(node, targetNode, anchorNode, OpenXmlCloneDepth::Deep);
        if (!copiedNode)
        {
            return nullptr;
        }

        auto parentNode = node.parent();
        if (parentNode)
        {
            parentNode.remove_child(node);
        }
        m_node = OpenXmlNodeHandle{};
        return Detail::CreateOpenXmlElementFromNode(copiedNode);
    }

    // Within one document the node is relinked, which keeps its identity and
    // therefore keeps every existing wrapper into this subtree usable.
    const auto movedNode =
        anchorNode ? targetNode.insert_move_before(node, anchorNode) : targetNode.append_move(node);
    if (!movedNode)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(movedNode);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::MoveAfter(const std::shared_ptr<OpenXMLElement>& after)
{
    const auto anchorNode = Detail::NodeOf(after);
    if (!m_node || !anchorNode || anchorNode == Detail::ToNode(m_node))
    {
        return nullptr;
    }

    const auto parentNode = anchorNode.parent();
    if (!parentNode || parentNode.type() != Pugi::node_element)
    {
        return nullptr;
    }

    auto parent = Detail::CreateOpenXmlElementFromNode(parentNode);
    if (!parent)
    {
        return nullptr;
    }

    const auto nextNode = anchorNode.next_sibling();
    return MoveInto(parent, nextNode ? Detail::CreateOpenXmlElementFromNode(nextNode) : nullptr);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::ReplaceWith(const std::shared_ptr<OpenXMLElement>& replacement)
{
    auto node = Detail::ToNode(m_node);
    const auto replacementNode = Detail::NodeOf(replacement);
    if (!node || !replacementNode || replacementNode == node)
    {
        return nullptr;
    }

    auto parentNode = node.parent();
    if (!parentNode || parentNode.type() != Pugi::node_element)
    {
        return nullptr;
    }

    // Refuse an ancestor: it would have to be placed inside the subtree that is
    // about to be destroyed.
    for (auto current = parentNode; current; current = current.parent())
    {
        if (current == replacementNode)
        {
            return nullptr;
        }
    }

    std::shared_ptr<OpenXMLElement> placed;
    if (replacementNode.root() == parentNode.root())
    {
        const auto movedNode = parentNode.insert_move_before(replacementNode, node);
        if (!movedNode)
        {
            return nullptr;
        }
        placed = Detail::CreateOpenXmlElementFromNode(movedNode);
    }
    else
    {
        const auto copiedNode = OpenXmlElementXmlHelper::CopyNodeInto(replacementNode, parentNode, node, OpenXmlCloneDepth::Deep);
        if (!copiedNode)
        {
            return nullptr;
        }
        placed = Detail::CreateOpenXmlElementFromNode(copiedNode);
    }

    parentNode.remove_child(node);
    m_node = OpenXmlNodeHandle{};
    return placed;
}

std::vector<std::shared_ptr<OpenXMLElement>> OpenXMLElement::ReplaceWithChildren()
{
    std::vector<std::shared_ptr<OpenXMLElement>> promoted;

    auto node = Detail::ToNode(m_node);
    if (!node)
    {
        return promoted;
    }

    auto parentNode = node.parent();
    if (!parentNode || parentNode.type() != Pugi::node_element)
    {
        return promoted;
    }

    // Snapshot the children first: moving a node out detaches it from the sibling
    // chain the iteration would otherwise walk.
    std::vector<Pugi::xml_node> children;
    for (auto child = node.first_child(); child; child = child.next_sibling())
    {
        children.push_back(child);
    }

    for (const auto& child : children)
    {
        const auto movedNode = parentNode.insert_move_before(child, node);
        if (!movedNode)
        {
            continue;
        }
        if (movedNode.type() != Pugi::node_element)
        {
            continue;
        }
        if (auto element = Detail::CreateOpenXmlElementFromNode(movedNode))
        {
            promoted.push_back(std::move(element));
        }
    }

    parentNode.remove_child(node);
    m_node = OpenXmlNodeHandle{};
    return promoted;
}

bool OpenXMLElement::Remove()
{
    const auto node = Detail::ToNode(m_node);
    if (!node)
    {
        return false;
    }

    auto parentNode = node.parent();
    if (!parentNode)
    {
        return false;
    }

    if (!parentNode.remove_child(node))
    {
        return false;
    }

    m_node = OpenXmlNodeHandle{};
    return true;
}

bool OpenXMLElement::RemoveAllChildren()
{
    auto node = Detail::ToNode(m_node);
    if (!node)
    {
        return false;
    }

    node.remove_children();
    return true;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::Parent() const
{
    if (!m_node)
    {
        return nullptr;
    }

    auto parent = Detail::ToNode(m_node).parent();
    if (!parent || parent.type() != Pugi::node_element)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(parent);
}

std::vector<std::shared_ptr<OpenXMLElement>> OpenXMLElement::Children() const
{
    std::vector<std::shared_ptr<OpenXMLElement>> result;
    if (!m_node)
    {
        return result;
    }

    for (auto child = OpenXmlElementXmlHelper::FirstElementChild(Detail::ToNode(m_node)); child; child = OpenXmlElementXmlHelper::NextElementSibling(child))
    {
        if (auto element = Detail::CreateOpenXmlElementFromNode(child))
        {
            result.push_back(std::move(element));
        }
    }
    return result;
}

std::vector<std::shared_ptr<OpenXMLElement>> OpenXMLElement::ChildrenInContentModel() const
{
    std::vector<std::shared_ptr<OpenXMLElement>> result;
    if (!m_node)
    {
        return result;
    }

    const auto* elementClass = ElementMetaClass();
    const auto metadata = elementClass ? elementClass->GetMetadata() : nullptr;
    const MetadataParticlePtr particleTree = metadata ? metadata->ParticleTree() : MetadataParticlePtr{};

    for (auto child = OpenXmlElementXmlHelper::FirstElementChild(Detail::ToNode(m_node)); child; child = OpenXmlElementXmlHelper::NextElementSibling(child))
    {
        const OpenXMLElementClass* preferred = nullptr;
        std::string childNamespace;
        if (metadata)
        {
            if (const auto childName = OpenXmlElementNameHelper::NodeQualifiedName(child, childNamespace);
                childName && Generated::OpenXmlElementFactory::IsAmbiguousElementName(*childName))
            {
                if (const auto* particle = particleTree ? OpenXmlElementNameHelper::FindElementParticle(particleTree, *childName) : nullptr)
                {
                    preferred = Generated::OpenXmlElementFactory::ResolveClassByTypeName(particle->ElementType());
                }
                else
                {
                    // An open content model (`xsd:any`, as in a:graphicData) does
                    // not name its children, but the metadata still lists the
                    // elements known to occur there and the type each one has in
                    // that position: c:chart under a:graphicData is the CT_RelId
                    // reference, not the CT_Chart body a bare-name lookup yields.
                    for (const auto& additional : metadata->AdditionalElements())
                    {
                        if (additional.Name == *childName)
                        {
                            preferred = Generated::OpenXmlElementFactory::ResolveClassByTypeName(additional.TypeName);
                            break;
                        }
                    }
                }
            }
        }
        if (auto element = Detail::CreateOpenXmlElementFromNode(child, preferred))
        {
            result.push_back(std::move(element));
        }
    }
    return result;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::GetChild(const OpenXmlQualifiedName& name) const
{
    if (!m_node)
    {
        return nullptr;
    }

    for (auto child = OpenXmlElementXmlHelper::FirstElementChild(Detail::ToNode(m_node)); child; child = OpenXmlElementXmlHelper::NextElementSibling(child))
    {
        if (OpenXmlElementXmlHelper::NodeMatchesQualifiedName(child, name))
        {
            return Detail::CreateOpenXmlElementFromNode(child);
        }
    }

    return nullptr;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::FirstSibling() const
{
    if (!m_node)
    {
        return nullptr;
    }

    const auto node = Detail::ToNode(m_node);
    auto parent = node.parent();
    if (!parent)
    {
        return Detail::CreateOpenXmlElementFromNode(node);
    }

    auto first = OpenXmlElementXmlHelper::FirstElementChild(parent);
    if (!first)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(first);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::PreviousSibling() const
{
    if (!m_node)
    {
        return nullptr;
    }

    auto previous = OpenXmlElementXmlHelper::PreviousElementSibling(Detail::ToNode(m_node));
    if (!previous)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(previous);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::NextSibling() const
{
    if (!m_node)
    {
        return nullptr;
    }

    auto next = OpenXmlElementXmlHelper::NextElementSibling(Detail::ToNode(m_node));
    if (!next)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(next);
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::LastSibling() const
{
    if (!m_node)
    {
        return nullptr;
    }

    const auto node = Detail::ToNode(m_node);
    auto parent = node.parent();
    if (!parent)
    {
        return Detail::CreateOpenXmlElementFromNode(node);
    }

    auto last = OpenXmlElementXmlHelper::LastElementChild(parent);
    if (!last)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(last);
}

bool OpenXMLElement::HasAttribute(const OpenXmlQualifiedName& name) const
{
    if (!m_node)
    {
        return false;
    }

    const auto attribute = OpenXmlElementNameHelper::FindAttribute(Detail::ToNode(m_node), name);
    return static_cast<bool>(attribute);
}

bool OpenXMLElement::TryGetAttribute(const OpenXmlQualifiedName& name, std::string_view& value) const
{
    value = {};

    if (!m_node)
    {
        return false;
    }

    const Pugi::xml_attribute attribute = OpenXmlElementNameHelper::FindAttribute(Detail::ToNode(m_node), name);
    if (!attribute)
    {
        return false;
    }

    const char* attributeValue = attribute.value();
    if (attributeValue == nullptr)
    {
        return false;
    }

    value = std::string_view{attributeValue};
    return true;
}

std::string_view OpenXMLElement::GetAttribute(const OpenXmlQualifiedName& name) const
{
    std::string_view value;
    static_cast<void>(TryGetAttribute(name, value));
    return value;
}

void OpenXMLElement::SetAttribute(const OpenXmlQualifiedName& name, std::string_view value)
{
    if (!m_node)
    {
        return;
    }

    auto node = Detail::ToNode(m_node);
    std::string prefix;
    if (!name.namespaceUri().empty())
    {
        auto existingPrefix = Xml::NamespaceResolver::LookupPrefixForUri(node, name.namespaceUri());
        if (!existingPrefix.has_value())
        {
            if (auto suggested = ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(name.namespaceUri()))
            {
                existingPrefix = *suggested;
            }
        }

        prefix = Xml::NamespaceResolver::EnsurePrefix(node, name.namespaceUri(), existingPrefix);
    }

    std::string qualifiedName;
    if (prefix.empty())
    {
        qualifiedName.assign(name.localName());
    }
    else
    {
        qualifiedName.reserve(prefix.size() + 1 + name.localName().size());
        qualifiedName.append(prefix);
        qualifiedName.push_back(':');
        qualifiedName.append(name.localName());
    }

    auto attribute = OpenXmlElementNameHelper::FindAttribute(node, name);
    if (!attribute)
    {
        attribute = node.append_attribute(qualifiedName.c_str());
    }

    attribute.set_value(std::string(value).c_str());
}

void OpenXMLElement::RemoveAttribute(const OpenXmlQualifiedName& name)
{
    if (!m_node)
    {
        return;
    }

    auto node = Detail::ToNode(m_node);
    auto attribute = OpenXmlElementNameHelper::FindAttribute(node, name);
    if (attribute)
    {
        node.remove_attribute(attribute);
    }
}

std::string_view OpenXmlLeafTextElement::GetText() const
{
    if (!m_node)
    {
        return {};
    }

    const auto child = Detail::ToNode(m_node).first_child();
    if (!child)
    {
        return {};
    }

    const char* value = child.value();
    return value ? std::string_view(value) : std::string_view{};
}

void OpenXmlLeafTextElement::SetText(std::string_view text)
{
    if (!m_node)
    {
        return;
    }

    auto node = Detail::ToNode(m_node);
    auto child = node.first_child();
    if (!child || child.type() != Pugi::node_pcdata)
    {
        node.remove_children();
        child = node.append_child(Pugi::node_pcdata);
    }

    child.set_value(std::string(text).c_str());
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::AppendChildInternal(const OpenXMLElementClass* metaClass,
                                                                    bool schemaAware)
{
    if (!m_node || !metaClass)
    {
        return nullptr;
    }

    const auto qualifiedInfo = metaClass->QualifiedName();
    if (qualifiedInfo.localName().empty())
    {
        return nullptr;
    }
    if (schemaAware && !OpenXmlSchemaInsertionHelpers::CanInsert(*this, qualifiedInfo))
    {
        return nullptr;
    }

    auto element = metaClass->Create();
    if (!element)
    {
        return nullptr;
    }

    auto node = Detail::ToNode(m_node);
    auto qualifiedName = OpenXmlElementXmlHelper::BuildElementQualifiedName(node, qualifiedInfo);
    Pugi::xml_node child;
    const auto before = schemaAware
                            ? OpenXmlSchemaInsertionHelpers::FindInsertionAnchor(*this, qualifiedInfo)
                            : nullptr;
    if (const auto beforeNode = Detail::NodeOf(before))
    {
        child = node.insert_child_before(qualifiedName.c_str(), beforeNode);
    }
    else
    {
        child = node.append_child(qualifiedName.c_str());
    }
    if (!child)
    {
        return nullptr;
    }

    element->SetNodeHandle(Detail::ToHandle(child));
    return element;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::InsertChildInternal(const OpenXMLElementClass* metaClass,
                                                                    const std::shared_ptr<OpenXMLElement>& before,
                                                                    bool schemaAware)
{
    if (!m_node || !metaClass)
    {
        return nullptr;
    }

    const auto qualifiedInfo = metaClass->QualifiedName();
    if (qualifiedInfo.localName().empty())
    {
        return nullptr;
    }
    if (schemaAware && !OpenXmlSchemaInsertionHelpers::CanInsert(*this, qualifiedInfo))
    {
        return nullptr;
    }

    auto element = metaClass->Create();
    if (!element)
    {
        return nullptr;
    }

    auto node = Detail::ToNode(m_node);
    auto qualifiedName = OpenXmlElementXmlHelper::BuildElementQualifiedName(node, qualifiedInfo);
    auto insertionAnchor = before;
    if (schemaAware)
    {
        insertionAnchor = OpenXmlSchemaInsertionHelpers::FindInsertionAnchor(*this, qualifiedInfo, before);
    }

    ExyokiOffice::Pugi::xml_node child;
    const auto anchorNode = Detail::NodeOf(insertionAnchor);
    if (anchorNode && anchorNode.parent() == node)
    {
        child = node.insert_child_before(qualifiedName.c_str(), anchorNode);
    }
    else
    {
        child = node.append_child(qualifiedName.c_str());
    }

    if (!child)
    {
        return nullptr;
    }

    element->SetNodeHandle(Detail::ToHandle(child));
    return element;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::InsertChildAfterInternal(
    const OpenXMLElementClass* metaClass,
    const std::shared_ptr<OpenXMLElement>& after,
    bool schemaAware)
{
    if (!m_node)
    {
        return nullptr;
    }

    // "After the anchor" is "before whatever follows it", which is the position
    // InsertChildInternal already knows how to place and to schema-correct.
    std::shared_ptr<OpenXMLElement> before;
    const auto anchorNode = Detail::NodeOf(after);
    if (anchorNode && anchorNode.parent() == Detail::ToNode(m_node))
    {
        if (const auto nextNode = OpenXmlElementXmlHelper::NextElementSibling(anchorNode))
        {
            before = Detail::CreateOpenXmlElementFromNode(nextNode);
        }
    }

    return InsertChildInternal(metaClass, before, schemaAware);
}

bool OpenXMLElement::RemoveChildInternal(const std::shared_ptr<OpenXMLElement>& child)
{
    auto node = Detail::ToNode(m_node);
    const auto childNode = Detail::NodeOf(child);
    if (!node || !childNode || childNode.parent() != node)
    {
        return false;
    }

    if (!node.remove_child(childNode))
    {
        return false;
    }

    if (child)
    {
        child->SetNodeHandle(OpenXmlNodeHandle{});
    }
    return true;
}

std::vector<std::shared_ptr<OpenXMLElement>> OpenXMLElement::ElementsInternal(
    const OpenXMLElementClass* metaClass) const
{
    std::vector<std::shared_ptr<OpenXMLElement>> result;
    if (!m_node || !metaClass)
    {
        return result;
    }

    for (auto child = OpenXmlElementXmlHelper::FirstElementChild(Detail::ToNode(m_node)); child; child = OpenXmlElementXmlHelper::NextElementSibling(child))
    {
        auto element = Detail::CreateOpenXmlElementFromNode(child, metaClass);
        if (!element)
        {
            continue;
        }

        if (element->ElementMetaClass()->Inherits(metaClass))
        {
            result.push_back(std::move(element));
        }
    }

    return result;
}

std::vector<std::shared_ptr<OpenXMLElement>> OpenXMLElement::DescendantsInternal(
    const OpenXMLElementClass* metaClass) const
{
    std::vector<std::shared_ptr<OpenXMLElement>> result;
    if (!m_node || !metaClass)
    {
        return result;
    }

    std::vector<ExyokiOffice::Pugi::xml_node> stack;
    for (auto child = OpenXmlElementXmlHelper::LastElementChild(Detail::ToNode(m_node)); child; child = OpenXmlElementXmlHelper::PreviousElementSibling(child))
    {
        stack.push_back(child);
    }

    while (!stack.empty())
    {
        auto node = stack.back();
        stack.pop_back();

        auto element = Detail::CreateOpenXmlElementFromNode(node, metaClass);
        if (element && element->ElementMetaClass()->Inherits(metaClass))
        {
            result.push_back(std::move(element));
        }

        for (auto child = OpenXmlElementXmlHelper::LastElementChild(node); child; child = OpenXmlElementXmlHelper::PreviousElementSibling(child))
        {
            stack.push_back(child);
        }
    }

    return result;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::GetFirstChildOfTypeInternal(
    const OpenXMLElementClass* metaClass) const
{
    if (!m_node || !metaClass)
    {
        return nullptr;
    }

    for (auto child = OpenXmlElementXmlHelper::FirstElementChild(Detail::ToNode(m_node)); child; child = OpenXmlElementXmlHelper::NextElementSibling(child))
    {
        auto element = Detail::CreateOpenXmlElementFromNode(child, metaClass);
        if (!element)
        {
            continue;
        }

        if (element->ElementMetaClass()->Inherits(metaClass))
        {
            return element;
        }
    }

    return nullptr;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::GetNextSiblingOfTypeInternal(
    const OpenXMLElementClass* metaClass) const
{
    if (!m_node || !metaClass)
    {
        return nullptr;
    }

    for (auto sibling = OpenXmlElementXmlHelper::NextElementSibling(Detail::ToNode(m_node)); sibling; sibling = OpenXmlElementXmlHelper::NextElementSibling(sibling))
    {
        auto element = Detail::CreateOpenXmlElementFromNode(sibling, metaClass);
        if (!element)
        {
            continue;
        }

        if (element->ElementMetaClass()->Inherits(metaClass))
        {
            return element;
        }
    }

    return nullptr;
}

std::shared_ptr<OpenXMLElement> OpenXMLElement::GetPreviousSiblingOfTypeInternal(
    const OpenXMLElementClass* metaClass) const
{
    if (!m_node || !metaClass)
    {
        return nullptr;
    }

    for (auto sibling = OpenXmlElementXmlHelper::PreviousElementSibling(Detail::ToNode(m_node)); sibling; sibling = OpenXmlElementXmlHelper::PreviousElementSibling(sibling))
    {
        auto element = Detail::CreateOpenXmlElementFromNode(sibling, metaClass);
        if (!element)
        {
            continue;
        }

        if (element->ElementMetaClass()->Inherits(metaClass))
        {
            return element;
        }
    }

    return nullptr;
}

OpenXMLElementClass::OpenXMLElementClass() noexcept = default;

OpenXml::FileFormatVersions OpenXMLElementClass::GetVersion() const noexcept
{
    return OpenXml::FileFormatVersions::Office2007;
}

OpenXmlElementClassFlags OpenXMLElementClass::GetFlags() const noexcept
{
    return {};
}

std::span<const OpenXmlAttribute> OpenXMLElementClass::GetAttributes() const noexcept
{
    return {};
}

std::shared_ptr<MetadataDefinition> OpenXMLElementClass::GetMetadata() const
{
    std::call_once(metadataOnce_, [this]()
                   {
        MetadataBuilder builder;
        builder.SetElementName(QualifiedName());
        builder.SetTypeName(TypeQualifiedName());
        builder.SetAvailability(GetVersion());
        // A derived element class shares the schema type of its base: w:top is
        // a CT_Border like the abstract BorderType that declares its attributes
        // and their facets, and w:b an OnOffType. The generator emits that
        // metadata once, on the base, so the chain is configured base first and
        // this class last, which lets a class of its own restate the parts it
        // overrides (its particle, its schema name, its version).
        std::vector<const OpenXMLElementClass*> chain;
        for (const auto* current = this; current != nullptr; current = current->GetBaseMetaClass())
        {
            chain.push_back(current);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            (*it)->ConfigureMetadata(builder);
        }
        builder.SetElementName(QualifiedName());
        builder.SetTypeName(TypeQualifiedName());
        metadata_ = builder.Build(); });
    return metadata_;
}

OpenXmlMetaEnumFlags OpenXmlMetaEnum::GetFlags() const noexcept
{
    return {};
}

OpenXml::FileFormatVersions OpenXmlMetaEnum::GetVersion() const noexcept
{
    return OpenXml::FileFormatVersions::Office2007;
}

OpenXml::FileFormatVersions OpenXmlMetaEnum::GetVersionForEnumValue(UInt32) const noexcept
{
    return GetVersion();
}

bool OpenXMLElementClass::Inherits(const OpenXMLElementClass* base) const noexcept
{
    if (base == nullptr)
    {
        return false;
    }

    const auto* current = this;
    while (current != nullptr)
    {
        if (current == base)
        {
            return true;
        }

        current = current->GetBaseMetaClass();
    }

    return false;
}

void OpenXMLElementClass::ConfigureMetadata(MetadataBuilder& builder) const
{
    (void)builder;
}

class CompositeElementMetaClass final : public OpenXMLElementClass
{
public:
    virtual const OpenXMLElementClass* GetBaseMetaClass() const noexcept override { return nullptr; }
    virtual OpenXmlQualifiedName QualifiedName() const noexcept override { return {}; }
    virtual OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return {}; }
    virtual OpenXml::FileFormatVersions GetVersion() const noexcept override { return OpenXml::FileFormatVersions::None; }
    virtual std::shared_ptr<OpenXMLElement> Create() const override { return nullptr; }
};

class PartRootElementMetaClass final : public OpenXMLElementClass
{
public:
    const OpenXMLElementClass* GetBaseMetaClass() const noexcept override
    {
        return OpenXmlCompositeElement::StaticMetaClass();
    }

    virtual OpenXmlQualifiedName QualifiedName() const noexcept override { return {}; }
    virtual OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return {}; }
    virtual OpenXml::FileFormatVersions GetVersion() const noexcept override { return OpenXml::FileFormatVersions::None; }
    virtual std::shared_ptr<OpenXMLElement> Create() const override { return nullptr; }
};

class LeafElementMetaClass final : public OpenXMLElementClass
{
public:
    const OpenXMLElementClass* GetBaseMetaClass() const noexcept override { return nullptr; }
    virtual OpenXmlQualifiedName QualifiedName() const noexcept override { return {}; }
    virtual OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return {}; }
    virtual OpenXml::FileFormatVersions GetVersion() const noexcept override { return OpenXml::FileFormatVersions::None; }
    virtual std::shared_ptr<OpenXMLElement> Create() const override { return nullptr; }
};

class LeafTextElementMetaClass final : public OpenXMLElementClass
{
public:
    const OpenXMLElementClass* GetBaseMetaClass() const noexcept override
    {
        return OpenXmlLeafElement::StaticMetaClass();
    }

    virtual OpenXmlQualifiedName QualifiedName() const noexcept override { return {}; }
    virtual OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return {}; }
    virtual OpenXml::FileFormatVersions GetVersion() const noexcept override { return OpenXml::FileFormatVersions::None; }
    virtual std::shared_ptr<OpenXMLElement> Create() const override { return nullptr; }
};

const OpenXMLElementClass* OpenXmlCompositeElement::StaticMetaClass() noexcept
{
    static const CompositeElementMetaClass kClass{};
    return &kClass;
}

const OpenXMLElementClass* OpenXmlCompositeElement::ElementMetaClass() const noexcept
{
    return OpenXmlCompositeElement::StaticMetaClass();
}

const OpenXMLElementClass* OpenXmlPartRootElement::StaticMetaClass() noexcept
{
    static const PartRootElementMetaClass kClass{};
    return &kClass;
}

const OpenXMLElementClass* OpenXmlPartRootElement::ElementMetaClass() const noexcept
{
    return OpenXmlPartRootElement::StaticMetaClass();
}

const OpenXMLElementClass* OpenXmlLeafElement::StaticMetaClass() noexcept
{
    static const LeafElementMetaClass kClass{};
    return &kClass;
}

const OpenXMLElementClass* OpenXmlLeafElement::ElementMetaClass() const noexcept
{
    return OpenXmlLeafElement::StaticMetaClass();
}

const OpenXMLElementClass* OpenXmlLeafTextElement::StaticMetaClass() noexcept
{
    static const LeafTextElementMetaClass kClass{};
    return &kClass;
}

const OpenXMLElementClass* OpenXmlLeafTextElement::ElementMetaClass() const noexcept
{
    return OpenXmlLeafTextElement::StaticMetaClass();
}

void OpenXMLElement::SetNodeHandle(OpenXmlNodeHandle node) noexcept
{
    m_node = node;
}

OpenXmlNodeHandle OpenXMLElement::GetNodeHandle() const noexcept
{
    return m_node;
}

bool OpenXMLElement::IsSameNode(const std::shared_ptr<OpenXMLElement>& other) const noexcept
{
    return other && IsSameNode(*other);
}

} // namespace ExyokiOffice
