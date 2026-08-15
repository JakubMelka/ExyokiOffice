// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Xml/XmlQuery.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

namespace ExyokiOffice::Xml
{

using ExyokiOffice::Detail::NodeOf;
using ExyokiOffice::Pugi::xml_node;
using ExyokiOffice::Pugi::xml_node_type;

/// File-local helpers behind namespace-aware XPath queries.
class XmlQueryHelper
{
public:
    // XML names may be written in any script, so every byte outside ASCII is a
    // name byte here. This scans the query the caller wrote rather than
    // validating a name, so recognizing the whole of a UTF-8 sequence is all
    // that is asked of it; `Utf8Text::IsNcName` is the test with the real rule.
    static bool IsNameStart(char c) noexcept
    {
        return c == '_' || AsciiText::IsAlpha(c) || AsciiText::IsNonAscii(c);
    }

    static bool IsNameChar(char c) noexcept
    {
        return IsNameStart(c) || AsciiText::IsDigit(c) || c == '-' || c == '.';
    }

    /// Resolves a query prefix to a namespace URI using bindings, then the document, then well-known table.
    static std::optional<std::string> ResolvePrefix(std::string_view prefix, const XmlQueryOptions& options,
                                                    const xml_node& contextNode)
    {
        for (const auto& [boundPrefix, uri] : options.NamespaceBindings)
        {
            if (boundPrefix == prefix)
            {
                return uri;
            }
        }

        if (prefix == "xml")
        {
            return std::string("http://www.w3.org/XML/1998/namespace");
        }

        if (auto declared = NamespaceResolver::LookupUriForPrefix(contextNode, prefix))
        {
            return std::string(*declared);
        }

        if (auto wellKnown = ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getUrlForPrefix(prefix))
        {
            return std::string(*wellKnown);
        }

        return std::nullopt;
    }

    /// Emits a namespace-aware node test predicate for a resolved prefix/local pair.
    static void AppendNameTest(std::string& out, const std::string& uri, std::string_view local)
    {
        out += "*[namespace-uri()='";
        out += uri;
        out += "'";
        if (local != "*")
        {
            out += " and local-name()='";
            out += local;
            out += "'";
        }
        out += "]";
    }

    /**
     * Rewrites prefixed element/attribute name tests (w:p, @w:val, w:*) into
     * namespace-aware predicates so pugixml matches by namespace URI instead of by
     * the literal document prefix. String literals, axis specifiers (child::),
     * function calls, and everything else are copied verbatim.
     */
    static bool RewriteQuery(std::string_view xpath, const XmlQueryOptions& options, const xml_node& contextNode,
                             std::string& out, std::string& error)
    {
        out.clear();
        out.reserve(xpath.size() + 32);

        const Size n = xpath.size();
        Size i = 0;
        while (i < n)
        {
            const char c = xpath[i];

            if (c == '\'' || c == '"')
            {
                const char quote = c;
                out += c;
                ++i;
                while (i < n && xpath[i] != quote)
                {
                    out += xpath[i++];
                }
                if (i < n)
                {
                    out += xpath[i++]; // closing quote
                }
                continue;
            }

            if (IsNameStart(c))
            {
                Size start = i;
                while (i < n && IsNameChar(xpath[i]))
                {
                    ++i;
                }
                std::string_view ncName = xpath.substr(start, i - start);

                // A QName name test: NCName ':' (NCName | '*'), but not the '::' axis separator.
                if (i + 1 < n && xpath[i] == ':' && xpath[i + 1] != ':' &&
                    (IsNameStart(xpath[i + 1]) || xpath[i + 1] == '*'))
                {
                    ++i; // consume ':'
                    std::string_view local;
                    if (xpath[i] == '*')
                    {
                        local = xpath.substr(i, 1);
                        ++i;
                    }
                    else
                    {
                        Size localStart = i;
                        while (i < n && IsNameChar(xpath[i]))
                        {
                            ++i;
                        }
                        local = xpath.substr(localStart, i - localStart);
                    }

                    // A namespaced function call (prefix:fn(...)) is left untouched.
                    Size peek = i;
                    while (peek < n && (xpath[peek] == ' ' || xpath[peek] == '\t'))
                    {
                        ++peek;
                    }
                    if (peek < n && xpath[peek] == '(')
                    {
                        out += ncName;
                        out += ':';
                        out += local;
                        continue;
                    }

                    auto uri = ResolvePrefix(ncName, options, contextNode);
                    if (!uri)
                    {
                        error = "unbound namespace prefix '" + std::string(ncName) + "'";
                        return false;
                    }
                    // For an attribute test the '@' has already been emitted; for an element test this
                    // stands on its own. Both cases append the same '*[...]' predicate.
                    AppendNameTest(out, *uri, local);
                    continue;
                }

                out += ncName;
                continue;
            }

            out += c;
            ++i;
        }

        return true;
    }

