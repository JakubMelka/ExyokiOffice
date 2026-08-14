// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Excel/WorksheetDrawingHelpers.hpp"

#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "XmlNamespaceResolver.hpp"
#include "XmlParseOptions.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <vector>

namespace ExyokiOffice::Excel::Detail
{

/// File-local helpers for worksheet drawing anchors.
class WorksheetDrawingDetailHelper
{
public:
    static constexpr const char* SpreadsheetDrawingNs = "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing";
    static constexpr const char* DrawingNs = "http://schemas.openxmlformats.org/drawingml/2006/main";
    static constexpr const char* RelationshipsNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    static constexpr const char* SpreadsheetMlNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

    static void CollectDrawingObjectIds(Pugi::xml_node node, std::vector<UInt32>& ids)
    {
        for (auto child = node.first_child(); child; child = child.next_sibling())
        {
            if (std::string_view(child.name()) == "xdr:cNvPr")
            {
                ids.push_back(child.attribute("id").as_uint());
            }
            CollectDrawingObjectIds(child, ids);
        }
    }

    /**
     * Elements that CT_Worksheet orders after `drawing`.
     *
     * `drawing` has to be inserted ahead of the first of these that is already
     * present, otherwise a worksheet that gained a `tableParts` or an `extLst`
     * before its first drawing ends up schema-invalid and Excel repairs it.
     */
    static constexpr std::string_view kElementsAfterDrawing[] = {"legacyDrawing", "legacyDrawingHF", "drawingHF",
                                                                 "picture", "oleObjects", "controls",
                                                                 "webPublishItems", "tableParts", "extLst"};

    static Pugi::xml_node FindDrawingInsertionAnchor(Pugi::xml_node root)
    {
        for (auto child = root.first_child(); child; child = child.next_sibling())
        {
            // Worksheet children may or may not carry a namespace prefix depending
            // on how the part was written, so compare the local name only.
            std::string_view name(child.name());
            if (const auto colon = name.rfind(':'); colon != std::string_view::npos)
            {
                name.remove_prefix(colon + 1);
            }
            if (std::ranges::find(kElementsAfterDrawing, name) != std::end(kElementsAfterDrawing))
            {
                return child;
            }
        }
        return {};
    }
};

std::string ResolveNamespace(Pugi::xml_node node, std::string_view uri)
{
    std::optional<std::string_view> suggested;
    if (const auto canonical = OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(uri))
    {
        suggested = *canonical;
    }
    return Xml::NamespaceResolver::EnsurePrefix(node, uri, suggested);
}

Pugi::xml_node LoadOrCreateWorksheetDrawing(Pugi::xml_document& document, const std::string& xml)
{
    Pugi::xml_node root;
    if (xml.empty())
    {
        root = document.append_child("xdr:wsDr");
    }
    else if (document.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
    {
        root = document.document_element();
    }
    if (!root)
    {
        return root;
    }

    // Every anchor kind (picture or chart) uses the a and r prefixes in
    // addition to xdr, regardless of which feature created the part first.
    ResolveNamespace(root, WorksheetDrawingDetailHelper::SpreadsheetDrawingNs);
    ResolveNamespace(root, WorksheetDrawingDetailHelper::DrawingNs);
    ResolveNamespace(root, WorksheetDrawingDetailHelper::RelationshipsNs);
    return root;
}

std::string SerializeRaw(Pugi::xml_document& document)
{
    std::ostringstream stream;
    document.print(stream, "", Pugi::format_raw);
    return stream.str();
}

UInt32 MaxDrawingObjectId(Pugi::xml_node worksheetDrawing)
{
    std::vector<UInt32> ids;
    WorksheetDrawingDetailHelper::CollectDrawingObjectIds(worksheetDrawing, ids);
    return ids.empty() ? 0 : *std::ranges::max_element(ids);
}

bool DrawingObjectIdExists(Pugi::xml_node worksheetDrawing, UInt32 id)
{
    std::vector<UInt32> ids;
    WorksheetDrawingDetailHelper::CollectDrawingObjectIds(worksheetDrawing, ids);
    return std::ranges::find(ids, id) != ids.end();
}

bool LinkWorksheetDrawing(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart,
                          const std::string& relationshipId)
{
    if (!worksheetPart)
    {
        return false;
    }
    // load_buffer over the whole string rather than load_string over c_str():
    // an embedded NUL would otherwise truncate the document silently.
    const auto worksheetXml = worksheetPart->GetXmlString();
    Pugi::xml_document document;
    if (!document.load_buffer(worksheetXml.data(), worksheetXml.size(), Xml::ParseOptions::Preserving))
    {
        return false;
    }
    auto root = document.document_element();
    if (!root)
    {
        return false;
    }
    ResolveNamespace(root, WorksheetDrawingDetailHelper::RelationshipsNs);
    // The link element belongs to the SpreadsheetML namespace. Writing it
    // unprefixed puts it in no namespace, which makes the worksheet fail schema
    // validation whenever the part uses prefixed element names.
    const auto prefix = ResolveNamespace(root, WorksheetDrawingDetailHelper::SpreadsheetMlNs);
    const auto elementName = prefix.empty() ? std::string("drawing") : prefix + ":drawing";
    const auto anchor = WorksheetDrawingDetailHelper::FindDrawingInsertionAnchor(root);
    auto link = anchor ? root.insert_child_before(elementName.c_str(), anchor)
                       : root.append_child(elementName.c_str());
    link.append_attribute("r:id").set_value(relationshipId.c_str());
    worksheetPart->SetXmlString(SerializeRaw(document));
    return true;
}

void UnlinkWorksheetDrawing(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart)
{
    if (!worksheetPart)
    {
        return;
    }
    const auto worksheetXml = worksheetPart->GetXmlString();
    Pugi::xml_document document;
    if (!document.load_buffer(worksheetXml.data(), worksheetXml.size(), Xml::ParseOptions::Preserving))
    {
        return;
    }
    auto root = document.document_element();
    if (!root)
    {
        return;
    }
    // Match the link whether or not it carries a namespace prefix, so a
    // worksheet written by another producer is unlinked as well.
    for (auto child = root.first_child(); child; child = child.next_sibling())
    {
        std::string_view name(child.name());
        if (const auto colon = name.rfind(':'); colon != std::string_view::npos)
        {
            name.remove_prefix(colon + 1);
        }
        if (name == "drawing")
        {
            root.remove_child(child);
            worksheetPart->SetXmlString(SerializeRaw(document));
            return;
        }
    }
}

} // namespace ExyokiOffice::Excel::Detail
