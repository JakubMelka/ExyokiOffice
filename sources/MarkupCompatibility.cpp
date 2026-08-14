// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/MarkupCompatibility.hpp"

#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "MarkupCompatibilityInternal.hpp"
#include "OpenXmlDiagnosticNames.hpp"
#include "OpenXmlDomInternal.hpp"
#include "OpenXmlVersionSupport.hpp"
#include "XmlNamespaceResolver.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <optional>

namespace ExyokiOffice
{

/// File-local attribute parsing for markup compatibility.
class MarkupCompatibilityHelper
{
public:
    /**
     * @brief Splits a whitespace-separated attribute value into its entries.
     *
     * Every markup compatibility attribute is a list of prefixes or names in this
     * form. Empty runs are skipped, so leading, trailing and repeated whitespace do
     * not produce empty entries.
     */
    static std::vector<std::string> SplitTokens(std::string_view value)
    {
        std::vector<std::string> tokens;
        Size position = 0;
        while (position < value.size())
        {
            while (position < value.size() && IsXmlWhitespace(value[position]))
            {
                ++position;
            }

            const auto start = position;
            while (position < value.size() && !IsXmlWhitespace(value[position]))
            {
                ++position;
            }

            if (position > start)
            {
                tokens.emplace_back(value.substr(start, position - start));
            }
        }

        return tokens;
    }

    /** Reads a markup compatibility attribute as a token list. */
    static std::vector<std::string> ReadTokens(const OpenXMLElement& element, const OpenXmlQualifiedName& attribute)
    {
        std::string_view value;
        if (!element.TryGetAttribute(attribute, value))
        {
            return {};
        }

        return SplitTokens(value);
    }

    /**
     * @brief Meta-class for the hand-written markup compatibility elements.
     *
     * Deliberately carries no particle metadata: the children of `mc:AlternateContent`
     * and its branches are arbitrary subtrees of other vocabularies, so there is no
     * content model to validate them against. The DOM validator skips elements whose
     * meta-class has no metadata, which is exactly the intended behavior here.
     */
    class MarkupCompatibilityMetaClass final : public OpenXMLElementClass
    {
    public:
        using Factory = std::shared_ptr<OpenXMLElement> (*)();

        MarkupCompatibilityMetaClass(std::string_view localName, Factory factory) noexcept
            : localName_(localName), factory_(factory)
        {
        }

        const OpenXMLElementClass* GetBaseMetaClass() const noexcept override
        {
            return OpenXmlCompositeElement::StaticMetaClass();
        }

        OpenXmlQualifiedName QualifiedName() const noexcept override
        {
            return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, localName_);
        }

        OpenXmlQualifiedName TypeQualifiedName() const noexcept override { return QualifiedName(); }

        OpenXml::FileFormatVersions GetVersion() const noexcept override
        {
            return OpenXml::FileFormatVersions::Office2007;
        }

        std::shared_ptr<OpenXMLElement> Create() const override { return factory_(); }

    private:
        std::string_view localName_;
        Factory factory_;
    };

    static const MarkupCompatibilityMetaClass& AlternateContentMeta() noexcept
    {
        static const MarkupCompatibilityMetaClass kMeta("AlternateContent",
                                                        []
                                                        { return std::static_pointer_cast<OpenXMLElement>(
                                                              std::make_shared<AlternateContent>()); });
        return kMeta;
    }

    static const MarkupCompatibilityMetaClass& ChoiceMeta() noexcept
    {
        static const MarkupCompatibilityMetaClass kMeta("Choice",
                                                        []
                                                        { return std::static_pointer_cast<OpenXMLElement>(
                                                              std::make_shared<AlternateContentChoice>()); });
        return kMeta;
    }

    static const MarkupCompatibilityMetaClass& FallbackMeta() noexcept
    {
        static const MarkupCompatibilityMetaClass kMeta("Fallback",
                                                        []
                                                        { return std::static_pointer_cast<OpenXMLElement>(
                                                              std::make_shared<AlternateContentFallback>()); });
        return kMeta;
    }
};

bool IsXmlWhitespace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

OpenXmlQualifiedName MarkupCompatibilityNames::AlternateContent() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "AlternateContent");
}

OpenXmlQualifiedName MarkupCompatibilityNames::Choice() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "Choice");
}

OpenXmlQualifiedName MarkupCompatibilityNames::Fallback() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "Fallback");
}

