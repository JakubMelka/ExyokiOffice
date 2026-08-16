// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "SignatureXml.hpp"

#include "SignatureNames.hpp"
#include "XmlCanonicalization.hpp"
#include "XmlNamespaceResolver.hpp"

#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <utility>

namespace ExyokiOffice::Security
{

/// Element and attribute names of the XML Signature vocabulary.
class SignatureXmlNames final
{
public:
    SignatureXmlNames() = delete;

    static constexpr const char* SignatureElement = "Signature";
    static constexpr const char* SignedInfoElement = "SignedInfo";
    static constexpr const char* CanonicalizationMethodElement = "CanonicalizationMethod";
    static constexpr const char* SignatureMethodElement = "SignatureMethod";
    static constexpr const char* ReferenceElement = "Reference";
    static constexpr const char* TransformsElement = "Transforms";
    static constexpr const char* TransformElement = "Transform";
    static constexpr const char* DigestMethodElement = "DigestMethod";
    static constexpr const char* DigestValueElement = "DigestValue";
    static constexpr const char* SignatureValueElement = "SignatureValue";
    static constexpr const char* KeyInfoElement = "KeyInfo";
    static constexpr const char* X509DataElement = "X509Data";
    static constexpr const char* X509CertificateElement = "X509Certificate";
    static constexpr const char* ObjectElement = "Object";
    static constexpr const char* ManifestElement = "Manifest";
    static constexpr const char* SignaturePropertiesElement = "SignatureProperties";
    static constexpr const char* SignaturePropertyElement = "SignatureProperty";
    static constexpr const char* SignatureTimeElement = "SignatureTime";
    static constexpr const char* ValueElement = "Value";

    static constexpr const char* AlgorithmAttribute = "Algorithm";
    static constexpr const char* UriAttribute = "URI";
    static constexpr const char* TypeAttribute = "Type";
    static constexpr const char* IdAttribute = "Id";

    /// Returns the part of a qualified name after the prefix.
    static std::string_view LocalNameOf(std::string_view qualifiedName) noexcept
    {
        const auto colon = qualifiedName.find(':');
        return colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
    }

    /// Returns the prefix of a qualified name, empty when there is none.
    static std::string_view PrefixOf(std::string_view qualifiedName) noexcept
    {
        const auto colon = qualifiedName.find(':');
        return colon == std::string_view::npos ? std::string_view{} : qualifiedName.substr(0, colon);
    }
};

bool SignatureXml::IsElement(const Pugi::xml_node& node, std::string_view namespaceUri, std::string_view localName)
{
    if (!node || node.type() != Pugi::node_element)
    {
        return false;
    }

    const std::string_view name = node.name();
    if (SignatureXmlNames::LocalNameOf(name) != localName)
    {
        return false;
    }

    const auto prefix = SignatureXmlNames::PrefixOf(name);
    const auto uri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
    return uri.has_value() && *uri == namespaceUri;
}

Pugi::xml_node SignatureXml::FindChild(const Pugi::xml_node& parent,
                                       std::string_view namespaceUri,
                                       std::string_view localName)
{
    for (const auto& child : parent.children())
    {
        if (IsElement(child, namespaceUri, localName))
        {
            return child;
        }
    }
    return {};
}

std::vector<Pugi::xml_node> SignatureXml::FindChildren(const Pugi::xml_node& parent,
                                                       std::string_view namespaceUri,
                                                       std::string_view localName)
{
    std::vector<Pugi::xml_node> result;
    for (const auto& child : parent.children())
    {
        if (IsElement(child, namespaceUri, localName))
        {
            result.push_back(child);
        }
    }
    return result;
}

Pugi::xml_node SignatureXml::FindDescendant(const Pugi::xml_node& parent,
                                            std::string_view namespaceUri,
                                            std::string_view localName)
{
    for (const auto& child : parent.children())
    {
        if (IsElement(child, namespaceUri, localName))
        {
            return child;
        }
        if (auto found = FindDescendant(child, namespaceUri, localName))
        {
            return found;
        }
    }
    return {};
}

std::string SignatureXml::TextOf(const Pugi::xml_node& node)
{
    std::string text;
    for (const auto& child : node.children())
    {
        if (child.type() == Pugi::node_pcdata || child.type() == Pugi::node_cdata)
        {
            text.append(child.value());
        }
    }
    return text;
}

bool SignatureXml::DecodeBase64(std::string_view text, std::vector<Byte>& value)
{
    return ExyokiOffice::detail::Base64BinaryTraits::TryParse(text, value);
}

std::string SignatureXml::EncodeBase64(const std::vector<Byte>& value)
{
    return ExyokiOffice::detail::Base64BinaryTraits::Format(value);
}

std::optional<std::chrono::system_clock::time_point> SignatureXml::ParseSigningTime(std::string_view text)
{
    std::chrono::system_clock::time_point value{};
    if (!ExyokiOffice::detail::DateTimeValueTraits::TryParse(text, value))
    {
        return std::nullopt;
    }
    return value;
}

std::string SignatureXml::FormatSigningTime(std::chrono::system_clock::time_point time)
{
    // Signatures are written with whole seconds, as Office does.
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
    return ExyokiOffice::detail::DateTimeValueTraits::Format(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(seconds));
}

/// Reads the Reference elements below \p parent.
class SignatureReferenceReader final
{
public:
    SignatureReferenceReader() = delete;