    static void CollectText(const xml_node& node, std::string& out)
    {
        for (xml_node child = node.first_child(); child; child = child.next_sibling())
        {
            const auto type = child.type();
            if (type == xml_node_type::node_pcdata || type == xml_node_type::node_cdata)
            {
                out += child.value();
            }
            else if (type == xml_node_type::node_element)
            {
                CollectText(child, out);
            }
        }
    }

    template <typename Fn>
    static void ForEachDescendantElement(const xml_node& node, const Fn& fn)
    {
        for (xml_node child = node.first_child(); child; child = child.next_sibling())
        {
            if (child.type() == xml_node_type::node_element)
            {
                fn(child);
                ForEachDescendantElement(child, fn);
            }
        }
    }
};

std::string InnerText(const std::shared_ptr<OpenXMLElement>& element)
{
    std::string out;
    const xml_node node = NodeOf(element);
    if (node)
    {
        XmlQueryHelper::CollectText(node, out);
    }
    return out;
}

std::vector<std::shared_ptr<OpenXMLElement>> SelectNodes(const std::shared_ptr<OpenXMLElement>& root,
                                                         std::string_view xpath,
                                                         const XmlQueryOptions& options, std::string* error)
{
    std::vector<std::shared_ptr<OpenXMLElement>> results;

    if (error)
    {
        error->clear();
    }

    const xml_node rootNode = NodeOf(root);
    if (!rootNode)
    {
        if (error)
        {
            *error = "query root is empty";
        }
        return results;
    }

    std::string rewritten;
    std::string rewriteError;
    if (!XmlQueryHelper::RewriteQuery(xpath, options, rootNode, rewritten, rewriteError))
    {
        if (error)
        {
            *error = rewriteError;
        }
        return results;
    }

    // pugixml is built with PUGIXML_NO_EXCEPTIONS, so a malformed expression surfaces as a
    // parse-result error state rather than a thrown exception.
    const ExyokiOffice::Pugi::xpath_query query(rewritten.c_str());
    if (query.result().error != nullptr)
    {
        if (error)
        {
            *error = std::string("invalid XPath expression: ") + query.result().error;
        }
        return results;
    }

    const ExyokiOffice::Pugi::xpath_node_set nodes = rootNode.select_nodes(query);
    // pugixml returns a document-ordered, duplicate-free node set.
    for (const auto& hit : nodes)
    {
        const xml_node node = hit.node();
        if (!node || node.type() != xml_node_type::node_element)
        {
            continue;
        }
        if (auto element = Detail::CreateOpenXmlElementFromNode(node))
        {
            results.push_back(std::move(element));
        }
    }

    return results;
}

XmlQueryMatch Describe(const std::shared_ptr<OpenXMLElement>& element)
{
    XmlQueryMatch match;
    match.Element = element;
    if (!element)
    {
        return match;
    }

    match.Location = element->GetXmlLocation().Path;

    const auto qualifiedName = element->QualifiedName();
    match.NamespaceUri = std::string(qualifiedName.namespaceUri());
    match.LocalName = std::string(qualifiedName.localName());

    const xml_node node = NodeOf(element);
    if (node)
    {
        match.Name = node.name();
        for (auto attribute = node.first_attribute(); attribute; attribute = attribute.next_attribute())
        {
            std::string_view name = attribute.name();
            if (name == "xmlns" || name.rfind("xmlns:", 0) == 0)
            {
                continue;
            }
            match.Attributes.emplace_back(std::string(name), std::string(attribute.value()));
        }
    }

    match.Text = InnerText(element);
    return match;
}

XmlQuery XmlQuery::Select(const std::shared_ptr<OpenXMLElement>& root, std::string_view xpath,
                          const XmlQueryOptions& options)
{
    XmlQuery query;
    query.elements_ = SelectNodes(root, xpath, options, nullptr);
    return query;
}

XmlQuery& XmlQuery::Where(const std::function<bool(const OpenXMLElement&)>& predicate)
{
    std::vector<std::shared_ptr<OpenXMLElement>> kept;
    kept.reserve(elements_.size());
    for (auto& element : elements_)
    {
        if (element && predicate(*element))
        {
            kept.push_back(std::move(element));
        }
    }
    elements_ = std::move(kept);
    return *this;
}

XmlQuery& XmlQuery::SelectDescendants(std::string_view localName, std::optional<std::string_view> namespaceUri)
{
    std::vector<std::shared_ptr<OpenXMLElement>> descendants;
    for (const auto& element : elements_)
    {
        const xml_node node = NodeOf(element);
        if (!node)
        {
            continue;
        }
        XmlQueryHelper::ForEachDescendantElement(node, [&](const xml_node& child)
                                                 {
            auto wrapped = Detail::CreateOpenXmlElementFromNode(child);
            if (!wrapped)
            {
                return;
            }
            const auto qualifiedName = wrapped->QualifiedName();
            if (qualifiedName.localName() != localName)
            {
                return;
            }
            if (namespaceUri && qualifiedName.namespaceUri() != *namespaceUri)
            {
                return;
            }
            descendants.push_back(std::move(wrapped)); });
    }
    elements_ = std::move(descendants);
    return *this;
}

std::vector<std::shared_ptr<OpenXMLElement>> XmlQuery::ToList() const
{
    return elements_;
}

std::shared_ptr<OpenXMLElement> XmlQuery::FirstOrDefault() const
{
    return elements_.empty() ? nullptr : elements_.front();
}

Size XmlQuery::Count() const
{
    return elements_.size();
}

std::vector<std::shared_ptr<OpenXMLElement>> XmlHelpers::FindByAttribute(
    const std::shared_ptr<OpenXMLElement>& root, const OpenXmlQualifiedName& attribute, std::string_view value)
{
    std::vector<std::shared_ptr<OpenXMLElement>> results;

    const xml_node rootNode = NodeOf(root);
    if (!rootNode)
    {
        return results;
    }

    auto consider = [&](const std::shared_ptr<OpenXMLElement>& element)
    {
        std::string_view current;
        if (element && element->TryGetAttribute(attribute, current) && current == value)
        {
            results.push_back(element);
        }
    };

    consider(root);
    XmlQueryHelper::ForEachDescendantElement(rootNode, [&](const xml_node& child)
                                             { consider(Detail::CreateOpenXmlElementFromNode(child)); });

    return results;
}

std::vector<std::string> XmlHelpers::ExtractAllText(const std::shared_ptr<OpenXMLElement>& root)
{
    std::vector<std::string> texts;

    const xml_node rootNode = NodeOf(root);
    if (!rootNode)
    {
        return texts;
    }

    auto collect = [&](const xml_node& node)
    {
        for (xml_node child = node.first_child(); child; child = child.next_sibling())
        {
            const auto type = child.type();
            if ((type == xml_node_type::node_pcdata || type == xml_node_type::node_cdata) &&
                child.value() != nullptr && child.value()[0] != '\0')
            {
                texts.emplace_back(child.value());
            }
        }
    };

    collect(rootNode);
    XmlQueryHelper::ForEachDescendantElement(rootNode, collect);
    return texts;
}

} // namespace ExyokiOffice::Xml