OpenXmlQualifiedName MarkupCompatibilityNames::Requires() noexcept
{
    // Unprefixed: Requires is declared on mc:Choice itself, not in the mc namespace.
    return OpenXmlQualifiedName({}, "Requires");
}

OpenXmlQualifiedName MarkupCompatibilityNames::Ignorable() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "Ignorable");
}

OpenXmlQualifiedName MarkupCompatibilityNames::ProcessContent() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "ProcessContent");
}

OpenXmlQualifiedName MarkupCompatibilityNames::MustUnderstand() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "MustUnderstand");
}

OpenXmlQualifiedName MarkupCompatibilityNames::PreserveElements() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "PreserveElements");
}

OpenXmlQualifiedName MarkupCompatibilityNames::PreserveAttributes() noexcept
{
    return OpenXmlQualifiedName(kMarkupCompatibilityNamespace, "PreserveAttributes");
}

std::vector<std::shared_ptr<AlternateContentChoice>> AlternateContent::Choices() const
{
    return Elements<AlternateContentChoice>();
}

std::shared_ptr<AlternateContentFallback> AlternateContent::Fallback() const
{
    return GetFirstChildOfType<AlternateContentFallback>();
}

const OpenXMLElementClass* AlternateContent::StaticMetaClass() noexcept
{
    return &MarkupCompatibilityHelper::AlternateContentMeta();
}

const OpenXMLElementClass* AlternateContent::ElementMetaClass() const noexcept
{
    return AlternateContent::StaticMetaClass();
}

StringValue AlternateContentChoice::GetRequires() const
{
    return GetAttributeValue<StringValue>(MarkupCompatibilityNames::Requires());
}

void AlternateContentChoice::SetRequires(const StringValue& value)
{
    SetAttributeValue(MarkupCompatibilityNames::Requires(), value);
}

std::vector<std::string> AlternateContentChoice::RequiredPrefixes() const
{
    return MarkupCompatibilityHelper::ReadTokens(*this, MarkupCompatibilityNames::Requires());
}

const OpenXMLElementClass* AlternateContentChoice::StaticMetaClass() noexcept
{
    return &MarkupCompatibilityHelper::ChoiceMeta();
}

const OpenXMLElementClass* AlternateContentChoice::ElementMetaClass() const noexcept
{
    return AlternateContentChoice::StaticMetaClass();
}

const OpenXMLElementClass* AlternateContentFallback::StaticMetaClass() noexcept
{
    return &MarkupCompatibilityHelper::FallbackMeta();
}

const OpenXMLElementClass* AlternateContentFallback::ElementMetaClass() const noexcept
{
    return AlternateContentFallback::StaticMetaClass();
}

MarkupCompatibilityAttributes MarkupCompatibilityAttributes::Read(const OpenXMLElement& element)
{
    MarkupCompatibilityAttributes attributes;
    attributes.Ignorable = MarkupCompatibilityHelper::ReadTokens(element, MarkupCompatibilityNames::Ignorable());
    attributes.ProcessContent = MarkupCompatibilityHelper::ReadTokens(element, MarkupCompatibilityNames::ProcessContent());
    attributes.MustUnderstand = MarkupCompatibilityHelper::ReadTokens(element, MarkupCompatibilityNames::MustUnderstand());
    attributes.PreserveElements = MarkupCompatibilityHelper::ReadTokens(element, MarkupCompatibilityNames::PreserveElements());
    attributes.PreserveAttributes = MarkupCompatibilityHelper::ReadTokens(element, MarkupCompatibilityNames::PreserveAttributes());
    return attributes;
}

bool MarkupCompatibilityAttributes::IsEmpty() const noexcept
{
    return Ignorable.empty() && ProcessContent.empty() && MustUnderstand.empty() && PreserveElements.empty() &&
           PreserveAttributes.empty();
}

/// File-local scope bookkeeping for the markup compatibility walk.
class MarkupCompatibilityScopeHelper
{
public:
    /**
     * @brief Carries the markup compatibility rules that are in force at one element.
     *
     * `mc:Ignorable` and its companions apply to the element that declares them and to
     * everything below it, so the rules accumulate as the walk descends and are undone
     * on the way back up. Prefixes are resolved to namespace URIs at the point of
     * declaration, because the same prefix may be bound differently further down.
     */
    struct MarkupCompatibilityScope
    {
        std::vector<std::string> IgnorableNamespaces;
        std::vector<OpenXmlQualifiedName> ProcessContentNames;
        std::vector<OpenXmlQualifiedName> PreserveElementNames;
        std::vector<OpenXmlQualifiedName> PreserveAttributeNames;
        bool PreserveAllElements = false;
        bool PreserveAllAttributes = false;

