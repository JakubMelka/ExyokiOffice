// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"

#include "pugixml/pugixml.hpp"

#include <optional>
#include <string>

namespace ExyokiOffice::Security
{

/// Canonicalization algorithms this library can apply.
enum class CanonicalizationMethod
{
    /// Inclusive XML canonicalization 1.0, comments removed.
    InclusiveC14N,
    /// Inclusive XML canonicalization 1.0, comments kept.
    InclusiveC14NWithComments
};

/// \brief Inclusive XML canonicalization (C14N 1.0, REC-xml-c14n-20010315).
///
/// A digital signature never hashes an XML tree directly; it hashes the
/// canonical byte form of that tree, so that irrelevant differences (attribute
/// order, redundant namespace declarations, empty element syntax, whitespace in
/// tags) do not change the digest. This is the algorithm every package
/// signature written by Word uses, both for SignedInfo and for the signed
/// Object elements.
///
/// The implementation canonicalizes a subtree: namespace declarations and the
/// xml:base/xml:lang/xml:space attributes that the subtree inherits from its
/// ancestors are rendered on the apex element, as the specification requires for
/// a document subset.
///
/// Exclusive canonicalization is deliberately not implemented; package
/// signatures do not use it, and the signature layer reports it as unsupported
/// instead of silently producing a wrong digest.
/// \note The class is exported so the unit tests can check it against the
///       examples in the specification. The header is internal and not installed.
class EXYOKIOFFICE_EXPORT XmlCanonicalization final
{
public:
    XmlCanonicalization() = delete;

    /// Parse options that must be used for XML which will be canonicalized.
    ///
    /// The default pugixml options drop comments, processing instructions and
    /// whitespace-only text. That is harmless for document content, but a
    /// canonical form has to reproduce the input faithfully - the specification
    /// keeps every character of character data - so signature XML is always
    /// parsed with these options instead.
    ///
    /// \note Deliberately stronger than Xml::ParseOptions::Preserving, which
    ///       keeps whitespace-only text only where it is an element's whole
    ///       content. Canonicalization must not decide which whitespace was
    ///       meant; a digest taken over a tree that dropped any of it does not
    ///       match the one Office computed. This is safe only because a
    ///       signature part is written back byte for byte - see
    ///       RequiresByteStableXml in OpenXmlPackagePart.cpp. Without that, the
    ///       indentation the writer adds would reach the verifier but not the
    ///       signer, and the two would stop agreeing with each other.
    static constexpr unsigned int ParseOptions =
        Pugi::parse_default | Pugi::parse_comments | Pugi::parse_pi | Pugi::parse_ws_pcdata;

    /// \brief Largest element nesting depth that is canonicalized.
    ///
    /// The writer descends one recursive call per level, carrying a copy of the
    /// in-scope namespace map on each frame, so nesting depth is bounded by the
    /// call stack and by nothing else. That matters here more than elsewhere in
    /// the library: signature verification parses part bytes itself, so it is
    /// not behind OpenXmlPackageLimits::MaxXmlDepth, and it runs on documents
    /// chosen by whoever supplied the package.
    ///
    /// Far above real markup — even pathologically nested tables stay under a
    /// hundred levels — and far below what exhausts a default thread stack.
    static constexpr unsigned int MaximumDepth = 512;

    /// \brief Canonicalizes the subtree rooted at \p node (an element).
    ///
    /// Failure is reported as an empty optional rather than as an empty string,
    /// and deliberately so: every caller here feeds the result to a digest, and
    /// a caller that forgets to check gets a compile error instead of hashing
    /// nothing at all and reporting the result as intact content.
    ///
    /// \return UTF-8 canonical form, or nothing when \p node is not an element
    ///         or the subtree is nested deeper than MaximumDepth.
    static std::optional<std::string> CanonicalizeSubtree(
        const Pugi::xml_node& node, CanonicalizationMethod method = CanonicalizationMethod::InclusiveC14N);

    /// Canonicalizes an entire document, including top level processing instructions.
    /// \return Nothing when the document is nested deeper than MaximumDepth.
    static std::optional<std::string> CanonicalizeDocument(
        const Pugi::xml_document& document, CanonicalizationMethod method = CanonicalizationMethod::InclusiveC14N);
};

} // namespace ExyokiOffice::Security
