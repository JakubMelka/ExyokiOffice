// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Security
{

/// One Transform element of a Reference, kept with its node so the
/// relationship transform can read its selection children.
struct SignatureTransformInfo
{
    std::string Algorithm;
    Pugi::xml_node Node;
};

/// One Reference element, either from SignedInfo or from a Manifest.
struct SignatureReferenceInfo
{
    std::string Uri;
    std::string Type;
    std::string DigestAlgorithmUri;
    std::vector<Byte> DigestValue;
    std::vector<SignatureTransformInfo> Transforms;
};

/// The contents of one XML signature, as far as a package signature uses them.
///
/// The nodes point into the document the caller parsed, so that document has to
/// stay alive for as long as the parsed signature is used.
struct ParsedSignature
{
    Pugi::xml_node SignatureNode;
    Pugi::xml_node SignedInfoNode;
    std::string Id;
    std::string CanonicalizationAlgorithmUri;
    std::string SignatureAlgorithmUri;
    /// References that cover the Object elements of this signature.
    std::vector<SignatureReferenceInfo> SignedInfoReferences;
    /// References that cover package parts, taken from the Manifest.
    std::vector<SignatureReferenceInfo> ManifestReferences;
    std::vector<Byte> SignatureValue;
    std::vector<std::vector<Byte>> Certificates;
    std::string SigningTimeText;
    /// Elements of the signature by their Id attribute, used to resolve
    /// same-document references.
    ///
    /// Not only the Object elements: a bare-name reference points at whatever
    /// element carries the id, and Office signs its XAdES SignedProperties that
    /// way - `#idSignedProperties` names an element nested inside an Object, not
    /// the Object itself.
    std::map<std::string, Pugi::xml_node> ElementsById;
};

/// \brief Reading and writing the XML Signature vocabulary with pugixml.
///
/// There is no generated DOM for xmldsig, so signature parts are handled as
/// plain XML. Elements are matched by namespace and local name rather than by
/// qualified name, because the prefixes are up to whoever wrote the signature.
/// \note The class is exported so the unit tests can use it directly. The header
///       is internal and not installed.
class EXYOKIOFFICE_EXPORT SignatureXml final
{
public:
    SignatureXml() = delete;

    /// Returns true when \p node is an element with the given namespace and local name.
    static bool IsElement(const Pugi::xml_node& node, std::string_view namespaceUri, std::string_view localName);

    /// Returns the first matching child element, or an empty node.
    static Pugi::xml_node FindChild(const Pugi::xml_node& parent,
                                    std::string_view namespaceUri,
                                    std::string_view localName);

    /// Returns every matching child element.
    static std::vector<Pugi::xml_node> FindChildren(const Pugi::xml_node& parent,
                                                    std::string_view namespaceUri,
                                                    std::string_view localName);

    /// Returns the first matching descendant element, or an empty node.
    static Pugi::xml_node FindDescendant(const Pugi::xml_node& parent,
                                         std::string_view namespaceUri,
                                         std::string_view localName);

    /// Concatenates the text children of \p node.
    static std::string TextOf(const Pugi::xml_node& node);

    /// Decodes base64 text, ignoring the line breaks signatures usually contain.
    static bool DecodeBase64(std::string_view text, std::vector<Byte>& value);

    /// Encodes bytes as base64 on a single line.
    static std::string EncodeBase64(const std::vector<Byte>& value);

    /// Reads a Signature element. Returns false with a reason when it is not
    /// shaped like an XML signature at all.
    static bool Parse(const Pugi::xml_node& signatureNode, ParsedSignature& parsed, std::string& error);

    /// Parses the value of mdssi:SignatureTime.
    static std::optional<std::chrono::system_clock::time_point> ParseSigningTime(std::string_view text);

    /// Formats a signing time the way mdssi:SignatureTime expects it.
    static std::string FormatSigningTime(std::chrono::system_clock::time_point time);
};

} // namespace ExyokiOffice::Security