        [[nodiscard]] bool IsIgnorable(std::string_view namespaceUri) const
        {
            return std::find(IgnorableNamespaces.begin(), IgnorableNamespaces.end(), namespaceUri) !=
                   IgnorableNamespaces.end();
        }

        [[nodiscard]] bool IsProcessContent(const OpenXmlQualifiedName& name) const
        {
            return std::find(ProcessContentNames.begin(), ProcessContentNames.end(), name) !=
                   ProcessContentNames.end();
        }

        [[nodiscard]] bool IsPreservedElement(const OpenXmlQualifiedName& name) const
        {
            return PreserveAllElements || std::find(PreserveElementNames.begin(), PreserveElementNames.end(),
                                                    name) != PreserveElementNames.end();
        }

        [[nodiscard]] bool IsPreservedAttribute(const OpenXmlQualifiedName& name) const
        {
            return PreserveAllAttributes || std::find(PreserveAttributeNames.begin(),
                                                      PreserveAttributeNames.end(),
                                                      name) != PreserveAttributeNames.end();
        }
    };

    /** Resolves a prefix against the namespace declarations in scope at @p element. */
    static std::optional<std::string> ResolvePrefix(const OpenXMLElement& element, std::string_view prefix)
    {
        const auto node = Detail::ToNode(element.GetNodeHandle());
        if (!node)
        {
            return std::nullopt;
        }

        if (const auto uri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix))
        {
            return std::string(*uri);
        }

        return std::nullopt;
    }

    /**
     * @brief Resolves a `prefix:localName` token into a qualified name.
     *
     * `*` is reported separately by the caller, and a token without a prefix names an
     * element in no namespace.
     */
    static std::optional<OpenXmlQualifiedName> ResolveNameToken(const OpenXMLElement& element,
                                                                std::string_view token,
                                                                std::string& namespaceStorage)
    {
        const auto colon = token.find(':');
        if (colon == std::string_view::npos)
        {
            namespaceStorage.clear();
            return OpenXmlQualifiedName({}, token);
        }

        const auto prefix = token.substr(0, colon);
        const auto localName = token.substr(colon + 1);
        auto uri = ResolvePrefix(element, prefix);
        if (!uri)
        {
            return std::nullopt;
        }

        namespaceStorage = std::move(*uri);
        return OpenXmlQualifiedName(namespaceStorage, localName);
    }
};

/**
 * @brief Recursive worker holding the state of one processing run.
 *
 * Kept out of the public class so that the scope stack and the diagnostics plumbing
 * do not leak into the header.
 */
class MarkupCompatibilityWalker
{
public:
    MarkupCompatibilityWalker(const MarkupCompatibilityProcessor& processor,
                              DiagnosticSink* diagnostics) noexcept
        : processor_(processor), diagnostics_(diagnostics)
    {
    }

    bool Process(const std::shared_ptr<OpenXMLElement>& element, MarkupCompatibilityScopeHelper::MarkupCompatibilityScope scope)
    {
        if (!element || element->IsNull())
        {
            return true;
        }

        const auto attributes = MarkupCompatibilityAttributes::Read(*element);
        if (!CheckMustUnderstand(*element, attributes))
        {
            return false;
        }

        ExtendScope(*element, attributes, scope);
        RemoveIgnorableAttributes(*element, scope);

        // Snapshot first: the loop rewrites the child list as it goes.
        const auto children = element->Children();
        for (const auto& child : children)
        {
            if (!child || child->IsNull())
            {
                continue;
            }

            if (!ProcessChild(child, scope))
            {
                return false;
            }
        }

        RemoveConsumedAttributes(*element, attributes);
        return true;
    }

private:
    /** Handles one child: alternate content, ignorable content, or plain recursion. */
    bool ProcessChild(const std::shared_ptr<OpenXMLElement>& child, const MarkupCompatibilityScopeHelper::MarkupCompatibilityScope& scope)
    {
        if (auto alternate = openxmlelement_cast<AlternateContent>(child))
        {
            return ResolveAlternateContent(alternate, scope);
        }

        const auto name = child->QualifiedName();
        if (scope.IsIgnorable(name.namespaceUri()) && !processor_.IsUnderstoodNamespace(name.namespaceUri()))
        {
            if (scope.IsPreservedElement(name))
            {
                // Preserved content is kept exactly as it is, so it is not descended
                // into either: nothing inside it may be rewritten.
                return true;
            }

            if (scope.IsProcessContent(name))
            {
                // Back through ProcessChild, not Process: what surfaces here may
                // itself be alternate content or more ignorable markup.
                for (const auto& promoted : child->ReplaceWithChildren())
                {
                    if (!ProcessChild(promoted, scope))
                    {
                        return false;
                    }
                }
                return true;
            }

            child->Remove();
            return true;
        }

        return Process(child, scope);
    }