    static std::vector<SignatureReferenceInfo> ReadReferences(const Pugi::xml_node& parent)
    {
        std::vector<SignatureReferenceInfo> references;
        for (const auto& node :
             SignatureXml::FindChildren(parent, SignatureNames::DsigNamespace, SignatureXmlNames::ReferenceElement))
        {
            references.push_back(ReadReference(node));
        }
        return references;
    }

private:
    static SignatureReferenceInfo ReadReference(const Pugi::xml_node& node)
    {
        SignatureReferenceInfo reference;
        reference.Uri = node.attribute(SignatureXmlNames::UriAttribute).as_string();
        reference.Type = node.attribute(SignatureXmlNames::TypeAttribute).as_string();

        const auto transforms =
            SignatureXml::FindChild(node, SignatureNames::DsigNamespace, SignatureXmlNames::TransformsElement);
        for (const auto& transform : SignatureXml::FindChildren(transforms,
                                                                SignatureNames::DsigNamespace,
                                                                SignatureXmlNames::TransformElement))
        {
            SignatureTransformInfo info;
            info.Algorithm = transform.attribute(SignatureXmlNames::AlgorithmAttribute).as_string();
            info.Node = transform;
            reference.Transforms.push_back(std::move(info));
        }

        const auto digestMethod =
            SignatureXml::FindChild(node, SignatureNames::DsigNamespace, SignatureXmlNames::DigestMethodElement);
        reference.DigestAlgorithmUri = digestMethod.attribute(SignatureXmlNames::AlgorithmAttribute).as_string();

        const auto digestValue =
            SignatureXml::FindChild(node, SignatureNames::DsigNamespace, SignatureXmlNames::DigestValueElement);
        SignatureXml::DecodeBase64(SignatureXml::TextOf(digestValue), reference.DigestValue);

        return reference;
    }
};

/// Builds the id index a same-document reference is resolved through.
class SignatureElementIndex final
{
public:
    SignatureElementIndex() = delete;

