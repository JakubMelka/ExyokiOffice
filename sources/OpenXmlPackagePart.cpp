// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Packaging/OpenXmlPackageFactory.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "OpenXmlDomInternal.hpp"
#include "OpenXmlPackageInternal.hpp"
#include "OpenXmlPackageUri.hpp"
#include "Security/SignatureNames.hpp"
#include "XmlNamespaceResolver.hpp"
#include "XmlParseOptions.hpp"

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ExyokiOffice
{

namespace
{

std::optional<UInt32> ParseRelationshipNumber(std::string_view id)
{
    constexpr std::string_view prefix = "rId";
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix)
    {
        return std::nullopt;
    }

    UInt32 value = 0;
    for (char ch : id.substr(prefix.size()))
    {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
        {
            return std::nullopt;
        }

        const auto digit = static_cast<UInt32>(ch - '0');
        if (value > (std::numeric_limits<UInt32>::max() - digit) / 10)
        {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

} // namespace

OpenXmlPartContainer::OpenXmlPartContainer(OpenXmlPackage* package) noexcept
    : m_impl(std::make_unique<OpenXmlPartContainerImpl>())
{
    m_impl->package = package;
}

OpenXmlPartContainer::~OpenXmlPartContainer()
{
    // Surviving children (held elsewhere via shared_ptr) must not keep a dangling Parent()
    // pointer back to this container. Unlike ResetContainerState(), this path is guaranteed
    // not to throw, which is required in a destructor.
    for (auto& child : m_impl->childParts)
    {
        if (child)
        {
            child->DetachIncomingRelationshipsFrom(this);
        }
    }
}

const std::vector<std::shared_ptr<OpenXmlPackagePart>>& OpenXmlPartContainer::Parts() const noexcept
{
    return m_impl->childParts;
}

const std::vector<OpenXmlRelationship>& OpenXmlPartContainer::Relationships() const noexcept
{
    return m_impl->relationships;
}

std::string OpenXmlPartContainer::AddPartReference(const std::shared_ptr<OpenXmlPackagePart>& part,
                                                   std::string_view relationshipType)
{
    if (!part || relationshipType.empty() || !HasPackage() || part->Package() != Package() || part->Uri().empty())
    {
        return {};
    }

    const auto sourceUri = [this]()
    {
        if (const auto* sourcePart = dynamic_cast<const OpenXmlPackagePart*>(this))
        {
            return sourcePart->Uri();
        }
        return ContainerUri();
    }();
    for (const auto& incoming : part->IncomingRelationships())
    {
        if (incoming.SourceUri == sourceUri && incoming.Type == relationshipType)
        {
            return incoming.Id;
        }
    }

    auto& packageImpl = *Package()->m_impl;
    OpenXmlRelationship relationship;
    relationship.Id = AllocateRelationshipId();
    relationship.Type = std::string(relationshipType);
    relationship.Target = packageImpl.BuildRelationshipTarget(ContainerUri(), part->Uri());
    relationship.IsExternal = false;
    if (!AttachExistingChildPart(part, relationship.Id, relationship.Target, relationship.Type, false))
    {
        return {};
    }
    return relationship.Id;
}

bool OpenXmlPartContainer::AttachCustomPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                                            const OpenXmlPartDescriptor& descriptor,
                                            bool allowMultiple)
{
    return AttachChildPart(part, descriptor, allowMultiple);
}

bool OpenXmlPartContainer::RemovePartReference(const std::shared_ptr<OpenXmlPackagePart>& part)
{
    return DetachChildPart(part);
}

bool OpenXmlPartContainer::RetargetRelationship(std::string_view relationshipId,
                                                const std::shared_ptr<OpenXmlPackagePart>& newTarget)
{
    if (relationshipId.empty() || !newTarget || !HasPackage() || newTarget->Package() != Package() ||
        newTarget->Uri().empty())
    {
        return false;
    }

    const std::string id(relationshipId);
    const auto relationship = FindRelationship(id);
    if (!relationship || relationship->IsExternal)
    {
        return false;
    }

    auto* package = Package();
    auto& packageImpl = *package->m_impl;
    const auto sourceUri = [this]() -> std::string
    {
        if (const auto* sourcePart = dynamic_cast<const OpenXmlPackagePart*>(this))
        {
            return sourcePart->Uri();
        }
        return ContainerUri();
    }();

    const auto oldTargetUri = packageImpl.ResolveRelationshipTarget(sourceUri, relationship->Target);
    if (oldTargetUri == newTarget->Uri())
    {
        return true;
    }
    auto oldTarget = package->GetPartByUri(oldTargetUri);

    RemoveRelationship(id);
    if (oldTarget)
    {
        oldTarget->RemoveIncomingRelationship(this, id);
    }

    if (!AttachExistingChildPart(newTarget, id,
                                 packageImpl.BuildRelationshipTarget(ContainerUri(), newTarget->Uri()),
                                 relationship->Type, false, relationship->TargetMode))
    {
        // Restore the original edge so a failed retarget has no side effects.
        if (oldTarget)
        {
            AttachExistingChildPart(oldTarget, id, relationship->Target, relationship->Type, false,
                                    relationship->TargetMode);
        }
        else
        {
            RegisterRelationship(*relationship);
        }
        return false;
    }

    if (oldTarget)
    {
        // Drop the child membership when no other edge from this container
        // points at the previous target anymore.
        bool stillReferenced = false;
        for (const auto& incoming : oldTarget->IncomingRelationships())
        {
            if (incoming.SourceUri == sourceUri)
            {
                stillReferenced = true;
                break;
            }
        }
        if (!stillReferenced)
        {
            auto& children = m_impl->childParts;
            children.erase(std::remove(children.begin(), children.end(), oldTarget), children.end());
        }
    }

    packageImpl.RemoveUnreachableParts(*package);
    return true;
}

std::shared_ptr<OpenXmlPackagePart> OpenXmlPartContainer::ClonePartGraph(
    const std::shared_ptr<OpenXmlPackagePart>& source,
    const std::vector<std::string_view>& sharedRelationshipTypes)
{
    if (!source || !HasPackage() || source->Package() != Package())
    {
        return nullptr;
    }

    auto isShared = [&](std::string_view type)
    {
        return std::find(sharedRelationshipTypes.begin(), sharedRelationshipTypes.end(), type) !=
               sharedRelationshipTypes.end();
    };
    std::unordered_map<const OpenXmlPackagePart*, std::shared_ptr<OpenXmlPackagePart>> copies;
    std::shared_ptr<OpenXmlPackagePart> rootCopy;
    std::function<std::shared_ptr<OpenXmlPackagePart>(OpenXmlPartContainer&, const std::shared_ptr<OpenXmlPackagePart>&,
                                                      const OpenXmlRelationship*, bool)>
        clone;
    clone = [&](OpenXmlPartContainer& parent, const std::shared_ptr<OpenXmlPackagePart>& original,
                const OpenXmlRelationship* sourceEdge, bool share) -> std::shared_ptr<OpenXmlPackagePart>
    {
        if (share)
        {
            if (!sourceEdge || !parent.AttachExistingChildPart(original, sourceEdge->Id, sourceEdge->Target,
                                                               sourceEdge->Type, false, sourceEdge->TargetMode))
            {
                return nullptr;
            }
            return original;
        }
        if (auto found = copies.find(original.get()); found != copies.end())
        {
            if (!sourceEdge || !parent.AttachExistingChildPart(found->second, sourceEdge->Id, sourceEdge->Target,
                                                               sourceEdge->Type, false, sourceEdge->TargetMode))
            {
                return nullptr;
            }
            return found->second;
        }

        auto copy = Generated::CreatePackagePart(original->RelationshipType(), original->ContentType(), *Package());
        if (!copy)
        {
            copy = std::make_shared<OpaquePackagePart>(std::string(original->RelationshipType()),
                                                       std::string(original->ContentType()),
                                                       original->IsXmlPart() ? OpenXmlPartKind::Xml : OpenXmlPartKind::Binary);
        }
        if (!parent.AttachChildPart(copy, original->Descriptor(), true))
        {
            return nullptr;
        }
        if (!sourceEdge)
        {
            rootCopy = copy;
        }
        copies.emplace(original.get(), copy);
        if (original->IsXmlPart())
        {
            copy->SetXmlString(original->GetXmlString());
        }
        else
        {
            copy->SetBinaryData(original->GetBinaryData());
        }
        copy->SetContentType(std::string(original->ContentType()));

        if (sourceEdge)
        {
            const auto generatedId = copy->RelationshipId();
            const auto generatedEdge = parent.FindRelationship(generatedId);
            parent.RemoveRelationship(generatedId);
            copy->RemoveIncomingRelationship(&parent, generatedId);
            if (!generatedEdge)
            {
                return nullptr;
            }
            OpenXmlRelationship replacement = *sourceEdge;
            replacement.Target = generatedEdge->Target;
            copy->AddIncomingRelationship(&parent, replacement);
            parent.RegisterRelationship(std::move(replacement));
        }
        for (const auto& relationship : original->Relationships())
        {
            if (relationship.IsExternal)
            {
                copy->RegisterRelationship(relationship);
                continue;
            }
            std::shared_ptr<OpenXmlPackagePart> child;
            for (const auto& candidate : original->Parts())
            {
                for (const auto& incoming : candidate->IncomingRelationships())
                {
                    if (incoming.SourceUri == original->Uri() && incoming.Id == relationship.Id)
                    {
                        child = candidate;
                    }
                }
            }
            if (!child || !clone(*copy, child, &relationship, isShared(relationship.Type)))
            {
                return nullptr;
            }
        }
        return copy;
    };

    auto result = clone(*this, source, nullptr, false);
    if (!result && rootCopy)
    {
        DetachChildPart(rootCopy);
    }
    return result;
}

std::shared_ptr<OpenXmlPackagePart> OpenXmlPartContainer::ImportPartGraph(
    const std::shared_ptr<OpenXmlPackagePart>& source)
{
    if (!source || !HasPackage())
    {
        return nullptr;
    }

    std::unordered_map<const OpenXmlPackagePart*, std::shared_ptr<OpenXmlPackagePart>> copies;
    std::shared_ptr<OpenXmlPackagePart> rootCopy;
    std::function<std::shared_ptr<OpenXmlPackagePart>(OpenXmlPartContainer&,
                                                      const std::shared_ptr<OpenXmlPackagePart>&, const OpenXmlRelationship*)>
        import;
    import = [&](OpenXmlPartContainer& parent, const std::shared_ptr<OpenXmlPackagePart>& original,
                 const OpenXmlRelationship* sourceEdge) -> std::shared_ptr<OpenXmlPackagePart>
    {
        if (auto found = copies.find(original.get()); found != copies.end())
        {
            if (!sourceEdge)
            {
                return nullptr;
            }
            const auto generatedId = parent.AddPartReference(found->second, sourceEdge->Type);
            const auto generatedEdge = parent.FindRelationship(generatedId);
            if (generatedId.empty() || !generatedEdge)
            {
                return nullptr;
            }
            parent.RemoveRelationship(generatedId);
            found->second->RemoveIncomingRelationship(&parent, generatedId);
            OpenXmlRelationship replacement = *sourceEdge;
            replacement.Target = generatedEdge->Target;
            found->second->AddIncomingRelationship(&parent, replacement);
            parent.RegisterRelationship(std::move(replacement));
            return found->second;
        }

        auto copy = Generated::CreatePackagePart(original->RelationshipType(), original->ContentType(), *Package());
        if (!copy)
        {
            copy = std::make_shared<OpaquePackagePart>(std::string(original->RelationshipType()),
                                                       std::string(original->ContentType()),
                                                       original->IsXmlPart() ? OpenXmlPartKind::Xml : OpenXmlPartKind::Binary);
        }
        if (!parent.AttachChildPart(copy, original->Descriptor(), true))
        {
            return nullptr;
        }
        if (!sourceEdge)
        {
            rootCopy = copy;
        }
        copies.emplace(original.get(), copy);
        if (original->IsXmlPart())
        {
            copy->SetXmlString(original->GetXmlString());
        }
        else
        {
            copy->SetBinaryData(original->GetBinaryData());
        }
        copy->SetContentType(std::string(original->ContentType()));

        if (sourceEdge)
        {
            const auto generatedId = copy->RelationshipId();
            const auto generatedEdge = parent.FindRelationship(generatedId);
            parent.RemoveRelationship(generatedId);
            copy->RemoveIncomingRelationship(&parent, generatedId);
            if (!generatedEdge)
            {
                return nullptr;
            }
            OpenXmlRelationship replacement = *sourceEdge;
            replacement.Target = generatedEdge->Target;
            copy->AddIncomingRelationship(&parent, replacement);
            parent.RegisterRelationship(std::move(replacement));
        }

        for (const auto& relationship : original->Relationships())
        {
            if (relationship.IsExternal)
            {
                copy->RegisterRelationship(relationship);
                continue;
            }
            std::shared_ptr<OpenXmlPackagePart> child;
            for (const auto& candidate : original->Parts())
            {
                for (const auto& incoming : candidate->IncomingRelationships())
                {
                    if (incoming.SourceUri == original->Uri() && incoming.Id == relationship.Id)
                    {
                        child = candidate;
                        break;
                    }
                }
                if (child)
                {
                    break;
                }
            }
            if (!child || !import(*copy, child, &relationship))
            {
                return nullptr;
            }
        }
        return copy;
    };

    auto result = import(*this, source, nullptr);
    if (!result && rootCopy)
    {
        DetachChildPart(rootCopy);
    }
    return result;
}

std::vector<OpenXmlRelationship> OpenXmlPartContainer::RelationshipsByType(
    std::string_view relationshipType) const
{
    std::vector<OpenXmlRelationship> result;
    for (const auto& relationship : m_impl->relationships)
    {
        if (!relationship.IsExternal && relationship.Type == relationshipType)
        {
            result.push_back(relationship);
        }
    }
    return result;
}

std::string OpenXmlPartContainer::AddDataPartReferenceRelationship(std::string_view relationshipType,
                                                                   std::string targetUri)
{
    if (relationshipType.empty() || targetUri.empty())
    {
        return {};
    }
    OpenXmlRelationship relationship;
    relationship.Id = AllocateRelationshipId();
    relationship.Type = std::string(relationshipType);
    relationship.Target = std::move(targetUri);
    relationship.IsExternal = false;
    const auto id = relationship.Id;
    RegisterRelationship(std::move(relationship));
    return id;
}

bool OpenXmlPartContainer::RemoveDataPartReferenceRelationship(std::string_view relationshipType,
                                                               std::string_view relationshipId)
{
    const auto found = std::find_if(m_impl->relationships.begin(), m_impl->relationships.end(),
                                    [relationshipType, relationshipId](const OpenXmlRelationship& relationship)
                                    {
                                        return !relationship.IsExternal && relationship.Type == relationshipType && relationship.Id == relationshipId;
                                    });
    if (found == m_impl->relationships.end())
    {
        return false;
    }
    m_impl->relationships.erase(found);
    return true;
}

void OpenXmlPartContainer::SetPackage(OpenXmlPackage* package)
{
    m_impl->package = package;
}

void OpenXmlPartContainer::ResetContainerState()
{
    for (auto& child : m_impl->childParts)
    {
        if (child)
        {
            child->RemoveIncomingRelationshipsFrom(this);
        }
    }

    m_impl->childParts.clear();
    m_impl->relationships.clear();
    m_impl->nextRelationshipId = 1;
}

void OpenXmlPartContainer::ReleaseChildParts(const std::function<bool(const OpenXmlPackagePart&)>& shouldRelease)
{
    if (!shouldRelease)
    {
        return;
    }

    auto& children = m_impl->childParts;
    std::vector<std::shared_ptr<OpenXmlPackagePart>> kept;
    std::vector<std::shared_ptr<OpenXmlPackagePart>> released;
    kept.reserve(children.size());
    for (auto& child : children)
    {
        if (child && shouldRelease(*child))
        {
            released.push_back(std::move(child));
        }
        else
        {
            kept.push_back(std::move(child));
        }
    }

    children = std::move(kept);

    // The released children are still held here while their back references are
    // dropped, so a child that another owner keeps alive does not end up pointing
    // at a container it no longer belongs to.
    for (auto& child : released)
    {
        child->DetachIncomingRelationshipsFrom(this);
    }
}

bool OpenXmlPartContainer::HasPackage() const noexcept
{
    return m_impl->package != nullptr;
}

OpenXmlPackage* OpenXmlPartContainer::Package() noexcept
{
    return m_impl->package;
}

const OpenXmlPackage* OpenXmlPartContainer::Package() const noexcept
{
    return m_impl->package;
}

std::string OpenXmlPartContainer::AllocateRelationshipId()
{
    while (true)
    {
        auto candidate = "rId" + std::to_string(m_impl->nextRelationshipId++);
        if (!FindRelationship(candidate).has_value())
        {
            return candidate;
        }
    }
}

void OpenXmlPartContainer::RegisterRelationship(OpenXmlRelationship relationship)
{
    if (auto relationshipNumber = ParseRelationshipNumber(relationship.Id))
    {
        if (*relationshipNumber >= m_impl->nextRelationshipId)
        {
            m_impl->nextRelationshipId = *relationshipNumber + 1;
        }
    }
    m_impl->relationships.push_back(std::move(relationship));
}

void OpenXmlPartContainer::RemoveRelationship(const std::string& id)
{
    auto& relationships = m_impl->relationships;
    relationships.erase(std::remove_if(relationships.begin(),
                                       relationships.end(),
                                       [&id](const OpenXmlRelationship& rel)
                                       { return rel.Id == id; }),
                        relationships.end());
}

std::optional<OpenXmlRelationship> OpenXmlPartContainer::FindRelationship(const std::string& id) const
{
    const auto& relationships = m_impl->relationships;
    const auto it = std::find_if(relationships.begin(), relationships.end(), [&id](const OpenXmlRelationship& rel)
                                 { return rel.Id == id; });
    if (it == relationships.end())
    {
        return std::nullopt;
    }
    return *it;
}

void OpenXmlPartContainer::ForEachPart(
    const std::function<void(const std::shared_ptr<OpenXmlPackagePart>&)>& visitor) const
{
    if (!visitor)
    {
        return;
    }

    std::unordered_set<const OpenXmlPackagePart*> visited;
    std::function<void(const OpenXmlPartContainer&)> walk = [&](const OpenXmlPartContainer& container)
    {
        for (const auto& child : container.Parts())
        {
            if (!child)
            {
                continue;
            }
            if (!visited.insert(child.get()).second)
            {
                continue;
            }
            visitor(child);
            walk(*child);
        }
    };

    walk(*this);
}

std::string OpenXmlPartContainer::AddExternalRelationship(std::string_view relationshipType,
                                                          std::string targetUri)
{
    if (relationshipType.empty())
    {
        return {};
    }

    auto relationshipId = AllocateRelationshipId();
    OpenXmlRelationship relationship;
    relationship.Id = relationshipId;
    relationship.Type = std::string(relationshipType);
    relationship.Target = std::move(targetUri);
    relationship.TargetMode = "External";
    relationship.IsExternal = true;
    RegisterRelationship(std::move(relationship));
    return relationshipId;
}

void OpenXmlPartContainer::RemoveExternalRelationship(const std::string& relationshipId)
{
    RemoveRelationship(relationshipId);
}

bool OpenXmlPartContainer::AttachChildPart(const std::shared_ptr<OpenXmlPackagePart>& part,
                                           const OpenXmlPartDescriptor& descriptor,
                                           bool allowMultiple)
{
    if (!part)
    {
        return false;
    }

    if (!HasPackage())
    {
        return false;
    }

    auto* package = Package();
    auto& packageImpl = *package->m_impl;
    const auto uri = packageImpl.BuildUniquePartUri(*package, descriptor, *this, allowMultiple, part->ContentType());
    part->SetPackage(package);
    part->SetUri(uri);
    package->RegisterPart(part);

    const auto relationshipId = AllocateRelationshipId();
    m_impl->childParts.push_back(part);

    OpenXmlRelationship relationship;
    relationship.Id = relationshipId;
    relationship.Type = std::string(descriptor.RelationshipType);
    relationship.Target = packageImpl.BuildRelationshipTarget(ContainerUri(), part->Uri());
    relationship.IsExternal = false;
    part->AddIncomingRelationship(this, relationship);
    RegisterRelationship(std::move(relationship));

    return true;
}

bool OpenXmlPartContainer::DetachChildPart(const std::shared_ptr<OpenXmlPackagePart>& part)
{
    if (!part)
    {
        return false;
    }
    if (!HasPackage())
    {
        return false;
    }

    auto& children = m_impl->childParts;
    const auto initialSize = children.size();
    children.erase(std::remove_if(children.begin(),
                                  children.end(),
                                  [&](const auto& candidate)
                                  { return candidate == part; }),
                   children.end());

    if (children.size() == initialSize)
    {
        return false;
    }

    auto* package = Package();
    auto& packageImpl = *package->m_impl;
    auto& relationships = m_impl->relationships;
    const auto relationshipSourceUri = [this]() -> std::string
    {
        if (auto* sourcePart = dynamic_cast<const OpenXmlPackagePart*>(this))
        {
            return sourcePart->Uri();
        }
        return ContainerUri();
    }();
    std::vector<std::string> removedRelationshipIds;
    relationships.erase(std::remove_if(relationships.begin(),
                                       relationships.end(),
                                       [&](const OpenXmlRelationship& relationship)
                                       {
                                           if (relationship.IsExternal)
                                           {
                                               return false;
                                           }
                                           const bool pointsToPart =
                                               packageImpl.ResolveRelationshipTarget(relationshipSourceUri, relationship.Target) == part->Uri();
                                           if (pointsToPart)
                                           {
                                               removedRelationshipIds.push_back(relationship.Id);
                                           }
                                           return pointsToPart;
                                       }),
                        relationships.end());

    for (const auto& relationshipId : removedRelationshipIds)
    {
        part->RemoveIncomingRelationship(this, relationshipId);
    }

    packageImpl.RemoveUnreachableParts(*package);
    return true;
}

OpenXmlPackagePart::OpenXmlPackagePart(const OpenXmlPartDescriptor& descriptor, OpenXmlPackage* package)
    : OpenXmlPartContainer(package), m_impl(std::make_unique<OpenXmlPackagePartImpl>(descriptor))
{
}

OpenXmlPackagePart::~OpenXmlPackagePart() = default;

OpaquePackagePart::OpaquePackagePart(std::string relationshipType,
                                     std::string contentType,
                                     OpenXmlPartKind kind,
                                     OpenXmlPackage* package)
    : OpenXmlPackagePart(OpenXmlPartDescriptor{"OpaquePackagePart",
                                               relationshipType,
                                               contentType,
                                               "part",
                                               kind == OpenXmlPartKind::Xml ? ".xml" : ".bin",
                                               ".",
                                               kind,
                                               OpenXml::FileFormatVersions::Office2007},
                         package)
{
}

const OpenXmlPartDescriptor& OpenXmlPackagePart::Descriptor() const noexcept
{
    return m_impl->descriptor;
}

const std::string& OpenXmlPackagePart::Uri() const noexcept
{
    return m_impl->uri;
}

std::string_view OpenXmlPackagePart::RelationshipType() const noexcept
{
    return m_impl->descriptor.RelationshipType;
}

std::string_view OpenXmlPackagePart::ContentType() const noexcept
{
    if (!m_impl->contentTypeOverride.empty())
    {
        return m_impl->contentTypeOverride;
    }
    return m_impl->descriptor.ContentType;
}

const std::string& OpenXmlPackagePart::RelationshipId() const noexcept
{
    return m_impl->relationshipId;
}

const std::vector<OpenXmlIncomingRelationship>& OpenXmlPackagePart::IncomingRelationships() const noexcept
{
    return m_impl->incomingRelationshipView;
}

bool OpenXmlPackagePart::IsXmlPart() const noexcept
{
    return m_impl->descriptor.Kind == OpenXmlPartKind::Xml;
}

bool OpenXmlPackagePart::IsBinaryPart() const noexcept
{
    return m_impl->descriptor.Kind == OpenXmlPartKind::Binary;
}

/// True for parts whose XML is a byte stream first and a tree second.
///
/// An XML digital signature signs the characters of the signature part, not the
/// element tree behind them: canonicalization keeps every space between two
/// attributes and every newline between two elements. Re-serializing such a
/// part - which is what the writer does with every other XML part, complete
/// with indentation and a declaration of its own - therefore replaces the very
/// bytes the SignedInfo digest was taken over, and no conforming verifier can
/// check it afterwards. Only the signature part itself qualifies; the origin
/// and certificate parts are not XML.
static bool RequiresByteStableXml(std::string_view contentType) noexcept
{
    return contentType == Security::SignatureNames::SignatureContentType;
}

std::string OpenXmlPackagePart::GetXmlString() const
{
    if (!IsXmlPart())
    {
        return {};
    }

    // Byte-stable parts answer with the characters they were given. Serializing
    // the tree would be a different document as far as a signature is concerned,
    // and this is the accessor the package writer and the verifier both read.
    if (m_impl->hasVerbatimXml)
    {
        return m_impl->verbatimXml;
    }

    if (!m_impl->xmlData)
    {
        return {};
    }

    std::ostringstream stream;
    m_impl->xmlData->Document.save(stream);
    return stream.str();
}

void OpenXmlPackagePart::SetXmlString(const std::string& xml)
{
    if (!IsXmlPart())
    {
        return;
    }

    if (!m_impl->xmlData)
    {
        m_impl->xmlData = std::make_unique<XmlDocumentStorage>();
    }

    m_impl->xmlData->Document.reset();

    // load_buffer, not load_string: c_str() would end the document at the first
    // embedded NUL, so a part carrying one - a `&#0;` reference is enough -
    // would be silently truncated instead of parsed or rejected. This also
    // matches how the package loader reads parts, which lets load_buffer detect
    // the encoding from the BOM or the XML declaration. The part parse options
    // keep a whitespace-only text node such as the space in
    // `<w:t xml:space="preserve"> </w:t>`, which saving would otherwise drop.
    m_impl->xmlData->Document.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving);

    // Kept for the parts a signature covers character by character. This is the
    // only way content enters such a part - both the signature writer and a
    // caller tampering with one go through here - so the copy cannot go stale
    // behind an edit made somewhere else.
    m_impl->hasVerbatimXml = RequiresByteStableXml(ContentType());
    m_impl->verbatimXml = m_impl->hasVerbatimXml ? xml : std::string();
}

void OpenXmlPackagePart::SetContentType(std::string contentType)
{
    m_impl->contentTypeOverride = std::move(contentType);
}

std::vector<Byte> OpenXmlPackagePart::GetBinaryData() const
{
    return m_impl->binaryData;
}

void OpenXmlPackagePart::SetBinaryData(std::vector<Byte> data)
{
    m_impl->binaryData = std::move(data);
}

bool OpenXmlPackagePart::HasOriginalBytes() const noexcept
{
    return m_impl->hasOriginalBytes;
}

std::vector<Byte> OpenXmlPackagePart::GetOriginalBytes() const
{
    return m_impl->originalBytes;
}

void OpenXmlPackagePart::SetOriginalBytes(std::vector<Byte> data)
{
    m_impl->originalBytes = std::move(data);
    m_impl->hasOriginalBytes = true;
}

std::shared_ptr<OpenXMLElement> OpenXmlPackagePart::GetRootElement() const
{
    if (!IsXmlPart() || !m_impl->xmlData)
    {
        return nullptr;
    }

    auto root = m_impl->xmlData->Document.document_element();
    if (!root)
    {
        return nullptr;
    }

    return Detail::CreateOpenXmlElementFromNode(root);
}

std::filesystem::path OpenXmlPackagePart::ContainerPath() const
{
    return std::filesystem::path(Detail::PartUriDirectory(m_impl->uri));
}

std::string OpenXmlPackagePart::ContainerUri() const
{
    return Detail::PartUriDirectory(m_impl->uri);
}

void OpenXmlPackagePart::SetParent(OpenXmlPartContainer* parent, std::string relationshipId)
{
    m_impl->parent = parent;
    m_impl->relationshipId = std::move(relationshipId);

    if (parent != nullptr && !HasPackage())
    {
        SetPackage(parent->Package());
    }
}

void OpenXmlPackagePart::AddIncomingRelationship(OpenXmlPartContainer* source,
                                                 const OpenXmlRelationship& relationship)
{
    if (!source)
    {
        return;
    }

    OpenXmlIncomingRelationship incoming;
    if (auto* sourcePart = dynamic_cast<OpenXmlPackagePart*>(source))
    {
        incoming.SourceUri = sourcePart->Uri();
    }
    else
    {
        incoming.SourceUri = source->ContainerUri();
    }
    incoming.Id = relationship.Id;
    incoming.Type = relationship.Type;
    incoming.Target = relationship.Target;
    incoming.TargetMode = relationship.TargetMode;
    incoming.IsExternal = relationship.IsExternal;
    m_impl->incomingRelationships.push_back(OpenXmlIncomingRelationshipState{source, std::move(incoming)});
    RefreshPrimaryRelationship();

    if (!HasPackage())
    {
        SetPackage(source->Package());
    }
}

void OpenXmlPackagePart::RemoveIncomingRelationshipsFrom(const OpenXmlPartContainer* source)
{
    if (!source)
    {
        return;
    }

    auto& incoming = m_impl->incomingRelationships;
    incoming.erase(std::remove_if(incoming.begin(),
                                  incoming.end(),
                                  [source](const OpenXmlIncomingRelationshipState& edge)
                                  {
                                      return edge.source == source;
                                  }),
                   incoming.end());
    RefreshPrimaryRelationship();
}

void OpenXmlPackagePart::DetachIncomingRelationshipsFrom(const OpenXmlPartContainer* source) noexcept
{
    if (!source)
    {
        return;
    }

    auto& incoming = m_impl->incomingRelationships;
    incoming.erase(std::remove_if(incoming.begin(),
                                  incoming.end(),
                                  [source](const OpenXmlIncomingRelationshipState& edge)
                                  {
                                      return edge.source == source;
                                  }),
                   incoming.end());

    if (m_impl->parent == source)
    {
        m_impl->parent = nullptr;
        m_impl->relationshipId.clear();
    }
}

void OpenXmlPackagePart::RemoveIncomingRelationship(const OpenXmlPartContainer* source,
                                                    std::string_view relationshipId)
{
    if (!source)
    {
        return;
    }

    auto& incoming = m_impl->incomingRelationships;
    incoming.erase(std::remove_if(incoming.begin(),
                                  incoming.end(),
                                  [source, relationshipId](const OpenXmlIncomingRelationshipState& edge)
                                  {
                                      return edge.source == source && edge.relationship.Id == relationshipId;
                                  }),
                   incoming.end());
    RefreshPrimaryRelationship();
}

void OpenXmlPackagePart::ClearIncomingRelationships()
{
    m_impl->incomingRelationships.clear();
    RefreshPrimaryRelationship();
}

void OpenXmlPackagePart::RefreshPrimaryRelationship()
{
    m_impl->incomingRelationshipView.clear();
    m_impl->incomingRelationshipView.reserve(m_impl->incomingRelationships.size());
    for (const auto& incoming : m_impl->incomingRelationships)
    {
        m_impl->incomingRelationshipView.push_back(incoming.relationship);
    }

    if (m_impl->incomingRelationships.size() == 1)
    {
        m_impl->parent = m_impl->incomingRelationships.front().source;
        m_impl->relationshipId = m_impl->incomingRelationships.front().relationship.Id;
        return;
    }

    m_impl->parent = nullptr;
    m_impl->relationshipId.clear();
}

void OpenXmlPackagePart::SetUri(std::string uri)
{
    m_impl->uri = std::move(uri);
    if (IsXmlPart() && !m_impl->xmlData)
    {
        m_impl->xmlData = std::make_unique<XmlDocumentStorage>();
    }
}

void OpenXmlPackagePart::InitializeRootElement(const OpenXMLElementClass* metaClass)
{
    if (!IsXmlPart() || !metaClass)
    {
        return;
    }

    if (!m_impl->xmlData)
    {
        m_impl->xmlData = std::make_unique<XmlDocumentStorage>();
    }

    auto& doc = m_impl->xmlData->Document;
    if (doc.document_element())
    {
        return;
    }

    const auto qualifiedName = metaClass->QualifiedName();
    if (qualifiedName.localName().empty())
    {
        return;
    }

    std::string localName(qualifiedName.localName());
    auto root = doc.append_child(localName.c_str());
    if (!root)
    {
        return;
    }

    if (!qualifiedName.namespaceUri().empty())
    {
        auto suggested =
            ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(qualifiedName.namespaceUri());
        const std::optional<std::string_view> suggestedPrefix =
            suggested ? std::optional<std::string_view>(*suggested) : std::nullopt;
        const auto prefix =
            Xml::NamespaceResolver::EnsurePrefix(root, qualifiedName.namespaceUri(), suggestedPrefix);
        if (!prefix.empty())
        {
            std::string fullName;
            fullName.reserve(prefix.size() + 1 + localName.size());
            fullName.append(prefix);
            fullName.push_back(':');
            fullName.append(localName);
            root.set_name(fullName.c_str());
        }
    }
}

void OpenXmlPackagePart::InitializeRootElement(OpenXmlQualifiedName qualifiedName)
{
    if (!IsXmlPart() || qualifiedName.localName().empty())
    {
        return;
    }
    if (!m_impl->xmlData)
    {
        m_impl->xmlData = std::make_unique<XmlDocumentStorage>();
    }
    auto& doc = m_impl->xmlData->Document;
    if (doc.document_element())
    {
        return;
    }

    std::string localName(qualifiedName.localName());
    auto root = doc.append_child(localName.c_str());
    if (!qualifiedName.namespaceUri().empty())
    {
        const auto suggested =
            ExyokiOffice::OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(qualifiedName.namespaceUri());
        const std::optional<std::string_view> suggestedPrefix =
            suggested ? std::optional<std::string_view>(*suggested) : std::nullopt;
        const auto prefix = Xml::NamespaceResolver::EnsurePrefix(root, qualifiedName.namespaceUri(), suggestedPrefix);
        if (!prefix.empty())
        {
            root.set_name((prefix + ':' + localName).c_str());
        }
    }
}

} // namespace ExyokiOffice
