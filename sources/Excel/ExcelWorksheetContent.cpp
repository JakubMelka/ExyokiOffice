// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/Guid.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2019/Excel/ThreadedComments.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Vml.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Vml/Office.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Vml/Spreadsheet.hpp"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "Excel/WorksheetDrawingHelpers.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <map>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ExyokiOffice::Excel
{

class WorksheetContentHelpers final
{
public:
    WorksheetContentHelpers() = delete;

    static constexpr std::string_view HyperlinkRelationship =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";

    static std::string CurrentTime()
    {
        return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    }

    static std::string ContentType(ExcelImageFormat format)
    {
        switch (format)
        {
            case ExcelImageFormat::Png:
                return "image/png";
            case ExcelImageFormat::Jpeg:
                return "image/jpeg";
            case ExcelImageFormat::Gif:
                return "image/gif";
            case ExcelImageFormat::Bmp:
                return "image/bmp";
            case ExcelImageFormat::Tiff:
                return "image/tiff";
        }

        return "image/png";
    }
};

namespace Xltc = ExyokiOffice::DocumentFormat::OpenXml::Office2019::Excel::ThreadedComments;

/// File-local lookup and ordering helpers for worksheet content.
class WorksheetContentHelper
{
public:
    /** @brief SpreadsheetML main namespace URI: the worksheet root and the standalone comments part. */
    static constexpr const char* SpreadsheetMlNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    /** @brief OOXML relationships namespace URI, used for the hyperlink `r:id` attribute. */
    static constexpr const char* RelationshipsNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

    /** @brief Prefixes @p localName with @p prefix (`prefix:localName`), or returns it unchanged when @p prefix is empty. */
    static std::string Qualify(const std::string& prefix, std::string_view localName)
    {
        return prefix.empty() ? std::string(localName) : prefix + ":" + std::string(localName);
    }

    /** @brief Typed DOM of the threaded-comments and person-list parts (the `xltc` namespace). */

    /** @brief One entry of the workbook-level person list. */
    struct ThreadedCommentPerson
    {
        std::string Name;
        std::string Email;
    };

    /**
     * @brief Helpers for the threaded-comments part and the person list it references.
     *
     * Everything here goes through the typed DOM. The elements carry the `xltc`
     * namespace, which the typed layer resolves for us; matching element names as
     * raw text would make the reader depend on whether the writer used a prefix or
     * a default namespace, and Excel and this library do not agree on that.
     */
    class ThreadedCommentHelpers final
    {
    public:
        ThreadedCommentHelpers() = delete;

        /// Reads the workbook person list, keyed by person id.
        static std::map<std::string, ThreadedCommentPerson> ReadPersons(
            const std::shared_ptr<Packaging::WorkbookPart>& workbook)
        {
            std::map<std::string, ThreadedCommentPerson> persons;
            if (!workbook)
            {
                return persons;
            }

            const auto parts = workbook->GetWorkbookPersonParts();
            if (parts.empty() || parts.front() == nullptr)
            {
                return persons;
            }

            const auto root = parts.front()->GetTypedRootElement();
            if (!root)
            {
                return persons;
            }

            for (const auto& person : root->Elements<Xltc::Person>())
            {
                if (!person)
                {
                    continue;
                }

                ThreadedCommentPerson record;
                record.Name = person->GetDisplayName().ToString();
                record.Email = person->GetUserId().ToString();
                persons.insert_or_assign(person->GetId().ToString(), std::move(record));
            }

            return persons;
        }

        /// The workbook person list, created when the workbook has none yet.
        static std::shared_ptr<Xltc::PersonList> EnsurePersonList(
            const std::shared_ptr<Packaging::WorkbookPart>& workbook)
        {
            if (!workbook)
            {
                return nullptr;
            }

            const auto parts = workbook->GetWorkbookPersonParts();
            const auto part = parts.empty() ? workbook->AddWorkbookPersonPart() : parts.front();
            return part ? part->GetTypedRootElement() : nullptr;
        }

        /// True when any worksheet of the workbook still holds a threaded comment.
        static bool WorkbookHasThreadedComments(const std::shared_ptr<Packaging::WorkbookPart>& workbook)
        {
            if (!workbook)
            {
                return false;
            }

            for (const auto& worksheet : workbook->GetWorksheetParts())
            {
                if (!worksheet)
                {
                    continue;
                }

                for (const auto& part : worksheet->GetWorksheetThreadedCommentsParts())
                {
                    if (!part)
                    {
                        continue;
                    }

                    const auto root = part->GetTypedRootElement();
                    if (root && !root->Elements<Xltc::ThreadedComment>().empty())
                    {
                        return true;
                    }
                }
            }

            return false;
        }
    };

    /** @brief Collects the element names of a particle tree in schema order. */
    static void FlattenParticle(const MetadataParticlePtr& particle, std::vector<OpenXmlQualifiedName>& order)
    {
        if (!particle)
        {
            return;
        }
        if (particle->Kind() == MetadataParticleKind::Element)
        {
            order.push_back(static_cast<const MetadataElementParticle&>(*particle).Element());
            return;
        }
        if (const auto composite = std::dynamic_pointer_cast<MetadataCompositeParticle>(particle))
        {
            for (const auto& child : composite->Children())
            {
                FlattenParticle(child, order);
            }
        }
    }

    /**
     * @brief Returns the worksheet's child @p localName, creating it in schema order.
     *
     * SpreadsheetML fixes the order of the worksheet's children, so a new element
     * cannot simply be appended: it has to precede every already present child that
     * the content model places after it. Only worksheet children live in the
     * SpreadsheetML namespace here, so comparing local names is sufficient.
     */
    static Pugi::xml_node EnsureWorksheetChild(Pugi::xml_node& root, const std::string& prefix, std::string_view localName)
    {
        const auto qualified = Qualify(prefix, localName);
        if (auto existing = root.child(qualified.c_str()))
        {
            return existing;
        }

        std::vector<OpenXmlQualifiedName> order;
        FlattenParticle(DocumentFormat::OpenXml::Spreadsheet::Worksheet::StaticMetaClass()->GetMetadata()->ParticleTree(),
                        order);
        const auto target = std::find_if(order.begin(), order.end(), [localName](const OpenXmlQualifiedName& name)
                                         { return name.localName() == localName; });

        Pugi::xml_node before;
        if (target != order.end())
        {
            for (auto child = root.first_child(); child && !before; child = child.next_sibling())
            {
                const std::string_view childName = child.name();
                const auto separator = childName.rfind(':');
                const auto childLocalName =
                    separator == std::string_view::npos ? childName : childName.substr(separator + 1);
                for (auto candidate = std::next(target); candidate != order.end(); ++candidate)
                {
                    if (candidate->localName() == childLocalName)
                    {
                        before = child;
                        break;
                    }
                }
            }
        }

        return before ? root.insert_child_before(qualified.c_str(), before) : root.append_child(qualified.c_str());
    }

    /** @brief VML namespaces the legacy comment drawing declares on its root element. */
    static constexpr const char* VmlNs = "urn:schemas-microsoft-com:vml";
    static constexpr const char* VmlOfficeNs = "urn:schemas-microsoft-com:office:office";
    static constexpr const char* VmlExcelNs = "urn:schemas-microsoft-com:office:excel";

    /**
     * @brief Rebuilds the legacy VML drawing that carries a worksheet's comment boxes.
     *
     * SpreadsheetML splits a comment in two: the text lives in the comments part,
     * while its on-sheet box - position, size, fill and shadow - is a VML shape in a
     * separate drawing that the worksheet references through `legacyDrawing`. Excel
     * treats a comments part without that drawing as damaged content, so both are
     * always written together.
     *
     * The drawing is built with the typed VML DOM. Its root is the plain `xml`
     * element the format prescribes, which belongs to no schema, so the typed
     * children are appended to it without a content model to check against.
     */
    static void WriteCommentVmlDrawing(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart,
                                       const std::vector<ExcelComment>& comments)
    {
        namespace Vml = DocumentFormat::OpenXml::Vml;
        namespace VmlOffice = DocumentFormat::OpenXml::Vml::Office;
        namespace VmlExcel = DocumentFormat::OpenXml::Vml::Spreadsheet;

        if (!worksheetPart)
        {
            return;
        }

        auto drawings = worksheetPart->GetVmlDrawingParts();
        auto part = drawings.empty() ? nullptr : drawings.front();
        if (comments.empty())
        {
            if (part)
            {
                worksheetPart->RemoveVmlDrawingPart(part);
            }
            return;
        }
        if (!part)
        {
            part = worksheetPart->AddVmlDrawingPart();
        }
        if (!part)
        {
            return;
        }

        Pugi::xml_document document;
        auto rootNode = document.append_child("xml");
        Xml::NamespaceResolver::EnsurePrefix(rootNode, VmlNs, "v");
        Xml::NamespaceResolver::EnsurePrefix(rootNode, VmlOfficeNs, "o");
        Xml::NamespaceResolver::EnsurePrefix(rootNode, VmlExcelNs, "x");
        auto root = ExyokiOffice::Detail::CreateOpenXmlElementFromNode(rootNode);
        if (!root)
        {
            return;
        }

        auto layout = root->AppendChildRaw<VmlOffice::ShapeLayout>();
        auto idMap = layout ? layout->AppendChildRaw<VmlOffice::ShapeIdMap>() : nullptr;
        if (!idMap)
        {
            return;
        }
        layout->SetExtension(
            EnumValue<Vml::ExtensionHandlingBehaviorValues>(Vml::ExtensionHandlingBehaviorValues::Edit));
        idMap->SetExtension(
            EnumValue<Vml::ExtensionHandlingBehaviorValues>(Vml::ExtensionHandlingBehaviorValues::Edit));
        idMap->SetData(StringValue("1"));

        // The text-box shape type every comment shape derives from.
        auto shapeType = root->AppendChildRaw<Vml::Shapetype>();
        auto shapeTypeStroke = shapeType ? shapeType->AppendChildRaw<Vml::Stroke>() : nullptr;
        auto shapeTypePath = shapeType ? shapeType->AppendChildRaw<Vml::Path>() : nullptr;
        if (!shapeTypeStroke || !shapeTypePath)
        {
            return;
        }
        shapeType->SetId(StringValue("_x0000_t202"));
        shapeType->SetCoordinateSize(StringValue("21600,21600"));
        shapeType->SetOptionalNumber(Int32Value(202));
        shapeType->SetEdgePath(StringValue("m,l,21600r21600,l21600,xe"));
        shapeTypeStroke->SetJoinStyle(
            EnumValue<Vml::StrokeJoinStyleValues>(Vml::StrokeJoinStyleValues::Miter));
        shapeTypePath->SetAllowGradientShape(TrueFalseValue(true));
        shapeTypePath->SetConnectionPointType(
            EnumValue<VmlOffice::ConnectValues>(VmlOffice::ConnectValues::Rectangle));

        UInt32 shapeId = 1025;
        for (const auto& comment : comments)
        {
            // Rows and columns are zero-based in the client data, and the anchor
            // spans two columns and four rows starting one cell to the right.
            const auto row = comment.Address.Row().Value() - 1;
            const auto column = comment.Address.Column().Value() - 1;

            auto shape = root->AppendChildRaw<Vml::Shape>();
            auto fill = shape ? shape->AppendChildRaw<Vml::Fill>() : nullptr;
            auto shadow = shape ? shape->AppendChildRaw<Vml::Shadow>() : nullptr;
            auto path = shape ? shape->AppendChildRaw<Vml::Path>() : nullptr;
            auto textBox = shape ? shape->AppendChildRaw<Vml::TextBox>() : nullptr;
            auto clientData = shape ? shape->AppendChildRaw<VmlExcel::ClientData>() : nullptr;
            if (!fill || !shadow || !path || !textBox || !clientData)
            {
                return;
            }

            shape->SetId(StringValue("_x0000_s" + std::to_string(shapeId++)));
            shape->SetType(StringValue("#_x0000_t202"));
            shape->SetStyle(StringValue("position:absolute;width:108pt;height:59.25pt;z-index:1;"
                                        "visibility:hidden;mso-wrap-style:tight"));
            shape->SetFillColor(StringValue("#ffffe1"));
            shape->SetInsetMode(EnumValue<VmlOffice::InsetMarginValues>(VmlOffice::InsetMarginValues::Auto));
            fill->SetColor2(StringValue("#ffffe1"));
            shadow->SetOn(TrueFalseValue(true));
            shadow->SetColor(StringValue("black"));
            shadow->SetObscured(TrueFalseValue(true));
            path->SetConnectionPointType(EnumValue<VmlOffice::ConnectValues>(VmlOffice::ConnectValues::None));
            textBox->SetStyle(StringValue("mso-direction-alt:auto"));

            clientData->SetObjectType(EnumValue<VmlExcel::ObjectValues>(VmlExcel::ObjectValues::Note));
            auto moveWithCells = clientData->AppendChildRaw<VmlExcel::MoveWithCells>();
            auto sizeWithCells = clientData->AppendChildRaw<VmlExcel::ResizeWithCells>();
            auto anchor = clientData->AppendChildRaw<VmlExcel::Anchor>();
            auto autoFill = clientData->AppendChildRaw<VmlExcel::AutoFill>();
            auto rowTarget = clientData->AppendChildRaw<VmlExcel::CommentRowTarget>();
            auto columnTarget = clientData->AppendChildRaw<VmlExcel::CommentColumnTarget>();
            if (!moveWithCells || !sizeWithCells || !anchor || !autoFill || !rowTarget || !columnTarget)
            {
                return;
            }
            anchor->SetText(std::to_string(column + 1) + ", 15, " + std::to_string(row) + ", 10, " +
                            std::to_string(column + 3) + ", 15, " + std::to_string(row + 4) + ", 4");
            autoFill->SetText("False");
            rowTarget->SetText(std::to_string(row));
            columnTarget->SetText(std::to_string(column));
        }

        const auto xml = Detail::SerializeRaw(document);
        part->SetBinaryData(std::vector<Byte>(xml.begin(), xml.end()));
    }

    /** @brief Points the worksheet's `legacyDrawing` element at the comment VML, or removes it. */
    static void UpdateLegacyDrawingReference(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart)
    {
        if (!worksheetPart)
        {
            return;
        }

        // load_buffer over the whole string, not load_string over c_str(): the
        // latter ends the document at the first embedded NUL, so a part carrying
        // one would be parsed as its prefix and quietly accepted here while
        // OpenXmlPackagePart, which already reads parts this way, rejects it.
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
        const auto smlPrefix = Detail::ResolveNamespace(root, SpreadsheetMlNs);
        const auto elementName = Qualify(smlPrefix, "legacyDrawing");

        auto drawings = worksheetPart->GetVmlDrawingParts();
        if (drawings.empty())
        {
            if (auto existing = root.child(elementName.c_str()))
            {
                root.remove_child(existing);
                worksheetPart->SetXmlString(Detail::SerializeRaw(document));
            }
            return;
        }

        const auto relationshipId = worksheetPart->AddPartReference(
            drawings.front(), "http://schemas.openxmlformats.org/officeDocument/2006/relationships/vmlDrawing");
        if (relationshipId.empty())
        {
            return;
        }
        Detail::ResolveNamespace(root, RelationshipsNs);
        auto element = EnsureWorksheetChild(root, smlPrefix, "legacyDrawing");
        if (!element)
        {
            return;
        }
        if (auto attribute = element.attribute("r:id"))
        {
            attribute.set_value(relationshipId.c_str());
        }
        else
        {
            element.append_attribute("r:id").set_value(relationshipId.c_str());
        }
        worksheetPart->SetXmlString(Detail::SerializeRaw(document));
    }
};

bool Worksheet::SetHyperlink(const ExcelHyperlink& hyperlink)
{
    if (!m_part || !hyperlink.Address.IsValid() || (hyperlink.Target.empty() && hyperlink.Location.empty()))
    {
        return false;
    }

    RemoveHyperlink(hyperlink.Address);

    const auto worksheetXml = m_part->GetXmlString();
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

    std::string relationshipId;
    if (!hyperlink.Target.empty())
    {
        relationshipId =
            m_part->AddExternalRelationship(WorksheetContentHelpers::HyperlinkRelationship, hyperlink.Target);
        if (relationshipId.empty())
        {
            return false;
        }
    }

    // The worksheet root already declares its own prefix (canonically "x") for
    // this namespace; reuse it instead of assuming or hardcoding a literal.
    const auto smlPrefix = Detail::ResolveNamespace(root, WorksheetContentHelper::SpreadsheetMlNs);
    if (!relationshipId.empty())
    {
        Detail::ResolveNamespace(root, WorksheetContentHelper::RelationshipsNs);
    }

    auto container = WorksheetContentHelper::EnsureWorksheetChild(root, smlPrefix, "hyperlinks");
    if (!container)
    {
        return false;
    }

    auto element = container.append_child(WorksheetContentHelper::Qualify(smlPrefix, "hyperlink").c_str());
    element.append_attribute("ref").set_value(hyperlink.Address.ToA1().c_str());
    if (!relationshipId.empty())
    {
        element.append_attribute("r:id").set_value(relationshipId.c_str());
    }
    if (!hyperlink.Location.empty())
    {
        element.append_attribute("location").set_value(hyperlink.Location.c_str());
    }
    if (!hyperlink.Display.empty())
    {
        element.append_attribute("display").set_value(hyperlink.Display.c_str());
    }
    if (!hyperlink.Tooltip.empty())
    {
        element.append_attribute("tooltip").set_value(hyperlink.Tooltip.c_str());
    }

    m_part->SetXmlString(Detail::SerializeRaw(document));
    return true;
}

std::vector<ExcelHyperlink> Worksheet::Hyperlinks() const
{
    std::vector<ExcelHyperlink> result;
    if (!m_part)
    {
        return result;
    }

    const auto worksheetXml = m_part->GetXmlString();
    Pugi::xml_document document;
    if (!document.load_buffer(worksheetXml.data(), worksheetXml.size(), Xml::ParseOptions::Preserving))
    {
        return result;
    }
    auto root = document.document_element();
    if (!root)
    {
        return result;
    }

    const auto smlPrefix = Detail::ResolveNamespace(root, WorksheetContentHelper::SpreadsheetMlNs);
    auto container = root.child(WorksheetContentHelper::Qualify(smlPrefix, "hyperlinks").c_str());
    if (!container)
    {
        return result;
    }

    // Named (not temporary) so the pointer stays valid across the whole range-for,
    // which lazily re-uses it on every increment, not just at the initial call.
    const auto hyperlinkName = WorksheetContentHelper::Qualify(smlPrefix, "hyperlink");
    for (auto tag : container.children(hyperlinkName.c_str()))
    {
        const auto address = CellAddress::ParseA1(tag.attribute("ref").as_string());
        if (!address)
        {
            continue;
        }

        ExcelHyperlink hyperlink;
        hyperlink.Address = *address;
        hyperlink.Location = tag.attribute("location").as_string();
        hyperlink.Display = tag.attribute("display").as_string();
        hyperlink.Tooltip = tag.attribute("tooltip").as_string();

        const std::string relationshipId = tag.attribute("r:id").as_string();
        for (const auto& relationship : m_part->Relationships())
        {
            if (relationship.Id == relationshipId && relationship.IsExternal)
            {
                hyperlink.Target = relationship.Target;
                break;
            }
        }

        result.push_back(std::move(hyperlink));
    }

    return result;
}

std::optional<ExcelHyperlink> Worksheet::GetHyperlink(CellAddress address) const
{
    const auto hyperlinks = Hyperlinks();
    const auto iterator = std::find_if(hyperlinks.begin(), hyperlinks.end(),
                                       [&](const auto& item)
                                       { return item.Address.ToA1() == address.ToA1(); });
    return iterator == hyperlinks.end() ? std::nullopt : std::optional<ExcelHyperlink>(*iterator);
}

bool Worksheet::RemoveHyperlink(CellAddress address)
{
    if (!m_part || !address.IsValid())
    {
        return false;
    }

    const auto worksheetXml = m_part->GetXmlString();
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

    const auto smlPrefix = Detail::ResolveNamespace(root, WorksheetContentHelper::SpreadsheetMlNs);
    auto container = root.child(WorksheetContentHelper::Qualify(smlPrefix, "hyperlinks").c_str());
    if (!container)
    {
        return false;
    }

    // Named (not temporary) so the pointer stays valid across the whole range-for,
    // which lazily re-uses it on every increment, not just at the initial call.
    const auto hyperlinkName = WorksheetContentHelper::Qualify(smlPrefix, "hyperlink");
    for (auto tag : container.children(hyperlinkName.c_str()))
    {
        if (std::string_view(tag.attribute("ref").as_string()) != address.ToA1())
        {
            continue;
        }

        const std::string relationshipId = tag.attribute("r:id").as_string();
        container.remove_child(tag);
        if (!relationshipId.empty())
        {
            m_part->RemoveExternalRelationship(relationshipId);
        }

        if (!container.first_child())
        {
            root.remove_child(container);
        }

        m_part->SetXmlString(Detail::SerializeRaw(document));
        return true;
    }

    return false;
}

bool Worksheet::SetComment(const ExcelComment& comment)
{
    if (!m_part || !comment.Address.IsValid() || comment.Author.empty())
    {
        return false;
    }

    auto comments = Comments();
    const auto iterator = std::find_if(comments.begin(), comments.end(),
                                       [&](const auto& item)
                                       { return item.Address.ToA1() == comment.Address.ToA1(); });
    if (iterator == comments.end())
    {
        comments.push_back(comment);
    }
    else
    {
        *iterator = comment;
    }

    auto part = m_part->GetWorksheetCommentsPart();
    if (!part)
    {
        part = m_part->AddWorksheetCommentsPart();
    }
    if (!part)
    {
        return false;
    }

    std::vector<std::string> authors;
    for (const auto& item : comments)
    {
        if (std::find(authors.begin(), authors.end(), item.Author) == authors.end())
        {
            authors.push_back(item.Author);
        }
    }

    // Comments are always rewritten as a whole from the in-memory list rather
    // than patched incrementally, so this builds a fresh standalone document.
    Pugi::xml_document document;
    auto root = document.append_child("comments");
    root.append_attribute("xmlns").set_value(WorksheetContentHelper::SpreadsheetMlNs);
    auto authorsElement = root.append_child("authors");
    for (const auto& author : authors)
    {
        authorsElement.append_child("author").text().set(author.c_str());
    }

    auto commentList = root.append_child("commentList");
    for (const auto& item : comments)
    {
        const auto authorIndex = std::find(authors.begin(), authors.end(), item.Author) - authors.begin();
        auto commentElement = commentList.append_child("comment");
        commentElement.append_attribute("ref").set_value(item.Address.ToA1().c_str());
        commentElement.append_attribute("authorId").set_value(std::to_string(authorIndex).c_str());
        commentElement.append_child("text").append_child("t").text().set(item.Text.c_str());
    }

    part->SetXmlString(Detail::SerializeRaw(document));

    // Excel needs the matching VML box for every comment, and the worksheet has
    // to point at the drawing that holds them.
    WorksheetContentHelper::WriteCommentVmlDrawing(m_part, comments);
    WorksheetContentHelper::UpdateLegacyDrawingReference(m_part);
    return true;
}

std::vector<ExcelComment> Worksheet::Comments() const
{
    std::vector<ExcelComment> result;
    const auto part = m_part ? m_part->GetWorksheetCommentsPart() : nullptr;
    if (!part)
    {
        return result;
    }

    const auto commentsXml = part->GetXmlString();
    Pugi::xml_document document;
    if (!document.load_buffer(commentsXml.data(), commentsXml.size(), Xml::ParseOptions::Preserving))
    {
        return result;
    }
    auto root = document.document_element();
    if (!root)
    {
        return result;
    }

    std::vector<std::string> authors;
    for (auto author : root.child("authors").children("author"))
    {
        authors.push_back(author.text().as_string());
    }

    for (auto commentElement : root.child("commentList").children("comment"))
    {
        const auto address = CellAddress::ParseA1(commentElement.attribute("ref").as_string());
        if (!address)
        {
            continue;
        }
        const auto authorIndex = static_cast<Size>(commentElement.attribute("authorId").as_uint());
        ExcelComment item;
        item.Address = *address;
        item.Author = authorIndex < authors.size() ? authors[authorIndex] : std::string{};
        item.Text = commentElement.child("text").child("t").text().as_string();
        result.push_back(std::move(item));
    }
    return result;
}

std::optional<ExcelComment> Worksheet::GetComment(CellAddress address) const
{
    const auto comments = Comments();
    const auto iterator = std::find_if(comments.begin(), comments.end(),
                                       [&](const auto& item)
                                       { return item.Address.ToA1() == address.ToA1(); });
    return iterator == comments.end() ? std::nullopt : std::optional<ExcelComment>(*iterator);
}

bool Worksheet::RemoveComment(CellAddress address)
{
    auto comments = Comments();
    const auto oldSize = comments.size();
    std::erase_if(comments, [&](const auto& item)
                  { return item.Address.ToA1() == address.ToA1(); });
    if (comments.size() == oldSize)
    {
        return false;
    }
    if (comments.empty())
    {
        const bool removed = m_part->RemoveWorksheetCommentsPart();
        WorksheetContentHelper::WriteCommentVmlDrawing(m_part, comments);
        WorksheetContentHelper::UpdateLegacyDrawingReference(m_part);
        return removed;
    }

    const auto first = comments.front();
    m_part->RemoveWorksheetCommentsPart();
    for (const auto& item : comments)
    {
        if (!SetComment(item))
        {
            return false;
        }
    }
    return GetComment(first.Address).has_value();
}

std::optional<std::string> Worksheet::AddThreadedComment(ExcelThreadedComment comment)
{
    if (!m_part || !m_document || !comment.Address.IsValid())
    {
        return std::nullopt;
    }

    // A display name is needed only to mint a new person record. A reply that
    // names a person the workbook already knows carries the id alone, which is
    // exactly what ThreadedComments() hands back.
    const auto knownPersons = WorksheetContentHelper::ThreadedCommentHelpers::ReadPersons(m_document->GetWorkbookPart());
    if (comment.PersonName.empty() && (comment.PersonId.empty() || !knownPersons.contains(comment.PersonId)))
    {
        return std::nullopt;
    }

    if (comment.Id.empty())
    {
        comment.Id = Guid::New();
    }
    if (comment.PersonId.empty())
    {
        comment.PersonId = Guid::New();
    }
    if (comment.DateTime.empty())
    {
        comment.DateTime = WorksheetContentHelpers::CurrentTime();
    }

    const auto parts = m_part->GetWorksheetThreadedCommentsParts();
    const auto part = parts.empty() ? m_part->AddWorksheetThreadedCommentsPart() : parts.front();
    const auto workbook = m_document->GetWorkbookPart();
    if (!part || !workbook)
    {
        return std::nullopt;
    }

    const auto root = part->GetTypedRootElement();
    if (!root)
    {
        return std::nullopt;
    }

    for (const auto& existing : root->Elements<Xltc::ThreadedComment>())
    {
        if (existing && existing->GetId().ToString() == comment.Id)
        {
            return std::nullopt;
        }
    }

    if (!knownPersons.contains(comment.PersonId))
    {
        const auto personList = WorksheetContentHelper::ThreadedCommentHelpers::EnsurePersonList(workbook);
        if (!personList)
        {
            return std::nullopt;
        }

        auto person = personList->AppendChild<Xltc::Person>();
        if (!person)
        {
            return std::nullopt;
        }

        person->SetId(StringValue(comment.PersonId));
        person->SetDisplayName(StringValue(comment.PersonName));
        person->SetUserId(StringValue(comment.PersonEmail));
        // No identity provider backs a person this library mints.
        person->SetProviderId(StringValue("None"));
    }

    auto element = root->AppendChild<Xltc::ThreadedComment>();
    if (!element)
    {
        return std::nullopt;
    }

    element->SetRef(StringValue(comment.Address.ToA1()));
    element->SetDT(DateTimeValue(std::string_view(comment.DateTime)));
    element->SetPersonId(StringValue(comment.PersonId));
    element->SetId(StringValue(comment.Id));
    if (!comment.ParentId.empty())
    {
        element->SetParentId(StringValue(comment.ParentId));
    }

    auto text = element->AppendChild<Xltc::ThreadedCommentText>();
    if (!text)
    {
        root->RemoveChild(element);
        return std::nullopt;
    }

    text->SetText(comment.Text);
    return comment.Id;
}

std::vector<ExcelThreadedComment> Worksheet::ThreadedComments() const
{
    std::vector<ExcelThreadedComment> result;
    if (!m_part)
    {
        return result;
    }
    const auto parts = m_part->GetWorksheetThreadedCommentsParts();
    if (parts.empty() || parts.front() == nullptr)
    {
        return result;
    }

    const auto root = parts.front()->GetTypedRootElement();
    if (!root)
    {
        return result;
    }

    // The author of each entry lives in the workbook-level person list; without
    // this join every reader would see an anonymous comment, and a caller that
    // wrote back what it read would lose the author entirely.
    const auto persons =
        WorksheetContentHelper::ThreadedCommentHelpers::ReadPersons(m_document ? m_document->GetWorkbookPart() : nullptr);

    for (const auto& element : root->Elements<Xltc::ThreadedComment>())
    {
        if (!element)
        {
            continue;
        }

        const auto address = CellAddress::ParseA1(element->GetRef().ToString());
        if (!address)
        {
            continue;
        }
        ExcelThreadedComment item;
        item.Id = element->GetId().ToString();
        item.Address = *address;
        item.PersonId = element->GetPersonId().ToString();
        item.DateTime = element->GetDT().ToString();
        item.ParentId = element->GetParentId().ToString();
        if (const auto text = element->GetFirstChildOfType<Xltc::ThreadedCommentText>())
        {
            item.Text = std::string(text->GetText());
        }

        const auto person = persons.find(item.PersonId);
        if (person != persons.end())
        {
            item.PersonName = person->second.Name;
            item.PersonEmail = person->second.Email;
        }

        result.push_back(std::move(item));
    }
    return result;
}

bool Worksheet::RemoveThreadedComment(std::string_view id)
{
    if (!m_part)
    {
        return false;
    }

    const auto parts = m_part->GetWorksheetThreadedCommentsParts();
    if (parts.empty() || parts.front() == nullptr)
    {
        return false;
    }

    const auto root = parts.front()->GetTypedRootElement();
    if (!root)
    {
        return false;
    }

    // The comment goes with its replies. Only the matching elements are
    // detached: rewriting the part from a re-read list would put the survivors
    // back through the validation meant for new comments, which drops them.
    std::vector<std::shared_ptr<Xltc::ThreadedComment>> doomed;
    for (const auto& element : root->Elements<Xltc::ThreadedComment>())
    {
        if (!element)
        {
            continue;
        }

        if (element->GetId().ToString() == id || element->GetParentId().ToString() == id)
        {
            doomed.push_back(element);
        }
    }

    if (doomed.empty())
    {
        return false;
    }

    for (const auto& element : doomed)
    {
        root->RemoveChild(element);
    }

    if (!root->Elements<Xltc::ThreadedComment>().empty())
    {
        return true;
    }

    const auto removed = m_part->RemoveWorksheetThreadedCommentsPart(parts.front());
    const auto workbook = m_document ? m_document->GetWorkbookPart() : nullptr;
    if (workbook && !WorksheetContentHelper::ThreadedCommentHelpers::WorkbookHasThreadedComments(workbook))
    {
        // The person list is workbook-level, so it may only go once no
        // worksheet references it any more.
        const auto people = workbook->GetWorkbookPersonParts();
        if (!people.empty())
        {
            workbook->RemoveWorkbookPersonPart(people.front());
        }
    }

    return removed;
}

/// File-local ordering helpers for worksheet content.
class WorksheetContentOrderHelper
{
public:
    /** @brief True when @p anchor's graphic content is a picture (`xdr:pic`), as opposed to a chart graphic frame. */
    static bool IsPictureAnchor(Pugi::xml_node anchor)
    {
        return static_cast<bool>(anchor.child("xdr:pic"));
    }

    /** @brief Appends a `xdr:from`/`xdr:to` pair of zero-based cell markers to @p anchor. */
    static void AppendImageAnchorBounds(Pugi::xml_node anchor, const CellAddress& from, const CellAddress& to)
    {
        auto fromMarker = anchor.append_child("xdr:from");
        fromMarker.append_child("xdr:col").text().set(from.Column().Value() - 1);
        fromMarker.append_child("xdr:colOff").text().set(0);
        fromMarker.append_child("xdr:row").text().set(from.Row().Value() - 1);
        fromMarker.append_child("xdr:rowOff").text().set(0);

        auto toMarker = anchor.append_child("xdr:to");
        toMarker.append_child("xdr:col").text().set(to.Column().Value() - 1);
        toMarker.append_child("xdr:colOff").text().set(0);
        toMarker.append_child("xdr:row").text().set(to.Row().Value() - 1);
        toMarker.append_child("xdr:rowOff").text().set(0);
    }
};

std::optional<UInt32> Worksheet::AddImage(ExcelWorksheetImage image)
{
    if (!m_part || image.Data.empty() || !image.From.IsValid() || !image.To.IsValid() ||
        image.To.Row().Value() < image.From.Row().Value() || image.To.Column().Value() < image.From.Column().Value())
    {
        return std::nullopt;
    }

    auto drawing = m_part->GetDrawingsPart();
    if (!drawing)
    {
        drawing = m_part->AddDrawingsPart();
        if (!drawing || !Detail::LinkWorksheetDrawing(m_part, drawing->RelationshipId()))
        {
            return std::nullopt;
        }
    }

    Pugi::xml_document document;
    auto worksheetDrawing = Detail::LoadOrCreateWorksheetDrawing(document, drawing->GetXmlString());
    if (!worksheetDrawing)
    {
        return std::nullopt;
    }

    // Drawing object ids are shared with charts, so the id pool spans every anchor kind.
    if (image.Id == 0)
    {
        image.Id = Detail::MaxDrawingObjectId(worksheetDrawing) + 1;
    }
    else if (Detail::DrawingObjectIdExists(worksheetDrawing, image.Id))
    {
        return std::nullopt;
    }

    // Attaching the part after its content type is known lets the package name
    // the file after the image format instead of the `.bin` placeholder.
    auto media = std::make_shared<Packaging::ImagePart>();
    media->SetContentType(WorksheetContentHelpers::ContentType(image.Format));
    if (!drawing->AddImagePart(media))
    {
        return std::nullopt;
    }
    media->SetBinaryData(std::move(image.Data));

    auto anchor = worksheetDrawing.append_child("xdr:twoCellAnchor");
    anchor.append_attribute("editAs").set_value("twoCell");
    WorksheetContentOrderHelper::AppendImageAnchorBounds(anchor, image.From, image.To);

    auto pic = anchor.append_child("xdr:pic");
    auto nonVisual = pic.append_child("xdr:nvPicPr");
    auto properties = nonVisual.append_child("xdr:cNvPr");
    properties.append_attribute("id").set_value(image.Id);
    properties.append_attribute("name").set_value(
        (image.Name.empty() ? "Picture " + std::to_string(image.Id) : image.Name).c_str());
    properties.append_attribute("descr").set_value(image.Description.c_str());
    nonVisual.append_child("xdr:cNvPicPr");

    auto blipFill = pic.append_child("xdr:blipFill");
    blipFill.append_child("a:blip").append_attribute("r:embed").set_value(media->RelationshipId().c_str());
    blipFill.append_child("a:stretch").append_child("a:fillRect");

    auto shapeProperties = pic.append_child("xdr:spPr");
    shapeProperties.append_child("a:xfrm");
    shapeProperties.append_child("a:prstGeom").append_attribute("prst").set_value("rect");
    shapeProperties.child("a:prstGeom").append_child("a:avLst");

    anchor.append_child("xdr:clientData");

    drawing->SetXmlString(Detail::SerializeRaw(document));
    return image.Id;
}

std::vector<ExcelWorksheetImage> Worksheet::Images() const
{
    std::vector<ExcelWorksheetImage> result;
    const auto drawing = m_part ? m_part->GetDrawingsPart() : nullptr;
    const auto xml = drawing ? drawing->GetXmlString() : std::string{};
    if (xml.empty())
    {
        return result;
    }

    Pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
    {
        return result;
    }

    for (auto anchor = document.document_element().first_child(); anchor; anchor = anchor.next_sibling())
    {
        if (!WorksheetContentOrderHelper::IsPictureAnchor(anchor))
        {
            continue;
        }
        auto pic = anchor.child("xdr:pic");
        auto properties = pic.child("xdr:nvPicPr").child("xdr:cNvPr");
        auto blip = pic.child("xdr:blipFill").child("a:blip");
        if (!properties || !blip)
        {
            continue;
        }

        ExcelWorksheetImage item;
        item.Id = properties.attribute("id").as_uint();
        item.Name = properties.attribute("name").as_string();
        item.Description = properties.attribute("descr").as_string();
        const std::string relationshipId = blip.attribute("r:embed").as_string();
        for (const auto& media : drawing->GetImageParts())
        {
            if (media->RelationshipId() != relationshipId)
            {
                continue;
            }
            item.Data = media->GetBinaryData();
            if (media->ContentType() == "image/jpeg")
            {
                item.Format = ExcelImageFormat::Jpeg;
            }
            else if (media->ContentType() == "image/gif")
            {
                item.Format = ExcelImageFormat::Gif;
            }
            else if (media->ContentType() == "image/bmp")
            {
                item.Format = ExcelImageFormat::Bmp;
            }
            else if (media->ContentType() == "image/tiff")
            {
                item.Format = ExcelImageFormat::Tiff;
            }
            break;
        }
        result.push_back(std::move(item));
    }
    return result;
}

bool Worksheet::RemoveImage(UInt32 id)
{
    const auto drawing = m_part ? m_part->GetDrawingsPart() : nullptr;
    const auto xml = drawing ? drawing->GetXmlString() : std::string{};
    if (xml.empty() || id == 0)
    {
        return false;
    }

    Pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
    {
        return false;
    }
    auto worksheetDrawing = document.document_element();
    if (!worksheetDrawing)
    {
        return false;
    }

    Pugi::xml_node target;
    std::string relationshipId;
    for (auto anchor = worksheetDrawing.first_child(); anchor; anchor = anchor.next_sibling())
    {
        if (!WorksheetContentOrderHelper::IsPictureAnchor(anchor))
        {
            continue;
        }
        auto pic = anchor.child("xdr:pic");
        if (pic.child("xdr:nvPicPr").child("xdr:cNvPr").attribute("id").as_uint() != id)
        {
            continue;
        }
        relationshipId = pic.child("xdr:blipFill").child("a:blip").attribute("r:embed").as_string();
        target = anchor;
        break;
    }
    if (!target)
    {
        return false;
    }
    worksheetDrawing.remove_child(target);

    for (const auto& media : drawing->GetImageParts())
    {
        if (media->RelationshipId() == relationshipId)
        {
            drawing->RemoveImagePart(media);
        }
    }

    if (!worksheetDrawing.first_child())
    {
        Detail::UnlinkWorksheetDrawing(m_part);
        return m_part->RemoveDrawingsPart();
    }

    drawing->SetXmlString(Detail::SerializeRaw(document));
    return true;
}

} // namespace ExyokiOffice::Excel