    /** Replaces an mc:AlternateContent with the content of the branch that applies. */
    bool ResolveAlternateContent(const std::shared_ptr<AlternateContent>& alternate,
                                 const MarkupCompatibilityScopeHelper::MarkupCompatibilityScope& scope)
    {
        std::shared_ptr<OpenXMLElement> selected;
        for (const auto& choice : alternate->Choices())
        {
            const auto prefixes = choice->RequiredPrefixes();
            if (prefixes.empty())
            {
                Report(*choice, ValidationErrorId::MarkupCompatibilityMalformedAlternateContent,
                       "An mc:Choice must carry a non-empty Requires attribute.");
                continue;
            }

            if (RequirementsSatisfied(*choice, prefixes))
            {
                selected = choice;
                break;
            }
        }

        if (!selected)
        {
            selected = alternate->Fallback();
        }

        for (const auto& branch : alternate->Children())
        {
            if (!branch || branch->IsNull())
            {
                continue;
            }

            const auto name = branch->QualifiedName();
            if (name != MarkupCompatibilityNames::Choice() && name != MarkupCompatibilityNames::Fallback())
            {
                Report(*branch, ValidationErrorId::MarkupCompatibilityMalformedAlternateContent,
                       "An mc:AlternateContent may only contain mc:Choice and mc:Fallback.");
            }

            if (!selected || !branch->IsSameNode(*selected))
            {
                branch->Remove();
            }
        }

        // Promote the winning branch twice: first out of its mc:Choice or
        // mc:Fallback, then out of the mc:AlternateContent, which lands its content
        // exactly where the alternate content stood.
        if (selected)
        {
            selected->ReplaceWithChildren();
        }

        // The promoted content is examined as if it had been written in place: a
        // branch may well contain further alternate content of its own.
        const auto promoted = alternate->ReplaceWithChildren();
        for (const auto& element : promoted)
        {
            if (!ProcessChild(element, scope))
            {
                return false;
            }
        }

        return true;
    }

    /** Reports whether every prefix a choice requires resolves to an understood namespace. */
    bool RequirementsSatisfied(const OpenXMLElement& choice, const std::vector<std::string>& prefixes)
    {
        for (const auto& prefix : prefixes)
        {
            const auto uri = MarkupCompatibilityScopeHelper::ResolvePrefix(choice, prefix);
            if (!uri)
            {
                Report(choice, ValidationErrorId::MarkupCompatibilityUnresolvablePrefix,
                       "Namespace prefix '" + prefix + "' required by mc:Choice is not declared in scope.");
                return false;
            }

            if (!processor_.IsUnderstoodNamespace(*uri))
            {
                return false;
            }
        }

        return true;
    }

    /** Fails the run when the content demands a namespace the target does not cover. */
    bool CheckMustUnderstand(const OpenXMLElement& element, const MarkupCompatibilityAttributes& attributes)
    {
        for (const auto& prefix : attributes.MustUnderstand)
        {
            const auto uri = MarkupCompatibilityScopeHelper::ResolvePrefix(element, prefix);
            if (!uri)
            {
                Report(element, ValidationErrorId::MarkupCompatibilityUnresolvablePrefix,
                       "Namespace prefix '" + prefix + "' in mc:MustUnderstand is not declared in scope.");
                return false;
            }

            if (!processor_.IsUnderstoodNamespace(*uri))
            {
                Report(element, ValidationErrorId::MarkupCompatibilityMustUnderstandUnsupported,
                       "The document requires namespace '" + *uri +
                           "', which the requested Office version does not support.");
                return false;
            }
        }

        return true;
    }

