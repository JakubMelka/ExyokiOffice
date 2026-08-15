// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "pugixml/pugixml.hpp"

namespace ExyokiOffice::Xml
{

/// The parse options every reader in the library uses.
///
/// pugixml's defaults are tuned for consuming XML, not for preserving it, and
/// the library exists to give a document back unchanged where it did not mean
/// to change it. Anything that parses XML - the package loader, the validator,
/// the Flat OPC converter, the archiver, the tools that rewrite a part in
/// place - parses with these options, so what a part carries does not depend on
/// which reader happened to open it.
///
/// Signature verification is the one exception: canonicalization has to
/// reproduce its input exactly, comments and processing instructions included,
/// so it uses XmlCanonicalization::ParseOptions instead.
class ParseOptions final
{
public:
    ParseOptions() = delete;

    /// Parse options that keep the content of a document.
    ///
    /// The pugixml default drops a text node whose content is only whitespace,
    /// which is exactly how OOXML stores an ordinary space between two runs:
    /// `<w:t xml:space="preserve"> </w:t>` would be parsed as an empty element
    /// and written back as `<w:t xml:space="preserve"/>`, so the space would be
    /// lost - silently, and in every part of the document at once.
    ///
    /// `parse_ws_pcdata_single` keeps such a node where it is the only child of
    /// its element, which is every text-carrying OOXML element (`w:t`,
    /// `w:delText`, `w:instrText`, `a:t`, `m:t`, the shared string `t`). The
    /// unconditional `parse_ws_pcdata` is deliberately not used: it would also
    /// keep the indentation between the elements of a pretty-printed part, turn
    /// it into content, and then re-indent it on every save.
    static constexpr unsigned int Preserving = Pugi::parse_default | Pugi::parse_ws_pcdata_single;
};

} // namespace ExyokiOffice::Xml
