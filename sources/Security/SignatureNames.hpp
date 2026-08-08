// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <string_view>

namespace ExyokiOffice::Security
{

/// Fixed names of the OPC digital signature vocabulary: relationship types,
/// content types, XML namespaces, transform identifiers, and the element and
/// attribute identifiers Office expects inside a package signature.
class SignatureNames final
{
public:
    SignatureNames() = delete;

    /// Relationship from the package root to the signature origin part.
    static constexpr std::string_view OriginRelationshipType =
        "http://schemas.openxmlformats.org/package/2006/relationships/digital-signature/origin";
    /// Relationship from the origin part to a single signature part.
    static constexpr std::string_view SignatureRelationshipType =
        "http://schemas.openxmlformats.org/package/2006/relationships/digital-signature/signature";
    /// Relationship to a certificate part; preserved but not produced by this library.
    static constexpr std::string_view CertificateRelationshipType =
        "http://schemas.openxmlformats.org/package/2006/relationships/digital-signature/certificate";

    static constexpr std::string_view OriginContentType =
        "application/vnd.openxmlformats-package.digital-signature-origin";
    static constexpr std::string_view SignatureContentType =
        "application/vnd.openxmlformats-package.digital-signature-xmlsignature+xml";
    static constexpr std::string_view CertificateContentType =
        "application/vnd.openxmlformats-package.digital-signature-certificate";
    static constexpr std::string_view RelationshipsContentType =
        "application/vnd.openxmlformats-package.relationships+xml";

    /// XML Signature namespace.
    static constexpr std::string_view DsigNamespace = "http://www.w3.org/2000/09/xmldsig#";
    /// OPC digital signature namespace (usually bound to the mdssi prefix).
    static constexpr std::string_view PackageDigitalSignatureNamespace =
        "http://schemas.openxmlformats.org/package/2006/digital-signature";
    /// Office signature details namespace (SignatureInfoV1).
    static constexpr std::string_view OfficeDigitalSignatureNamespace =
        "http://schemas.microsoft.com/office/2006/digsig";

    /// Inclusive XML canonicalization, the algorithm used by package signatures.
    static constexpr std::string_view CanonicalizationC14N = "http://www.w3.org/TR/2001/REC-xml-c14n-20010315";
    static constexpr std::string_view CanonicalizationC14NWithComments =
        "http://www.w3.org/TR/2001/REC-xml-c14n-20010315#WithComments";
    /// Exclusive canonicalization; recognized so it can be reported as unsupported.
    static constexpr std::string_view CanonicalizationExcC14N = "http://www.w3.org/2001/10/xml-exc-c14n#";
    static constexpr std::string_view CanonicalizationExcC14NWithComments =
        "http://www.w3.org/2001/10/xml-exc-c14n#WithComments";
    /// OPC transform that selects and normalizes relationships before hashing.
    static constexpr std::string_view RelationshipTransform =
        "http://schemas.openxmlformats.org/package/2006/RelationshipTransform";

    /// Reference/@Type written for a reference that points at an Object element.
    static constexpr std::string_view ObjectReferenceType = "http://www.w3.org/2000/09/xmldsig#Object";

    /// Identifiers Office uses inside the signature; kept so a signature written
    /// here looks like the one Word produces.
    static constexpr std::string_view PackageObjectId = "idPackageObject";
    static constexpr std::string_view OfficeObjectId = "idOfficeObject";
    static constexpr std::string_view SignatureTimePropertyId = "idSignatureTime";
    static constexpr std::string_view OfficeDetailsPropertyId = "idOfficeV1Details";
    static constexpr std::string_view DefaultSignatureId = "idPackageSignature";

    /// Time format string written into mdssi:SignatureTime/mdssi:Format.
    static constexpr std::string_view SignatureTimeFormat = "YYYY-MM-DDThh:mm:ssTZD";

    /// Folder that holds the origin part and all signature parts.
    static constexpr std::string_view SignaturePartFolder = "/_xmlsignatures/";
};

} // namespace ExyokiOffice::Security