    /** Adds the rules declared on @p element to the inherited ones. */
    void ExtendScope(const OpenXMLElement& element,
                     const MarkupCompatibilityAttributes& attributes,
                     MarkupCompatibilityScopeHelper::MarkupCompatibilityScope& scope)
    {
        for (const auto& prefix : attributes.Ignorable)
        {
            if (auto uri = MarkupCompatibilityScopeHelper::ResolvePrefix(element, prefix))
            {
                if (!scope.IsIgnorable(*uri))
                {
                    scope.IgnorableNamespaces.push_back(std::move(*uri));
                }
            }
            else
            {
                Report(element, ValidationErrorId::MarkupCompatibilityUnresolvablePrefix,
                       "Namespace prefix '" + prefix + "' in mc:Ignorable is not declared in scope.");
            }
        }

        CollectNames(element, attributes.ProcessContent, scope.ProcessContentNames, nullptr);
        CollectNames(element, attributes.PreserveElements, scope.PreserveElementNames,
                     &scope.PreserveAllElements);
        CollectNames(element, attributes.PreserveAttributes, scope.PreserveAttributeNames,
                     &scope.PreserveAllAttributes);
    }

    /** Resolves a list of name tokens into the scope, honoring the `*` wildcard. */
    void CollectNames(const OpenXMLElement& element,
                      const std::vector<std::string>& tokens,
                      std::vector<OpenXmlQualifiedName>& target,
                      bool* wildcard)
    {
        for (const auto& token : tokens)
        {
            if (token == "*")
            {
                if (wildcard)
                {
                    *wildcard = true;
                }
                continue;
            }

            std::string namespaceStorage;
            const auto name = MarkupCompatibilityScopeHelper::ResolveNameToken(element, token, namespaceStorage);
            if (!name)
            {
                Report(element, ValidationErrorId::MarkupCompatibilityUnresolvablePrefix,
                       "Name '" + token + "' in a markup compatibility attribute uses an undeclared prefix.");
                continue;
            }

            // The scope outlives the storage the qualified name views, so both halves
            // are interned here.
            names_.push_back(std::move(namespaceStorage));
            const auto& internedNamespace = names_.back();
            names_.push_back(std::string(name->localName()));
            const auto& internedLocalName = names_.back();
            target.emplace_back(internedNamespace, internedLocalName);
        }
    }

    /** Drops attributes in ignorable namespaces that nothing preserves. */
    void RemoveIgnorableAttributes(OpenXMLElement& element, const MarkupCompatibilityScopeHelper::MarkupCompatibilityScope& scope)
    {
        if (scope.IgnorableNamespaces.empty())
        {
            return;
        }

        auto node = Detail::ToNode(element.GetNodeHandle());
        if (!node)
        {
            return;
        }

        std::vector<std::string> removable;
        for (const auto& attribute : node.attributes())
        {
            const std::string_view rawName = attribute.name() ? attribute.name() : "";
            const auto colon = rawName.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }

            const auto prefix = rawName.substr(0, colon);
            if (prefix == "xmlns")
            {
                continue;
            }

            const auto uri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
            if (!uri || !scope.IsIgnorable(*uri) || processor_.IsUnderstoodNamespace(*uri))
            {
                continue;
            }

            if (scope.IsPreservedAttribute(OpenXmlQualifiedName(*uri, rawName.substr(colon + 1))))
            {
                continue;
            }

            removable.emplace_back(rawName);
        }

        for (const auto& name : removable)
        {
            node.remove_attribute(name.c_str());
        }
    }

    /** Removes the markup compatibility attributes whose rules have now been applied. */
    void RemoveConsumedAttributes(OpenXMLElement& element, const MarkupCompatibilityAttributes& attributes)
    {
        if (attributes.IsEmpty())
        {
            return;
        }

        element.RemoveAttribute(MarkupCompatibilityNames::Ignorable());
        element.RemoveAttribute(MarkupCompatibilityNames::ProcessContent());
        element.RemoveAttribute(MarkupCompatibilityNames::MustUnderstand());
        element.RemoveAttribute(MarkupCompatibilityNames::PreserveElements());
        element.RemoveAttribute(MarkupCompatibilityNames::PreserveAttributes());
    }

    void Report(const OpenXMLElement& element, ValidationErrorId id, std::string message)
    {
        if (!diagnostics_)
        {
            return;
        }

        ValidationIssue issue;
        issue.Severity = ValidationSeverity::Error;
        issue.Domain = ValidationDomain::Dom;
        issue.Id = id;
        issue.Location = element.GetXmlLocation();
        issue.Message = std::move(message);
        diagnostics_->Report(std::move(issue));
    }

    const MarkupCompatibilityProcessor& processor_;
    DiagnosticSink* diagnostics_;

    // Stable storage for the strings the scope's qualified names refer to.
    // OpenXmlQualifiedName holds views, and the scope outlives the attribute values
    // they were parsed from.
    std::deque<std::string> names_;
};