    /// Records the given element and every descendant that carries an Id.
    ///
    /// A bare-name reference points at whatever element carries the id, not at
    /// an Object: Office signs its XAdES SignedProperties as `#idSignedProperties`,
    /// which names an element nested two levels inside an Object.
    ///
    /// The root Signature element is indexed together with its descendants. Its
    /// Id is an id like any other, so leaving it out would let `<Signature
    /// Id="x">` coexist with a nested `<Object Id="x">`: this library would
    /// silently resolve `#x` to the Object while another verifier could resolve
    /// it to the root, which is exactly the ambiguity the uniqueness check
    /// exists to reject.
    ///
    /// \return False when an id occurs more than once. Which of the two a
    ///         verifier picks would then decide what is checked, and nothing in
    ///         the document says which one another implementation would pick, so
    ///         a repeated id fails the signature instead of resolving to one of
    ///         them.
    static bool Build(const Pugi::xml_node& node, std::map<std::string, Pugi::xml_node>& elements)
    {
        bool unique = true;
        const std::string id = node.attribute(SignatureXmlNames::IdAttribute).as_string();
        if (!id.empty() && !elements.emplace(id, node).second)
        {
            unique = false;
        }
        Descend(node, elements, 0, unique);
        return unique;
    }

private:
    /// Bounded for the same reason XmlCanonicalization::MaximumDepth is: the
    /// signature part is parsed by the verifier itself, so its nesting is not
    /// behind OpenXmlPackageLimits::MaxXmlDepth, and it comes from whoever
    /// supplied the package. Elements below the limit are simply not indexed,
    /// which makes a reference into them unresolvable rather than fatal.
    static void Descend(const Pugi::xml_node& node,
                        std::map<std::string, Pugi::xml_node>& elements,
                        unsigned int depth,
                        bool& unique)
    {
        if (depth >= XmlCanonicalization::MaximumDepth)
        {
            return;
        }
        for (auto child = node.first_child(); child; child = child.next_sibling())
        {
            if (child.type() != Pugi::node_element)
            {
                continue;
            }
            const std::string id = child.attribute(SignatureXmlNames::IdAttribute).as_string();
            if (!id.empty() && !elements.emplace(id, child).second)
            {
                unique = false;
            }
            Descend(child, elements, depth + 1, unique);
        }
    }
};

bool SignatureXml::Parse(const Pugi::xml_node& signatureNode, ParsedSignature& parsed, std::string& error)
{
    parsed = ParsedSignature{};

    if (!IsElement(signatureNode, SignatureNames::DsigNamespace, SignatureXmlNames::SignatureElement))
    {
        error = "The part does not contain an XML signature.";
        return false;
    }

    parsed.SignatureNode = signatureNode;
    parsed.Id = signatureNode.attribute(SignatureXmlNames::IdAttribute).as_string();

    parsed.SignedInfoNode =
        FindChild(signatureNode, SignatureNames::DsigNamespace, SignatureXmlNames::SignedInfoElement);
    if (!parsed.SignedInfoNode)
    {
        error = "The signature has no SignedInfo element.";
        return false;
    }

    const auto canonicalization = FindChild(parsed.SignedInfoNode,
                                            SignatureNames::DsigNamespace,
                                            SignatureXmlNames::CanonicalizationMethodElement);
    parsed.CanonicalizationAlgorithmUri = canonicalization.attribute(SignatureXmlNames::AlgorithmAttribute).as_string();

    const auto signatureMethod =
        FindChild(parsed.SignedInfoNode, SignatureNames::DsigNamespace, SignatureXmlNames::SignatureMethodElement);
    parsed.SignatureAlgorithmUri = signatureMethod.attribute(SignatureXmlNames::AlgorithmAttribute).as_string();

    parsed.SignedInfoReferences = SignatureReferenceReader::ReadReferences(parsed.SignedInfoNode);
    if (parsed.SignedInfoReferences.empty())
    {
        error = "The signature has no references in SignedInfo.";
        return false;
    }

    const auto signatureValue =
        FindChild(signatureNode, SignatureNames::DsigNamespace, SignatureXmlNames::SignatureValueElement);
    if (!DecodeBase64(TextOf(signatureValue), parsed.SignatureValue))
    {
        error = "The signature value is not valid base64.";
        return false;
    }

    const auto keyInfo = FindChild(signatureNode, SignatureNames::DsigNamespace, SignatureXmlNames::KeyInfoElement);
    for (const auto& x509Data :
         FindChildren(keyInfo, SignatureNames::DsigNamespace, SignatureXmlNames::X509DataElement))
    {
        for (const auto& certificate :
             FindChildren(x509Data, SignatureNames::DsigNamespace, SignatureXmlNames::X509CertificateElement))
        {
            std::vector<Byte> der;
            if (DecodeBase64(TextOf(certificate), der) && !der.empty())
            {
                parsed.Certificates.push_back(std::move(der));
            }
        }
    }

    if (!SignatureElementIndex::Build(signatureNode, parsed.ElementsById))
    {
        error = "The signature contains more than one element with the same Id.";
        return false;
    }

    for (const auto& object :
         FindChildren(signatureNode, SignatureNames::DsigNamespace, SignatureXmlNames::ObjectElement))
    {
        if (parsed.SigningTimeText.empty())
        {
            const auto signatureTime =
                FindDescendant(object, SignatureNames::PackageDigitalSignatureNamespace,
                               SignatureXmlNames::SignatureTimeElement);
            if (signatureTime)
            {
                const auto value = FindChild(signatureTime,
                                             SignatureNames::PackageDigitalSignatureNamespace,
                                             SignatureXmlNames::ValueElement);
                parsed.SigningTimeText = TextOf(value);
            }
        }
    }

    return true;
}

std::vector<SignatureReferenceInfo> SignatureXml::ReadManifestReferences(const Pugi::xml_node& element)
{
    std::vector<SignatureReferenceInfo> references;
    if (!element)
    {
        return references;
    }

    const auto append = [&references](const Pugi::xml_node& manifest)
    {
        auto read = SignatureReferenceReader::ReadReferences(manifest);
        references.insert(references.end(),
                          std::make_move_iterator(read.begin()),
                          std::make_move_iterator(read.end()));
    };

    if (IsElement(element, SignatureNames::DsigNamespace, SignatureXmlNames::ManifestElement))
    {
        append(element);
        return references;
    }

    // Depth-bounded and iterative for the same reason the id index is: this runs
    // on a signature part supplied with the package, ahead of any depth limit.
    std::vector<std::pair<Pugi::xml_node, unsigned int>> pending;
    for (auto child = element.last_child(); child; child = child.previous_sibling())
    {
        pending.emplace_back(child, 1U);
    }

    while (!pending.empty())
    {
        const auto [node, depth] = pending.back();
        pending.pop_back();

        if (node.type() != Pugi::node_element || depth >= XmlCanonicalization::MaximumDepth)
        {
            continue;
        }

        if (IsElement(node, SignatureNames::DsigNamespace, SignatureXmlNames::ManifestElement))
        {
            // A Manifest holds References, not further Manifests.
            append(node);
            continue;
        }

        for (auto child = node.last_child(); child; child = child.previous_sibling())
        {
            pending.emplace_back(child, depth + 1);
        }
    }

    return references;
}

} // namespace ExyokiOffice::Security
