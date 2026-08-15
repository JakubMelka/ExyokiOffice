// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "XmlCanonicalization.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Security
{

/// Name splitting and character escaping rules of the canonical form.
class C14NHelpers final
{
public:
    C14NHelpers() = delete;

    /// The xml prefix is bound to this namespace without being declared.
    static constexpr std::string_view XmlNamespaceUri = "http://www.w3.org/XML/1998/namespace";

    static std::string_view PrefixOf(std::string_view qualifiedName) noexcept
    {
        const auto colon = qualifiedName.find(':');
        return colon == std::string_view::npos ? std::string_view{} : qualifiedName.substr(0, colon);
    }

    static std::string_view LocalNameOf(std::string_view qualifiedName) noexcept
    {
        const auto colon = qualifiedName.find(':');
        return colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
    }

    static bool IsNamespaceDeclaration(std::string_view attributeName) noexcept
    {
        return attributeName == "xmlns" || attributeName.rfind("xmlns:", 0) == 0;
    }

    /// Returns the prefix a namespace declaration binds ("" for xmlns=...).
    static std::string_view DeclaredPrefix(std::string_view attributeName) noexcept
    {
        return attributeName == "xmlns" ? std::string_view{} : attributeName.substr(6);
    }

    /// Text and CDATA content: ampersand, angle brackets, and carriage return.
    static void AppendEscapedText(std::string& output, std::string_view text)
    {
        for (const auto character : text)
        {
            switch (character)
            {
                case '&':
                    output += "&amp;";
                    break;
                case '<':
                    output += "&lt;";
                    break;
                case '>':
                    output += "&gt;";
                    break;
                case '\r':
                    output += "&#xD;";
                    break;
                default:
                    output.push_back(character);
                    break;
            }
        }
    }

    /// Attribute values: ampersand, less than, quote, and the white space
    /// characters that an XML parser would otherwise normalize away.
    static void AppendEscapedAttributeValue(std::string& output, std::string_view value)
    {
        for (const auto character : value)
        {
            switch (character)
            {
                case '&':
                    output += "&amp;";
                    break;
                case '<':
                    output += "&lt;";
                    break;
                case '"':
                    output += "&quot;";
                    break;
                case '\t':
                    output += "&#x9;";
                    break;
                case '\n':
                    output += "&#xA;";
                    break;
                case '\r':
                    output += "&#xD;";
                    break;
                default:
                    output.push_back(character);
                    break;
            }
        }
    }
};

/// Produces the canonical form of a subtree.
///
/// Two namespace maps are threaded through the traversal: the in-scope map,
/// which resolves attribute prefixes, and the rendered map, which records what
/// the output ancestors already declared so redundant declarations are dropped.
class C14NWriter final
{
public:
    using NamespaceMap = std::map<std::string, std::string, std::less<>>;

    explicit C14NWriter(bool withComments)
        : m_withComments(withComments)
    {
    }

    std::string TakeOutput() { return std::move(m_output); }

    /// True when the input was nested deeper than XmlCanonicalization::MaximumDepth.
    [[nodiscard]] bool DepthExceeded() const noexcept { return m_depthExceeded; }

    /// Writes an element together with the namespaces and xml: attributes it
    /// inherits from ancestors outside the canonicalized subset.
    void WriteApexElement(const Pugi::xml_node& node)
    {
        NamespaceMap inheritedNamespaces;
        NamespaceMap inheritedXmlAttributes;
        CollectInheritedState(node, inheritedNamespaces, inheritedXmlAttributes);
        WriteElement(node, inheritedNamespaces, NamespaceMap{}, inheritedXmlAttributes, true);
    }

    void WriteTopLevelNode(const Pugi::xml_node& node, bool beforeDocumentElement)
    {
        switch (node.type())
        {
            case Pugi::node_comment:
                if (!m_withComments)
                {
                    return;
                }
                break;
            case Pugi::node_pi:
                break;
            default:
                return;
        }

        if (!beforeDocumentElement)
        {
            m_output.push_back('\n');
        }
        WriteCommentOrProcessingInstruction(node);
        if (beforeDocumentElement)
        {
            m_output.push_back('\n');
        }
    }

private:
    /// Walks the ancestors of \p node and replays their namespace declarations
    /// and xml: attributes, so the apex element can render them itself.
    static void CollectInheritedState(const Pugi::xml_node& node,
                                      NamespaceMap& namespaces,
                                      NamespaceMap& xmlAttributes)
    {
        std::vector<Pugi::xml_node> ancestors;
        for (auto parent = node.parent(); parent && parent.type() == Pugi::node_element; parent = parent.parent())
        {
            ancestors.push_back(parent);
        }

        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
        {
            for (const auto& attribute : it->attributes())
            {
                const std::string_view name = attribute.name();
                if (C14NHelpers::IsNamespaceDeclaration(name))
                {
                    namespaces[std::string(C14NHelpers::DeclaredPrefix(name))] = attribute.value();
                }
                else if (C14NHelpers::PrefixOf(name) == "xml")
                {
                    xmlAttributes[std::string(name)] = attribute.value();
                }
            }
        }
    }

    void WriteElement(const Pugi::xml_node& node,
                      const NamespaceMap& parentInScope,
                      const NamespaceMap& parentRendered,
                      const NamespaceMap& inheritedXmlAttributes,
                      bool isApex)
    {
        // Checked before the frame does any work, because the frame is the
        // expensive part: the namespace map below is copied per level, so a
        // document nested deeply enough exhausts the stack long before it
        // exhausts anything else. Once tripped, the flag stays set and the
        // caller discards the partial output rather than digesting it.
        if (m_depth >= XmlCanonicalization::MaximumDepth)
        {
            m_depthExceeded = true;
            return;
        }

        const DepthGuard guard(m_depth);

        NamespaceMap inScope = parentInScope;
        NamespaceMap ownDeclarations;
        for (const auto& attribute : node.attributes())
        {
            const std::string_view name = attribute.name();
            if (!C14NHelpers::IsNamespaceDeclaration(name))
            {
                continue;
            }
            std::string prefix(C14NHelpers::DeclaredPrefix(name));
            ownDeclarations[prefix] = attribute.value();
            inScope[prefix] = attribute.value();
        }

        // The apex must render everything it inherits; a descendant only needs
        // to render what it declares itself.
        const NamespaceMap& candidates = isApex ? inScope : ownDeclarations;

        NamespaceMap rendered = parentRendered;
        std::string declarations;
        for (const auto& [prefix, uri] : candidates)
        {
            if (uri.empty())
            {
                // An empty default namespace is only written to undeclare an
                // inherited one; there is nothing to undeclare otherwise.
                const auto inherited = rendered.find(std::string_view{});
                if (prefix.empty() && inherited != rendered.end() && !inherited->second.empty())
                {
                    declarations += " xmlns=\"\"";
                    rendered[prefix] = uri;
                }
                continue;
            }

            if (const auto existing = rendered.find(prefix); existing != rendered.end() && existing->second == uri)
            {
                continue;
            }

            if (prefix.empty())
            {
                declarations += " xmlns=\"";
            }
            else
            {
                declarations += " xmlns:";
                declarations += prefix;
                declarations += "=\"";
            }
            C14NHelpers::AppendEscapedAttributeValue(declarations, uri);
            declarations.push_back('"');
            rendered[prefix] = uri;
        }

        const std::string_view elementName = node.name();
        m_output.push_back('<');
        m_output.append(elementName);
        m_output += declarations;
        WriteAttributes(node, inScope, inheritedXmlAttributes, isApex);
        m_output.push_back('>');

        for (const auto& child : node.children())
        {
            WriteChild(child, inScope, rendered);
        }

        m_output += "</";
        m_output.append(elementName);
        m_output.push_back('>');
    }

    void WriteAttributes(const Pugi::xml_node& node,
                         const NamespaceMap& inScope,
                         const NamespaceMap& inheritedXmlAttributes,
                         bool isApex)
    {
        // Sorted by namespace URI first and local name second, with unprefixed
        // attributes (empty namespace URI) coming first.
        std::vector<std::pair<std::string, std::string>> attributes;
        for (const auto& attribute : node.attributes())
        {
            const std::string_view name = attribute.name();
            if (C14NHelpers::IsNamespaceDeclaration(name))
            {
                continue;
            }
            attributes.emplace_back(std::string(name), std::string(attribute.value()));
        }

        if (isApex)
        {
            for (const auto& [name, value] : inheritedXmlAttributes)
            {
                const auto present = std::any_of(attributes.begin(),
                                                 attributes.end(),
                                                 [&name](const auto& entry)
                                                 { return entry.first == name; });
                if (!present)
                {
                    attributes.emplace_back(name, value);
                }
            }
        }

        const auto namespaceUriOf = [&inScope](std::string_view qualifiedName) -> std::string_view
        {
            const auto prefix = C14NHelpers::PrefixOf(qualifiedName);
            if (prefix.empty())
            {
                // An unprefixed attribute is never in the default namespace.
                return {};
            }
            if (prefix == "xml")
            {
                return C14NHelpers::XmlNamespaceUri;
            }
            if (const auto binding = inScope.find(prefix); binding != inScope.end())
            {
                return binding->second;
            }
            return prefix;
        };

        std::sort(attributes.begin(),
                  attributes.end(),
                  [&namespaceUriOf](const auto& left, const auto& right)
                  {
                      const auto leftUri = namespaceUriOf(left.first);
                      const auto rightUri = namespaceUriOf(right.first);
                      if (leftUri != rightUri)
                      {
                          return leftUri < rightUri;
                      }
                      return C14NHelpers::LocalNameOf(left.first) < C14NHelpers::LocalNameOf(right.first);
                  });

        for (const auto& [name, value] : attributes)
        {
            m_output.push_back(' ');
            m_output.append(name);
            m_output += "=\"";
            C14NHelpers::AppendEscapedAttributeValue(m_output, value);
            m_output.push_back('"');
        }
    }

    void WriteChild(const Pugi::xml_node& node, const NamespaceMap& inScope, const NamespaceMap& rendered)
    {
        switch (node.type())
        {
            case Pugi::node_element:
                WriteElement(node, inScope, rendered, NamespaceMap{}, false);
                break;
            case Pugi::node_pcdata:
            case Pugi::node_cdata:
                C14NHelpers::AppendEscapedText(m_output, node.value());
                break;
            case Pugi::node_comment:
                if (m_withComments)
                {
                    WriteCommentOrProcessingInstruction(node);
                }
                break;
            case Pugi::node_pi:
                WriteCommentOrProcessingInstruction(node);
                break;
            default:
                break;
        }
    }

    void WriteCommentOrProcessingInstruction(const Pugi::xml_node& node)
    {
        if (node.type() == Pugi::node_comment)
        {
            m_output += "<!--";
            m_output.append(node.value());
            m_output += "-->";
            return;
        }

        m_output += "<?";
        m_output.append(node.name());
        const std::string_view value = node.value();
        if (!value.empty())
        {
            m_output.push_back(' ');
            m_output.append(value);
        }
        m_output += "?>";
    }

    /// Counts one level of element nesting for as long as the frame lives.
    class DepthGuard final
    {
    public:
        explicit DepthGuard(unsigned int& depth) noexcept
            : m_depth(&depth)
        {
            ++*m_depth;
        }

        ~DepthGuard() { --*m_depth; }

        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        DepthGuard(DepthGuard&&) = delete;
        DepthGuard& operator=(DepthGuard&&) = delete;

    private:
        unsigned int* m_depth;
    };

    std::string m_output;
    bool m_withComments = false;
    unsigned int m_depth = 0;
    bool m_depthExceeded = false;
};

std::optional<std::string> XmlCanonicalization::CanonicalizeSubtree(const Pugi::xml_node& node,
                                                                    CanonicalizationMethod method)
{
    if (!node || node.type() != Pugi::node_element)
    {
        return std::nullopt;
    }

    C14NWriter writer(method == CanonicalizationMethod::InclusiveC14NWithComments);
    writer.WriteApexElement(node);
    if (writer.DepthExceeded())
    {
        // A truncated canonical form is worse than none: it would still hash to
        // something, and a digest over part of a document is a digest that means
        // nothing. Reporting no result at all makes the caller decide.
        return std::nullopt;
    }

    return writer.TakeOutput();
}

std::optional<std::string> XmlCanonicalization::CanonicalizeDocument(const Pugi::xml_document& document,
                                                                     CanonicalizationMethod method)
{
    C14NWriter writer(method == CanonicalizationMethod::InclusiveC14NWithComments);
    bool beforeDocumentElement = true;
    for (const auto& child : document.children())
    {
        if (child.type() == Pugi::node_element)
        {
            writer.WriteApexElement(child);
            beforeDocumentElement = false;
            continue;
        }
        writer.WriteTopLevelNode(child, beforeDocumentElement);
    }

    if (writer.DepthExceeded())
    {
        return std::nullopt;
    }

    return writer.TakeOutput();
}

} // namespace ExyokiOffice::Security