MarkupCompatibilityProcessor::MarkupCompatibilityProcessor(MarkupCompatibilityProcessSettings settings,
                                                           DiagnosticSink* diagnostics) noexcept
    : settings_(settings), diagnostics_(diagnostics)
{
}

bool MarkupCompatibilityProcessor::Process(const std::shared_ptr<OpenXMLElement>& element)
{
    MarkupCompatibilityWalker walker(*this, diagnostics_);
    return walker.Process(element, MarkupCompatibilityScopeHelper::MarkupCompatibilityScope{});
}

bool MarkupCompatibilityProcessor::IsUnderstoodNamespace(std::string_view namespaceUri) const
{
    if (namespaceUri.empty() || namespaceUri == kMarkupCompatibilityNamespace)
    {
        return true;
    }

    // A namespace is understood when it is one this library knows and the target
    // generation already had. Anything outside the well-known table belongs to some
    // other producer's extension and cannot be understood by definition.
    const auto prefix = OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(namespaceUri);
    if (!prefix)
    {
        return false;
    }

    const auto version = OpenXml::Features::OpenXmlNamespaceResolver::GetVersionForPrefix(*prefix);
    return Detail::SupportsOfficeVersion(settings_.TargetFileFormatVersions, version);
}

bool ProcessMarkupCompatibility(OpenXmlPackage& package,
                                const std::shared_ptr<OpenXmlPackagePart>& mainPart,
                                const MarkupCompatibilityProcessSettings& settings,
                                DiagnosticSink* diagnostics)
{
    if (settings.ProcessMode == MarkupCompatibilityProcessMode::NoProcess)
    {
        return true;
    }

    std::vector<std::shared_ptr<OpenXmlPackagePart>> parts;
    std::vector<const OpenXmlPackagePart*> visited;
    const auto collect = [&](const std::shared_ptr<OpenXmlPackagePart>& part)
    {
        if (!part || std::find(visited.begin(), visited.end(), part.get()) != visited.end())
        {
            return false;
        }
        visited.push_back(part.get());
        parts.push_back(part);
        return true;
    };

    if (settings.ProcessMode == MarkupCompatibilityProcessMode::ProcessLoadedPartsOnly)
    {
        // "Loaded" is the document itself and what it points at directly: enough for
        // the main content, without walking into every theme and media graph.
        if (collect(mainPart))
        {
            for (const auto& related : mainPart->Parts())
            {
                collect(related);
            }
        }
    }
    else
    {
        const std::function<void(const OpenXmlPartContainer&)> walk = [&](const OpenXmlPartContainer& container)
        {
            for (const auto& child : container.Parts())
            {
                if (collect(child))
                {
                    walk(*child);
                }
            }
        };
        walk(package);
    }

    MarkupCompatibilityProcessor processor(settings, diagnostics);
    for (const auto& part : parts)
    {
        if (!part->IsXmlPart())
        {
            continue;
        }

        auto root = part->GetRootElement();
        if (!root)
        {
            continue;
        }

        if (!processor.Process(root))
        {
            return false;
        }
    }

    return true;
}

std::vector<std::string> CollectIgnorableNamespaces(const OpenXMLElement& element)
{
    std::vector<std::string> namespaces;
    const auto collect = [&namespaces](const OpenXMLElement& current)
    {
        for (const auto& prefix : MarkupCompatibilityAttributes::Read(current).Ignorable)
        {
            auto uri = MarkupCompatibilityScopeHelper::ResolvePrefix(current, prefix);
            if (uri && std::find(namespaces.begin(), namespaces.end(), *uri) == namespaces.end())
            {
                namespaces.push_back(std::move(*uri));
            }
        }
    };

    collect(element);
    for (auto ancestor = element.Parent(); ancestor; ancestor = ancestor->Parent())
    {
        collect(*ancestor);
    }
    return namespaces;
}

const OpenXMLElementClass* ResolveMarkupCompatibilityClass(const OpenXmlQualifiedName& name) noexcept
{
    if (name.namespaceUri() != kMarkupCompatibilityNamespace)
    {
        return nullptr;
    }

    if (name.localName() == "AlternateContent")
    {
        return AlternateContent::StaticMetaClass();
    }
    if (name.localName() == "Choice")
    {
        return AlternateContentChoice::StaticMetaClass();
    }
    if (name.localName() == "Fallback")
    {
        return AlternateContentFallback::StaticMetaClass();
    }

    return nullptr;
}

} // namespace ExyokiOffice
