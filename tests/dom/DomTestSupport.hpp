// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Shared helpers for the generated-DOM unit tests.
//
// Generated DOM elements are node-views, not value objects: a bare
// `std::make_shared<T>()` has no backing XML node, so `Set*()`/`Get*()` are
// no-ops until the element is hosted inside a live package part. These helpers
// build a minimal in-memory OPC package around a single element, graft the live
// node onto a correctly typed wrapper, and round-trip it through real XML
// serialization so the tests exercise the whole persistence path.
//
// This header is deliberately doctest-aware (it uses REQUIRE inside the zip
// helpers); every test translation unit already includes doctest first.
// ---------------------------------------------------------------------------

#pragma once

#include "doctest.h"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DomTest
{

inline void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

inline std::vector<ExyokiOffice::Byte> FinishZip(zip_t* archive)
{
    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

inline std::vector<ExyokiOffice::Byte> BuildSingleXmlPartPackage(std::string_view xml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);
    return FinishZip(archive);
}

// The element wrappers handed out below are lightweight views over pugixml
// nodes owned by their package. Keeping every package alive for the whole test
// run means those views never dangle.
inline std::vector<std::shared_ptr<ExyokiOffice::OpenXmlPackage>>& LivePackages()
{
    static std::vector<std::shared_ptr<ExyokiOffice::OpenXmlPackage>> packages;
    return packages;
}

/// An element hosted on a live node, together with the part that serializes it.
struct HostedRoot
{
    std::shared_ptr<ExyokiOffice::OpenXMLElement> root;
    std::shared_ptr<ExyokiOffice::OpenXmlPackagePart> part;
};

/// Loads a single-part package whose part body is @p xml. Returns false instead
/// of asserting so callers that sweep the whole element corpus stay resilient.
inline bool TryHostRootXml(std::string_view xml, HostedRoot& out)
{
    auto package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
    if (!package->LoadFromMemory(BuildSingleXmlPartPackage(xml)))
    {
        return false;
    }

    auto part = package->GetPartByUri("/custom.xml");
    if (!part)
    {
        return false;
    }

    auto root = part->GetRootElement();
    if (!root)
    {
        return false;
    }

    LivePackages().push_back(std::move(package));
    out = HostedRoot{std::move(root), std::move(part)};
    return true;
}

/// Builds a minimal document whose single root element carries @p name.
inline std::string MinimalDocumentXml(const ExyokiOffice::OpenXmlQualifiedName& name)
{
    const std::string localName(name.localName());
    const std::string namespaceUri(name.namespaceUri());

    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    if (namespaceUri.empty())
    {
        xml += "<" + localName + "/>";
    }
    else
    {
        xml += "<n:" + localName + " xmlns:n=\"" + namespaceUri + "\"/>";
    }
    return xml;
}

/// Hosts the element described by @p metaClass on a live node (untyped root).
inline bool TryHostMeta(const ExyokiOffice::OpenXMLElementClass* metaClass, HostedRoot& out)
{
    if (!metaClass)
    {
        return false;
    }
    return TryHostRootXml(MinimalDocumentXml(metaClass->QualifiedName()), out);
}

/// A live, strongly typed element plus the part that can serialize it.
template <typename T>
struct Hosted
{
    std::shared_ptr<T> element;
    std::shared_ptr<ExyokiOffice::OpenXmlPackagePart> part;

    /// Serializes the hosting part, reloads it from scratch, and returns a fresh
    /// live T over the reloaded node - a full XML persistence round-trip.
    Hosted<T> RoundTrip() const;
};

/// Loads @p xml and grafts the live root node onto a fresh, correctly typed T.
/// Grafting sidesteps element-name ambiguity in the factory: T is exactly the
/// type under test, regardless of what the element name alone resolves to.
template <typename T>
inline Hosted<T> HostFromXml(std::string_view xml)
{
    HostedRoot hosted;
    REQUIRE(TryHostRootXml(xml, hosted));

    auto element = std::make_shared<T>();
    element->SetNodeHandle(hosted.root->GetNodeHandle());
    return Hosted<T>{std::move(element), std::move(hosted.part)};
}

/// Builds a minimal document whose root element is T and returns it hosted live.
template <typename T>
inline Hosted<T> HostElement()
{
    return HostFromXml<T>(MinimalDocumentXml(T::StaticMetaClass()->QualifiedName()));
}

template <typename T>
inline Hosted<T> Hosted<T>::RoundTrip() const
{
    return HostFromXml<T>(part->GetXmlString());
}

} // namespace DomTest
