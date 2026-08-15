// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Word/WordDocument.hpp"

#include "ExyokiOffice/Packaging/PackageUtilities.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Pictures.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Wordprocessing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2013/Word.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2019/Word/Cid.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2021/Word/CommentsExt.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <set>

namespace ExyokiOffice::Word
{

using OpenXMLElement = ExyokiOffice::OpenXMLElement;
using ExyokiOffice::BooleanValue;
using ExyokiOffice::DateTimeValue;
using ExyokiOffice::EnumValue;
using ExyokiOffice::HexBinaryValue;
using ExyokiOffice::Int16Value;
using ExyokiOffice::Int32Value;
using ExyokiOffice::Int64Value;
using ExyokiOffice::IntegerValue;
using ExyokiOffice::OnOffValue;
using ExyokiOffice::StringValue;
using ExyokiOffice::UInt32Value;

constexpr std::string_view kDrawingPictureNamespace =
    "http://schemas.openxmlformats.org/drawingml/2006/picture";
constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kOfficeRelationshipsNamespace =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kHyperlinkRelationshipType =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";

/// Document-wide identifier allocation: the next free id of each kind.
class WordIdHelper
{
public:
    static UInt32 NextDocPropertyId(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Document>& document)
    {
        UInt32 maxId = 0;
        if (!document)
        {
            return 1;
        }

        for (const auto& docProps : document->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::DocProperties>())
        {
            if (!docProps)
            {
                continue;
            }
            maxId = std::max(maxId, static_cast<UInt32>(docProps->GetId().Value()));
        }
        for (const auto& nvProps : document->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualDrawingProperties>())
        {
            if (!nvProps)
            {
                continue;
            }
            maxId = std::max(maxId, static_cast<UInt32>(nvProps->GetId().Value()));
        }

        return maxId + 1;
    }

    static int NextBookmarkId(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart,
                              const std::shared_ptr<ExyokiOffice::OpenXMLElement>& fallbackScope)
    {
        int maxId = -1;
        auto scan = [&maxId](const std::shared_ptr<ExyokiOffice::OpenXMLElement>& root)
        {
            if (!root)
            {
                return;
            }
            for (const auto& start :
                 root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>())
            {
                if (!start)
                {
                    continue;
                }
                const auto idText = start->GetId().ToString();
                int id = 0;
                const auto result = std::from_chars(idText.data(), idText.data() + idText.size(), id);
                if (result.ec == std::errc())
                {
                    maxId = std::max(maxId, id);
                }
            }
        };

        if (mainDocumentPart)
        {
            scan(mainDocumentPart->GetTypedRootElement());
        }
        else
        {
            scan(fallbackScope);
        }
        return maxId + 1;
    }

    // Rewrites a run's combined text content in place, preserving the run's own formatting
    // (`w:rPr`). Runs with more than one `<w:t>` child are normalized to a single text node.
};

/// Run text content and attribute reads shared by the editors below.
class WordRunTextHelper
{
public:
    static void SetRunPlainText(const std::shared_ptr<Run>& run, const std::string& newText)
    {
        if (!run)
        {
            return;
        }

        auto texts = run->Texts();
        if (texts.empty())
        {
            if (!newText.empty())
            {
                if (auto text = run->AddText(newText))
                {
                    text->SetPreserveSpaces(true);
                }
            }
            return;
        }

        if (newText.empty())
        {
            for (const auto& text : texts)
            {
                if (auto lowLevel = text->GetLowLevelApi())
                {
                    if (auto parent = lowLevel->Parent())
                    {
                        parent->RemoveChild(lowLevel);
                    }
                }
            }
            return;
        }

        texts.front()->SetText(newText);
        texts.front()->SetPreserveSpaces(true);
        for (Size i = 1; i < texts.size(); ++i)
        {
            if (auto lowLevel = texts[i]->GetLowLevelApi())
            {
                if (auto parent = lowLevel->Parent())
                {
                    parent->RemoveChild(lowLevel);
                }
            }
        }
    }

    static std::string TrimAsciiWhitespace(std::string value)
    {
        return std::string(AsciiText::Trim(value));
    }

    static std::string WordAttributeOrEmpty(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
                                            std::string_view localName)
    {
        if (!element)
        {
            return {};
        }

        std::string_view value;
        if (element->TryGetAttribute(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, localName), value))
        {
            return std::string(value);
        }
        if (element->TryGetAttribute(ExyokiOffice::OpenXmlQualifiedName({}, localName), value))
        {
            return std::string(value);
        }
        return {};
    }
};

/// Tracked-change markup: recognizing, unwrapping and stamping revisions.
class WordRevisionHelper
{
public:
    static bool IsRevisionName(const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        if (name.namespaceUri() != kWordNamespace)
        {
            return false;
        }
        const auto local = name.localName();
        return local == "ins" || local == "del" || local == "moveFrom" || local == "moveTo" ||
               local == "pPrChange" || local == "rPrChange" || local == "tblPrChange" ||
               local == "trPrChange" || local == "tcPrChange" || local == "sectPrChange";
    }

    static RevisionType RevisionTypeFromName(const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        if (name.namespaceUri() != kWordNamespace)
        {
            return RevisionType::Unknown;
        }
        const auto local = name.localName();
        if (local == "ins")
        {
            return RevisionType::Insertion;
        }
        if (local == "del")
        {
            return RevisionType::Deletion;
        }
        if (local == "moveFrom")
        {
            return RevisionType::MoveFrom;
        }
        if (local == "moveTo")
        {
            return RevisionType::MoveTo;
        }
        return RevisionType::Unknown;
    }

    static void CollectRevisionElements(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& root,
                                        std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>>& revisions)
    {
        if (!root)
        {
            return;
        }
        for (const auto& child : root->Children())
        {
            if (!child)
            {
                continue;
            }
            if (IsRevisionName(child->QualifiedName()))
            {
                revisions.push_back(child);
            }
            CollectRevisionElements(child, revisions);
        }
    }

    static void ConvertDeletedTextToText(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& root)
    {
        if (!root)
        {
            return;
        }
        const ExyokiOffice::OpenXmlQualifiedName deletedTextName(kWordNamespace, "delText");
        auto children = root->Children();
        for (const auto& child : children)
        {
            if (!child)
            {
                continue;
            }
            ConvertDeletedTextToText(child);
            if (child->QualifiedName() != deletedTextName)
            {
                continue;
            }
            auto parent = child->Parent();
            if (!parent)
            {
                continue;
            }
            auto text = parent->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>(child);
            if (text)
            {
                if (auto leaf = std::dynamic_pointer_cast<ExyokiOffice::OpenXmlLeafTextElement>(child))
                {
                    text->SetText(leaf->GetText());
                }
            }
            parent->RemoveChild(child);
        }
    }

    static bool UnwrapRevisionElement(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
                                      bool convertDeletedText)
    {
        if (!element)
        {
            return false;
        }
        if (convertDeletedText)
        {
            ConvertDeletedTextToText(element);
        }
        if (!element->Parent())
        {
            return false;
        }

        element->ReplaceWithChildren();
        return true;
    }

    static bool RemoveRevisionElement(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
                                      bool removeEmptyParagraph = false)
    {
        auto parent = element ? element->Parent() : nullptr;
        if (!parent || !parent->RemoveChild(element))
        {
            return false;
        }
        if (removeEmptyParagraph &&
            parent->QualifiedName() == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "p") &&
            parent->Children().empty())
        {
            if (auto grandParent = parent->Parent())
            {
                grandParent->RemoveChild(parent);
            }
        }
        return true;
    }

    static std::string NextRevisionId(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& root)
    {
        int maxId = -1;
        std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>> revisions;
        CollectRevisionElements(root, revisions);
        for (const auto& revision : revisions)
        {
            const auto idText = WordRunTextHelper::WordAttributeOrEmpty(revision, "id");
            int id = 0;
            const auto parse = std::from_chars(idText.data(), idText.data() + idText.size(), id);
            if (parse.ec == std::errc() && parse.ptr == idText.data() + idText.size())
            {
                maxId = std::max(maxId, id);
            }
        }
        return std::to_string(maxId + 1);
    }

    static void ApplyRevisionMetadata(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
                                      const RevisionAuthor& author,
                                      const std::string& fallbackId)
    {
        if (!element)
        {
            return;
        }
        element->SetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "id"),
                                   StringValue(author.Id.empty() ? fallbackId : author.Id));
        element->SetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "author"),
                                   StringValue(author.Name));
        if (!author.Date.empty())
        {
            element->SetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "date"),
                                       StringValue(author.Date));
        }
    }
};

/// Field instructions and the run structure that carries them.
class WordFieldHelper
{
public:
    static std::string FieldInstructionName(std::string_view instruction)
    {
        auto text = WordRunTextHelper::TrimAsciiWhitespace(std::string(instruction));
        if (text.empty())
        {
            return {};
        }
        if (text.front() == '=')
        {
            return "=";
        }

        Size end = 0;
        while (end < text.size())
        {
            if (AsciiText::IsSpace(text[end]) || text[end] == '\\' || text[end] == '"' ||
                text[end] == '\'')
            {
                break;
            }
            ++end;
        }
        return AsciiText::ToUpper(text.substr(0, end));
    }

    static bool IsLayoutDependentFieldInstruction(std::string_view instruction)
    {
        const auto name = FieldInstructionName(instruction);
        return name == "PAGE" || name == "NUMPAGES" || name == "SECTIONPAGES";
    }

    static ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::Value ReadFieldCharType(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>& fieldChar)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues;
        if (!fieldChar)
        {
            return FieldCharValues::NotDefinedEnumValue;
        }
        auto value = fieldChar->GetFieldCharType();
        if (value.IsDefined())
        {
            return value.Value().GetValue();
        }

        const auto raw = WordRunTextHelper::WordAttributeOrEmpty(fieldChar, "fldCharType");
        if (raw == "begin")
        {
            return FieldCharValues::Begin;
        }
        if (raw == "separate")
        {
            return FieldCharValues::Separate;
        }
        if (raw == "end")
        {
            return FieldCharValues::End;
        }
        return FieldCharValues::NotDefinedEnumValue;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run> ParentRunOfFieldChar(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>& fieldChar)
    {
        return fieldChar ? std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(
                               fieldChar->Parent())
                         : nullptr;
    }

    static std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>> ParagraphChildrenBetween(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& after,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& before)
    {
        std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>> result;
        if (!paragraph || !after || !before)
        {
            return result;
        }

        for (auto child = after->NextSibling(); child; child = child->NextSibling())
        {
            if (child->IsSameNode(*before))
            {
                break;
            }
            result.push_back(child);
        }
        return result;
    }

    static void RemoveParagraphChildrenBetween(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& after,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& before)
    {
        for (const auto& child : ParagraphChildrenBetween(paragraph, after, before))
        {
            paragraph->RemoveChild(child);
        }
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run> InsertRunWithTextBefore(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& before,
        std::string_view text,
        bool preserveSpaces)
    {
        if (!paragraph)
        {
            return nullptr;
        }
        auto run = paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(before);
        if (!run)
        {
            return nullptr;
        }
        std::make_shared<Run>(run)->AddText(text, preserveSpaces);
        return run;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run> AppendRunWithText(
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent,
        std::string_view text,
        bool preserveSpaces)
    {
        if (!parent)
        {
            return nullptr;
        }
        auto run = parent->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
        if (!run)
        {
            return nullptr;
        }
        std::make_shared<Run>(run)->AddText(text, preserveSpaces);
        return run;
    }

    static void AppendFieldCodeRun(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
        std::string_view instruction,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& before = nullptr)
    {
        if (!paragraph)
        {
            return;
        }
        auto run = before ? paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(before)
                          : paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
        if (!run)
        {
            return;
        }
        auto code = run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCode>();
        if (code)
        {
            code->SetText(instruction);
        }
    }
};

/// Measurement conversion, parsing and the DOM enum mappings.
class WordValueHelper
{
public:
    static std::string DirectLeafTextByWordName(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
                                                std::string_view localName)
    {
        std::string result;
        if (!element)
        {
            return result;
        }
        const ExyokiOffice::OpenXmlQualifiedName name(kWordNamespace, localName);
        for (const auto& child : element->Children())
        {
            if (!child || child->QualifiedName() != name)
            {
                continue;
            }
            if (auto textElement = std::dynamic_pointer_cast<ExyokiOffice::OpenXmlLeafTextElement>(child))
            {
                result += std::string(textElement->GetText());
            }
        }
        return result;
    }

    static std::string ContentTypeFromExtension(std::filesystem::path path)
    {
        auto ext = AsciiText::ToLower(path.extension().string());
        if (ext == ".png")
        {
            return "image/png";
        }
        if (ext == ".jpg" || ext == ".jpeg")
        {
            return "image/jpeg";
        }
        if (ext == ".gif")
        {
            return "image/gif";
        }
        if (ext == ".bmp")
        {
            return "image/bmp";
        }
        if (ext == ".tif" || ext == ".tiff")
        {
            return "image/tiff";
        }
        if (ext == ".emf")
        {
            return "image/x-emf";
        }
        if (ext == ".wmf")
        {
            return "image/x-wmf";
        }
        return {};
    }

    static Int64 ToEmuInt64(const ExyokiOffice::MeasuringUnits& value)
    {
        return static_cast<Int64>(std::llround(value.ToEmu().GetValue()));
    }

    static UInt32 ToEmuUInt32(const ExyokiOffice::MeasuringUnits& value)
    {
        const auto rounded = std::llround(value.ToEmu().GetValue());
        if (rounded <= 0)
        {
            return 0;
        }
        if (rounded > static_cast<Int64>(std::numeric_limits<UInt32>::max()))
        {
            return std::numeric_limits<UInt32>::max();
        }
        return static_cast<UInt32>(rounded);
    }

    static int ToTwipsInt(const ExyokiOffice::MeasuringUnits& value)
    {
        return static_cast<int>(std::lround(value.ToTw().GetValue()));
    }

    static UInt32 ToTwipsUInt32(const ExyokiOffice::MeasuringUnits& value)
    {
        const auto rounded = std::llround(value.ToTw().GetValue());
        if (rounded <= 0)
        {
            return 0;
        }
        if (rounded > static_cast<Int64>(std::numeric_limits<UInt32>::max()))
        {
            return std::numeric_limits<UInt32>::max();
        }
        return static_cast<UInt32>(rounded);
    }

    static UInt32 ToBorderSizeUInt32(const ExyokiOffice::MeasuringUnits& value)
    {
        const auto points = value.ToPt().GetValue();
        if (points <= 0.0)
        {
            return 0;
        }
        const auto eighths = std::llround(points * 8.0);
        if (eighths <= 0)
        {
            return 0;
        }
        if (eighths > static_cast<Int64>(std::numeric_limits<UInt32>::max()))
        {
            return std::numeric_limits<UInt32>::max();
        }
        return static_cast<UInt32>(eighths);
    }

    static std::optional<int> TryParseInt(std::string_view text)
    {
        if (text.empty())
        {
            return std::nullopt;
        }

        int value = 0;
        const auto* begin = text.data();
        const auto* end = text.data() + text.size();
        auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc() || result.ptr != end)
        {
            return std::nullopt;
        }
        return value;
    }

    static std::optional<Int64> TryParseInt64(std::string_view text)
    {
        if (text.empty())
        {
            return std::nullopt;
        }

        Int64 value = 0;
        const auto* begin = text.data();
        const auto* end = text.data() + text.size();
        auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc() || result.ptr != end)
        {
            return std::nullopt;
        }
        return value;
    }

    template <typename TEnum>
    static std::optional<TEnum> TryParseEnumValue(std::string_view text)
    {
        EnumValue<TEnum> parsed;
        if (!parsed.AssignFromString(text))
        {
            return std::nullopt;
        }
        if (!parsed.IsDefined())
        {
            return std::nullopt;
        }
        return parsed.Value();
    }

    static std::optional<int> GetDefinedInt32(const Int32Value& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        return value.Value();
    }

    static std::optional<ExyokiOffice::MeasuringUnits> GetDefinedTwips(const StringValue& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        const auto parsed = TryParseInt(value.ToString());
        if (!parsed)
        {
            return std::nullopt;
        }
        return ExyokiOffice::MeasuringUnits(static_cast<Real>(*parsed),
                                            ExyokiOffice::MeasurementUnit::Twip);
    }

    static std::optional<ExyokiOffice::MeasuringUnits> GetDefinedTwips(const Int32Value& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        return ExyokiOffice::MeasuringUnits(static_cast<Real>(value.Value()),
                                            ExyokiOffice::MeasurementUnit::Twip);
    }

    static std::optional<ExyokiOffice::MeasuringUnits> GetDefinedTwips(const UInt32Value& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        return ExyokiOffice::MeasuringUnits(static_cast<Real>(value.Value()),
                                            ExyokiOffice::MeasurementUnit::Twip);
    }

    static std::optional<ExyokiOffice::MeasuringUnits> GetDefinedHalfPoints(const StringValue& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        const auto parsed = TryParseInt(value.ToString());
        if (!parsed)
        {
            return std::nullopt;
        }
        return ExyokiOffice::MeasuringUnits(static_cast<Real>(*parsed) / 2.0,
                                            ExyokiOffice::MeasurementUnit::Point);
    }

    static std::optional<ExyokiOffice::MeasuringUnits> GetDefinedHalfPoints(const UInt32Value& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        return ExyokiOffice::MeasuringUnits(static_cast<Real>(value.Value()) / 2.0,
                                            ExyokiOffice::MeasurementUnit::Point);
    }

    static ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionMarkValues ToDomSectionMark(
        SectionStartType startType)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionMarkValues;
        switch (startType)
        {
            case SectionStartType::NextColumn:
                return SectionMarkValues::NextColumn;
            case SectionStartType::Continuous:
                return SectionMarkValues::Continuous;
            case SectionStartType::EvenPage:
                return SectionMarkValues::EvenPage;
            case SectionStartType::OddPage:
                return SectionMarkValues::OddPage;
            case SectionStartType::NextPage:
            default:
                return SectionMarkValues::NextPage;
        }
    }

    static std::optional<SectionStartType> FromDomSectionMark(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionMarkValues value)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionMarkValues;
        switch (value.GetValue())
        {
            case SectionMarkValues::NextPage:
                return SectionStartType::NextPage;
            case SectionMarkValues::NextColumn:
                return SectionStartType::NextColumn;
            case SectionMarkValues::Continuous:
                return SectionStartType::Continuous;
            case SectionMarkValues::EvenPage:
                return SectionStartType::EvenPage;
            case SectionMarkValues::OddPage:
                return SectionStartType::OddPage;
            default:
                return std::nullopt;
        }
    }

    static ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues ToDomPageOrientation(
        PageOrientation orientation)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues;
        return orientation == PageOrientation::Landscape ? PageOrientationValues::Landscape
                                                         : PageOrientationValues::Portrait;
    }

    static std::optional<PageOrientation> FromDomPageOrientation(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues value)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues;
        switch (value.GetValue())
        {
            case PageOrientationValues::Portrait:
                return PageOrientation::Portrait;
            case PageOrientationValues::Landscape:
                return PageOrientation::Landscape;
            default:
                return std::nullopt;
        }
    }
};

/// The main document part, its body, and the markers inside it.
class WordStructureHelper
{
public:
    template <typename TParent, typename TChild>
    static std::shared_ptr<TChild> EnsureChildOfType(const std::shared_ptr<TParent>& parent)
    {
        if (!parent)
        {
            return nullptr;
        }
        auto child = parent->template GetFirstChildOfType<TChild>();
        if (!child)
        {
            child = parent->template AppendChild<TChild>();
        }
        return child;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body> EnsureBody(const std::shared_ptr<Packaging::MainDocumentPart>& mainPart)
    {
        if (!mainPart)
        {
            return nullptr;
        }
        auto document = mainPart->GetTypedRootElement();
        if (!document)
        {
            return nullptr;
        }
        return EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Document, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>(document);
    }

    static std::shared_ptr<Packaging::MainDocumentPart> EnsureMainDocumentPart(const WordDocument::Ptr& document)
    {
        if (!document)
        {
            return nullptr;
        }

        auto mainPart = document->GetMainDocumentPart();
        if (!mainPart)
        {
            mainPart = document->AddMainDocumentPart();
        }
        return mainPart;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body> EnsureBody(const WordDocument::Ptr& document)
    {
        return EnsureBody(EnsureMainDocumentPart(document));
    }

    static std::shared_ptr<Packaging::MainDocumentPart> GetMainDocumentPart(const WordDocument::Ptr& document)
    {
        return document ? document->GetMainDocumentPart() : nullptr;
    }

    // Removes a markup marker element. When the marker's direct parent is a run whose sole
    // purpose is to carry that marker (the shape produced by AddFootnote()/AddComment() for
    // reference markers), the whole run is removed; otherwise only the marker itself is
    // removed (the shape used by comment range markers, which are direct paragraph children).
    static void RemoveMarkerAndOwningRun(const std::shared_ptr<OpenXMLElement>& marker)
    {
        if (!marker)
        {
            return;
        }
        auto parent = marker->Parent();
        if (!parent)
        {
            return;
        }
        if (parent->QualifiedName() == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "r"))
        {
            if (auto grandParent = parent->Parent())
            {
                grandParent->RemoveChild(parent);
                return;
            }
        }
        parent->RemoveChild(marker);
    }

    static int NextSdtId(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart,
                         const std::shared_ptr<OpenXMLElement>& fallbackScope)
    {
        int maxId = 0;
        auto scan = [&maxId](const std::shared_ptr<OpenXMLElement>& root)
        {
            if (!root)
            {
                return;
            }
            for (const auto& id :
                 root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtId>())
            {
                if (!id)
                {
                    continue;
                }
                const auto value = id->GetVal();
                if (value.IsDefined())
                {
                    maxId = std::max(maxId, value.Value());
                }
            }
        };

        if (mainDocumentPart)
        {
            scan(mainDocumentPart->GetTypedRootElement());
        }
        else
        {
            scan(fallbackScope);
        }
        return maxId + 1;
    }

    // Footnote/endnote entries (`w:footnote`/`w:endnote`) share their element name with an
    // unrelated special-reference type (`FootnoteSpecialReference`/`EndnoteSpecialReference`,
    // used inside `w:footnotePr`/`w:endnotePr`). The generated element factory can only bind one
    // concrete C++ type per element name, so re-reading existing entries via `Elements<TEntry>()`
    // or `Descendants<TEntry>()` is unreliable: it silently returns nothing when the factory
    // picks the other type for that tag. All entry scanning below therefore matches children by
    // qualified name and reads/writes the `id`/`type` attributes generically instead of going
    // through Footnote/Endnote's typed accessors. Freshly creating an entry with
    // `AppendChild<TEntry>()` is unaffected by this and remains safe, since it constructs the
    // exact requested type directly rather than resolving it from a tag name.
};

/// Footnotes, endnotes and comments as parts and numbered entries.
class WordNoteHelper
{
public:
    template <typename TEntry>
    static std::vector<std::shared_ptr<OpenXMLElement>> FindNoteEntries(const std::shared_ptr<OpenXMLElement>& root)
    {
        std::vector<std::shared_ptr<OpenXMLElement>> result;
        if (!root)
        {
            return result;
        }
        const auto entryName = TEntry::StaticMetaClass()->QualifiedName();
        for (const auto& child : root->Children())
        {
            if (child && child->QualifiedName() == entryName)
            {
                result.push_back(child);
            }
        }
        return result;
    }

    // Ensures the standard Separator/ContinuationSeparator bookkeeping entries (IDs -1 and 0)
    // exist in a footnotes/endnotes part root, matching what Word itself generates.
    template <typename TEntry>
    static void EnsureNoteSeparators(const std::shared_ptr<OpenXMLElement>& root)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues;

        if (!root)
        {
            return;
        }

        const ExyokiOffice::OpenXmlQualifiedName typeAttribute(kWordNamespace, "type");

        bool hasSeparator = false;
        bool hasContinuation = false;
        for (const auto& entry : FindNoteEntries<TEntry>(root))
        {
            const auto type = entry->template GetAttributeValue<EnumValue<FootnoteEndnoteValues>>(typeAttribute);
            if (!type.IsDefined())
            {
                continue;
            }
            if (type.Value() == FootnoteEndnoteValues::Separator)
            {
                hasSeparator = true;
            }
            else if (type.Value() == FootnoteEndnoteValues::ContinuationSeparator)
            {
                hasContinuation = true;
            }
        }

        if (!hasSeparator)
        {
            if (auto entry = root->AppendChild<TEntry>())
            {
                entry->SetId(IntegerValue(static_cast<Int64>(-1)));
                entry->SetType(EnumValue<FootnoteEndnoteValues>(FootnoteEndnoteValues::Separator));
                if (auto paragraph =
                        entry->template AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
                {
                    if (auto run =
                            paragraph->template AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
                    {
                        run->template AppendChild<
                            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SeparatorMark>();
                    }
                }
            }
        }

        if (!hasContinuation)
        {
            if (auto entry = root->AppendChild<TEntry>())
            {
                entry->SetId(IntegerValue(static_cast<Int64>(0)));
                entry->SetType(EnumValue<FootnoteEndnoteValues>(FootnoteEndnoteValues::ContinuationSeparator));
                if (auto paragraph =
                        entry->template AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
                {
                    if (auto run =
                            paragraph->template AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
                    {
                        run->template AppendChild<
                            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ContinuationSeparatorMark>();
                    }
                }
            }
        }
    }

    template <typename TEntry>
    static int NextNoteId(const std::shared_ptr<OpenXMLElement>& root)
    {
        const ExyokiOffice::OpenXmlQualifiedName idAttribute(kWordNamespace, "id");
        int maxId = -2;
        for (const auto& entry : FindNoteEntries<TEntry>(root))
        {
            const auto value = entry->template GetAttributeValue<IntegerValue>(idAttribute);
            if (value.IsDefined())
            {
                maxId = std::max(maxId, static_cast<int>(value.Value()));
            }
        }
        return maxId + 1;
    }

    // Creates a new footnote/endnote entry (with the standard reference mark) and appends the
    // matching reference run at the end of `paragraph`. TEntry is Footnote or Endnote,
    // TReferenceMark is FootnoteReferenceMark or EndnoteReferenceMark, TReference is
    // FootnoteReference or EndnoteReference, and TPart is FootnotesPart or EndnotesPart.
    template <typename TEntry, typename TReferenceMark, typename TReference, typename TPart>
    static std::shared_ptr<Note> AddNoteToDocument(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
        const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart,
        NoteKind kind,
        const std::shared_ptr<TPart>& part,
        std::string_view text,
        bool preserveSpaces)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues;

        if (!paragraph || !part)
        {
            return nullptr;
        }

        std::shared_ptr<OpenXMLElement> root = part->GetTypedRootElement();
        if (!root)
        {
            return nullptr;
        }

        EnsureNoteSeparators<TEntry>(root);
        const int id = NextNoteId<TEntry>(root);

        auto entry = root->AppendChild<TEntry>();
        if (!entry)
        {
            return nullptr;
        }
        entry->SetId(IntegerValue(static_cast<Int64>(id)));
        entry->SetType(EnumValue<FootnoteEndnoteValues>(FootnoteEndnoteValues::Normal));

        auto contentParagraph =
            entry->template AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
        if (!contentParagraph)
        {
            root->RemoveChild(entry);
            return nullptr;
        }
        if (auto markRun =
                contentParagraph->template AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
        {
            markRun->template AppendChild<TReferenceMark>();
        }
        if (!text.empty())
        {
            WordFieldHelper::AppendRunWithText(contentParagraph, text, preserveSpaces);
        }

        auto refRun = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
        if (!refRun)
        {
            root->RemoveChild(entry);
            return nullptr;
        }
        auto reference = refRun->AppendChild<TReference>();
        if (!reference)
        {
            paragraph->RemoveChild(refRun);
            root->RemoveChild(entry);
            return nullptr;
        }
        reference->SetId(IntegerValue(static_cast<Int64>(id)));

        return std::make_shared<Note>(kind, entry, mainDocumentPart);
    }

    static int NextCommentId(const std::shared_ptr<Packaging::WordprocessingCommentsPart>& part)
    {
        int maxId = -1;
        if (part)
        {
            if (auto root = part->GetTypedRootElement())
            {
                for (const auto& entry :
                     root->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>())
                {
                    if (!entry)
                    {
                        continue;
                    }
                    if (auto parsed = WordValueHelper::TryParseInt(entry->GetId().ToString()))
                    {
                        maxId = std::max(maxId, *parsed);
                    }
                }
            }
        }
        return maxId + 1;
    }
};

/// The style definitions part and the style-type mapping.
class WordStylePartHelper
{
public:
    static std::shared_ptr<Packaging::StyleDefinitionsPart> EnsureStyleDefinitionsPart(const WordDocument::Ptr& document)
    {
        auto mainPart = WordStructureHelper::EnsureMainDocumentPart(document);
        if (!mainPart)
        {
            return nullptr;
        }

        auto stylesPart = mainPart->GetStyleDefinitionsPart();
        if (!stylesPart)
        {
            stylesPart = mainPart->AddStyleDefinitionsPart();
        }
        return stylesPart;
    }

    static std::shared_ptr<Packaging::StyleDefinitionsPart> GetStyleDefinitionsPart(const WordDocument::Ptr& document)
    {
        auto mainPart = WordStructureHelper::GetMainDocumentPart(document);
        return mainPart ? mainPart->GetStyleDefinitionsPart() : nullptr;
    }

    static ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues ToDomStyleType(StyleType type)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues;
        switch (type)
        {
            case StyleType::Character:
                return StyleValues::Character;
            case StyleType::Table:
                return StyleValues::Table;
            case StyleType::Numbering:
                return StyleValues::Numbering;
            case StyleType::Paragraph:
            default:
                return StyleValues::Paragraph;
        }
    }

    static std::optional<StyleType> FromDomStyleType(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues value)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues;
        switch (value.GetValue())
        {
            case StyleValues::Paragraph:
                return StyleType::Paragraph;
            case StyleValues::Character:
                return StyleType::Character;
            case StyleValues::Table:
                return StyleType::Table;
            case StyleValues::Numbering:
                return StyleType::Numbering;
            default:
                return std::nullopt;
        }
    }
};

/// Reading and writing typed property children of a Word element.
class WordPropertyReadHelper
{
public:
    static bool IsOn(const OnOffValue& value)
    {
        return value.ValueOr(false);
    }

    static std::optional<bool> OptionalOnOff(const OnOffValue& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        return value.Value();
    }

    static std::optional<int> OptionalInt32(const Int32Value& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        return value.Value();
    }

    static std::string StringValueOrEmpty(const StringValue& value)
    {
        return value.IsDefined() ? value.ToString() : std::string{};
    }

    static std::string RawValAttributeOrEmpty(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element)
    {
        if (!element)
        {
            return {};
        }

        std::string_view value;
        if (element->TryGetAttribute(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "val"), value))
        {
            return std::string(value);
        }
        if (element->TryGetAttribute(ExyokiOffice::OpenXmlQualifiedName({}, "val"), value))
        {
            return std::string(value);
        }
        return {};
    }

    static std::optional<bool> RawOnOffOrEmpty(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element)
    {
        const auto value = AsciiText::ToLower(RawValAttributeOrEmpty(element));
        if (value.empty())
        {
            return std::nullopt;
        }
        if (value == "1" || value == "true" || value == "on")
        {
            return true;
        }
        if (value == "0" || value == "false" || value == "off")
        {
            return false;
        }
        return std::nullopt;
    }

    template <typename TElement>
    static std::optional<bool> ReadOnOffChild(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent)
    {
        std::shared_ptr<ExyokiOffice::OpenXMLElement> child = parent ? parent->GetFirstChildOfType<TElement>() : nullptr;
        if (!child && parent)
        {
            const auto target = TElement::StaticMetaClass()->QualifiedName();
            for (const auto& candidate : parent->Children())
            {
                if (candidate && candidate->QualifiedName() == target)
                {
                    child = candidate;
                    break;
                }
            }
        }
        if (!child)
        {
            return std::nullopt;
        }
        if (auto typed = std::dynamic_pointer_cast<TElement>(child); typed && typed->GetVal().IsDefined())
        {
            return typed->GetVal().Value();
        }
        if (auto raw = RawOnOffOrEmpty(child))
        {
            return raw;
        }
        return true;
    }

    template <typename TElement, typename TEnum>
    static std::optional<TEnum> ReadEnumValChild(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent)
    {
        std::shared_ptr<ExyokiOffice::OpenXMLElement> child = parent ? parent->GetFirstChildOfType<TElement>() : nullptr;
        if (!child && parent)
        {
            const auto target = TElement::StaticMetaClass()->QualifiedName();
            for (const auto& candidate : parent->Children())
            {
                if (candidate && candidate->QualifiedName() == target)
                {
                    child = candidate;
                    break;
                }
            }
        }
        if (!child)
        {
            return std::nullopt;
        }
        if (auto typed = std::dynamic_pointer_cast<TElement>(child))
        {
            auto value = typed->GetVal();
            if (value.IsDefined())
            {
                return value.Value();
            }
        }
        return WordValueHelper::TryParseEnumValue<TEnum>(RawValAttributeOrEmpty(child));
    }

    static std::shared_ptr<ExyokiOffice::OpenXMLElement> FindDirectWordChild(
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent,
        std::string_view localName)
    {
        if (!parent)
        {
            return nullptr;
        }

        const ExyokiOffice::OpenXmlQualifiedName target(kWordNamespace, localName);
        for (const auto& child : parent->Children())
        {
            if (child && child->QualifiedName() == target)
            {
                return child;
            }
        }
        return nullptr;
    }

    static std::string GetStringChildValueByName(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent,
                                                 std::string_view localName)
    {
        return RawValAttributeOrEmpty(FindDirectWordChild(parent, localName));
    }

    static void RemoveDirectWordChildren(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent,
                                         std::string_view localName)
    {
        if (!parent)
        {
            return;
        }

        const ExyokiOffice::OpenXmlQualifiedName target(kWordNamespace, localName);
        for (const auto& child : parent->Children())
        {
            if (child && child->QualifiedName() == target)
            {
                parent->RemoveChild(child);
            }
        }
    }

    template <typename TParent, typename TChild>
    static void RemoveChildOfType(const std::shared_ptr<TParent>& parent)
    {
        if (!parent)
        {
            return;
        }
        if (auto child = parent->template GetFirstChildOfType<TChild>())
        {
            parent->RemoveChild(child);
            return;
        }
        RemoveDirectWordChildren(parent, TChild::StaticMetaClass()->QualifiedName().localName());
    }

    template <typename TParent, typename TChild>
    static void RemoveChildOfType(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent)
    {
        (void)sizeof(TParent);
        if (!parent)
        {
            return;
        }
        if (auto child = parent->GetFirstChildOfType<TChild>())
        {
            parent->RemoveChild(child);
            return;
        }
        RemoveDirectWordChildren(parent, TChild::StaticMetaClass()->QualifiedName().localName());
    }

    template <typename TChild>
    static std::shared_ptr<TChild> GetFirstTypedChildOrNull(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent)
    {
        return parent ? parent->GetFirstChildOfType<TChild>() : nullptr;
    }

    template <typename TChild>
    static std::shared_ptr<ExyokiOffice::OpenXMLElement> GetFirstChildElementByTypeOrName(
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent)
    {
        if (!parent)
        {
            return nullptr;
        }
        auto typed = parent->GetFirstChildOfType<TChild>();
        return typed ? std::static_pointer_cast<ExyokiOffice::OpenXMLElement>(typed)
                     : FindDirectWordChild(parent, TChild::StaticMetaClass()->QualifiedName().localName());
    }

    template <typename TChild>
    static std::string GetStringChildValue(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& parent)
    {
        std::shared_ptr<ExyokiOffice::OpenXMLElement> child = parent ? parent->GetFirstChildOfType<TChild>() : nullptr;
        if (!child && parent)
        {
            child = FindDirectWordChild(parent, TChild::StaticMetaClass()->QualifiedName().localName());
        }
        auto typed = std::dynamic_pointer_cast<TChild>(child);
        auto value = typed ? StringValueOrEmpty(typed->GetVal()) : std::string{};
        return value.empty() ? RawValAttributeOrEmpty(child) : value;
    }

    template <typename TParent, typename TChild>
    static void SetOptionalStringChild(const std::shared_ptr<TParent>& parent, std::string_view value)
    {
        if (!parent)
        {
            return;
        }
        const auto childName = TChild::StaticMetaClass()->QualifiedName();
        RemoveDirectWordChildren(parent, childName.localName());
        if (value.empty())
        {
            return;
        }

        // SetVal writes the namespace-qualified `w:val`; adding a raw prefixed
        // attribute on top of it would emit a duplicate and break well-formedness.
        auto child = parent->template AppendChild<TChild>();
        if (child)
        {
            child->SetVal(StringValue(std::string(value)));
        }
    }
};

/// Style definitions: lookup, read, apply, and unique identifiers.
class WordStyleDefinitionHelper
{
public:
    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style> FindStyleById(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Styles>& styles,
        std::string_view styleId)
    {
        if (!styles || styleId.empty())
        {
            return nullptr;
        }

        for (const auto& style : styles->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>())
        {
            if (style && WordPropertyReadHelper::StringValueOrEmpty(style->GetStyleId()) == styleId)
            {
                return style;
            }
        }
        return nullptr;
    }

    static StyleDefinition ReadStyleDefinition(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>& style)
    {
        StyleDefinition definition{};
        if (!style)
        {
            return definition;
        }

        definition.StyleId = WordPropertyReadHelper::StringValueOrEmpty(style->GetStyleId());
        definition.Type = WordStylePartHelper::FromDomStyleType(style->GetType().ValueOr(
                                                                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues::Paragraph))
                              .value_or(StyleType::Paragraph);
        definition.IsDefault = WordPropertyReadHelper::IsOn(style->GetDefault());
        definition.IsCustom = WordPropertyReadHelper::IsOn(style->GetCustomStyle());

        using namespace ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
        definition.Name = WordPropertyReadHelper::GetStringChildValueByName(style, "name");
        definition.BasedOnStyleId = WordPropertyReadHelper::GetStringChildValueByName(style, "basedOn");
        definition.NextStyleId = WordPropertyReadHelper::GetStringChildValueByName(style, "next");
        definition.LinkedStyleId = WordPropertyReadHelper::GetStringChildValueByName(style, "link");
        definition.Aliases = WordPropertyReadHelper::GetStringChildValueByName(style, "aliases");
        if (auto priority = style->GetFirstChildOfType<UIPriority>())
        {
            definition.UiPriority = WordPropertyReadHelper::OptionalInt32(priority->GetVal());
        }
        definition.IsPrimary = style->GetFirstChildOfType<PrimaryStyle>() != nullptr;
        definition.IsSemiHidden = style->GetFirstChildOfType<SemiHidden>() != nullptr;
        definition.IsUnhideWhenUsed = style->GetFirstChildOfType<UnhideWhenUsed>() != nullptr;
        return definition;
    }

    static void ApplyStyleDefinition(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>& style,
        const StyleDefinition& definition)
    {
        if (!style || definition.StyleId.empty())
        {
            return;
        }

        using namespace ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
        style->SetType(EnumValue<StyleValues>(WordStylePartHelper::ToDomStyleType(definition.Type)));
        style->SetStyleId(StringValue(definition.StyleId));
        style->SetDefault(definition.IsDefault ? OnOffValue(true) : OnOffValue{});
        style->SetCustomStyle(definition.IsCustom ? OnOffValue(true) : OnOffValue{});

        WordPropertyReadHelper::SetOptionalStringChild<Style, StyleName>(style, definition.Name);
        WordPropertyReadHelper::SetOptionalStringChild<Style, BasedOn>(style, definition.BasedOnStyleId);
        WordPropertyReadHelper::SetOptionalStringChild<Style, NextParagraphStyle>(style, definition.NextStyleId);
        WordPropertyReadHelper::SetOptionalStringChild<Style, LinkedStyle>(style, definition.LinkedStyleId);
        WordPropertyReadHelper::SetOptionalStringChild<Style, Aliases>(style, definition.Aliases);

        if (definition.UiPriority)
        {
            if (auto priority = WordStructureHelper::EnsureChildOfType<Style, UIPriority>(style))
            {
                priority->SetVal(Int32Value(*definition.UiPriority));
            }
        }
        else
        {
            WordPropertyReadHelper::RemoveChildOfType<Style, UIPriority>(style);
        }

        if (definition.IsPrimary)
        {
            WordStructureHelper::EnsureChildOfType<Style, PrimaryStyle>(style);
        }
        else
        {
            WordPropertyReadHelper::RemoveChildOfType<Style, PrimaryStyle>(style);
        }
        if (definition.IsSemiHidden)
        {
            WordStructureHelper::EnsureChildOfType<Style, SemiHidden>(style);
        }
        else
        {
            WordPropertyReadHelper::RemoveChildOfType<Style, SemiHidden>(style);
        }
        if (definition.IsUnhideWhenUsed)
        {
            WordStructureHelper::EnsureChildOfType<Style, UnhideWhenUsed>(style);
        }
        else
        {
            WordPropertyReadHelper::RemoveChildOfType<Style, UnhideWhenUsed>(style);
        }
    }

    static std::string MakeUniqueStyleId(const StyleManager& manager, std::string base)
    {
        if (base.empty())
        {
            base = "ImportedStyle";
        }
        if (!manager.HasStyle(base))
        {
            return base;
        }

        for (int suffix = 2; suffix < 10000; ++suffix)
        {
            auto candidate = base + "_" + std::to_string(suffix);
            if (!manager.HasStyle(candidate))
            {
                return candidate;
            }
        }
        return {};
    }
};

/// Body traversal and section-properties lookup.
class WordBodyHelper
{
public:
    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Document> GetMainDocumentRoot(
        const WordDocument::Ptr& document)
    {
        auto mainPart = WordStructureHelper::GetMainDocumentPart(document);
        return mainPart ? mainPart->GetTypedRootElement() : nullptr;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body> GetBody(
        const WordDocument::Ptr& document)
    {
        auto root = GetMainDocumentRoot(document);
        return root ? root->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>() : nullptr;
    }

    static void CollectDescendantsByName(const std::shared_ptr<OpenXMLElement>& element,
                                         const ExyokiOffice::OpenXmlQualifiedName& name,
                                         std::vector<std::shared_ptr<OpenXMLElement>>& output)
    {
        if (!element)
        {
            return;
        }

        for (const auto& child : element->Children())
        {
            if (!child)
            {
                continue;
            }
            if (child->QualifiedName() == name)
            {
                output.push_back(child);
            }
            CollectDescendantsByName(child, name, output);
        }
    }

    static std::shared_ptr<OpenXMLElement> FindTrailingSectionProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>& body)
    {
        if (!body)
        {
            return nullptr;
        }

        auto children = body->Children();
        if (children.empty())
        {
            return nullptr;
        }

        auto last = children.back();
        if (last && last->QualifiedName() == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sectPr"))
        {
            return last;
        }
        return nullptr;
    }

    static std::shared_ptr<OpenXMLElement> FindFirstBodyChild(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>& body)
    {
        if (!body)
        {
            return nullptr;
        }

        auto children = body->Children();
        if (children.empty())
        {
            return nullptr;
        }
        return children.front();
    }

    static std::shared_ptr<OpenXMLElement> FindFirstChildByName(
        const std::shared_ptr<OpenXMLElement>& element,
        const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        if (!element)
        {
            return nullptr;
        }

        for (const auto& child : element->Children())
        {
            if (child && child->QualifiedName() == name)
            {
                return child;
            }
        }
        return nullptr;
    }
};

/// Header and footer references, their parts and relationships.
class WordHeaderFooterHelper
{
public:
    static ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterValues ToDomHeaderFooterType(
        HeaderFooterType type)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterValues;
        switch (type)
        {
            case HeaderFooterType::Even:
                return HeaderFooterValues::Even;
            case HeaderFooterType::First:
                return HeaderFooterValues::First;
            case HeaderFooterType::Default:
            default:
                return HeaderFooterValues::Default;
        }
    }

    static std::optional<HeaderFooterType> FromDomHeaderFooterType(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterValues value)
    {
        using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterValues;
        switch (value.GetValue())
        {
            case HeaderFooterValues::Default:
                return HeaderFooterType::Default;
            case HeaderFooterValues::Even:
                return HeaderFooterType::Even;
            case HeaderFooterValues::First:
                return HeaderFooterType::First;
            default:
                return std::nullopt;
        }
    }

    static std::optional<HeaderFooterType> TryParseHeaderFooterTypeString(std::string value)
    {
        value = AsciiText::ToLower(std::move(value));
        if (value == "default")
        {
            return HeaderFooterType::Default;
        }
        if (value == "even")
        {
            return HeaderFooterType::Even;
        }
        if (value == "first")
        {
            return HeaderFooterType::First;
        }
        return std::nullopt;
    }

    static std::optional<HeaderFooterType> GetHeaderFooterReferenceType(
        const std::shared_ptr<OpenXMLElement>& reference)
    {
        if (!reference)
        {
            return std::nullopt;
        }

        if (auto typed = std::dynamic_pointer_cast<
                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterReferenceType>(reference))
        {
            auto value = typed->GetType();
            if (value.IsDefined())
            {
                if (auto parsed = FromDomHeaderFooterType(value.Value()))
                {
                    return parsed;
                }
                if (auto parsed = TryParseHeaderFooterTypeString(value.ToString()))
                {
                    return parsed;
                }
            }
        }

        EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterValues> rawValue;
        if (reference->TryGetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "type"),
                                            rawValue) &&
            rawValue.IsDefined())
        {
            if (auto parsed = FromDomHeaderFooterType(rawValue.Value()))
            {
                return parsed;
            }
            if (auto parsed = TryParseHeaderFooterTypeString(rawValue.ToString()))
            {
                return parsed;
            }
        }

        StringValue rawString;
        if (reference->TryGetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "type"),
                                            rawString) &&
            rawString.IsDefined())
        {
            return TryParseHeaderFooterTypeString(rawString.ToString());
        }

        return std::nullopt;
    }

    static std::string GetRelationshipId(const std::shared_ptr<OpenXMLElement>& reference)
    {
        if (!reference)
        {
            return {};
        }

        std::string_view raw;
        if (reference->TryGetAttribute(ExyokiOffice::OpenXmlQualifiedName(kOfficeRelationshipsNamespace, "id"),
                                       raw))
        {
            return std::string(raw);
        }

        if (auto typed = std::dynamic_pointer_cast<
                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterReferenceType>(reference))
        {
            auto value = typed->GetId();
            if (value.IsDefined())
            {
                return value.ToString();
            }
        }

        StringValue value;
        if (reference->TryGetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kOfficeRelationshipsNamespace, "id"),
                                            value) &&
            value.IsDefined())
        {
            return value.ToString();
        }
        return {};
    }

    static bool IsHeaderReferenceName(const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        return name == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "headerReference");
    }

    static bool IsFooterReferenceName(const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        return name == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "footerReference");
    }

    static bool IsHeaderFooterReferenceName(const ExyokiOffice::OpenXmlQualifiedName& name)
    {
        return IsHeaderReferenceName(name) || IsFooterReferenceName(name);
    }

    static std::shared_ptr<OpenXMLElement> FindHeaderFooterReference(
        const std::shared_ptr<OpenXMLElement>& sectionProperties,
        bool header,
        HeaderFooterType type)
    {
        if (!sectionProperties)
        {
            return nullptr;
        }

        for (const auto& child : sectionProperties->Children())
        {
            if (!child)
            {
                continue;
            }
            const auto& name = child->QualifiedName();
            if ((header && !IsHeaderReferenceName(name)) || (!header && !IsFooterReferenceName(name)))
            {
                continue;
            }
            if (GetHeaderFooterReferenceType(child).value_or(HeaderFooterType::Default) == type)
            {
                return child;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<OpenXMLElement> FindHeaderFooterReferenceInsertBefore(
        const std::shared_ptr<OpenXMLElement>& sectionProperties)
    {
        if (!sectionProperties)
        {
            return nullptr;
        }

        for (const auto& child : sectionProperties->Children())
        {
            if (child && !IsHeaderFooterReferenceName(child->QualifiedName()))
            {
                return child;
            }
        }
        return nullptr;
    }

    template <typename TReference>
    static std::shared_ptr<TReference> AppendHeaderFooterReference(
        const std::shared_ptr<OpenXMLElement>& sectionProperties,
        HeaderFooterType type,
        std::string_view relationshipId)
    {
        if (!sectionProperties)
        {
            return nullptr;
        }

        auto reference = sectionProperties->InsertChild<TReference>(
            FindHeaderFooterReferenceInsertBefore(sectionProperties));
        if (!reference)
        {
            return nullptr;
        }
        // The typed setters already write `w:type` and a namespace-qualified `r:id`;
        // writing them again as raw prefixed names would emit a second, literally
        // named attribute and leave the part malformed.
        reference->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderFooterValues>(
            ToDomHeaderFooterType(type)));
        reference->SetId(StringValue(std::string(relationshipId)));
        return reference;
    }

    static std::shared_ptr<Packaging::HeaderPart> FindHeaderPartByRelationshipId(
        const std::shared_ptr<Packaging::MainDocumentPart>& mainPart,
        std::string_view relationshipId)
    {
        if (!mainPart || relationshipId.empty())
        {
            return nullptr;
        }

        for (const auto& part : mainPart->GetHeaderParts())
        {
            if (!part)
            {
                continue;
            }
            if (part->RelationshipId() == relationshipId)
            {
                return part;
            }
            for (const auto& incoming : part->IncomingRelationships())
            {
                if (incoming.Id == relationshipId)
                {
                    return part;
                }
            }
        }
        return nullptr;
    }

    static std::shared_ptr<Packaging::FooterPart> FindFooterPartByRelationshipId(
        const std::shared_ptr<Packaging::MainDocumentPart>& mainPart,
        std::string_view relationshipId)
    {
        if (!mainPart || relationshipId.empty())
        {
            return nullptr;
        }

        for (const auto& part : mainPart->GetFooterParts())
        {
            if (!part)
            {
                continue;
            }
            if (part->RelationshipId() == relationshipId)
            {
                return part;
            }
            for (const auto& incoming : part->IncomingRelationships())
            {
                if (incoming.Id == relationshipId)
                {
                    return part;
                }
            }
        }
        return nullptr;
    }

    static Size CountSectionReferencesToRelationship(
        const std::shared_ptr<Packaging::MainDocumentPart>& mainPart,
        const std::string& relationshipId)
    {
        if (!mainPart || relationshipId.empty())
        {
            return 0;
        }

        auto root = mainPart->GetTypedRootElement();
        if (!root)
        {
            return 0;
        }

        std::vector<std::shared_ptr<OpenXMLElement>> sections;
        WordBodyHelper::CollectDescendantsByName(root, ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sectPr"), sections);

        Size count = 0;
        for (const auto& section : sections)
        {
            if (!section)
            {
                continue;
            }
            for (const auto& child : section->Children())
            {
                if (!child || !IsHeaderFooterReferenceName(child->QualifiedName()))
                {
                    continue;
                }
                if (GetRelationshipId(child) == relationshipId)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    static bool RemoveHeaderFooterReference(
        const std::shared_ptr<OpenXMLElement>& sectionProperties,
        const std::shared_ptr<Packaging::MainDocumentPart>& mainPart,
        bool header,
        HeaderFooterType type)
    {
        auto reference = FindHeaderFooterReference(sectionProperties, header, type);
        if (!sectionProperties || !reference)
        {
            return false;
        }

        const auto relationshipId = GetRelationshipId(reference);
        if (!sectionProperties->RemoveChild(reference))
        {
            return false;
        }

        if (!mainPart || relationshipId.empty() ||
            CountSectionReferencesToRelationship(mainPart, relationshipId) != 0)
        {
            return true;
        }

        if (header)
        {
            return mainPart->RemoveHeaderPart(FindHeaderPartByRelationshipId(mainPart, relationshipId));
        }
        return mainPart->RemoveFooterPart(FindFooterPartByRelationshipId(mainPart, relationshipId));
    }
};

/// The pPr/rPr/tblPr property elements, created on demand.
class WordPropertiesElementHelper
{
public:
    // The two accessors below are defined further down; at namespace scope they
    // needed declaring first, inside a class they do not.
    static std::shared_ptr<ExyokiOffice::OpenXMLElement> EnsureParagraphProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph)
    {
        if (!paragraph)
        {
            return nullptr;
        }
        auto properties = GetParagraphPropertiesElement(paragraph);
        if (properties)
        {
            return properties;
        }
        auto children = paragraph->Children();
        return paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties>(
            children.empty() ? nullptr : children.front());
    }

    static std::shared_ptr<ExyokiOffice::OpenXMLElement> EnsureRunProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>& run)
    {
        if (!run)
        {
            return nullptr;
        }
        auto properties = GetRunPropertiesElement(run);
        if (properties)
        {
            return properties;
        }
        auto children = run->Children();
        return run->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties>(
            children.empty() ? nullptr : children.front());
    }

    static std::shared_ptr<ExyokiOffice::OpenXMLElement> GetParagraphPropertiesElement(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph)
    {
        if (!paragraph)
        {
            return nullptr;
        }
        auto properties = paragraph->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties>();
        return properties ? std::static_pointer_cast<ExyokiOffice::OpenXMLElement>(properties)
                          : WordPropertyReadHelper::FindDirectWordChild(paragraph, "pPr");
    }

    static std::shared_ptr<ExyokiOffice::OpenXMLElement> GetRunPropertiesElement(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>& run)
    {
        if (!run)
        {
            return nullptr;
        }
        auto properties = run->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties>();
        return properties ? std::static_pointer_cast<ExyokiOffice::OpenXMLElement>(properties)
                          : WordPropertyReadHelper::FindDirectWordChild(run, "rPr");
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableProperties> EnsureTableProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table)
    {
        return WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableProperties>(table);
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties> EnsureTableCellProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell)
    {
        return WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties>(cell);
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow> EnsureTableRow(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table, Size rowIndex)
    {
        if (!table)
        {
            return nullptr;
        }

        auto rows = table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
        if (rowIndex < rows.size())
        {
            return rows[rowIndex];
        }

        for (Size i = rows.size(); i <= rowIndex; ++i)
        {
            auto row = table->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
            if (!row)
            {
                return nullptr;
            }
        }

        rows = table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
        if (rowIndex < rows.size())
        {
            return rows[rowIndex];
        }
        return nullptr;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowProperties> EnsureTableRowProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>& row)
    {
        return WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow,
                                                      ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowProperties>(row);
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableGrid> EnsureTableGrid(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table)
    {
        return WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table,
                                                      ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableGrid>(table);
    }
};

/// The numbering part: abstract definitions, instances and levels.
class WordNumberingHelper
{
public:
    static std::shared_ptr<Packaging::NumberingDefinitionsPart> EnsureNumberingDefinitionsPart(
        const WordDocument::Ptr& document)
    {
        if (!document)
        {
            return nullptr;
        }

        auto mainPart = document->GetMainDocumentPart();
        if (!mainPart)
        {
            mainPart = document->AddMainDocumentPart();
        }
        if (!mainPart)
        {
            return nullptr;
        }

        auto numberingPart = mainPart->GetNumberingDefinitionsPart();
        if (!numberingPart)
        {
            numberingPart = mainPart->AddNumberingDefinitionsPart();
        }
        return numberingPart;
    }

    static std::shared_ptr<Packaging::NumberingDefinitionsPart> GetNumberingDefinitionsPart(
        const WordDocument::Ptr& document)
    {
        auto mainPart = WordStructureHelper::GetMainDocumentPart(document);
        return mainPart ? mainPart->GetNumberingDefinitionsPart() : nullptr;
    }

    static int NextAbstractNumberingId(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering)
    {
        int maxId = 0;
        if (!numbering)
        {
            return 1;
        }

        for (const auto& abstractNum : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::AbstractNum>())
        {
            if (!abstractNum)
            {
                continue;
            }
            const auto current = abstractNum->GetAbstractNumberId().Value();
            maxId = std::max(maxId, current);
        }

        return maxId + 1;
    }

    static int NextNumberingInstanceId(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering)
    {
        int maxId = 0;
        if (!numbering)
        {
            return 1;
        }

        for (const auto& instance : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance>())
        {
            if (!instance)
            {
                continue;
            }
            const auto current = instance->GetNumberID().Value();
            maxId = std::max(maxId, current);
        }

        return maxId + 1;
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::AbstractNum> FindAbstractNumByName(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering,
        std::string_view name)
    {
        if (!numbering || name.empty())
        {
            return nullptr;
        }

        for (const auto& abstractNum : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::AbstractNum>())
        {
            if (!abstractNum)
            {
                continue;
            }
            auto defName = abstractNum->GetFirstChildOfType<
                DocumentFormat::OpenXml::Wordprocessing::AbstractNumDefinitionName>();
            if (!defName)
            {
                continue;
            }
            if (defName->GetVal().ToString() == name)
            {
                return abstractNum;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::AbstractNum> FindAbstractNumById(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering,
        int abstractId)
    {
        if (!numbering)
        {
            return nullptr;
        }

        for (const auto& abstractNum : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::AbstractNum>())
        {
            if (abstractNum && abstractNum->GetAbstractNumberId().IsDefined() && abstractNum->GetAbstractNumberId().Value() == abstractId)
            {
                return abstractNum;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance> FindNumberingInstanceForAbstract(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering,
        int abstractId)
    {
        if (!numbering)
        {
            return nullptr;
        }

        for (const auto& instance : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance>())
        {
            if (!instance)
            {
                continue;
            }
            auto abstractNumId = instance->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::AbstractNumId>();
            if (abstractNumId && abstractNumId->GetVal().Value() == abstractId)
            {
                return instance;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance> FindNumberingInstanceById(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering,
        int numberingId)
    {
        if (!numbering)
        {
            return nullptr;
        }

        for (const auto& instance : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance>())
        {
            if (instance && instance->GetNumberID().IsDefined() && instance->GetNumberID().Value() == numberingId)
            {
                return instance;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::AbstractNum> GetAbstractNumForInstance(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering,
        int numberingId)
    {
        auto instance = FindNumberingInstanceById(numbering, numberingId);
        auto abstractNumId = instance ? instance->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::AbstractNumId>()
                                      : nullptr;
        if (!abstractNumId || !abstractNumId->GetVal().IsDefined())
        {
            return nullptr;
        }
        return FindAbstractNumById(numbering, abstractNumId->GetVal().Value());
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering> GetNumberingRoot(
        const WordDocument::Ptr& document,
        bool create)
    {
        auto numberingPart = create ? EnsureNumberingDefinitionsPart(document) : GetNumberingDefinitionsPart(document);
        return numberingPart ? numberingPart->GetTypedRootElement() : nullptr;
    }

    static std::string DefaultLevelText(int level)
    {
        if (level < 0)
        {
            level = 0;
        }
        if (level > 8)
        {
            level = 8;
        }

        std::string text;
        for (int index = 0; index <= level; ++index)
        {
            if (!text.empty())
            {
                text += ".";
            }
            text += "%";
            text += std::to_string(index + 1);
        }
        text += ".";
        return text;
    }

    static NumberingLevelDefinition NormalizeLevel(NumberingLevelDefinition level)
    {
        level.Level = std::clamp(level.Level, 0, 8);
        if (level.Start < 0)
        {
            level.Start = 0;
        }
        if (level.LevelText.empty())
        {
            level.LevelText = level.Format == DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues::Bullet
                                  ? "*"
                                  : DefaultLevelText(level.Level);
        }
        return level;
    }

    static void WriteLevelDefinition(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Level>& levelElement,
        const NumberingLevelDefinition& input)
    {
        if (!levelElement)
        {
            return;
        }

        const auto level = NormalizeLevel(input);
        levelElement->SetLevelIndex(Int32Value(level.Level));

        auto start = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                            DocumentFormat::OpenXml::Wordprocessing::StartNumberingValue>(levelElement);
        if (start)
        {
            start->SetVal(Int32Value(level.Start));
        }

        auto format = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                             DocumentFormat::OpenXml::Wordprocessing::NumberingFormat>(levelElement);
        if (format)
        {
            format->SetVal(EnumValue<DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues>(level.Format));
        }

        if (!level.ParagraphStyleId.empty())
        {
            auto paragraphStyle = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                                         DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>(levelElement);
            if (paragraphStyle)
            {
                paragraphStyle->SetVal(StringValue(level.ParagraphStyleId));
            }
        }

        auto text = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                           DocumentFormat::OpenXml::Wordprocessing::LevelText>(levelElement);
        if (text)
        {
            text->SetVal(StringValue(level.LevelText));
        }

        auto suffix = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                             DocumentFormat::OpenXml::Wordprocessing::LevelSuffix>(levelElement);
        if (suffix)
        {
            suffix->SetVal(EnumValue<DocumentFormat::OpenXml::Wordprocessing::LevelSuffixValues>(level.Suffix));
        }

        auto justification = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                                    DocumentFormat::OpenXml::Wordprocessing::LevelJustification>(levelElement);
        if (justification)
        {
            justification->SetVal(EnumValue<DocumentFormat::OpenXml::Wordprocessing::LevelJustificationValues>(
                level.Justification));
        }

        if (level.LeftIndent || level.HangingIndent)
        {
            auto pProps = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                                 DocumentFormat::OpenXml::Wordprocessing::PreviousParagraphProperties>(
                levelElement);
            auto indent =
                WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::PreviousParagraphProperties,
                                                       DocumentFormat::OpenXml::Wordprocessing::Indentation>(pProps);
            if (indent)
            {
                if (level.LeftIndent)
                {
                    indent->SetLeft(StringValue(std::to_string(WordValueHelper::ToTwipsUInt32(*level.LeftIndent))));
                }
                if (level.HangingIndent)
                {
                    indent->SetHanging(StringValue(std::to_string(WordValueHelper::ToTwipsUInt32(*level.HangingIndent))));
                }
            }
        }

        if (level.RestartAfterLevel)
        {
            auto restart = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                                  DocumentFormat::OpenXml::Wordprocessing::LevelRestart>(levelElement);
            if (restart)
            {
                restart->SetVal(Int32Value(*level.RestartAfterLevel));
            }
        }

        if (level.LegalNumbering)
        {
            auto legal = WordStructureHelper::EnsureChildOfType<DocumentFormat::OpenXml::Wordprocessing::Level,
                                                                DocumentFormat::OpenXml::Wordprocessing::IsLegalNumberingStyle>(levelElement);
            if (legal)
            {
                legal->SetVal(OnOffValue(true));
            }
        }
    }

    static std::optional<NumberingLevelDefinition> ReadLevelDefinition(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Level>& level)
    {
        if (!level || !level->GetLevelIndex().IsDefined())
        {
            return std::nullopt;
        }

        NumberingLevelDefinition result;
        result.Level = level->GetLevelIndex().Value();
        if (auto start = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::StartNumberingValue>();
            start && start->GetVal().IsDefined())
        {
            result.Start = start->GetVal().Value();
        }
        if (auto format = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::NumberingFormat>();
            format && format->GetVal().IsDefined())
        {
            result.Format = format->GetVal().Value();
        }
        if (auto text = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelText>();
            text && text->GetVal().IsDefined())
        {
            result.LevelText = text->GetVal().ToString();
        }
        if (auto suffix = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelSuffix>();
            suffix && suffix->GetVal().IsDefined())
        {
            result.Suffix = suffix->GetVal().Value();
        }
        if (auto justification = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelJustification>();
            justification && justification->GetVal().IsDefined())
        {
            result.Justification = justification->GetVal().Value();
        }
        if (auto paragraphStyle = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>();
            paragraphStyle && paragraphStyle->GetVal().IsDefined())
        {
            result.ParagraphStyleId = paragraphStyle->GetVal().ToString();
        }
        if (auto restart = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelRestart>();
            restart && restart->GetVal().IsDefined())
        {
            result.RestartAfterLevel = restart->GetVal().Value();
        }
        if (auto legal = level->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::IsLegalNumberingStyle>())
        {
            result.LegalNumbering = legal->GetVal().ValueOr(true);
        }
        return result;
    }

    static std::vector<NumberingLevelOverride> ReadLevelOverrides(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance>& instance)
    {
        std::vector<NumberingLevelOverride> overrides;
        if (!instance)
        {
            return overrides;
        }

        for (const auto& levelOverride : instance->Elements<DocumentFormat::OpenXml::Wordprocessing::LevelOverride>())
        {
            if (!levelOverride || !levelOverride->GetLevelIndex().IsDefined())
            {
                continue;
            }

            NumberingLevelOverride output;
            output.Level = levelOverride->GetLevelIndex().Value();
            if (auto start =
                    levelOverride->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::StartOverrideNumberingValue>();
                start && start->GetVal().IsDefined())
            {
                output.Start = start->GetVal().Value();
            }
            overrides.push_back(output);
        }
        return overrides;
    }

    static std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance> AppendNumberingInstance(
        const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Numbering>& numbering,
        int abstractId,
        const std::vector<NumberingLevelOverride>& overrides)
    {
        if (!numbering)
        {
            return nullptr;
        }

        auto instance = numbering->AppendChild<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance>();
        if (!instance)
        {
            return nullptr;
        }

        instance->SetNumberID(Int32Value(NextNumberingInstanceId(numbering)));
        auto abstractNumId = instance->AppendChild<DocumentFormat::OpenXml::Wordprocessing::AbstractNumId>();
        if (abstractNumId)
        {
            abstractNumId->SetVal(Int32Value(abstractId));
        }

        for (const auto& input : overrides)
        {
            if (input.Level < 0 || input.Level > 8)
            {
                continue;
            }
            auto levelOverride = instance->AppendChild<DocumentFormat::OpenXml::Wordprocessing::LevelOverride>();
            if (!levelOverride)
            {
                continue;
            }
            levelOverride->SetLevelIndex(Int32Value(input.Level));
            auto start = levelOverride->AppendChild<DocumentFormat::OpenXml::Wordprocessing::StartOverrideNumberingValue>();
            if (start)
            {
                start->SetVal(Int32Value(std::max(0, input.Start)));
            }
        }
        return instance;
    }
};

/// Table cells, grid spans and the vertical merge model.
class WordTableHelper
{
public:
    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell> AppendEmptyTableCell(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>& row)
    {
        if (!row)
        {
            return nullptr;
        }

        auto cell = row->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>();
        if (cell)
        {
            cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
        }
        return cell;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell> InsertEmptyTableCell(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>& row,
        const std::shared_ptr<ExyokiOffice::OpenXMLElement>& before = nullptr)
    {
        if (!row)
        {
            return nullptr;
        }

        auto cell = row->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>(before);
        if (cell)
        {
            cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
        }
        return cell;
    }

    static int TableCellGridSpan(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell)
    {
        auto props = cell ? cell->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties>() : nullptr;
        auto span = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridSpan>() : nullptr;
        return std::max(1, span ? span->GetVal().ValueOr(1) : 1);
    }

    static std::string TableCellVerticalMergeValue(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell)
    {
        auto props = cell ? cell->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties>() : nullptr;
        auto merge = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::VerticalMerge>() : nullptr;
        if (!merge)
        {
            return {};
        }
        auto value = WordPropertyReadHelper::RawValAttributeOrEmpty(merge);
        return value.empty() ? std::string("continue") : value;
    }

    static void RemoveTableCellMergeMarkup(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell)
    {
        auto props = cell ? cell->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties>() : nullptr;
        if (!props)
        {
            return;
        }

        if (auto span = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridSpan>())
        {
            props->RemoveChild(span);
        }
        if (auto merge = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HorizontalMerge>())
        {
            props->RemoveChild(merge);
        }
        if (auto merge = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::VerticalMerge>())
        {
            props->RemoveChild(merge);
        }
    }

    static void RemoveTableCellBlockContent(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell)
    {
        if (!cell)
        {
            return;
        }

        for (const auto& child : cell->Children())
        {
            if (!ExyokiOffice::openxmlelement_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties>(child))
            {
                cell->RemoveChild(child);
            }
        }
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text> AppendTextRunToTableCellParagraph(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
        std::string_view text,
        bool preserveSpaces)
    {
        if (!paragraph)
        {
            return nullptr;
        }

        auto run = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
        if (!run)
        {
            return nullptr;
        }

        auto textElement = run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>();
        if (!textElement)
        {
            return nullptr;
        }

        textElement->SetText(text);
        if (preserveSpaces)
        {
            textElement->SetSpace(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues>(
                ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues::Preserve));
        }

        return textElement;
    }

    struct PhysicalTableCell
    {
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell> Cell;
        Size PhysicalIndex = 0;
        Size StartColumn = 0;
        Size ColumnSpan = 1;
    };

    static std::vector<PhysicalTableCell> PhysicalCellsForRow(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>& row)
    {
        std::vector<PhysicalTableCell> result;
        if (!row)
        {
            return result;
        }

        Size logicalColumn = 0;
        auto cells = row->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>();
        for (Size i = 0; i < cells.size(); ++i)
        {
            const auto span = static_cast<Size>(TableCellGridSpan(cells[i]));
            result.push_back({cells[i], i, logicalColumn, std::max<Size>(1, span)});
            logicalColumn += std::max<Size>(1, span);
        }
        return result;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell> FindLogicalTableCell(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table,
        Size rowIndex,
        Size columnIndex)
    {
        if (!table)
        {
            return nullptr;
        }

        const auto rows = table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
        if (rowIndex >= rows.size())
        {
            return nullptr;
        }

        for (const auto& cell : PhysicalCellsForRow(rows[rowIndex]))
        {
            if (columnIndex >= cell.StartColumn && columnIndex < cell.StartColumn + cell.ColumnSpan)
            {
                return cell.Cell;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell> EnsureLogicalTableCell(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table,
        Size rowIndex,
        Size columnIndex)
    {
        auto row = WordPropertiesElementHelper::EnsureTableRow(table, rowIndex);
        if (!row)
        {
            return nullptr;
        }

        if (auto existing = FindLogicalTableCell(table, rowIndex, columnIndex))
        {
            return existing;
        }

        Size logicalColumns = 0;
        for (const auto& cell : PhysicalCellsForRow(row))
        {
            logicalColumns = std::max(logicalColumns, cell.StartColumn + cell.ColumnSpan);
        }

        while (logicalColumns <= columnIndex)
        {
            if (!AppendEmptyTableCell(row))
            {
                return nullptr;
            }
            ++logicalColumns;
        }
        return FindLogicalTableCell(table, rowIndex, columnIndex);
    }

    static void EnsureTableGridColumnCount(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table,
        Size columnCount)
    {
        auto grid = WordPropertiesElementHelper::EnsureTableGrid(table);
        if (!grid)
        {
            return;
        }

        auto columns = grid->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridColumn>();
        while (columns.size() > columnCount)
        {
            grid->RemoveChild(columns.back());
            columns.pop_back();
        }
        while (columns.size() < columnCount)
        {
            if (!grid->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridColumn>())
            {
                return;
            }
            columns = grid->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridColumn>();
        }
    }

    static void SetTableCellGridSpan(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell,
        Size columnSpan)
    {
        auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
        if (!props)
        {
            return;
        }

        if (auto hMerge = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HorizontalMerge>())
        {
            props->RemoveChild(hMerge);
        }

        auto existing = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridSpan>();
        if (columnSpan <= 1)
        {
            if (existing)
            {
                props->RemoveChild(existing);
            }
            return;
        }

        auto span = existing ? existing
                             : props->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridSpan>();
        if (span)
        {
            span->SetVal(Int32Value(static_cast<int>(columnSpan)));
        }
    }

    static void SetTableCellVerticalMerge(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>& cell,
        std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::MergedCellValues> value)
    {
        auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
        if (!props)
        {
            return;
        }

        auto merge = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::VerticalMerge>();
        if (!value)
        {
            if (merge)
            {
                props->RemoveChild(merge);
            }
            return;
        }

        merge = merge ? merge : props->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::VerticalMerge>();
        if (merge)
        {
            merge->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::MergedCellValues>(*value));
        }
    }
};

/// Inline drawings: pictures, their parts and layout state.
class WordDrawingHelper
{
public:
    static bool PopulateDrawingWithPicture(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing,
                                           const std::string& relationshipId,
                                           Int64 widthEmu,
                                           Int64 heightEmu,
                                           ImageLayout layout,
                                           ImageWrap wrap,
                                           UInt32 docId,
                                           std::string_view name)
    {
        if (!drawing)
        {
            return false;
        }

        const auto pictureName = name.empty() ? "Picture " + std::to_string(docId) : std::string(name);

        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline> inlineDrawing;
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor> anchorDrawing;

        if (layout == ImageLayout::Inline)
        {
            inlineDrawing = drawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>();
            inlineDrawing->SetDistanceFromTop(UInt32Value(0));
            inlineDrawing->SetDistanceFromBottom(UInt32Value(0));
            inlineDrawing->SetDistanceFromLeft(UInt32Value(0));
            inlineDrawing->SetDistanceFromRight(UInt32Value(0));
        }
        else
        {
            anchorDrawing = drawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
            anchorDrawing->SetDistanceFromTop(UInt32Value(0));
            anchorDrawing->SetDistanceFromBottom(UInt32Value(0));
            anchorDrawing->SetDistanceFromLeft(UInt32Value(0));
            anchorDrawing->SetDistanceFromRight(UInt32Value(0));
            anchorDrawing->SetSimplePos(BooleanValue(false));
            anchorDrawing->SetRelativeHeight(UInt32Value(0));
            anchorDrawing->SetBehindDoc(BooleanValue(false));
            anchorDrawing->SetLocked(BooleanValue(false));
            anchorDrawing->SetLayoutInCell(BooleanValue(true));
            anchorDrawing->SetAllowOverlap(BooleanValue(true));

            auto simplePos = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::SimplePosition>();
            simplePos->SetX(Int64Value(0));
            simplePos->SetY(Int64Value(0));

            auto positionH = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition>();
            positionH->SetRelativeFrom(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues>(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::Page));
            auto hOffset = positionH->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>();
            hOffset->SetText("0");

            auto positionV = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition>();
            positionV->SetRelativeFrom(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues>(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::Page));
            auto vOffset = positionV->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>();
            vOffset->SetText("0");

            auto effectExtent = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::EffectExtent>();
            effectExtent->SetLeftEdge(Int64Value(0));
            effectExtent->SetTopEdge(Int64Value(0));
            effectExtent->SetRightEdge(Int64Value(0));
            effectExtent->SetBottomEdge(Int64Value(0));
        }

        std::shared_ptr<OpenXMLElement> container = inlineDrawing ? std::dynamic_pointer_cast<OpenXMLElement>(inlineDrawing) : std::dynamic_pointer_cast<OpenXMLElement>(anchorDrawing);
        if (!container)
        {
            return false;
        }

        auto extent = container->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Extent>();
        extent->SetCx(Int64Value(widthEmu));
        extent->SetCy(Int64Value(heightEmu));

        auto docProps = container->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::DocProperties>();
        docProps->SetId(UInt32Value(docId));
        docProps->SetName(StringValue(pictureName));

        auto graphicFrameProps = container->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::NonVisualGraphicFrameDrawingProperties>();
        auto graphicFrameLocks = graphicFrameProps->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::GraphicFrameLocks>();
        graphicFrameLocks->SetNoChangeAspect(BooleanValue(true));

        auto graphic = container->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>();
        auto graphicData = graphic->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::GraphicData>();
        graphicData->SetUri(StringValue(std::string(kDrawingPictureNamespace)));

        auto picture = graphicData->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::Picture>();
        auto nvPicPr = picture->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualPictureProperties>();
        auto cNvPr = nvPicPr->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualDrawingProperties>();
        cNvPr->SetId(UInt32Value(docId));
        cNvPr->SetName(StringValue(pictureName));
        auto cNvPicPr = nvPicPr->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualPictureDrawingProperties>();
        cNvPicPr->SetPreferRelativeResize(BooleanValue(true));

        auto blipFill = picture->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::BlipFill>();
        auto blip = blipFill->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Blip>();
        blip->SetEmbed(StringValue(relationshipId));
        blip->SetCompressionState(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::BlipCompressionValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::BlipCompressionValues::Print));

        auto stretch = blipFill->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Stretch>();
        stretch->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::FillRectangle>();

        auto shapeProps = picture->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties>();
        auto transform = shapeProps->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D>();
        auto offset = transform->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Offset>();
        offset->SetX(Int64Value(0));
        offset->SetY(Int64Value(0));
        auto extents = transform->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Extents>();
        extents->SetCx(Int64Value(widthEmu));
        extents->SetCy(Int64Value(heightEmu));
        auto geometry = shapeProps->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::PresetGeometry>();
        geometry->SetPreset(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::ShapeTypeValues>(ExyokiOffice::DocumentFormat::OpenXml::Drawing::ShapeTypeValues::Rectangle));
        geometry->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::AdjustValueList>();

        if (anchorDrawing)
        {
            switch (wrap)
            {
                case ImageWrap::None:
                    anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapNone>();
                    break;

                case ImageWrap::Square:
                {
                    auto wrapSquare = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapSquare>();
                    wrapSquare->SetWrapText(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues>(
                        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides));
                    break;
                }

                case ImageWrap::Tight:
                {
                    auto wrapTight = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTight>();
                    wrapTight->SetWrapText(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues>(
                        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides));
                    break;
                }

                case ImageWrap::Through:
                {
                    auto wrapThrough = anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapThrough>();
                    wrapThrough->SetWrapText(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues>(
                        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides));
                    break;
                }

                case ImageWrap::TopAndBottom:
                    anchorDrawing->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTopBottom>();
                    break;
            }
        }

        return true;
    }

    static std::shared_ptr<Packaging::ImagePart> CreateImagePartFromData(const std::shared_ptr<Packaging::MainDocumentPart>& mainPart,
                                                                         std::vector<Byte> data,
                                                                         std::string_view contentType)
    {
        if (!mainPart)
        {
            return nullptr;
        }

        // The content type is set before the part is attached so that the package
        // can name the file after the image format instead of the `.bin` placeholder.
        auto imagePart = std::make_shared<Packaging::ImagePart>();
        if (!contentType.empty())
        {
            imagePart->SetContentType(std::string(contentType));
        }
        if (!mainPart->AddImagePart(imagePart))
        {
            return nullptr;
        }
        imagePart->SetBinaryData(std::move(data));
        return imagePart;
    }

    struct DrawingInfo
    {
        std::string relationshipId;
        UInt32 docId = 1;
        std::string name;
        Int64 widthEmu = 0;
        Int64 heightEmu = 0;
    };

    static DrawingInfo ExtractDrawingInfo(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing)
    {
        DrawingInfo info{};
        if (!drawing)
        {
            return info;
        }

        std::shared_ptr<OpenXMLElement> container = drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>();
        if (!container)
        {
            container = drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
        }

        if (container)
        {
            if (auto extent = container->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Extent>())
            {
                info.widthEmu = extent->GetCx().Value();
                info.heightEmu = extent->GetCy().Value();
            }
            if (auto docProps = container->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::DocProperties>())
            {
                info.docId = static_cast<UInt32>(docProps->GetId().Value());
                info.name = docProps->GetName().ToString();
            }
        }

        auto graphic = container ? container->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>() : drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>();
        if (graphic)
        {
            auto graphicData = graphic->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::GraphicData>();
            if (graphicData)
            {
                auto picture = graphicData->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::Picture>();
                if (picture)
                {
                    auto blipFill = picture->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::BlipFill>();
                    if (blipFill)
                    {
                        if (auto blip = blipFill->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Blip>())
                        {
                            info.relationshipId = blip->GetEmbed().ToString();
                        }
                    }
                }
            }
        }

        if (info.name.empty())
        {
            info.name = "Picture " + std::to_string(info.docId);
        }

        return info;
    }

    struct ImageLayoutState
    {
        bool hasDistances = false;
        UInt32 distanceLeft = 0;
        UInt32 distanceTop = 0;
        UInt32 distanceRight = 0;
        UInt32 distanceBottom = 0;

        bool hasWrap = false;
        ImageWrap wrap = ImageWrap::Square;
        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues wrapText =
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides;

        bool hasHorizontalOffset = false;
        bool hasVerticalOffset = false;
        Int64 horizontalOffsetEmu = 0;
        Int64 verticalOffsetEmu = 0;
        bool hasHorizontalAlign = false;
        bool hasVerticalAlign = false;
        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues horizontalAlign =
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues::Left;
        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues verticalAlign =
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues::Top;
        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues horizontalFrom =
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::Page;
        ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues verticalFrom =
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::Page;

        bool hasAnchor = false;
        bool behindText = false;
        bool allowOverlap = true;
        bool locked = false;
        bool layoutInCell = true;
        bool simplePosEnabled = false;
        UInt32 relativeHeight = 0;
        bool hasSimplePosition = false;
        Int64 simplePosX = 0;
        Int64 simplePosY = 0;
    };

    static ImageLayoutState ExtractImageLayoutState(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing)
    {
        ImageLayoutState state{};
        if (!drawing)
        {
            return state;
        }

        if (auto inlineDrawing = drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>())
        {
            const auto left = inlineDrawing->GetDistanceFromLeft().ValueOr(0);
            const auto top = inlineDrawing->GetDistanceFromTop().ValueOr(0);
            const auto right = inlineDrawing->GetDistanceFromRight().ValueOr(0);
            const auto bottom = inlineDrawing->GetDistanceFromBottom().ValueOr(0);

            state.distanceLeft = left;
            state.distanceTop = top;
            state.distanceRight = right;
            state.distanceBottom = bottom;
            state.hasDistances = inlineDrawing->GetDistanceFromLeft().IsDefined() ||
                                 inlineDrawing->GetDistanceFromTop().IsDefined() ||
                                 inlineDrawing->GetDistanceFromRight().IsDefined() ||
                                 inlineDrawing->GetDistanceFromBottom().IsDefined() ||
                                 left != 0 || top != 0 || right != 0 || bottom != 0;
        }

        auto anchor = drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
        if (!anchor)
        {
            return state;
        }

        state.hasAnchor = true;
        state.behindText = anchor->GetBehindDoc().ValueOr(false);
        state.allowOverlap = anchor->GetAllowOverlap().ValueOr(true);
        state.locked = anchor->GetLocked().ValueOr(false);
        state.layoutInCell = anchor->GetLayoutInCell().ValueOr(true);
        state.simplePosEnabled = anchor->GetSimplePos().ValueOr(false);
        state.relativeHeight = anchor->GetRelativeHeight().ValueOr(0);

        if (!state.hasDistances)
        {
            const auto left = anchor->GetDistanceFromLeft().ValueOr(0);
            const auto top = anchor->GetDistanceFromTop().ValueOr(0);
            const auto right = anchor->GetDistanceFromRight().ValueOr(0);
            const auto bottom = anchor->GetDistanceFromBottom().ValueOr(0);

            state.distanceLeft = left;
            state.distanceTop = top;
            state.distanceRight = right;
            state.distanceBottom = bottom;
            state.hasDistances = anchor->GetDistanceFromLeft().IsDefined() ||
                                 anchor->GetDistanceFromTop().IsDefined() ||
                                 anchor->GetDistanceFromRight().IsDefined() ||
                                 anchor->GetDistanceFromBottom().IsDefined() ||
                                 left != 0 || top != 0 || right != 0 || bottom != 0;
        }

        if (auto simplePos = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::SimplePosition>())
        {
            state.simplePosX = simplePos->GetX().Value();
            state.simplePosY = simplePos->GetY().Value();
            state.hasSimplePosition = true;
        }

        if (auto positionH = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition>())
        {
            state.horizontalFrom = positionH->GetRelativeFrom().ValueOr(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::Page);
            if (auto align = positionH->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignment>())
            {
                const auto parsed = WordValueHelper::TryParseEnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues>(align->GetText());
                if (parsed)
                {
                    state.horizontalAlign = *parsed;
                    state.hasHorizontalAlign = true;
                }
            }
            if (auto offset = positionH->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>())
            {
                const auto parsed = WordValueHelper::TryParseInt64(offset->GetText());
                if (parsed)
                {
                    state.horizontalOffsetEmu = *parsed;
                    state.hasHorizontalOffset = true;
                }
            }
        }

        if (auto positionV = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition>())
        {
            state.verticalFrom = positionV->GetRelativeFrom().ValueOr(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::Page);
            if (auto align = positionV->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignment>())
            {
                const auto parsed = WordValueHelper::TryParseEnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues>(align->GetText());
                if (parsed)
                {
                    state.verticalAlign = *parsed;
                    state.hasVerticalAlign = true;
                }
            }
            if (auto offset = positionV->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>())
            {
                const auto parsed = WordValueHelper::TryParseInt64(offset->GetText());
                if (parsed)
                {
                    state.verticalOffsetEmu = *parsed;
                    state.hasVerticalOffset = true;
                }
            }
        }

        if (anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapNone>())
        {
            state.wrap = ImageWrap::None;
            state.hasWrap = true;
        }
        else if (auto wrapSquare = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapSquare>())
        {
            state.wrap = ImageWrap::Square;
            state.wrapText = wrapSquare->GetWrapText().ValueOr(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
            state.hasWrap = true;
        }
        else if (auto wrapTight = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTight>())
        {
            state.wrap = ImageWrap::Tight;
            state.wrapText = wrapTight->GetWrapText().ValueOr(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
            state.hasWrap = true;
        }
        else if (auto wrapThrough = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapThrough>())
        {
            state.wrap = ImageWrap::Through;
            state.wrapText = wrapThrough->GetWrapText().ValueOr(
                ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
            state.hasWrap = true;
        }
        else if (anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTopBottom>())
        {
            state.wrap = ImageWrap::TopAndBottom;
            state.hasWrap = true;
        }

        return state;
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::Picture> FindPicture(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing)
    {
        if (!drawing)
        {
            return nullptr;
        }

        std::shared_ptr<OpenXMLElement> container =
            drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>();
        if (!container)
        {
            container = drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
        }

        auto graphic = container ? container->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>()
                                 : drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>();
        if (!graphic)
        {
            return nullptr;
        }

        auto graphicData = graphic->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::GraphicData>();
        if (!graphicData)
        {
            return nullptr;
        }

        return graphicData->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::Picture>();
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualDrawingProperties>
    FindPictureNonVisualProperties(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing)
    {
        auto picture = FindPicture(drawing);
        if (!picture)
        {
            return nullptr;
        }

        auto nvPicPr = picture->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualPictureProperties>();
        if (!nvPicPr)
        {
            return nullptr;
        }

        return nvPicPr->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualDrawingProperties>();
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::BlipFill> FindPictureBlipFill(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing)
    {
        auto picture = FindPicture(drawing);
        if (!picture)
        {
            return nullptr;
        }

        return picture->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::BlipFill>();
    }

    static std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties> FindPictureShapeProperties(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing)
    {
        auto picture = FindPicture(drawing);
        if (!picture)
        {
            return nullptr;
        }

        return picture->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties>();
    }

    // --- Image format/size sniffing -------------------------------------------------
    //
    // These helpers inspect raw image bytes to recover the real format, pixel size, and
    // resolution instead of trusting a caller-supplied content type or file extension.
};

/// Image format probing over raw bytes.
class WordImageFormatHelper
{
public:
    static UInt16 ReadBigEndian16(std::span<const Byte> data, Size offset)
    {
        return static_cast<UInt16>((static_cast<UInt16>(data[offset]) << 8) |
                                   static_cast<UInt16>(data[offset + 1]));
    }

    static UInt32 ReadBigEndian32(std::span<const Byte> data, Size offset)
    {
        return (static_cast<UInt32>(data[offset]) << 24) |
               (static_cast<UInt32>(data[offset + 1]) << 16) |
               (static_cast<UInt32>(data[offset + 2]) << 8) |
               static_cast<UInt32>(data[offset + 3]);
    }

    static UInt16 ReadLittleEndian16(std::span<const Byte> data, Size offset)
    {
        return static_cast<UInt16>(static_cast<UInt16>(data[offset]) |
                                   (static_cast<UInt16>(data[offset + 1]) << 8));
    }

    static UInt32 ReadLittleEndian32(std::span<const Byte> data, Size offset)
    {
        return static_cast<UInt32>(data[offset]) | (static_cast<UInt32>(data[offset + 1]) << 8) |
               (static_cast<UInt32>(data[offset + 2]) << 16) |
               (static_cast<UInt32>(data[offset + 3]) << 24);
    }

    static std::optional<ImageFormatInfo> DetectPng(std::span<const Byte> data)
    {
        static constexpr UInt8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        if (data.size() < 8 + 8 + 8 || !std::equal(std::begin(kSignature), std::end(kSignature), data.begin()))
        {
            return std::nullopt;
        }
        if (data[12] != 'I' || data[13] != 'H' || data[14] != 'D' || data[15] != 'R')
        {
            return std::nullopt;
        }

        ImageFormatInfo info;
        info.ContentType = "image/png";
        info.Extension = ".png";
        info.PixelWidth = ReadBigEndian32(data, 16);
        info.PixelHeight = ReadBigEndian32(data, 20);

        // Optional pHYs chunk carries physical pixel density; fall back to 96 DPI otherwise.
        Size offset = 8;
        while (offset + 12 <= data.size())
        {
            const auto chunkLength = static_cast<Size>(ReadBigEndian32(data, offset));
            if (offset + 8 + chunkLength + 4 > data.size())
            {
                break;
            }
            const std::string_view chunkType(reinterpret_cast<const char*>(data.data() + offset + 4), 4);
            if (chunkType == "pHYs" && chunkLength >= 9)
            {
                const auto pixelsPerUnitX = ReadBigEndian32(data, offset + 8);
                const auto pixelsPerUnitY = ReadBigEndian32(data, offset + 12);
                const auto unit = data[offset + 16];
                if (unit == 1 && pixelsPerUnitX > 0 && pixelsPerUnitY > 0)
                {
                    info.HorizontalDpi = static_cast<Real>(pixelsPerUnitX) * 0.0254;
                    info.VerticalDpi = static_cast<Real>(pixelsPerUnitY) * 0.0254;
                }
                break;
            }
            if (chunkType == "IDAT" || chunkType == "IEND")
            {
                break;
            }
            offset += 8 + chunkLength + 4;
        }

        return info;
    }

    static std::optional<ImageFormatInfo> DetectJpeg(std::span<const Byte> data)
    {
        if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8)
        {
            return std::nullopt;
        }

        ImageFormatInfo info;
        info.ContentType = "image/jpeg";
        info.Extension = ".jpg";

        Size offset = 2;
        while (offset + 4 <= data.size())
        {
            if (data[offset] != 0xFF)
            {
                ++offset;
                continue;
            }
            const auto marker = data[offset + 1];
            if (marker == 0xFF)
            {
                ++offset;
                continue;
            }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            {
                offset += 2;
                continue;
            }
            if (marker == 0xD9)
            {
                break;
            }

            const auto segmentLength = static_cast<Size>(ReadBigEndian16(data, offset + 2));
            if (segmentLength < 2 || offset + 2 + segmentLength > data.size())
            {
                break;
            }

            if (marker == 0xE0 && segmentLength >= 14)
            {
                const Size base = offset + 4;
                if (base + 9 <= data.size() && data[base] == 'J' && data[base + 1] == 'F' && data[base + 2] == 'I' &&
                    data[base + 3] == 'F')
                {
                    const auto units = data[base + 7];
                    const auto xDensity = ReadBigEndian16(data, base + 8);
                    const auto yDensity = ReadBigEndian16(data, base + 10);
                    if (units == 1 && xDensity > 0 && yDensity > 0)
                    {
                        info.HorizontalDpi = xDensity;
                        info.VerticalDpi = yDensity;
                    }
                    else if (units == 2 && xDensity > 0 && yDensity > 0)
                    {
                        info.HorizontalDpi = xDensity * 2.54;
                        info.VerticalDpi = yDensity * 2.54;
                    }
                }
            }

            const bool isStartOfFrame =
                marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (isStartOfFrame)
            {
                if (offset + 9 <= data.size())
                {
                    info.PixelHeight = ReadBigEndian16(data, offset + 5);
                    info.PixelWidth = ReadBigEndian16(data, offset + 7);
                }
                break;
            }

            offset += 2 + segmentLength;
        }

        if (info.PixelWidth == 0 || info.PixelHeight == 0)
        {
            return std::nullopt;
        }
        return info;
    }

    static std::optional<ImageFormatInfo> DetectGif(std::span<const Byte> data)
    {
        if (data.size() < 10 || data[0] != 'G' || data[1] != 'I' || data[2] != 'F' || data[3] != '8' ||
            (data[4] != '7' && data[4] != '9') || data[5] != 'a')
        {
            return std::nullopt;
        }

        ImageFormatInfo info;
        info.ContentType = "image/gif";
        info.Extension = ".gif";
        info.PixelWidth = ReadLittleEndian16(data, 6);
        info.PixelHeight = ReadLittleEndian16(data, 8);
        return info;
    }

    static std::optional<ImageFormatInfo> DetectBmp(std::span<const Byte> data)
    {
        if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
        {
            return std::nullopt;
        }

        ImageFormatInfo info;
        info.ContentType = "image/bmp";
        info.Extension = ".bmp";
        info.PixelWidth = ReadLittleEndian32(data, 18);
        const auto rawHeight = static_cast<Int32>(ReadLittleEndian32(data, 22));
        info.PixelHeight = static_cast<UInt32>(rawHeight < 0 ? -rawHeight : rawHeight);

        const auto pixelsPerMeterX = static_cast<Int32>(ReadLittleEndian32(data, 38));
        const auto pixelsPerMeterY = static_cast<Int32>(ReadLittleEndian32(data, 42));
        if (pixelsPerMeterX > 0 && pixelsPerMeterY > 0)
        {
            info.HorizontalDpi = static_cast<Real>(pixelsPerMeterX) * 0.0254;
            info.VerticalDpi = static_cast<Real>(pixelsPerMeterY) * 0.0254;
        }
        return info;
    }

    // ---------------------------------------------------------------------------
    // WRD-015: cross-document body content merge (BodyCursor::InsertDocument).
    //
    // Deep-copying a subtree across documents (paragraphs/tables for body
    // content; footnote/endnote/comment entries for their parts) is done through
    // the generic OpenXMLElement::CopyInto (DOM-005), which also takes care of
    // namespace declarations that the copied content depends on. Everything
    // below this point is Word-specific: which qualified names carry IDs that
    // must stay unique (styles, numbering, bookmarks, notes, comments, content
    // controls) or point at a relationship (images, hyperlinks), and the policy
    // used to resolve a collision for each of those kinds.
    // ---------------------------------------------------------------------------

    // Threads the ID/relationship remapping tables built up over one
    // BodyCursor::InsertDocument call, so repeated references to the same source
    // style, list, bookmark, note, or comment resolve to the same target
    // identity instead of being imported once per occurrence.
};

/// Merging one document into another without colliding identifiers.
class WordMergeHelper
{
public:
    struct DocumentMergeState
    {
        DocumentMergeState(WordDocumentEditor targetEditor,
                           const WordDocumentEditor& sourceEditor,
                           DocumentMergeOptions options)
            : TargetEditor(std::move(targetEditor)), SourceEditor(sourceEditor), Options(std::move(options))
        {
        }

        WordDocumentEditor TargetEditor;
        const WordDocumentEditor& SourceEditor;
        DocumentMergeOptions Options;

        std::map<std::string, std::string> StyleIds;
        std::map<int, int> NumberingIds;
        std::map<int, int> BookmarkIds;
        std::map<std::string, std::string> BookmarkNames;
        std::set<std::string> UsedBookmarkNames;
        std::map<int, int> FootnoteIds;
        std::map<int, int> EndnoteIds;
        std::map<int, int> CommentIds;
    };

    static std::string MergeStyleId(DocumentMergeState& state, std::string_view sourceStyleId)
    {
        if (sourceStyleId.empty())
        {
            return {};
        }
        const std::string key(sourceStyleId);
        if (auto it = state.StyleIds.find(key); it != state.StyleIds.end())
        {
            return it->second;
        }

        auto targetId = state.TargetEditor.Styles().ImportStyle(state.SourceEditor.Styles(), key, state.Options.StyleConflictPolicy);
        if (targetId.empty())
        {
            // Best effort: if the source style could not be imported (e.g. the
            // source has no styles part), leave the reference as-is rather than
            // silently dropping the paragraph/run/table's formatting intent.
            targetId = key;
        }
        state.StyleIds.emplace(key, targetId);
        return targetId;
    }

    static int MergeNumberingId(DocumentMergeState& state, int sourceNumberingId)
    {
        if (auto it = state.NumberingIds.find(sourceNumberingId); it != state.NumberingIds.end())
        {
            return it->second;
        }
        auto imported = state.TargetEditor.Numbering().ImportList(state.SourceEditor.Numbering(), sourceNumberingId);
        state.NumberingIds.emplace(sourceNumberingId, imported.NumberingId);
        return imported.NumberingId;
    }

    static std::string MakeUniqueBookmarkName(std::set<std::string>& usedNames, std::string base)
    {
        if (base.empty())
        {
            base = "ImportedBookmark";
        }
        if (usedNames.insert(base).second)
        {
            return base;
        }
        usedNames.erase(base);

        for (int suffix = 2; suffix < 100000; ++suffix)
        {
            auto candidate = base + "_" + std::to_string(suffix);
            if (usedNames.insert(candidate).second)
            {
                return candidate;
            }
        }
        return base;
    }

    // Reassigns bookmark IDs to fresh, collision-free target values and keeps
    // bookmark names unique across the whole target document, remembering both
    // mappings on `state` so MergeHyperlinks can later rewrite internal anchors.
    static void MergeBookmarks(DocumentMergeState& state,
                               const std::shared_ptr<Packaging::MainDocumentPart>& targetMainPart,
                               const std::shared_ptr<OpenXMLElement>& root)
    {
        for (auto& start : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>())
        {
            if (!start)
            {
                continue;
            }
            const auto idText = start->GetId().ToString();
            int oldId = 0;
            if (std::from_chars(idText.data(), idText.data() + idText.size(), oldId).ec != std::errc())
            {
                continue;
            }

            int newId;
            if (auto it = state.BookmarkIds.find(oldId); it != state.BookmarkIds.end())
            {
                newId = it->second;
            }
            else
            {
                newId = WordIdHelper::NextBookmarkId(targetMainPart, nullptr);
                state.BookmarkIds.emplace(oldId, newId);
            }
            start->SetId(StringValue(std::to_string(newId)));

            const auto oldName = start->GetName().ToString();
            if (auto it = state.BookmarkNames.find(oldName); it != state.BookmarkNames.end())
            {
                start->SetName(StringValue(it->second));
            }
            else
            {
                auto newName = MakeUniqueBookmarkName(state.UsedBookmarkNames, oldName);
                state.BookmarkNames.emplace(oldName, newName);
                start->SetName(StringValue(newName));
            }
        }

        for (auto& end : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>())
        {
            if (!end)
            {
                continue;
            }
            const auto idText = end->GetId().ToString();
            int oldId = 0;
            if (std::from_chars(idText.data(), idText.data() + idText.size(), oldId).ec != std::errc())
            {
                continue;
            }
            if (auto it = state.BookmarkIds.find(oldId); it != state.BookmarkIds.end())
            {
                end->SetId(StringValue(std::to_string(it->second)));
            }
        }
    }

    static void MergeContentControlIds(const std::shared_ptr<Packaging::MainDocumentPart>& targetMainPart,
                                       const std::shared_ptr<OpenXMLElement>& root)
    {
        auto ids = root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtId>();
        if (ids.empty())
        {
            return;
        }
        int nextId = WordStructureHelper::NextSdtId(targetMainPart, nullptr);
        for (auto& id : ids)
        {
            if (!id)
            {
                continue;
            }
            id->SetVal(Int32Value(nextId++));
        }
    }

    // Footnote/endnote ENTRY types (`w:footnote`/`w:endnote`) share their element
    // name with an unrelated type (see WordNoteHelper::FindNoteEntries above), so entries are
    // located and re-tagged generically; the REFERENCE types (FootnoteReference/
    // EndnoteReference) are unambiguous and safe to use through the typed API.
    template <typename TEntry, typename TReference, typename TPart>
    static void MergeNoteReferences(const std::shared_ptr<OpenXMLElement>& root,
                                    const std::shared_ptr<TPart>& sourcePart,
                                    const std::shared_ptr<TPart>& targetPart,
                                    std::map<int, int>& idMap)
    {
        if (!root || !sourcePart || !targetPart)
        {
            return;
        }

        auto references = root->Descendants<TReference>();
        if (references.empty())
        {
            return;
        }

        std::shared_ptr<OpenXMLElement> sourceRoot = sourcePart->GetTypedRootElement();
        std::shared_ptr<OpenXMLElement> targetRoot = targetPart->GetTypedRootElement();
        if (!sourceRoot || !targetRoot)
        {
            return;
        }

        WordNoteHelper::EnsureNoteSeparators<TEntry>(targetRoot);
        const ExyokiOffice::OpenXmlQualifiedName idAttribute(kWordNamespace, "id");

        for (auto& reference : references)
        {
            if (!reference)
            {
                continue;
            }
            const int oldId = static_cast<int>(reference->GetId().Value());

            int newId;
            if (auto it = idMap.find(oldId); it != idMap.end())
            {
                newId = it->second;
            }
            else
            {
                std::shared_ptr<OpenXMLElement> sourceEntry;
                for (auto& entry : WordNoteHelper::FindNoteEntries<TEntry>(sourceRoot))
                {
                    if (entry && entry->template GetAttributeValue<IntegerValue>(idAttribute).Value() == oldId)
                    {
                        sourceEntry = entry;
                        break;
                    }
                }
                if (!sourceEntry)
                {
                    continue;
                }

                newId = WordNoteHelper::NextNoteId<TEntry>(targetRoot);
                if (auto wrapped = sourceEntry->CopyInto(targetRoot))
                {
                    wrapped->SetAttributeValue<IntegerValue>(idAttribute, IntegerValue(static_cast<Int64>(newId)));
                }
                idMap.emplace(oldId, newId);
            }
            reference->SetId(IntegerValue(static_cast<Int64>(newId)));
        }
    }

    static void MergeComments(const std::shared_ptr<OpenXMLElement>& root,
                              const std::shared_ptr<Packaging::WordprocessingCommentsPart>& sourcePart,
                              const std::shared_ptr<Packaging::WordprocessingCommentsPart>& targetPart,
                              std::map<int, int>& idMap)
    {
        if (!root || !sourcePart || !targetPart)
        {
            return;
        }

        auto starts = root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentRangeStart>();
        auto ends = root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentRangeEnd>();
        auto references = root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentReference>();
        if (starts.empty() && ends.empty() && references.empty())
        {
            return;
        }

        std::shared_ptr<OpenXMLElement> sourceRoot = sourcePart->GetTypedRootElement();
        std::shared_ptr<OpenXMLElement> targetRoot = targetPart->GetTypedRootElement();
        if (!sourceRoot || !targetRoot)
        {
            return;
        }

        const ExyokiOffice::OpenXmlQualifiedName idAttribute(kWordNamespace, "id");

        auto remap = [&](int oldId)
        {
            if (auto it = idMap.find(oldId); it != idMap.end())
            {
                return it->second;
            }

            std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment> sourceEntry;
            for (auto& entry : sourceRoot->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>())
            {
                if (entry)
                {
                    if (auto parsed = WordValueHelper::TryParseInt(entry->GetId().ToString()); parsed && *parsed == oldId)
                    {
                        sourceEntry = entry;
                        break;
                    }
                }
            }

            const int newId = WordNoteHelper::NextCommentId(targetPart);
            if (sourceEntry)
            {
                if (auto wrapped = sourceEntry->CopyInto(targetRoot))
                {
                    wrapped->SetAttributeValue<StringValue>(idAttribute, StringValue(std::to_string(newId)));
                }
            }
            idMap.emplace(oldId, newId);
            return newId;
        };

        for (auto& start : starts)
        {
            if (!start)
            {
                continue;
            }
            if (auto parsed = WordValueHelper::TryParseInt(start->GetId().ToString()))
            {
                start->SetId(StringValue(std::to_string(remap(*parsed))));
            }
        }
        for (auto& end : ends)
        {
            if (!end)
            {
                continue;
            }
            if (auto parsed = WordValueHelper::TryParseInt(end->GetId().ToString()))
            {
                end->SetId(StringValue(std::to_string(remap(*parsed))));
            }
        }
        for (auto& reference : references)
        {
            if (!reference)
            {
                continue;
            }
            if (auto parsed = WordValueHelper::TryParseInt(reference->GetId().ToString()))
            {
                reference->SetId(StringValue(std::to_string(remap(*parsed))));
            }
        }
    }

    // Copies every image payload referenced by a `w:drawing` in `root` into a new
    // target image part (no content-based deduplication, matching
    // AddImageFromData's existing behavior), rewrites the drawing's relationship
    // to point at it, and reassigns the drawing's shape IDs.
    static void MergeImages(const std::shared_ptr<Packaging::MainDocumentPart>& sourceMainPart,
                            const std::shared_ptr<Packaging::MainDocumentPart>& targetMainPart,
                            const std::shared_ptr<OpenXMLElement>& root)
    {
        if (!sourceMainPart || !targetMainPart)
        {
            return;
        }

        for (auto& drawing : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>())
        {
            if (!drawing)
            {
                continue;
            }
            auto info = WordDrawingHelper::ExtractDrawingInfo(drawing);
            if (info.relationshipId.empty())
            {
                continue;
            }

            std::shared_ptr<Packaging::ImagePart> sourceImage;
            for (auto& candidate : sourceMainPart->GetImageParts())
            {
                if (candidate && candidate->RelationshipId() == info.relationshipId)
                {
                    sourceImage = candidate;
                    break;
                }
            }
            if (!sourceImage)
            {
                continue;
            }

            auto targetImage = WordDrawingHelper::CreateImagePartFromData(targetMainPart, sourceImage->GetBinaryData(), sourceImage->ContentType());
            if (!targetImage)
            {
                continue;
            }

            for (auto& blip : drawing->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Blip>())
            {
                if (blip)
                {
                    blip->SetEmbed(StringValue(targetImage->RelationshipId()));
                }
            }

            const auto newDocId = WordIdHelper::NextDocPropertyId(targetMainPart->GetTypedRootElement());
            for (auto& docProps :
                 drawing->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::DocProperties>())
            {
                if (docProps)
                {
                    docProps->SetId(UInt32Value(newDocId));
                }
            }
            for (auto& nvProps : drawing->Descendants<
                                 ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::NonVisualDrawingProperties>())
            {
                if (nvProps)
                {
                    nvProps->SetId(UInt32Value(newDocId));
                }
            }
        }
    }

    // `w:hyperlink` shares its element name with the unrelated CT_HyperlinkRuby
    // shape (see WRD-012), so hyperlinks are located generically by qualified
    // name (mirroring Paragraph::Hyperlinks()) instead of through Descendants<Hyperlink>().
    static void MergeHyperlinks(DocumentMergeState& state,
                                const std::shared_ptr<Packaging::MainDocumentPart>& sourceMainPart,
                                const std::shared_ptr<Packaging::MainDocumentPart>& targetMainPart,
                                const std::shared_ptr<OpenXMLElement>& root)
    {
        if (!sourceMainPart || !targetMainPart)
        {
            return;
        }

        std::vector<std::shared_ptr<OpenXMLElement>> hyperlinks;
        WordBodyHelper::CollectDescendantsByName(root, ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "hyperlink"), hyperlinks);
        if (hyperlinks.empty())
        {
            return;
        }

        const ExyokiOffice::OpenXmlQualifiedName relationshipIdAttribute(kOfficeRelationshipsNamespace, "id");
        const ExyokiOffice::OpenXmlQualifiedName anchorAttribute(kWordNamespace, "anchor");

        for (auto& hyperlink : hyperlinks)
        {
            if (!hyperlink)
            {
                continue;
            }

            std::string_view oldRelationshipId;
            if (hyperlink->TryGetAttribute(relationshipIdAttribute, oldRelationshipId) && !oldRelationshipId.empty())
            {
                std::string target;
                for (const auto& relationship : sourceMainPart->Relationships())
                {
                    if (relationship.IsExternal && relationship.Type == kHyperlinkRelationshipType &&
                        relationship.Id == oldRelationshipId)
                    {
                        target = relationship.Target;
                        break;
                    }
                }
                if (!target.empty())
                {
                    auto newRelationshipId = targetMainPart->AddExternalRelationship(kHyperlinkRelationshipType, target);
                    if (!newRelationshipId.empty())
                    {
                        hyperlink->SetAttribute(relationshipIdAttribute, newRelationshipId);
                    }
                }
                continue;
            }

            std::string_view oldAnchor;
            if (hyperlink->TryGetAttribute(anchorAttribute, oldAnchor) && !oldAnchor.empty())
            {
                if (auto it = state.BookmarkNames.find(std::string(oldAnchor)); it != state.BookmarkNames.end())
                {
                    hyperlink->SetAttribute(anchorAttribute, it->second);
                }
            }
        }
    }

    /**
     * @brief Bookkeeping for Word's threaded comment model.
     *
     * `/word/comments.xml` knows nothing about threads: a reply is an ordinary
     * `<w:comment>` entry with its own numeric ID and its own body range markers.
     * The thread shape lives in four satellite parts, and this helper owns every
     * rule that keeps them consistent with the comment entries:
     *
     * - `commentsExtended` (`w15:commentEx`): parent link and resolution flag,
     *   keyed by the `w14:paraId` of the comment's **last** paragraph.
     * - `commentsIds` (`w16cid:commentId`): that paraId mapped to a durable ID.
     * - `commentsExtensible` (`w16cex:commentExtensible`): the UTC timestamp, keyed
     *   by the durable ID rather than by the paraId.
     * - `people` (`w15:person`): one entry per author display name.
     *
     * Both ID kinds are 4-byte hex values that Word ignores when the high bit is
     * set, so allocation is restricted to [0x00000001, 0x7FFFFFFF].
     */
    class CommentThreading final
    {
    public:
        using CommentEntry = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment;
        using CommentEx = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::CommentEx;
        using CommentsEx = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::CommentsEx;
        using CommentId = ExyokiOffice::DocumentFormat::OpenXml::Office2019::Word::Cid::CommentId;
        using CommentsIds = ExyokiOffice::DocumentFormat::OpenXml::Office2019::Word::Cid::CommentsIds;
        using CommentExtensible = ExyokiOffice::DocumentFormat::OpenXml::Office2021::Word::CommentsExt::CommentExtensible;
        using CommentsExtensible = ExyokiOffice::DocumentFormat::OpenXml::Office2021::Word::CommentsExt::CommentsExtensible;
        using People = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::People;
        using Person = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::Person;
        using PresenceInfo = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Word::PresenceInfo;
        using DomParagraph = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph;
        using DomRun = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run;
        using DomTableRow = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow;
        using DomCommentRangeStart = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentRangeStart;
        using DomCommentRangeEnd = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentRangeEnd;
        using DomCommentReference = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentReference;
        using MainPart = std::shared_ptr<Packaging::MainDocumentPart>;

        /// Lowest ID Word still reads back.
        static constexpr UInt32 kMinimumId = 0x00000001u;
        /// Highest ID without the high bit set; above this Word ignores the row.
        static constexpr UInt32 kMaximumId = 0x7FFFFFFFu;
        /// Fixed starting point for allocation, so output stays reproducible.
        static constexpr UInt32 kFirstAllocatedId = 0x10000000u;
        /// Guards the thread walks against cycles in a hand-edited document.
        static constexpr int kMaximumThreadDepth = 64;

        /// Reads a 4-byte hex ID as a number; returns 0 when absent or malformed.
        [[nodiscard]] static UInt32 ReadId(const HexBinaryValue& value)
        {
            if (!value.IsDefined())
            {
                return 0;
            }
            const auto& bytes = value.Value();
            if (bytes.empty() || bytes.size() > sizeof(UInt32))
            {
                return 0;
            }
            UInt32 result = 0;
            for (const auto byte : bytes)
            {
                result = (result << 8) | static_cast<UInt32>(byte);
            }
            return result;
        }

        /// Formats a number as the 4-byte, 8-digit uppercase hex ID Word expects.
        [[nodiscard]] static HexBinaryValue MakeId(UInt32 value)
        {
            return HexBinaryValue(std::vector<Byte>{static_cast<Byte>((value >> 24) & 0xFFu),
                                                    static_cast<Byte>((value >> 16) & 0xFFu),
                                                    static_cast<Byte>((value >> 8) & 0xFFu),
                                                    static_cast<Byte>(value & 0xFFu)});
        }

        [[nodiscard]] static std::shared_ptr<CommentsEx> GetCommentsExRoot(const MainPart& mainDocumentPart)
        {
            auto part = mainDocumentPart ? mainDocumentPart->GetWordprocessingCommentsExPart() : nullptr;
            return part ? part->GetTypedRootElement() : nullptr;
        }

        static std::shared_ptr<CommentsEx> EnsureCommentsExRoot(const MainPart& mainDocumentPart)
        {
            if (!mainDocumentPart)
            {
                return nullptr;
            }
            auto part = mainDocumentPart->GetWordprocessingCommentsExPart();
            if (!part)
            {
                part = mainDocumentPart->AddWordprocessingCommentsExPart();
            }
            return part ? part->GetTypedRootElement() : nullptr;
        }

        [[nodiscard]] static std::shared_ptr<CommentsIds> GetCommentsIdsRoot(const MainPart& mainDocumentPart)
        {
            auto part = mainDocumentPart ? mainDocumentPart->GetWordprocessingCommentsIdsPart() : nullptr;
            return part ? part->GetTypedRootElement() : nullptr;
        }

        static std::shared_ptr<CommentsIds> EnsureCommentsIdsRoot(const MainPart& mainDocumentPart)
        {
            if (!mainDocumentPart)
            {
                return nullptr;
            }
            auto part = mainDocumentPart->GetWordprocessingCommentsIdsPart();
            if (!part)
            {
                part = mainDocumentPart->AddWordprocessingCommentsIdsPart();
            }
            return part ? part->GetTypedRootElement() : nullptr;
        }

        [[nodiscard]] static std::shared_ptr<CommentsExtensible> GetCommentsExtensibleRoot(const MainPart& mainDocumentPart)
        {
            auto part = mainDocumentPart ? mainDocumentPart->GetWordCommentsExtensiblePart() : nullptr;
            return part ? part->GetTypedRootElement() : nullptr;
        }

        static std::shared_ptr<CommentsExtensible> EnsureCommentsExtensibleRoot(const MainPart& mainDocumentPart)
        {
            if (!mainDocumentPart)
            {
                return nullptr;
            }
            auto part = mainDocumentPart->GetWordCommentsExtensiblePart();
            if (!part)
            {
                part = mainDocumentPart->AddWordCommentsExtensiblePart();
            }
            return part ? part->GetTypedRootElement() : nullptr;
        }

        static std::shared_ptr<People> EnsurePeopleRoot(const MainPart& mainDocumentPart)
        {
            if (!mainDocumentPart)
            {
                return nullptr;
            }
            auto part = mainDocumentPart->GetWordprocessingPeoplePart();
            if (!part)
            {
                part = mainDocumentPart->AddWordprocessingPeoplePart();
            }
            return part ? part->GetTypedRootElement() : nullptr;
        }

        /// Every paragraph ID and durable ID the document already spends.
        [[nodiscard]] static std::set<UInt32> CollectUsedIds(const MainPart& mainDocumentPart)
        {
            std::set<UInt32> used;
            if (!mainDocumentPart)
            {
                return used;
            }

            const auto collectParagraphIds = [&used](const std::shared_ptr<OpenXMLElement>& root)
            {
                if (!root)
                {
                    return;
                }
                for (const auto& paragraph : root->Descendants<DomParagraph>())
                {
                    if (paragraph)
                    {
                        used.insert(ReadId(paragraph->GetParagraphId()));
                    }
                }
                for (const auto& row : root->Descendants<DomTableRow>())
                {
                    if (row)
                    {
                        used.insert(ReadId(row->GetParagraphId()));
                    }
                }
            };

            collectParagraphIds(mainDocumentPart->GetTypedRootElement());
            if (auto commentsPart = mainDocumentPart->GetWordprocessingCommentsPart())
            {
                collectParagraphIds(commentsPart->GetTypedRootElement());
            }
            if (auto root = GetCommentsExRoot(mainDocumentPart))
            {
                for (const auto& row : root->Elements<CommentEx>())
                {
                    if (row)
                    {
                        used.insert(ReadId(row->GetParaId()));
                        used.insert(ReadId(row->GetParaIdParent()));
                    }
                }
            }
            if (auto root = GetCommentsIdsRoot(mainDocumentPart))
            {
                for (const auto& row : root->Elements<CommentId>())
                {
                    if (row)
                    {
                        used.insert(ReadId(row->GetParaId()));
                        used.insert(ReadId(row->GetDurableId()));
                    }
                }
            }
            if (auto root = GetCommentsExtensibleRoot(mainDocumentPart))
            {
                for (const auto& row : root->Elements<CommentExtensible>())
                {
                    if (row)
                    {
                        used.insert(ReadId(row->GetDurableId()));
                    }
                }
            }
            return used;
        }

        /**
         * @brief Hands out an ID that is not taken yet and marks it as taken.
         *
         * The walk starts at a fixed seed and steps upwards, wrapping once through
         * the low range: no randomness, so the same document always produces the
         * same IDs.
         *
         * @return The allocated ID, or 0 when the whole positive range is spent.
         */
        static UInt32 AllocateId(std::set<UInt32>& used)
        {
            UInt32 candidate = kFirstAllocatedId;
            for (UInt32 step = 0; step < kMaximumId; ++step)
            {
                if (used.insert(candidate).second)
                {
                    return candidate;
                }
                candidate = (candidate == kMaximumId) ? kMinimumId : candidate + 1;
            }
            return 0;
        }

        [[nodiscard]] static std::shared_ptr<DomParagraph> LastParagraph(const std::shared_ptr<CommentEntry>& comment)
        {
            if (!comment)
            {
                return nullptr;
            }
            auto paragraphs = comment->Elements<DomParagraph>();
            return paragraphs.empty() ? nullptr : paragraphs.back();
        }

        /// The thread key of a comment: the paraId of its last paragraph, or 0.
        [[nodiscard]] static UInt32 GetThreadParaId(const std::shared_ptr<CommentEntry>& comment)
        {
            auto paragraph = LastParagraph(comment);
            return paragraph ? ReadId(paragraph->GetParagraphId()) : 0;
        }

        /// Same as GetThreadParaId(), but creates the anchor paragraph and/or its ID.
        static UInt32 EnsureThreadParaId(const MainPart& mainDocumentPart, const std::shared_ptr<CommentEntry>& comment)
        {
            if (!comment)
            {
                return 0;
            }
            auto paragraph = LastParagraph(comment);
            if (!paragraph)
            {
                paragraph = comment->AppendChild<DomParagraph>();
                if (!paragraph)
                {
                    return 0;
                }
            }
            if (const auto existing = ReadId(paragraph->GetParagraphId()); existing != 0)
            {
                return existing;
            }
            auto used = CollectUsedIds(mainDocumentPart);
            const auto allocated = AllocateId(used);
            if (allocated != 0)
            {
                paragraph->SetParagraphId(MakeId(allocated));
            }
            return allocated;
        }

        [[nodiscard]] static std::shared_ptr<CommentEx> FindCommentEx(const MainPart& mainDocumentPart, UInt32 paraId)
        {
            if (paraId == 0)
            {
                return nullptr;
            }
            auto root = GetCommentsExRoot(mainDocumentPart);
            if (!root)
            {
                return nullptr;
            }
            for (const auto& row : root->Elements<CommentEx>())
            {
                if (row && ReadId(row->GetParaId()) == paraId)
                {
                    return row;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static std::shared_ptr<CommentId> FindCommentId(const MainPart& mainDocumentPart, UInt32 paraId)
        {
            if (paraId == 0)
            {
                return nullptr;
            }
            auto root = GetCommentsIdsRoot(mainDocumentPart);
            if (!root)
            {
                return nullptr;
            }
            for (const auto& row : root->Elements<CommentId>())
            {
                if (row && ReadId(row->GetParaId()) == paraId)
                {
                    return row;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static std::shared_ptr<CommentExtensible> FindCommentExtensible(const MainPart& mainDocumentPart,
                                                                                      UInt32 durableId)
        {
            if (durableId == 0)
            {
                return nullptr;
            }
            auto root = GetCommentsExtensibleRoot(mainDocumentPart);
            if (!root)
            {
                return nullptr;
            }
            for (const auto& row : root->Elements<CommentExtensible>())
            {
                if (row && ReadId(row->GetDurableId()) == durableId)
                {
                    return row;
                }
            }
            return nullptr;
        }

        /**
         * @brief Creates or refreshes every satellite row describing one comment.
         *
         * @param parentParaId Thread key of the comment being replied to, or 0 for a
         * thread root. An existing parent link is never cleared by passing 0, so the
         * call stays safe to repeat on a reply.
         */
        static void Register(const MainPart& mainDocumentPart,
                             const std::shared_ptr<CommentEntry>& comment,
                             UInt32 parentParaId)
        {
            if (!mainDocumentPart || !comment)
            {
                return;
            }

            const auto paraId = EnsureThreadParaId(mainDocumentPart, comment);
            if (paraId == 0)
            {
                return;
            }

            auto commentsExRoot = EnsureCommentsExRoot(mainDocumentPart);
            if (!commentsExRoot)
            {
                return;
            }
            auto commentEx = FindCommentEx(mainDocumentPart, paraId);
            if (!commentEx)
            {
                commentEx = commentsExRoot->AppendChild<CommentEx>();
            }
            if (!commentEx)
            {
                return;
            }
            commentEx->SetParaId(MakeId(paraId));
            if (parentParaId != 0)
            {
                commentEx->SetParaIdParent(MakeId(parentParaId));
            }
            if (!commentEx->GetDone().IsDefined())
            {
                commentEx->SetDone(OnOffValue(false));
            }

            const auto durableId = EnsureDurableId(mainDocumentPart, paraId);
            if (durableId != 0)
            {
                if (auto root = EnsureCommentsExtensibleRoot(mainDocumentPart))
                {
                    auto extensible = FindCommentExtensible(mainDocumentPart, durableId);
                    if (!extensible)
                    {
                        extensible = root->AppendChild<CommentExtensible>();
                    }
                    if (extensible)
                    {
                        extensible->SetDurableId(MakeId(durableId));
                        const auto date = comment->GetDate();
                        extensible->SetDateUtc(date.IsDefined() ? date
                                                                : DateTimeValue(std::chrono::system_clock::now()));
                    }
                }
            }

            RegisterPerson(mainDocumentPart, comment->GetAuthor().ToString());
        }

        /// Adds a `w15:person` row for an author name, unless one already exists.
        static void RegisterPerson(const MainPart& mainDocumentPart, const std::string& author)
        {
            if (author.empty())
            {
                return;
            }
            auto root = EnsurePeopleRoot(mainDocumentPart);
            if (!root)
            {
                return;
            }
            for (const auto& person : root->Elements<Person>())
            {
                if (person && person->GetAuthor().ToString() == author)
                {
                    return;
                }
            }
            auto person = root->AppendChild<Person>();
            if (!person)
            {
                return;
            }
            person->SetAuthor(StringValue(author));
            if (auto presence = person->AppendChild<PresenceInfo>())
            {
                // "None" is what Word writes for an author without a linked account.
                presence->SetProviderId(StringValue(std::string("None")));
                presence->SetUserId(StringValue(author));
            }
        }

        /**
         * @brief Deletes the satellite rows of one comment.
         *
         * The `people` part is deliberately left alone: an author outlives the
         * comments they wrote, exactly as in Word.
         */
        static void Unregister(const MainPart& mainDocumentPart, UInt32 paraId)
        {
            if (!mainDocumentPart || paraId == 0)
            {
                return;
            }

            UInt32 durableId = 0;
            if (auto root = GetCommentsIdsRoot(mainDocumentPart))
            {
                for (const auto& row : root->Elements<CommentId>())
                {
                    if (row && ReadId(row->GetParaId()) == paraId)
                    {
                        durableId = ReadId(row->GetDurableId());
                        root->RemoveChild(row);
                    }
                }
            }
            if (auto root = GetCommentsExRoot(mainDocumentPart))
            {
                for (const auto& row : root->Elements<CommentEx>())
                {
                    if (row && ReadId(row->GetParaId()) == paraId)
                    {
                        root->RemoveChild(row);
                    }
                }
            }
            if (durableId != 0)
            {
                if (auto root = GetCommentsExtensibleRoot(mainDocumentPart))
                {
                    for (const auto& row : root->Elements<CommentExtensible>())
                    {
                        if (row && ReadId(row->GetDurableId()) == durableId)
                        {
                            root->RemoveChild(row);
                        }
                    }
                }
            }
        }

        /// Every `<w:comment>` entry of the document, in part order.
        [[nodiscard]] static std::vector<std::shared_ptr<CommentEntry>> AllComments(const MainPart& mainDocumentPart)
        {
            std::vector<std::shared_ptr<CommentEntry>> result;
            auto part = mainDocumentPart ? mainDocumentPart->GetWordprocessingCommentsPart() : nullptr;
            auto root = part ? part->GetTypedRootElement() : nullptr;
            if (!root)
            {
                return result;
            }
            for (const auto& entry : root->Elements<CommentEntry>())
            {
                if (entry)
                {
                    result.push_back(entry);
                }
            }
            return result;
        }

        [[nodiscard]] static std::shared_ptr<CommentEntry> FindByThreadParaId(const MainPart& mainDocumentPart,
                                                                              UInt32 paraId)
        {
            if (paraId == 0)
            {
                return nullptr;
            }
            for (const auto& entry : AllComments(mainDocumentPart))
            {
                if (GetThreadParaId(entry) == paraId)
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /// The comments whose `w15:paraIdParent` points at this thread key.
        [[nodiscard]] static std::vector<std::shared_ptr<CommentEntry>> DirectReplies(const MainPart& mainDocumentPart,
                                                                                      UInt32 paraId)
        {
            std::vector<std::shared_ptr<CommentEntry>> result;
            if (paraId == 0)
            {
                return result;
            }
            for (const auto& entry : AllComments(mainDocumentPart))
            {
                const auto replyParaId = GetThreadParaId(entry);
                if (replyParaId == 0 || replyParaId == paraId)
                {
                    continue;
                }
                auto row = FindCommentEx(mainDocumentPart, replyParaId);
                if (row && ReadId(row->GetParaIdParent()) == paraId)
                {
                    result.push_back(entry);
                }
            }
            return result;
        }

        /// Climbs `w15:paraIdParent` links to the thread root's key.
        [[nodiscard]] static UInt32 ThreadRootParaId(const MainPart& mainDocumentPart, UInt32 paraId)
        {
            UInt32 current = paraId;
            for (int depth = 0; depth < kMaximumThreadDepth && current != 0; ++depth)
            {
                auto row = FindCommentEx(mainDocumentPart, current);
                if (!row)
                {
                    break;
                }
                const auto parent = ReadId(row->GetParaIdParent());
                if (parent == 0 || parent == current || !FindCommentEx(mainDocumentPart, parent))
                {
                    break;
                }
                current = parent;
            }
            return current;
        }

        /// Writes `w15:done` on one thread key and on everything below it.
        static void SetDoneRecursive(const MainPart& mainDocumentPart, UInt32 paraId, bool resolved, int depth)
        {
            if (paraId == 0 || depth >= kMaximumThreadDepth)
            {
                return;
            }
            if (auto row = FindCommentEx(mainDocumentPart, paraId))
            {
                row->SetDone(OnOffValue(resolved));
            }
            for (const auto& reply : DirectReplies(mainDocumentPart, paraId))
            {
                SetDoneRecursive(mainDocumentPart, GetThreadParaId(reply), resolved, depth + 1);
            }
        }

        /**
         * @brief Inserts range markers and a reference for a reply.
         *
         * The reply must cover exactly the same span as the comment it answers, so
         * the markers are threaded through the parent's, producing the shape Word
         * itself writes: all range starts first, then the content, then one
         * `<w:commentRangeEnd/>` plus reference run per comment on the span.
         *
         * @return Whether the whole marker triple could be placed.
         */
        static bool AddReplyBodyMarkers(const MainPart& mainDocumentPart,
                                        const std::string& parentIdText,
                                        const std::string& replyIdText)
        {
            auto root = mainDocumentPart ? mainDocumentPart->GetTypedRootElement() : nullptr;
            if (!root)
            {
                return false;
            }

            std::shared_ptr<OpenXMLElement> parentStart;
            for (const auto& start : root->Descendants<DomCommentRangeStart>())
            {
                if (start && start->GetId().ToString() == parentIdText)
                {
                    parentStart = start;
                    break;
                }
            }
            std::shared_ptr<OpenXMLElement> parentEnd;
            for (const auto& end : root->Descendants<DomCommentRangeEnd>())
            {
                if (end && end->GetId().ToString() == parentIdText)
                {
                    parentEnd = end;
                    break;
                }
            }
            if (!parentStart || !parentEnd)
            {
                return false;
            }

            auto startOwner = parentStart->Parent();
            auto endOwner = parentEnd->Parent();
            if (!startOwner || !endOwner)
            {
                return false;
            }

            const ExyokiOffice::OpenXmlQualifiedName rangeStartName(kWordNamespace, "commentRangeStart");
            const ExyokiOffice::OpenXmlQualifiedName rangeEndName(kWordNamespace, "commentRangeEnd");

            auto startAnchor = parentStart;
            while (auto next = startAnchor->NextSibling())
            {
                if (next->QualifiedName() != rangeStartName)
                {
                    break;
                }
                startAnchor = next;
            }

            auto endAnchor = parentEnd;
            while (auto next = endAnchor->NextSibling())
            {
                if (next->QualifiedName() != rangeEndName && !IsCommentReferenceRun(next))
                {
                    break;
                }
                endAnchor = next;
            }

            auto rangeStart = startOwner->InsertChildAfter<DomCommentRangeStart>(startAnchor);
            if (!rangeStart)
            {
                return false;
            }
            rangeStart->SetId(StringValue(replyIdText));

            auto rangeEnd = endOwner->InsertChildAfter<DomCommentRangeEnd>(endAnchor);
            if (!rangeEnd)
            {
                startOwner->RemoveChild(rangeStart);
                return false;
            }
            rangeEnd->SetId(StringValue(replyIdText));

            auto referenceRun = endOwner->InsertChildAfter<DomRun>(rangeEnd);
            auto reference = referenceRun ? referenceRun->AppendChild<DomCommentReference>() : nullptr;
            if (!reference)
            {
                if (referenceRun)
                {
                    endOwner->RemoveChild(referenceRun);
                }
                endOwner->RemoveChild(rangeEnd);
                startOwner->RemoveChild(rangeStart);
                return false;
            }
            reference->SetId(StringValue(replyIdText));
            return true;
        }

        /**
         * @brief Restores the "one thread key per comment" invariant after a merge.
         *
         * Merging copies `<w:comment>` entries verbatim, paragraph IDs included,
         * while the satellite parts stay behind in the source. Since IDs are handed
         * out deterministically, two documents written by this library allocate from
         * the same sequence and the copies land on paragraph IDs the target already
         * spends. The first entry holding an ID keeps it (that is the target's own
         * comment, whose commentsExtended row has to stay valid) and every later
         * collision is re-keyed. Paragraphs without an ID are left as they are.
         */
        static void EnsureUniqueThreadParaIds(const MainPart& mainDocumentPart)
        {
            if (!mainDocumentPart)
            {
                return;
            }

            auto used = CollectUsedIds(mainDocumentPart);
            std::set<UInt32> seen;
            for (const auto& entry : AllComments(mainDocumentPart))
            {
                for (const auto& paragraph : entry->Elements<DomParagraph>())
                {
                    if (!paragraph)
                    {
                        continue;
                    }
                    const auto paraId = ReadId(paragraph->GetParagraphId());
                    if (paraId == 0 || seen.insert(paraId).second)
                    {
                        continue;
                    }
                    if (const auto replacement = AllocateId(used); replacement != 0)
                    {
                        paragraph->SetParagraphId(MakeId(replacement));
                        seen.insert(replacement);
                    }
                }
            }
        }

        /// Deletes every range marker and reference in the body for one comment ID.
        static void RemoveBodyMarkers(const MainPart& mainDocumentPart, const std::string& idText)
        {
            auto root = mainDocumentPart ? mainDocumentPart->GetTypedRootElement() : nullptr;
            if (!root)
            {
                return;
            }
            for (const auto& reference : root->Descendants<DomCommentReference>())
            {
                if (reference && reference->GetId().ToString() == idText)
                {
                    WordStructureHelper::RemoveMarkerAndOwningRun(reference);
                }
            }
            for (const auto& start : root->Descendants<DomCommentRangeStart>())
            {
                if (start && start->GetId().ToString() == idText)
                {
                    WordStructureHelper::RemoveMarkerAndOwningRun(start);
                }
            }
            for (const auto& end : root->Descendants<DomCommentRangeEnd>())
            {
                if (end && end->GetId().ToString() == idText)
                {
                    WordStructureHelper::RemoveMarkerAndOwningRun(end);
                }
            }
        }

    private:
        /// Maps a thread key to its durable ID, allocating and storing one if needed.
        static UInt32 EnsureDurableId(const MainPart& mainDocumentPart, UInt32 paraId)
        {
            auto root = EnsureCommentsIdsRoot(mainDocumentPart);
            if (!root)
            {
                return 0;
            }

            auto commentId = FindCommentId(mainDocumentPart, paraId);
            UInt32 durableId = commentId ? ReadId(commentId->GetDurableId()) : 0;
            if (durableId == 0)
            {
                auto used = CollectUsedIds(mainDocumentPart);
                durableId = AllocateId(used);
            }
            if (durableId == 0)
            {
                return 0;
            }

            if (!commentId)
            {
                commentId = root->AppendChild<CommentId>();
            }
            if (!commentId)
            {
                return 0;
            }
            commentId->SetParaId(MakeId(paraId));
            commentId->SetDurableId(MakeId(durableId));
            return durableId;
        }

        [[nodiscard]] static bool IsCommentReferenceRun(const std::shared_ptr<OpenXMLElement>& element)
        {
            if (!element || element->QualifiedName() != ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "r"))
            {
                return false;
            }
            return element->GetChild(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "commentReference")) != nullptr;
        }
    };
};

WordDocumentEditor::WordDocumentEditor(const WordDocument::Ptr& document)
    : m_document(document)
{
}

WordDocumentEditor::~WordDocumentEditor()
{
    if (m_transactionOwner)
    {
        m_transactionOwner->Invalidate(this);
    }
}

WordDocumentEditor::BodyCursor::BodyCursor(WordDocument::Ptr document,
                                           Placement placement,
                                           std::shared_ptr<ExyokiOffice::OpenXMLElement> anchor)
    : m_document(std::move(document)),
      m_placement(placement),
      m_anchor(std::move(anchor))
{
}

bool WordDocumentEditor::BodyCursor::IsValid() const
{
    if (!m_document)
    {
        return false;
    }
    if ((m_placement == Placement::Before || m_placement == Placement::After) && !m_anchor)
    {
        return false;
    }
    return GetBody() != nullptr;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>
WordDocumentEditor::BodyCursor::GetBody() const
{
    return WordStructureHelper::EnsureBody(m_document);
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> WordDocumentEditor::BodyCursor::ResolveInsertionReference(
    const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>& body) const
{
    if (!body)
    {
        return nullptr;
    }

    switch (m_placement)
    {
        case Placement::Start:
            return WordBodyHelper::FindFirstBodyChild(body);
        case Placement::End:
            return WordBodyHelper::FindTrailingSectionProperties(body);
        case Placement::Before:
            return m_anchor;
        case Placement::After:
            return m_anchor ? m_anchor->NextSibling() : nullptr;
    }
    return nullptr;
}

std::shared_ptr<Paragraph> WordDocumentEditor::BodyCursor::InsertParagraph() const
{
    if (!IsValid())
    {
        return nullptr;
    }

    auto body = GetBody();
    auto paragraph = body->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(
        ResolveInsertionReference(body));
    if (!paragraph)
    {
        return nullptr;
    }
    return std::make_shared<Paragraph>(paragraph, WordStructureHelper::GetMainDocumentPart(m_document));
}

std::shared_ptr<Paragraph> WordDocumentEditor::BodyCursor::InsertParagraph(std::string_view text) const
{
    auto paragraph = InsertParagraph();
    if (!paragraph)
    {
        return nullptr;
    }
    paragraph->AddText(text);
    return paragraph;
}

std::shared_ptr<Table> WordDocumentEditor::BodyCursor::InsertTable(Size rows, Size columns) const
{
    if (!IsValid())
    {
        return nullptr;
    }

    auto body = GetBody();
    auto table = body->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>(
        ResolveInsertionReference(body));
    if (!table)
    {
        return nullptr;
    }

    auto wrapper = std::make_shared<Table>(table);
    if (!wrapper)
    {
        return nullptr;
    }

    for (Size row = 0; row < rows; ++row)
    {
        wrapper->AddRow(columns);
    }
    return wrapper;
}

std::shared_ptr<Section> WordDocumentEditor::BodyCursor::InsertSectionBreak(SectionStartType startType) const
{
    auto paragraph = InsertParagraph();
    if (!paragraph)
    {
        return nullptr;
    }

    auto lowLevelParagraph = paragraph->GetLowLevelApi();
    if (!lowLevelParagraph)
    {
        return nullptr;
    }

    auto properties = WordPropertiesElementHelper::EnsureParagraphProperties(lowLevelParagraph);
    if (!properties)
    {
        return nullptr;
    }

    auto sectionProperties =
        WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionProperties>(
            properties);
    if (!sectionProperties)
    {
        return nullptr;
    }

    auto section = std::make_shared<Section>(sectionProperties, WordStructureHelper::GetMainDocumentPart(m_document));
    section->SetStartType(startType);
    return section;
}

std::shared_ptr<ContentControl> WordDocumentEditor::BodyCursor::InsertContentControl(std::string_view tag,
                                                                                     std::string_view alias) const
{
    if (!IsValid())
    {
        return nullptr;
    }

    auto body = GetBody();
    auto sdt = body->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtBlock>(
        ResolveInsertionReference(body));
    if (!sdt)
    {
        return nullptr;
    }

    auto control = std::make_shared<ContentControl>(sdt, ContentControlLevel::Block, WordStructureHelper::GetMainDocumentPart(m_document));
    control->EnsureId();
    if (!tag.empty())
    {
        control->SetTag(tag);
    }
    if (!alias.empty())
    {
        control->SetAlias(alias);
    }
    return control;
}

bool WordDocumentEditor::BodyCursor::InsertDocument(const WordDocumentEditor& source,
                                                    const DocumentMergeOptions& options) const
{
    if (!IsValid())
    {
        return false;
    }

    auto sourceMainPart = WordStructureHelper::GetMainDocumentPart(source.GetLowLevelApi());
    auto sourceBody = WordBodyHelper::GetBody(source.GetLowLevelApi());
    if (!sourceMainPart || !sourceBody)
    {
        // Nothing to copy is not a failure of this cursor or the target document.
        return true;
    }

    auto targetBody = GetBody();
    auto targetMainPart = WordStructureHelper::EnsureMainDocumentPart(m_document);
    if (!targetBody || !targetMainPart)
    {
        return false;
    }

    auto insertBeforeRef = ResolveInsertionReference(targetBody);
    auto trailingSectPr = WordBodyHelper::FindTrailingSectionProperties(sourceBody);

    WordDocumentEditor targetEditor(m_document);
    WordMergeHelper::DocumentMergeState state(targetEditor, source, options);

    // Every bookmark name already used in the target document must be known
    // before any copied bookmark is (possibly) renamed for uniqueness.
    for (const auto& bookmark : targetEditor.Bookmarks())
    {
        if (bookmark)
        {
            state.UsedBookmarkNames.insert(bookmark->GetName());
        }
    }

    // Phase 1: deep-copy every top-level body block (except the source's own
    // trailing section properties) into the target tree, preserving order.
    std::vector<std::shared_ptr<OpenXMLElement>> insertedRoots;
    for (const auto& child : sourceBody->Children())
    {
        if (!child)
        {
            continue;
        }
        if (trailingSectPr && child->IsSameNode(*trailingSectPr))
        {
            continue;
        }

        if (auto wrapped = child->CopyInto(targetBody, insertBeforeRef))
        {
            insertedRoots.push_back(wrapped);
        }
    }

    // Phase 2: remap styles, numbering, bookmarks, content control IDs,
    // images, and footnotes/endnotes/comments for each copied root. Bookmark
    // renames must be known before hyperlinks are rewritten (phase 3), since
    // an internal hyperlink can anchor into a differently-ordered block.
    auto sourceFootnotesPart = sourceMainPart->GetFootnotesPart();
    auto sourceEndnotesPart = sourceMainPart->GetEndnotesPart();
    auto sourceCommentsPart = sourceMainPart->GetWordprocessingCommentsPart();

    for (const auto& root : insertedRoots)
    {
        for (auto& pStyle :
             root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>())
        {
            if (!pStyle)
            {
                continue;
            }
            auto val = pStyle->GetVal().ToString();
            if (!val.empty())
            {
                pStyle->SetVal(StringValue(WordMergeHelper::MergeStyleId(state, val)));
            }
        }
        for (auto& rStyle : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunStyle>())
        {
            if (!rStyle)
            {
                continue;
            }
            auto val = rStyle->GetVal().ToString();
            if (!val.empty())
            {
                rStyle->SetVal(StringValue(WordMergeHelper::MergeStyleId(state, val)));
            }
        }
        for (auto& tStyle : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableStyle>())
        {
            if (!tStyle)
            {
                continue;
            }
            auto val = tStyle->GetVal().ToString();
            if (!val.empty())
            {
                tStyle->SetVal(StringValue(WordMergeHelper::MergeStyleId(state, val)));
            }
        }

        for (auto& numId : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingId>())
        {
            if (!numId)
            {
                continue;
            }
            auto val = numId->GetVal();
            if (val.IsDefined())
            {
                numId->SetVal(Int32Value(WordMergeHelper::MergeNumberingId(state, val.Value())));
            }
        }

        WordMergeHelper::MergeBookmarks(state, targetMainPart, root);
        WordMergeHelper::MergeContentControlIds(targetMainPart, root);
        WordMergeHelper::MergeImages(sourceMainPart, targetMainPart, root);

        if (sourceFootnotesPart)
        {
            auto targetFootnotesPart = targetMainPart->GetFootnotesPart();
            if (!targetFootnotesPart)
            {
                targetFootnotesPart = targetMainPart->AddFootnotesPart();
            }
            WordMergeHelper::MergeNoteReferences<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Footnote,
                                                 ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteReference>(
                root, sourceFootnotesPart, targetFootnotesPart, state.FootnoteIds);
        }
        if (sourceEndnotesPart)
        {
            auto targetEndnotesPart = targetMainPart->GetEndnotesPart();
            if (!targetEndnotesPart)
            {
                targetEndnotesPart = targetMainPart->AddEndnotesPart();
            }
            WordMergeHelper::MergeNoteReferences<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Endnote,
                                                 ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::EndnoteReference>(
                root, sourceEndnotesPart, targetEndnotesPart, state.EndnoteIds);
        }
        if (sourceCommentsPart)
        {
            auto targetCommentsPart = targetMainPart->GetWordprocessingCommentsPart();
            if (!targetCommentsPart)
            {
                targetCommentsPart = targetMainPart->AddWordprocessingCommentsPart();
            }
            WordMergeHelper::MergeComments(root, sourceCommentsPart, targetCommentsPart, state.CommentIds);

            // The copied entries bring the source's paragraph IDs along, which
            // are the keys the target's own comment threads hang from.
            WordMergeHelper::CommentThreading::EnsureUniqueThreadParaIds(targetMainPart);
        }
    }

    // Phase 3: rewrite hyperlinks now that all bookmark renames are known.
    for (const auto& root : insertedRoots)
    {
        WordMergeHelper::MergeHyperlinks(state, sourceMainPart, targetMainPart, root);
    }

    return true;
}

WordDocumentEditor::Ptr WordDocumentEditor::Create(const WordDocument::Ptr& document)
{
    return std::make_shared<WordDocumentEditor>(document);
}

WordDocumentEditor::Ptr WordDocumentEditor::CreateNew(WordprocessingDocumentType type)
{
    auto editor = std::make_shared<WordDocumentEditor>();
    if (!editor || !editor->CreateDefaultDocument(type))
    {
        return nullptr;
    }
    return editor;
}

WordDocumentEditor::Ptr WordDocumentEditor::Open(const std::filesystem::path& path,
                                                 const ExyokiOffice::Packaging::OpenSettings& settings,
                                                 const ICancellationToken* cancellationToken)
{
    auto document = WordDocument::Open(path, settings, cancellationToken);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<WordDocumentEditor>(document);
}

WordDocumentEditor::Ptr WordDocumentEditor::Open(const std::vector<Byte>& packageBuffer,
                                                 const ExyokiOffice::Packaging::OpenSettings& settings,
                                                 const ICancellationToken* cancellationToken)
{
    auto document = WordDocument::Open(packageBuffer, settings, cancellationToken);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<WordDocumentEditor>(document);
}

WordDocumentEditor::Ptr WordDocumentEditor::Open(std::span<const Byte> packageBuffer,
                                                 const ExyokiOffice::Packaging::OpenSettings& settings,
                                                 const ICancellationToken* cancellationToken)
{
    auto document = WordDocument::Open(packageBuffer, settings, cancellationToken);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<WordDocumentEditor>(document);
}

WordDocumentEditor::Ptr WordDocumentEditor::CreateFromTemplate(const std::filesystem::path& templatePath,
                                                               bool attachTemplate)
{
    auto document = WordDocument::CreateFromTemplate(templatePath, attachTemplate);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<WordDocumentEditor>(document);
}

bool WordDocumentEditor::CreateDefaultDocument(WordprocessingDocumentType type)
{
    auto document = WordDocument::Create(type);
    if (!document)
    {
        return false;
    }
    if (!document->InitDocument())
    {
        return false;
    }

    auto mainPart = document->GetMainDocumentPart();
    if (!WordStructureHelper::EnsureBody(mainPart))
    {
        return false;
    }

    m_document = document;
    return true;
}

bool WordDocumentEditor::SaveToFile(const std::filesystem::path& path,
                                    bool atomicSave,
                                    const ICancellationToken* cancellationToken)
{
    if (!m_document)
    {
        return false;
    }
    return m_document->SaveToFile(path, atomicSave, cancellationToken);
}

std::vector<Byte> WordDocumentEditor::SaveToMemory(const ICancellationToken* cancellationToken)
{
    if (!m_document)
    {
        return {};
    }
    return m_document->SaveToMemory(cancellationToken);
}

std::optional<DocumentEditMemento> WordDocumentEditor::CreateMemento(
    const ICancellationToken* cancellationToken)
{
    if (!m_document)
    {
        return std::nullopt;
    }

    auto bytes = m_document->SaveToMemory(cancellationToken);
    if (bytes.empty())
    {
        return std::nullopt;
    }

    return DocumentEditMemento(DocumentFamily::Word, std::move(bytes));
}

bool WordDocumentEditor::RestoreMemento(const DocumentEditMemento& memento,
                                        const ICancellationToken* cancellationToken)
{
    if (memento.Family() != DocumentFamily::Word || memento.Bytes().empty())
    {
        return false;
    }

    auto document = WordDocument::Open(memento.Bytes(), {}, cancellationToken);
    if (!document)
    {
        return false;
    }

    m_document = std::move(document);
    return true;
}

DocumentEditTransaction WordDocumentEditor::BeginTransaction(const ICancellationToken* cancellationToken)
{
    return detail::DocumentEditTransactionStarter::Begin(
        m_transactionOwner,
        this,
        [this, cancellationToken]
        { return CreateMemento(cancellationToken); },
        [this](const DocumentEditMemento& value)
        { return RestoreMemento(value); });
}

void WordDocumentEditor::SetDocument(const WordDocument::Ptr& document)
{
    m_document = document;
}

WordDocument::Ptr WordDocumentEditor::GetDocument() const
{
    return m_document;
}

WordDocument::Ptr WordDocumentEditor::GetLowLevelApi() const
{
    return m_document;
}

Packaging::DocumentProperties WordDocumentEditor::Properties() const
{
    return Packaging::DocumentProperties(*m_document);
}

std::optional<ExyokiOffice::ThemeSettings> WordDocumentEditor::ThemeSettings() const
{
    auto mainPart = m_document ? m_document->GetMainDocumentPart() : nullptr;
    return ThemeService::ReadSettings(mainPart ? mainPart->GetThemePart() : nullptr);
}

bool WordDocumentEditor::SetThemeSettings(const ExyokiOffice::ThemeSettings& settings)
{
    auto mainPart = m_document ? m_document->GetMainDocumentPart() : nullptr;
    return ThemeService::WriteSettings(mainPart ? mainPart->GetThemePart() : nullptr, settings);
}

std::optional<std::string> WordDocumentEditor::ThemeXml() const
{
    auto mainPart = m_document ? m_document->GetMainDocumentPart() : nullptr;
    return ThemeService::ReadXml(mainPart ? mainPart->GetThemePart() : nullptr);
}

bool WordDocumentEditor::SetThemeXml(std::string xml)
{
    auto mainPart = m_document ? m_document->GetMainDocumentPart() : nullptr;
    if (!mainPart || !ThemeService::IsValidThemeXml(xml))
    {
        return false;
    }
    auto theme = mainPart->GetThemePart();
    if (!theme)
    {
        theme = mainPart->AddThemePart();
    }
    return ThemeService::WriteXml(theme, std::move(xml));
}

bool WordDocumentEditor::EnsureTheme()
{
    auto mainPart = m_document ? m_document->GetMainDocumentPart() : nullptr;
    if (!mainPart)
    {
        return false;
    }
    if (mainPart->GetThemePart())
    {
        return true;
    }
    auto theme = mainPart->AddThemePart();
    if (!ThemeService::WriteDefaultTheme(theme))
    {
        mainPart->RemoveThemePart();
        return false;
    }
    return true;
}

bool WordDocumentEditor::RemoveTheme()
{
    auto mainPart = m_document ? m_document->GetMainDocumentPart() : nullptr;
    return mainPart && mainPart->RemoveThemePart();
}

StyleManager WordDocumentEditor::Styles() const
{
    return StyleManager(m_document);
}

NumberingManager WordDocumentEditor::Numbering() const
{
    return NumberingManager(m_document);
}

WordDocumentEditor::BodyCursor WordDocumentEditor::Body() const
{
    return BodyCursor(m_document, BodyCursor::Placement::End);
}

WordDocumentEditor::BodyCursor WordDocumentEditor::BodyStart() const
{
    return BodyCursor(m_document, BodyCursor::Placement::Start);
}

WordDocumentEditor::BodyCursor WordDocumentEditor::Before(const std::shared_ptr<Paragraph>& paragraph) const
{
    return BodyCursor(m_document,
                      BodyCursor::Placement::Before,
                      paragraph ? std::dynamic_pointer_cast<ExyokiOffice::OpenXMLElement>(paragraph->GetLowLevelApi())
                                : nullptr);
}

WordDocumentEditor::BodyCursor WordDocumentEditor::After(const std::shared_ptr<Paragraph>& paragraph) const
{
    return BodyCursor(m_document,
                      BodyCursor::Placement::After,
                      paragraph ? std::dynamic_pointer_cast<ExyokiOffice::OpenXMLElement>(paragraph->GetLowLevelApi())
                                : nullptr);
}

WordDocumentEditor::BodyCursor WordDocumentEditor::Before(const std::shared_ptr<Table>& table) const
{
    return BodyCursor(m_document,
                      BodyCursor::Placement::Before,
                      table ? std::dynamic_pointer_cast<ExyokiOffice::OpenXMLElement>(table->GetLowLevelApi())
                            : nullptr);
}

WordDocumentEditor::BodyCursor WordDocumentEditor::After(const std::shared_ptr<Table>& table) const
{
    return BodyCursor(m_document,
                      BodyCursor::Placement::After,
                      table ? std::dynamic_pointer_cast<ExyokiOffice::OpenXMLElement>(table->GetLowLevelApi())
                            : nullptr);
}

std::vector<BodyBlock> WordDocumentEditor::BodyBlocks() const
{
    std::vector<BodyBlock> blocks;
    auto body = WordBodyHelper::GetBody(m_document);
    if (!body)
    {
        return blocks;
    }

    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);
    for (const auto& child : body->Children())
    {
        blocks.emplace_back(child, mainPart);
    }
    return blocks;
}

std::vector<std::shared_ptr<Paragraph>> WordDocumentEditor::Paragraphs() const
{
    std::vector<std::shared_ptr<Paragraph>> paragraphs;
    auto body = WordBodyHelper::GetBody(m_document);
    if (!body)
    {
        return paragraphs;
    }

    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);
    for (const auto& paragraph : body->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
    {
        if (paragraph)
        {
            paragraphs.push_back(std::make_shared<Paragraph>(paragraph, mainPart));
        }
    }
    return paragraphs;
}

std::vector<std::shared_ptr<Table>> WordDocumentEditor::Tables() const
{
    std::vector<std::shared_ptr<Table>> tables;
    auto body = WordBodyHelper::GetBody(m_document);
    if (!body)
    {
        return tables;
    }

    for (const auto& table : body->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>())
    {
        if (table)
        {
            tables.push_back(std::make_shared<Table>(table));
        }
    }
    return tables;
}

std::vector<std::shared_ptr<Section>> WordDocumentEditor::Sections() const
{
    std::vector<std::shared_ptr<Section>> sections;
    auto root = WordBodyHelper::GetMainDocumentRoot(m_document);
    if (!root)
    {
        return sections;
    }

    std::vector<std::shared_ptr<OpenXMLElement>> sectionElements;
    WordBodyHelper::CollectDescendantsByName(root,
                                             ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sectPr"),
                                             sectionElements);

    for (const auto& section : sectionElements)
    {
        if (section)
        {
            sections.push_back(std::make_shared<Section>(section, WordStructureHelper::GetMainDocumentPart(m_document)));
        }
    }
    return sections;
}

std::vector<std::shared_ptr<Bookmark>> WordDocumentEditor::Bookmarks() const
{
    std::vector<std::shared_ptr<Bookmark>> bookmarks;
    auto root = WordBodyHelper::GetMainDocumentRoot(m_document);
    if (!root)
    {
        return bookmarks;
    }

    auto ends = root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>();
    for (const auto& start : root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>())
    {
        if (!start)
        {
            continue;
        }

        const auto id = start->GetId().ToString();
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd> matchedEnd;
        for (const auto& end : ends)
        {
            if (end && end->GetId().ToString() == id)
            {
                matchedEnd = end;
                break;
            }
        }
        bookmarks.push_back(std::make_shared<Bookmark>(start, matchedEnd));
    }
    return bookmarks;
}

std::vector<std::shared_ptr<Field>> WordDocumentEditor::Fields() const
{
    std::vector<std::shared_ptr<Field>> fields;
    for (const auto& paragraph : Paragraphs())
    {
        if (!paragraph)
        {
            continue;
        }
        auto paragraphFields = paragraph->Fields();
        fields.insert(fields.end(), paragraphFields.begin(), paragraphFields.end());
    }
    return fields;
}

/// File-local ASCII text helpers for Word field instructions.
class WordDocumentFieldTextHelper
{
public:
    static std::string TrimAscii(std::string_view value)
    {
        return std::string(AsciiText::Trim(value));
    }

    static std::optional<std::string> ParseMergeFieldName(std::string_view instruction)
    {
        auto text = TrimAscii(instruction);
        constexpr std::string_view prefix = "MERGEFIELD";
        if (text.size() < prefix.size() || AsciiText::ToUpper(std::string_view(text).substr(0, prefix.size())) != prefix)
        {
            return std::nullopt;
        }
        if (text.size() > prefix.size() && !AsciiText::IsSpace(text[prefix.size()]))
        {
            return std::nullopt;
        }

        Size pos = prefix.size();
        while (pos < text.size() && AsciiText::IsSpace(text[pos]))
        {
            ++pos;
        }
        if (pos >= text.size())
        {
            return std::nullopt;
        }

        if (text[pos] == '"' || text[pos] == '\'')
        {
            const char quote = text[pos++];
            const auto end = text.find(quote, pos);
            if (end == std::string::npos)
            {
                return std::nullopt;
            }
            auto fieldName = text.substr(pos, end - pos);
            pos = end + 1;
            while (pos < text.size() && AsciiText::IsSpace(text[pos]))
            {
                ++pos;
            }
            if (pos < text.size() && text[pos] != '\\')
            {
                return std::nullopt;
            }
            return fieldName;
        }

        const auto end = text.find_first_of(" \t\r\n", pos);
        auto fieldName = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end;
        while (pos < text.size() && AsciiText::IsSpace(text[pos]))
        {
            ++pos;
        }
        if (pos < text.size() && text[pos] != '\\')
        {
            return std::nullopt;
        }
        return fieldName;
    }

    enum class MergeRegionMarkerKind
    {
        None,
        Start,
        End
    };

    struct MergeRegionMarker
    {
        MergeRegionMarkerKind Kind = MergeRegionMarkerKind::None;
        std::string Name;
    };

    static MergeRegionMarker ParseRegionMarker(const std::shared_ptr<Paragraph>& paragraph)
    {
        if (!paragraph)
        {
            return {};
        }
        for (const auto& field : paragraph->Fields())
        {
            if (!field)
            {
                continue;
            }
            auto name = ParseMergeFieldName(field->GetInstruction());
            if (!name)
            {
                continue;
            }
            constexpr std::string_view startPrefix = "TableStart:";
            constexpr std::string_view endPrefix = "TableEnd:";
            if (name->rfind(startPrefix, 0) == 0 && name->size() > startPrefix.size())
            {
                return {MergeRegionMarkerKind::Start, name->substr(startPrefix.size())};
            }
            if (name->rfind(endPrefix, 0) == 0 && name->size() > endPrefix.size())
            {
                return {MergeRegionMarkerKind::End, name->substr(endPrefix.size())};
            }
        }
        return {};
    }

    static std::vector<std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>>
    CollectParagraphsInElement(const std::shared_ptr<OpenXMLElement>& element)
    {
        std::vector<std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>> paragraphs;
        if (!element)
        {
            return paragraphs;
        }
        if (auto paragraph =
                std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(element))
        {
            paragraphs.push_back(paragraph);
        }
        auto descendants = element->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
        paragraphs.insert(paragraphs.end(), descendants.begin(), descendants.end());
        return paragraphs;
    }

    static Size MergeFieldsInElement(
        const std::shared_ptr<OpenXMLElement>& element,
        const std::unordered_map<std::string, std::string>& values,
        const std::shared_ptr<Packaging::MainDocumentPart>& mainPart,
        bool preserveSpaces)
    {
        Size merged = 0;
        for (const auto& paragraphElement : CollectParagraphsInElement(element))
        {
            auto paragraph = std::make_shared<Paragraph>(paragraphElement, mainPart);
            for (const auto& field : paragraph->Fields())
            {
                if (!field)
                {
                    continue;
                }
                auto name = ParseMergeFieldName(field->GetInstruction());
                if (!name || name->rfind("TableStart:", 0) == 0 || name->rfind("TableEnd:", 0) == 0)
                {
                    continue;
                }
                auto it = values.find(*name);
                if (it != values.end() && field->SetResult(it->second, preserveSpaces))
                {
                    ++merged;
                }
            }
        }
        return merged;
    }

    static Size MergeBookmarksInElement(const std::shared_ptr<OpenXMLElement>& element,
                                        const std::unordered_map<std::string, std::string>& values,
                                        bool preserveSpaces)
    {
        Size merged = 0;
        if (!element)
        {
            return merged;
        }

        auto starts = element->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>();
        if (auto start = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>(element))
        {
            starts.insert(starts.begin(), start);
        }

        for (const auto& start : starts)
        {
            if (!start)
            {
                continue;
            }
            const auto name = start->GetName().ToString();
            auto value = values.find(name);
            if (value == values.end())
            {
                continue;
            }

            auto parent = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(
                start->Parent());
            if (!parent)
            {
                continue;
            }

            std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd> end;
            for (auto child = start->NextSibling(); child; child = child->NextSibling())
            {
                end = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>(child);
                if (end && end->GetId().ToString() == start->GetId().ToString())
                {
                    break;
                }
                end.reset();
            }
            if (!end)
            {
                continue;
            }

            WordFieldHelper::RemoveParagraphChildrenBetween(parent, start, end);
            if (!value->second.empty())
            {
                WordFieldHelper::InsertRunWithTextBefore(parent, end, value->second, preserveSpaces);
            }
            ++merged;
        }
        return merged;
    }
};

TemplateMergeResult WordDocumentEditor::MergeTemplate(const TemplateMergeData& data, bool preserveSpaces)
{
    TemplateMergeResult result;
    auto body = WordBodyHelper::GetBody(m_document);
    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);
    if (!body)
    {
        return result;
    }

    bool changedRegion = true;
    while (changedRegion)
    {
        changedRegion = false;
        auto children = body->Children();
        for (Size startIndex = 0; startIndex < children.size(); ++startIndex)
        {
            auto startParagraph = std::dynamic_pointer_cast<
                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(children[startIndex]);
            auto startMarker = WordDocumentFieldTextHelper::ParseRegionMarker(std::make_shared<Paragraph>(startParagraph, mainPart));
            if (startMarker.Kind != WordDocumentFieldTextHelper::MergeRegionMarkerKind::Start)
            {
                continue;
            }

            Size endIndex = startIndex + 1;
            for (; endIndex < children.size(); ++endIndex)
            {
                auto endParagraph = std::dynamic_pointer_cast<
                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(children[endIndex]);
                auto endMarker = WordDocumentFieldTextHelper::ParseRegionMarker(std::make_shared<Paragraph>(endParagraph, mainPart));
                if (endMarker.Kind == WordDocumentFieldTextHelper::MergeRegionMarkerKind::End && endMarker.Name == startMarker.Name)
                {
                    break;
                }
            }
            if (endIndex >= children.size())
            {
                continue;
            }

            std::vector<std::shared_ptr<OpenXMLElement>> templateNodes;
            for (Size i = startIndex + 1; i < endIndex; ++i)
            {
                templateNodes.push_back(children[i]);
            }

            auto before = children[endIndex];
            auto regionIt = data.Regions.find(startMarker.Name);
            if (regionIt != data.Regions.end())
            {
                for (const auto& row : regionIt->second)
                {
                    for (const auto& node : templateNodes)
                    {
                        auto copy = node ? node->CopyInto(body, before) : nullptr;
                        if (copy)
                        {
                            result.FieldsMerged += WordDocumentFieldTextHelper::MergeFieldsInElement(copy, row, mainPart, preserveSpaces);
                            result.BookmarksMerged += WordDocumentFieldTextHelper::MergeBookmarksInElement(copy, row, preserveSpaces);
                        }
                    }
                    ++result.RegionRowsInserted;
                }
            }

            for (Size i = startIndex; i <= endIndex; ++i)
            {
                body->RemoveChild(children[i]);
            }
            ++result.RegionsMerged;
            changedRegion = true;
            break;
        }
    }

    result.FieldsMerged += WordDocumentFieldTextHelper::MergeFieldsInElement(body, data.Values, mainPart, preserveSpaces);
    result.BookmarksMerged += WordDocumentFieldTextHelper::MergeBookmarksInElement(body, data.Values, preserveSpaces);
    return result;
}

std::vector<std::shared_ptr<Revision>> WordDocumentEditor::Revisions() const
{
    std::vector<std::shared_ptr<Revision>> result;
    auto root = WordBodyHelper::GetMainDocumentRoot(m_document);
    if (!root)
    {
        return result;
    }

    std::vector<std::shared_ptr<OpenXMLElement>> elements;
    WordRevisionHelper::CollectRevisionElements(root, elements);
    for (const auto& element : elements)
    {
        if (element)
        {
            result.push_back(std::make_shared<Revision>(element));
        }
    }
    return result;
}

Size WordDocumentEditor::AcceptAllRevisions()
{
    auto revisions = Revisions();
    Size processed = 0;
    for (auto it = revisions.rbegin(); it != revisions.rend(); ++it)
    {
        if (*it && (*it)->Accept())
        {
            ++processed;
        }
    }
    return processed;
}

Size WordDocumentEditor::RejectAllRevisions()
{
    auto revisions = Revisions();
    Size processed = 0;
    for (auto it = revisions.rbegin(); it != revisions.rend(); ++it)
    {
        if (*it && (*it)->Reject())
        {
            ++processed;
        }
    }
    return processed;
}

Size WordDocumentEditor::CompareWith(const WordDocumentEditor& revised,
                                     const RevisionAuthor& author)
{
    auto body = WordBodyHelper::GetBody(m_document);
    if (!body)
    {
        return 0;
    }

    auto originalParagraphs = Paragraphs();
    auto revisedParagraphs = revised.Paragraphs();
    const Size common = std::min(originalParagraphs.size(), revisedParagraphs.size());
    Size created = 0;
    auto nextId = WordRevisionHelper::NextRevisionId(WordBodyHelper::GetMainDocumentRoot(m_document));

    for (Size i = 0; i < common; ++i)
    {
        if (!originalParagraphs[i] || !revisedParagraphs[i] ||
            originalParagraphs[i]->PlainText() == revisedParagraphs[i]->PlainText())
        {
            continue;
        }

        auto paragraph = originalParagraphs[i]->GetLowLevelApi();
        if (!paragraph)
        {
            continue;
        }
        const auto originalText = originalParagraphs[i]->PlainText();
        const auto revisedText = revisedParagraphs[i]->PlainText();
        for (const auto& child : paragraph->Children())
        {
            if (child)
            {
                paragraph->RemoveChild(child);
            }
        }
        auto deletion = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DeletedRun>();
        if (deletion)
        {
            WordRevisionHelper::ApplyRevisionMetadata(deletion, author, nextId);
            auto run = deletion->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
            if (run)
            {
                auto text = run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DeletedText>();
                if (text)
                {
                    text->SetText(originalText);
                }
            }
            ++created;
        }

        auto insertion = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::InsertedRun>();
        if (insertion)
        {
            WordRevisionHelper::ApplyRevisionMetadata(insertion, author, std::to_string(std::stoi(nextId) + 1));
            WordFieldHelper::AppendRunWithText(insertion, revisedText, true);
            ++created;
        }
        nextId = std::to_string(std::stoi(nextId) + 2);
    }

    for (Size i = common; i < originalParagraphs.size(); ++i)
    {
        auto paragraph = originalParagraphs[i] ? originalParagraphs[i]->GetLowLevelApi() : nullptr;
        if (!paragraph)
        {
            continue;
        }
        const auto originalText = originalParagraphs[i]->PlainText();
        for (const auto& child : paragraph->Children())
        {
            if (child)
            {
                paragraph->RemoveChild(child);
            }
        }
        auto deletion = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DeletedRun>();
        if (!deletion)
        {
            continue;
        }
        WordRevisionHelper::ApplyRevisionMetadata(deletion, author, nextId);
        auto run = deletion->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
        if (run)
        {
            auto text = run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DeletedText>();
            if (text)
            {
                text->SetText(originalText);
            }
        }
        nextId = std::to_string(std::stoi(nextId) + 1);
        ++created;
    }

    for (Size i = common; i < revisedParagraphs.size(); ++i)
    {
        auto paragraph = Body().InsertParagraph();
        auto lowParagraph = paragraph ? paragraph->GetLowLevelApi() : nullptr;
        if (!lowParagraph)
        {
            continue;
        }
        auto insertion = lowParagraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::InsertedRun>();
        if (!insertion)
        {
            continue;
        }
        WordRevisionHelper::ApplyRevisionMetadata(insertion, author, nextId);
        WordFieldHelper::AppendRunWithText(insertion, revisedParagraphs[i]->PlainText(), true);
        nextId = std::to_string(std::stoi(nextId) + 1);
        ++created;
    }
    return created;
}

std::shared_ptr<Bookmark> WordDocumentEditor::FindBookmark(std::string_view name) const
{
    for (const auto& bookmark : Bookmarks())
    {
        if (bookmark && bookmark->GetName() == name)
        {
            return bookmark;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Note>> WordDocumentEditor::Footnotes() const
{
    std::vector<std::shared_ptr<Note>> notes;
    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);
    if (!mainPart)
    {
        return notes;
    }
    auto part = mainPart->GetFootnotesPart();
    if (!part)
    {
        return notes;
    }
    std::shared_ptr<OpenXMLElement> root = part->GetTypedRootElement();
    if (!root)
    {
        return notes;
    }
    for (const auto& entry :
         WordNoteHelper::FindNoteEntries<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Footnote>(root))
    {
        notes.push_back(std::make_shared<Note>(NoteKind::Footnote, entry, mainPart));
    }
    return notes;
}

std::vector<std::shared_ptr<Note>> WordDocumentEditor::Endnotes() const
{
    std::vector<std::shared_ptr<Note>> notes;
    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);
    if (!mainPart)
    {
        return notes;
    }
    auto part = mainPart->GetEndnotesPart();
    if (!part)
    {
        return notes;
    }
    std::shared_ptr<OpenXMLElement> root = part->GetTypedRootElement();
    if (!root)
    {
        return notes;
    }
    for (const auto& entry :
         WordNoteHelper::FindNoteEntries<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Endnote>(root))
    {
        notes.push_back(std::make_shared<Note>(NoteKind::Endnote, entry, mainPart));
    }
    return notes;
}

std::shared_ptr<Note> WordDocumentEditor::FindFootnote(int id) const
{
    for (const auto& note : Footnotes())
    {
        if (note && note->GetId() == id)
        {
            return note;
        }
    }
    return nullptr;
}

std::shared_ptr<Note> WordDocumentEditor::FindEndnote(int id) const
{
    for (const auto& note : Endnotes())
    {
        if (note && note->GetId() == id)
        {
            return note;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Comment>> WordDocumentEditor::Comments() const
{
    std::vector<std::shared_ptr<Comment>> comments;
    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);
    if (!mainPart)
    {
        return comments;
    }
    auto part = mainPart->GetWordprocessingCommentsPart();
    if (!part)
    {
        return comments;
    }
    auto root = part->GetTypedRootElement();
    if (!root)
    {
        return comments;
    }
    for (const auto& entry : root->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>())
    {
        if (entry)
        {
            comments.push_back(std::make_shared<Comment>(entry, mainPart));
        }
    }
    return comments;
}

std::shared_ptr<Comment> WordDocumentEditor::FindComment(int id) const
{
    for (const auto& comment : Comments())
    {
        if (comment && comment->GetId() == id)
        {
            return comment;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<ContentControl>> WordDocumentEditor::ContentControls() const
{
    std::vector<std::shared_ptr<ContentControl>> result;
    auto root = WordBodyHelper::GetMainDocumentRoot(m_document);
    if (!root)
    {
        return result;
    }
    auto mainPart = WordStructureHelper::GetMainDocumentPart(m_document);

    std::vector<std::shared_ptr<OpenXMLElement>> sdtElements;
    WordBodyHelper::CollectDescendantsByName(root, ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sdt"), sdtElements);

    for (const auto& sdt : sdtElements)
    {
        if (!sdt)
        {
            continue;
        }
        auto parent = sdt->Parent();
        const bool isInline =
            parent && parent->QualifiedName() == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "p");
        result.push_back(std::make_shared<ContentControl>(
            sdt, isInline ? ContentControlLevel::Inline : ContentControlLevel::Block, mainPart));
    }
    return result;
}

std::shared_ptr<ContentControl> WordDocumentEditor::FindContentControl(int id) const
{
    for (const auto& control : ContentControls())
    {
        if (control && control->GetId() == id)
        {
            return control;
        }
    }
    return nullptr;
}

std::shared_ptr<Section> WordDocumentEditor::EnsureFinalSection()
{
    auto body = WordStructureHelper::EnsureBody(m_document);
    if (!body)
    {
        return nullptr;
    }

    auto sectionProperties = WordBodyHelper::FindTrailingSectionProperties(body);
    if (!sectionProperties)
    {
        sectionProperties =
            body->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionProperties>();
    }
    return sectionProperties ? std::make_shared<Section>(sectionProperties, WordStructureHelper::GetMainDocumentPart(m_document)) : nullptr;
}

std::shared_ptr<Paragraph> WordDocumentEditor::AddParagraph()
{
    return Body().InsertParagraph();
}

std::shared_ptr<Paragraph> WordDocumentEditor::AddParagraph(std::string_view text)
{
    auto paragraph = AddParagraph();
    if (!paragraph)
    {
        return nullptr;
    }
    paragraph->AddText(text);
    return paragraph;
}

std::shared_ptr<Paragraph> WordDocumentEditor::AddPageBreak()
{
    auto paragraph = AddParagraph();
    if (!paragraph)
    {
        return nullptr;
    }
    paragraph->AddBreak(BreakType::Page);
    return paragraph;
}

/// File-local helper for the document comparison entry point.
class WordDocumentCompareHelper
{
public:
    // Word-like visual defaults for heading styles created by AddHeading(). Only
    // applied when the style does not exist yet; documents that already define
    // HeadingN keep their own design.
    static void ApplyDefaultHeadingFormatting(
        const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>& style,
        int level)
    {
        if (!style)
        {
            return;
        }

        using namespace ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

        auto paragraphProperties = WordStructureHelper::EnsureChildOfType<Style, StyleParagraphProperties>(style);
        if (paragraphProperties)
        {
            if (auto keepNext = WordStructureHelper::EnsureChildOfType<StyleParagraphProperties, KeepNext>(paragraphProperties))
            {
                keepNext->SetVal(OnOffValue(true));
            }
            if (auto keepLines = WordStructureHelper::EnsureChildOfType<StyleParagraphProperties, KeepLines>(paragraphProperties))
            {
                keepLines->SetVal(OnOffValue(true));
            }
            if (auto spacing = WordStructureHelper::EnsureChildOfType<StyleParagraphProperties, SpacingBetweenLines>(paragraphProperties))
            {
                spacing->SetBefore(StringValue(level == 1 ? "240" : "120"));
            }
            if (auto outline = WordStructureHelper::EnsureChildOfType<StyleParagraphProperties, OutlineLevel>(paragraphProperties))
            {
                outline->SetVal(Int32Value(level - 1));
            }
        }

        auto runProperties = WordStructureHelper::EnsureChildOfType<Style, StyleRunProperties>(style);
        if (runProperties)
        {
            if (auto bold = WordStructureHelper::EnsureChildOfType<StyleRunProperties, Bold>(runProperties))
            {
                bold->SetVal(OnOffValue(true));
            }
            if (auto color = WordStructureHelper::EnsureChildOfType<StyleRunProperties,
                                                                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Color>(runProperties))
            {
                color->SetVal(StringValue("2F5496"));
            }
            // Half-point sizes decreasing with the heading level: 16pt, 13pt,
            // 12pt, then 11pt for level 4 and deeper.
            const int halfPoints = level == 1 ? 32 : level == 2 ? 26
                                                 : level == 3   ? 24
                                                                : 22;
            if (auto fontSize = WordStructureHelper::EnsureChildOfType<StyleRunProperties, FontSize>(runProperties))
            {
                fontSize->SetVal(StringValue(std::to_string(halfPoints)));
            }
        }
    }
};

std::shared_ptr<Paragraph> WordDocumentEditor::AddHeading(std::string_view text, int level)
{
    if (!m_document)
    {
        return nullptr;
    }

    const int clampedLevel = std::clamp(level, 1, 9);
    const std::string styleId = "Heading" + std::to_string(clampedLevel);

    auto styles = Styles();
    if (!styles.HasStyle("Normal"))
    {
        StyleDefinition normal;
        normal.StyleId = "Normal";
        normal.Name = "Normal";
        normal.Type = StyleType::Paragraph;
        normal.IsDefault = true;
        normal.IsCustom = false;
        normal.IsPrimary = true;
        styles.CreateStyle(normal);
    }

    if (!styles.HasStyle(styleId))
    {
        StyleDefinition heading;
        heading.StyleId = styleId;
        heading.Name = "heading " + std::to_string(clampedLevel);
        heading.Type = StyleType::Paragraph;
        heading.IsCustom = false;
        heading.IsPrimary = true;
        heading.UiPriority = 9;
        heading.BasedOnStyleId = "Normal";
        heading.NextStyleId = "Normal";
        if (styles.CreateStyle(heading))
        {
            WordDocumentCompareHelper::ApplyDefaultHeadingFormatting(styles.GetLowLevelStyle(styleId), clampedLevel);
        }
    }

    auto paragraph = AddParagraph(text);
    if (!paragraph)
    {
        return nullptr;
    }
    paragraph->SetStyleId(styleId);
    return paragraph;
}

std::shared_ptr<Paragraph> WordDocumentEditor::AddTableOfContents(int fromLevel, int toLevel)
{
    const int from = std::clamp(fromLevel, 1, 9);
    const int to = std::clamp(toLevel, from, 9);

    auto paragraph = AddParagraph();
    if (!paragraph)
    {
        return nullptr;
    }

    const std::string instruction =
        "TOC \\o \"" + std::to_string(from) + "-" + std::to_string(to) + "\" \\h \\z \\u";
    auto field = paragraph->AddField(
        instruction, "Right-click and choose \"Update Field\" to build the table of contents.");
    if (field)
    {
        field->InvalidateResult();
    }
    return paragraph;
}

std::shared_ptr<Table> WordDocumentEditor::AddTable(Size rows, Size columns)
{
    return Body().InsertTable(rows, columns);
}

std::optional<ImageFormatInfo> DetectImageFormat(std::span<const Byte> data)
{
    if (auto png = WordImageFormatHelper::DetectPng(data))
    {
        return png;
    }
    if (auto jpeg = WordImageFormatHelper::DetectJpeg(data))
    {
        return jpeg;
    }
    if (auto gif = WordImageFormatHelper::DetectGif(data))
    {
        return gif;
    }
    if (auto bmp = WordImageFormatHelper::DetectBmp(data))
    {
        return bmp;
    }
    return std::nullopt;
}

std::shared_ptr<Image> WordDocumentEditor::AddImageFromFile(const std::filesystem::path& filePath,
                                                            const ExyokiOffice::MeasuringUnits& width,
                                                            const ExyokiOffice::MeasuringUnits& height,
                                                            ImageLayout layout,
                                                            ImageWrap wrap)
{
    if (!m_document)
    {
        return nullptr;
    }

    auto data = Packaging::ReadFileFully(filePath);
    if (data.empty())
    {
        return nullptr;
    }

    const auto contentType = WordValueHelper::ContentTypeFromExtension(filePath);
    return AddImageFromData(std::move(data), contentType, width, height, layout, wrap);
}

std::shared_ptr<Image> WordDocumentEditor::AddImageFromData(std::vector<Byte> data,
                                                            std::string_view contentType,
                                                            const ExyokiOffice::MeasuringUnits& width,
                                                            const ExyokiOffice::MeasuringUnits& height,
                                                            ImageLayout layout,
                                                            ImageWrap wrap)
{
    if (!m_document)
    {
        return nullptr;
    }

    auto mainPart = m_document->GetMainDocumentPart();
    if (!mainPart)
    {
        mainPart = m_document->AddMainDocumentPart();
    }
    if (!mainPart)
    {
        return nullptr;
    }

    auto imagePart = WordDrawingHelper::CreateImagePartFromData(mainPart, std::move(data), contentType);
    if (!imagePart)
    {
        return nullptr;
    }

    auto body = WordStructureHelper::EnsureBody(mainPart);
    if (!body)
    {
        return nullptr;
    }

    auto paragraph = body->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(
        WordBodyHelper::FindTrailingSectionProperties(body));
    if (!paragraph)
    {
        return nullptr;
    }
    auto run = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    if (!run)
    {
        return nullptr;
    }

    const auto relId = imagePart->RelationshipId();
    auto drawing = run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>();
    const auto docId = WordIdHelper::NextDocPropertyId(mainPart->GetTypedRootElement());
    const auto pictureName = "Picture " + std::to_string(docId);
    if (!WordDrawingHelper::PopulateDrawingWithPicture(drawing,
                                                       relId,
                                                       WordValueHelper::ToEmuInt64(width),
                                                       WordValueHelper::ToEmuInt64(height),
                                                       layout,
                                                       wrap,
                                                       docId,
                                                       pictureName))
    {
        return nullptr;
    }

    return std::make_shared<Image>(drawing, mainPart);
}

std::shared_ptr<Image> WordDocumentEditor::AddImageFromFile(const std::filesystem::path& filePath,
                                                            ImageLayout layout,
                                                            ImageWrap wrap)
{
    if (!m_document)
    {
        return nullptr;
    }

    auto data = Packaging::ReadFileFully(filePath);
    if (data.empty())
    {
        return nullptr;
    }

    return AddImageFromData(std::move(data), layout, wrap);
}

std::shared_ptr<Image> WordDocumentEditor::AddImageFromData(std::vector<Byte> data,
                                                            ImageLayout layout,
                                                            ImageWrap wrap)
{
    const auto format = DetectImageFormat(data);
    if (!format)
    {
        return nullptr;
    }

    const auto widthEmu = format->HorizontalDpi > 0.0
                              ? (static_cast<Real>(format->PixelWidth) / format->HorizontalDpi) * 914400.0
                              : 0.0;
    const auto heightEmu = format->VerticalDpi > 0.0
                               ? (static_cast<Real>(format->PixelHeight) / format->VerticalDpi) * 914400.0
                               : 0.0;

    const ExyokiOffice::MeasuringUnits width(widthEmu, ExyokiOffice::MeasurementUnit::Emu);
    const ExyokiOffice::MeasuringUnits height(heightEmu, ExyokiOffice::MeasurementUnit::Emu);

    return AddImageFromData(std::move(data), format->ContentType, width, height, layout, wrap);
}

ListStyle WordDocumentEditor::EnsureNumberedListStyle(
    std::string_view name,
    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues format,
    std::string_view levelText)
{
    NumberingDefinition definition;
    definition.Name = std::string(name);

    NumberingLevelDefinition level;
    level.Level = 0;
    level.Start = 1;
    level.Format = format;
    level.LevelText = std::string(levelText);
    level.LeftIndent = ExyokiOffice::MeasuringUnits(720.0, ExyokiOffice::MeasurementUnit::Twip);
    level.HangingIndent = ExyokiOffice::MeasuringUnits(360.0, ExyokiOffice::MeasurementUnit::Twip);
    definition.Levels.push_back(level);

    return Numbering().EnsureMultilevelList(definition);
}

ListStyle WordDocumentEditor::EnsureBulletedListStyle(std::string_view name, std::string_view bulletText)
{
    return EnsureNumberedListStyle(name,
                                   ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues::Bullet,
                                   bulletText);
}

NumberingManager::NumberingManager(const WordDocument::Ptr& document)
    : m_document(document)
{
}

bool NumberingManager::IsValid() const
{
    return m_document != nullptr;
}

bool NumberingManager::HasNumberingPart() const
{
    return WordNumberingHelper::GetNumberingDefinitionsPart(m_document) != nullptr;
}

ListStyle NumberingManager::EnsureMultilevelList(const NumberingDefinition& definition)
{
    ListStyle style{};
    if (!m_document || definition.Name.empty())
    {
        return style;
    }

    auto numbering = WordNumberingHelper::GetNumberingRoot(m_document, true);
    if (!numbering)
    {
        return style;
    }

    auto abstractNum = WordNumberingHelper::FindAbstractNumByName(numbering, definition.Name);
    if (!abstractNum)
    {
        abstractNum = numbering->AppendChild<DocumentFormat::OpenXml::Wordprocessing::AbstractNum>();
        if (!abstractNum)
        {
            return style;
        }

        const int abstractId = WordNumberingHelper::NextAbstractNumberingId(numbering);
        abstractNum->SetAbstractNumberId(Int32Value(abstractId));

        auto defName = abstractNum->AppendChild<DocumentFormat::OpenXml::Wordprocessing::AbstractNumDefinitionName>();
        if (defName)
        {
            defName->SetVal(StringValue(definition.Name));
        }

        auto multiLevel = abstractNum->AppendChild<DocumentFormat::OpenXml::Wordprocessing::MultiLevelType>();
        if (multiLevel)
        {
            multiLevel->SetVal(EnumValue<DocumentFormat::OpenXml::Wordprocessing::MultiLevelValues>(
                definition.Levels.size() <= 1
                    ? DocumentFormat::OpenXml::Wordprocessing::MultiLevelValues::SingleLevel
                    : DocumentFormat::OpenXml::Wordprocessing::MultiLevelValues::Multilevel));
        }

        std::vector<NumberingLevelDefinition> levels = definition.Levels;
        if (levels.empty())
        {
            NumberingLevelDefinition level;
            level.Level = 0;
            level.LevelText = "%1.";
            levels.push_back(level);
        }
        std::sort(levels.begin(), levels.end(), [](const auto& left, const auto& right)
                  { return left.Level < right.Level; });

        bool written[9] = {};
        for (const auto& inputLevel : levels)
        {
            auto level = WordNumberingHelper::NormalizeLevel(inputLevel);
            if (written[level.Level])
            {
                continue;
            }
            written[level.Level] = true;
            auto levelElement = abstractNum->AppendChild<DocumentFormat::OpenXml::Wordprocessing::Level>();
            WordNumberingHelper::WriteLevelDefinition(levelElement, level);
        }
    }

    const int abstractId = abstractNum->GetAbstractNumberId().Value();
    auto instance = WordNumberingHelper::FindNumberingInstanceForAbstract(numbering, abstractId);
    if (!instance)
    {
        instance = WordNumberingHelper::AppendNumberingInstance(numbering, abstractId, {});
    }
    if (!instance || !instance->GetNumberID().IsDefined())
    {
        return style;
    }

    style.NumberingId = instance->GetNumberID().Value();
    style.Level = 0;
    return style;
}

ListStyle NumberingManager::ContinueList(int numberingId)
{
    ListStyle style{};
    auto numbering = WordNumberingHelper::GetNumberingRoot(m_document, false);
    auto instance = WordNumberingHelper::FindNumberingInstanceById(numbering, numberingId);
    if (!instance || !instance->GetNumberID().IsDefined())
    {
        return style;
    }

    style.NumberingId = instance->GetNumberID().Value();
    style.Level = 0;
    return style;
}

ListStyle NumberingManager::RestartList(int numberingId, const std::vector<NumberingLevelOverride>& overrides)
{
    ListStyle style{};
    auto numbering = WordNumberingHelper::GetNumberingRoot(m_document, false);
    auto instance = WordNumberingHelper::FindNumberingInstanceById(numbering, numberingId);
    auto abstractNumId = instance ? instance->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::AbstractNumId>()
                                  : nullptr;
    if (!abstractNumId || !abstractNumId->GetVal().IsDefined())
    {
        return style;
    }

    auto restarted = WordNumberingHelper::AppendNumberingInstance(numbering, abstractNumId->GetVal().Value(), overrides);
    if (!restarted || !restarted->GetNumberID().IsDefined())
    {
        return style;
    }

    style.NumberingId = restarted->GetNumberID().Value();
    style.Level = 0;
    return style;
}

std::vector<NumberingInstanceInfo> NumberingManager::Instances() const
{
    std::vector<NumberingInstanceInfo> result;
    auto numbering = WordNumberingHelper::GetNumberingRoot(m_document, false);
    if (!numbering)
    {
        return result;
    }

    for (const auto& instance : numbering->Elements<DocumentFormat::OpenXml::Wordprocessing::NumberingInstance>())
    {
        if (!instance || !instance->GetNumberID().IsDefined())
        {
            continue;
        }

        NumberingInstanceInfo info;
        info.NumberingId = instance->GetNumberID().Value();
        if (auto abstractNumId =
                instance->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::AbstractNumId>();
            abstractNumId && abstractNumId->GetVal().IsDefined())
        {
            info.AbstractNumberingId = abstractNumId->GetVal().Value();
        }
        info.Overrides = WordNumberingHelper::ReadLevelOverrides(instance);
        result.push_back(info);
    }
    return result;
}

std::optional<NumberingInstanceInfo> NumberingManager::GetInstance(int numberingId) const
{
    for (const auto& instance : Instances())
    {
        if (instance.NumberingId == numberingId)
        {
            return instance;
        }
    }
    return std::nullopt;
}

std::optional<NumberingDefinition> NumberingManager::GetDefinition(int abstractNumberingId) const
{
    auto numbering = WordNumberingHelper::GetNumberingRoot(m_document, false);
    auto abstractNum = WordNumberingHelper::FindAbstractNumById(numbering, abstractNumberingId);
    if (!abstractNum)
    {
        return std::nullopt;
    }

    NumberingDefinition definition;
    if (auto name = abstractNum->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::AbstractNumDefinitionName>();
        name && name->GetVal().IsDefined())
    {
        definition.Name = name->GetVal().ToString();
    }

    for (const auto& levelElement : abstractNum->Elements<DocumentFormat::OpenXml::Wordprocessing::Level>())
    {
        if (!levelElement || !levelElement->GetLevelIndex().IsDefined())
        {
            continue;
        }

        NumberingLevelDefinition level;
        level.Level = levelElement->GetLevelIndex().Value();
        if (auto start = levelElement->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::StartNumberingValue>();
            start && start->GetVal().IsDefined())
        {
            level.Start = start->GetVal().Value();
        }
        if (auto format = levelElement->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::NumberingFormat>();
            format && format->GetVal().IsDefined())
        {
            level.Format = format->GetVal().Value();
        }
        if (auto text = levelElement->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelText>();
            text && text->GetVal().IsDefined())
        {
            level.LevelText = text->GetVal().ToString();
        }
        if (auto style = levelElement->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>();
            style && style->GetVal().IsDefined())
        {
            level.ParagraphStyleId = style->GetVal().ToString();
        }
        if (auto suffix = levelElement->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelSuffix>();
            suffix && suffix->GetVal().IsDefined())
        {
            level.Suffix = suffix->GetVal().Value();
        }
        if (auto justification = levelElement->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::LevelJustification>();
            justification && justification->GetVal().IsDefined())
        {
            level.Justification = justification->GetVal().Value();
        }
        definition.Levels.push_back(std::move(level));
    }
    return definition;
}

ListStyle NumberingManager::ImportList(const NumberingManager& source, int sourceNumberingId)
{
    ListStyle style{};
    auto sourceNumbering = WordNumberingHelper::GetNumberingRoot(source.m_document, false);
    auto targetNumbering = WordNumberingHelper::GetNumberingRoot(m_document, true);
    if (!sourceNumbering || !targetNumbering)
    {
        return style;
    }

    auto sourceInstance = WordNumberingHelper::FindNumberingInstanceById(sourceNumbering, sourceNumberingId);
    auto sourceAbstract = WordNumberingHelper::GetAbstractNumForInstance(sourceNumbering, sourceNumberingId);
    if (!sourceInstance || !sourceAbstract || !sourceAbstract->GetAbstractNumberId().IsDefined())
    {
        return style;
    }

    auto targetAbstract = targetNumbering->AppendChild<DocumentFormat::OpenXml::Wordprocessing::AbstractNum>();
    if (!targetAbstract)
    {
        return style;
    }

    const int targetAbstractId = WordNumberingHelper::NextAbstractNumberingId(targetNumbering);
    targetAbstract->SetAbstractNumberId(Int32Value(targetAbstractId));

    const std::string sourceName =
        WordPropertyReadHelper::GetStringChildValueByName(sourceAbstract, "name").empty()
            ? std::string("ImportedList")
            : WordPropertyReadHelper::GetStringChildValueByName(sourceAbstract, "name");
    auto defName = targetAbstract->AppendChild<DocumentFormat::OpenXml::Wordprocessing::AbstractNumDefinitionName>();
    if (defName)
    {
        defName->SetVal(StringValue(sourceName));
    }

    auto multiLevel = targetAbstract->AppendChild<DocumentFormat::OpenXml::Wordprocessing::MultiLevelType>();
    if (multiLevel)
    {
        multiLevel->SetVal(EnumValue<DocumentFormat::OpenXml::Wordprocessing::MultiLevelValues>(
            sourceAbstract->Elements<DocumentFormat::OpenXml::Wordprocessing::Level>().size() <= 1
                ? DocumentFormat::OpenXml::Wordprocessing::MultiLevelValues::SingleLevel
                : DocumentFormat::OpenXml::Wordprocessing::MultiLevelValues::Multilevel));
    }

    for (const auto& sourceLevel : sourceAbstract->Elements<DocumentFormat::OpenXml::Wordprocessing::Level>())
    {
        if (auto definition = WordNumberingHelper::ReadLevelDefinition(sourceLevel))
        {
            auto targetLevel = targetAbstract->AppendChild<DocumentFormat::OpenXml::Wordprocessing::Level>();
            WordNumberingHelper::WriteLevelDefinition(targetLevel, *definition);
        }
    }

    auto targetInstance = WordNumberingHelper::AppendNumberingInstance(targetNumbering, targetAbstractId, WordNumberingHelper::ReadLevelOverrides(sourceInstance));
    if (!targetInstance || !targetInstance->GetNumberID().IsDefined())
    {
        return style;
    }

    style.NumberingId = targetInstance->GetNumberID().Value();
    style.Level = 0;
    return style;
}

StyleManager::StyleManager(const WordDocument::Ptr& document)
    : m_document(document) {}

bool StyleManager::IsValid() const
{
    return m_document != nullptr;
}

std::shared_ptr<Packaging::StyleDefinitionsPart> StyleManager::GetStylesPart() const
{
    return WordStylePartHelper::GetStyleDefinitionsPart(m_document);
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Styles> StyleManager::GetRoot() const
{
    auto part = GetStylesPart();
    return part ? part->GetTypedRootElement() : nullptr;
}

bool StyleManager::HasStyle(std::string_view styleId) const
{
    return GetLowLevelStyle(styleId) != nullptr;
}

std::vector<StyleDefinition> StyleManager::Styles() const
{
    std::vector<StyleDefinition> result;
    auto root = GetRoot();
    if (!root)
    {
        return result;
    }

    for (const auto& style : root->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>())
    {
        if (style)
        {
            result.push_back(WordStyleDefinitionHelper::ReadStyleDefinition(style));
        }
    }
    return result;
}

std::vector<StyleDefinition> StyleManager::StylesByType(StyleType type) const
{
    std::vector<StyleDefinition> result;
    for (auto definition : Styles())
    {
        if (definition.Type == type)
        {
            result.push_back(std::move(definition));
        }
    }
    return result;
}

std::optional<StyleDefinition> StyleManager::GetStyle(std::string_view styleId) const
{
    auto style = GetLowLevelStyle(styleId);
    if (!style)
    {
        return std::nullopt;
    }
    return WordStyleDefinitionHelper::ReadStyleDefinition(style);
}

std::optional<StyleDefinition> StyleManager::GetDefaultStyle(StyleType type) const
{
    for (const auto& definition : StylesByType(type))
    {
        if (definition.IsDefault)
        {
            return definition;
        }
    }
    return std::nullopt;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style> StyleManager::GetLowLevelStyle(
    std::string_view styleId) const
{
    return WordStyleDefinitionHelper::FindStyleById(GetRoot(), styleId);
}

bool StyleManager::CreateStyle(const StyleDefinition& definition, bool replaceExisting)
{
    if (definition.StyleId.empty())
    {
        return false;
    }

    auto part = WordStylePartHelper::EnsureStyleDefinitionsPart(m_document);
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return false;
    }

    if (auto existing = WordStyleDefinitionHelper::FindStyleById(root, definition.StyleId))
    {
        if (!replaceExisting)
        {
            return false;
        }
        WordStyleDefinitionHelper::ApplyStyleDefinition(existing, definition);
        if (definition.IsDefault)
        {
            SetDefaultStyle(definition.Type, definition.StyleId);
        }
        return true;
    }

    auto style = root->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>();
    if (!style)
    {
        return false;
    }
    WordStyleDefinitionHelper::ApplyStyleDefinition(style, definition);
    if (definition.IsDefault)
    {
        SetDefaultStyle(definition.Type, definition.StyleId);
    }
    return true;
}

bool StyleManager::UpdateStyle(const StyleDefinition& definition)
{
    if (definition.StyleId.empty())
    {
        return false;
    }
    auto style = GetLowLevelStyle(definition.StyleId);
    if (!style)
    {
        return false;
    }
    WordStyleDefinitionHelper::ApplyStyleDefinition(style, definition);
    if (definition.IsDefault)
    {
        SetDefaultStyle(definition.Type, definition.StyleId);
    }
    return true;
}

bool StyleManager::UpsertStyle(const StyleDefinition& definition)
{
    return HasStyle(definition.StyleId) ? UpdateStyle(definition) : CreateStyle(definition);
}

bool StyleManager::RemoveStyle(std::string_view styleId)
{
    auto root = GetRoot();
    auto style = WordStyleDefinitionHelper::FindStyleById(root, styleId);
    if (!root || !style)
    {
        return false;
    }
    return root->RemoveChild(style);
}

bool StyleManager::SetDefaultStyle(StyleType type, std::string_view styleId)
{
    auto root = GetRoot();
    auto target = WordStyleDefinitionHelper::FindStyleById(root, styleId);
    if (!root || !target)
    {
        return false;
    }

    const auto targetType = WordStylePartHelper::FromDomStyleType(target->GetType().ValueOr(
                                                                      ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues::Paragraph))
                                .value_or(StyleType::Paragraph);
    if (targetType != type)
    {
        return false;
    }

    for (const auto& style : root->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>())
    {
        if (!style)
        {
            continue;
        }
        const auto currentType = WordStylePartHelper::FromDomStyleType(style->GetType().ValueOr(
                                                                           ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues::Paragraph))
                                     .value_or(StyleType::Paragraph);
        if (currentType == type)
        {
            style->SetDefault(OnOffValue{});
        }
    }
    target->SetDefault(OnOffValue(true));
    return true;
}

bool StyleManager::ClearDefaultStyle(StyleType type)
{
    auto root = GetRoot();
    if (!root)
    {
        return false;
    }

    bool changed = false;
    for (const auto& style : root->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>())
    {
        if (!style)
        {
            continue;
        }
        const auto currentType = WordStylePartHelper::FromDomStyleType(style->GetType().ValueOr(
                                                                           ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleValues::Paragraph))
                                     .value_or(StyleType::Paragraph);
        if (currentType == type && style->GetDefault().IsDefined())
        {
            style->SetDefault(OnOffValue{});
            changed = true;
        }
    }
    return changed;
}

LatentStyleSettings StyleManager::GetLatentStyleSettings() const
{
    LatentStyleSettings result{};
    auto root = GetRoot();
    auto latent = root ? root->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyles>()
                       : nullptr;
    if (!latent)
    {
        return result;
    }

    result.DefaultLocked = WordPropertyReadHelper::OptionalOnOff(latent->GetDefaultLockedState());
    result.DefaultUiPriority = WordPropertyReadHelper::OptionalInt32(latent->GetDefaultUiPriority());
    result.DefaultSemiHidden = WordPropertyReadHelper::OptionalOnOff(latent->GetDefaultSemiHidden());
    result.DefaultUnhideWhenUsed = WordPropertyReadHelper::OptionalOnOff(latent->GetDefaultUnhideWhenUsed());
    result.DefaultPrimaryStyle = WordPropertyReadHelper::OptionalOnOff(latent->GetDefaultPrimaryStyle());
    result.Count = WordPropertyReadHelper::OptionalInt32(latent->GetCount());
    return result;
}

bool StyleManager::SetLatentStyleSettings(const LatentStyleSettings& settings)
{
    auto part = WordStylePartHelper::EnsureStyleDefinitionsPart(m_document);
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return false;
    }

    auto latent = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Styles,
                                                         ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyles>(root);
    if (!latent)
    {
        return false;
    }

    latent->SetDefaultLockedState(settings.DefaultLocked ? OnOffValue(*settings.DefaultLocked) : OnOffValue{});
    latent->SetDefaultUiPriority(settings.DefaultUiPriority ? Int32Value(*settings.DefaultUiPriority) : Int32Value{});
    latent->SetDefaultSemiHidden(settings.DefaultSemiHidden ? OnOffValue(*settings.DefaultSemiHidden) : OnOffValue{});
    latent->SetDefaultUnhideWhenUsed(settings.DefaultUnhideWhenUsed ? OnOffValue(*settings.DefaultUnhideWhenUsed)
                                                                    : OnOffValue{});
    latent->SetDefaultPrimaryStyle(settings.DefaultPrimaryStyle ? OnOffValue(*settings.DefaultPrimaryStyle)
                                                                : OnOffValue{});
    latent->SetCount(settings.Count ? Int32Value(*settings.Count) : Int32Value{});
    return true;
}

std::vector<LatentStyleException> StyleManager::LatentStyleExceptions() const
{
    std::vector<LatentStyleException> result;
    auto root = GetRoot();
    auto latent = root ? root->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyles>()
                       : nullptr;
    if (!latent)
    {
        return result;
    }

    for (const auto& item :
         latent->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyleExceptionInfo>())
    {
        if (!item)
        {
            continue;
        }
        LatentStyleException exception;
        exception.Name = WordPropertyReadHelper::StringValueOrEmpty(item->GetName());
        exception.Locked = WordPropertyReadHelper::OptionalOnOff(item->GetLocked());
        exception.UiPriority = WordPropertyReadHelper::OptionalInt32(item->GetUiPriority());
        exception.SemiHidden = WordPropertyReadHelper::OptionalOnOff(item->GetSemiHidden());
        exception.UnhideWhenUsed = WordPropertyReadHelper::OptionalOnOff(item->GetUnhideWhenUsed());
        exception.PrimaryStyle = WordPropertyReadHelper::OptionalOnOff(item->GetPrimaryStyle());
        result.push_back(std::move(exception));
    }
    return result;
}

std::optional<LatentStyleException> StyleManager::GetLatentStyleException(std::string_view name) const
{
    for (const auto& exception : LatentStyleExceptions())
    {
        if (exception.Name == name)
        {
            return exception;
        }
    }
    return std::nullopt;
}

bool StyleManager::SetLatentStyleException(const LatentStyleException& exception)
{
    if (exception.Name.empty())
    {
        return false;
    }

    auto part = WordStylePartHelper::EnsureStyleDefinitionsPart(m_document);
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return false;
    }
    auto latent = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Styles,
                                                         ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyles>(root);
    if (!latent)
    {
        return false;
    }

    std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyleExceptionInfo> item;
    for (const auto& current :
         latent->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyleExceptionInfo>())
    {
        if (current && WordPropertyReadHelper::StringValueOrEmpty(current->GetName()) == exception.Name)
        {
            item = current;
            break;
        }
    }
    if (!item)
    {
        item = latent->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyleExceptionInfo>();
    }
    if (!item)
    {
        return false;
    }

    item->SetName(StringValue(exception.Name));
    item->SetLocked(exception.Locked ? OnOffValue(*exception.Locked) : OnOffValue{});
    item->SetUiPriority(exception.UiPriority ? Int32Value(*exception.UiPriority) : Int32Value{});
    item->SetSemiHidden(exception.SemiHidden ? OnOffValue(*exception.SemiHidden) : OnOffValue{});
    item->SetUnhideWhenUsed(exception.UnhideWhenUsed ? OnOffValue(*exception.UnhideWhenUsed) : OnOffValue{});
    item->SetPrimaryStyle(exception.PrimaryStyle ? OnOffValue(*exception.PrimaryStyle) : OnOffValue{});
    return true;
}

bool StyleManager::RemoveLatentStyleException(std::string_view name)
{
    auto root = GetRoot();
    auto latent = root ? root->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyles>()
                       : nullptr;
    if (!latent)
    {
        return false;
    }

    for (const auto& item :
         latent->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LatentStyleExceptionInfo>())
    {
        if (item && WordPropertyReadHelper::StringValueOrEmpty(item->GetName()) == name)
        {
            return latent->RemoveChild(item);
        }
    }
    return false;
}

std::string StyleManager::ImportStyle(const StyleManager& source,
                                      std::string_view sourceStyleId,
                                      StyleCopyConflictPolicy policy,
                                      std::string_view preferredStyleId)
{
    if (sourceStyleId.empty())
    {
        return {};
    }

    auto sourcePart = source.GetStylesPart();
    auto targetPart = WordStylePartHelper::EnsureStyleDefinitionsPart(m_document);
    if (!sourcePart || !targetPart)
    {
        return {};
    }

    const std::string requestedId =
        preferredStyleId.empty() ? std::string(sourceStyleId) : std::string(preferredStyleId);
    auto existing = GetLowLevelStyle(requestedId);
    if (existing && policy == StyleCopyConflictPolicy::KeepExisting)
    {
        return requestedId;
    }

    std::string targetId = requestedId;
    if (existing && policy == StyleCopyConflictPolicy::Rename)
    {
        targetId = WordStyleDefinitionHelper::MakeUniqueStyleId(*this, requestedId);
        if (targetId.empty())
        {
            return {};
        }
    }

    auto targetRoot = targetPart->GetTypedRootElement();
    auto sourceStyle = source.GetLowLevelStyle(sourceStyleId);
    if (!targetRoot || !sourceStyle)
    {
        return {};
    }

    // Only Replace can still meet an existing style here: KeepExisting returned
    // above and Rename has already picked an unused id.
    auto replaced = policy == StyleCopyConflictPolicy::Replace ? WordStyleDefinitionHelper::FindStyleById(targetRoot, targetId) : nullptr;

    // CopyInto carries the source subtree's namespace declarations across, so no
    // manual xmlns bookkeeping is needed and the target part's live tree - along
    // with every wrapper into it - survives the import.
    auto imported = openxmlelement_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>(
        sourceStyle->CopyInto(targetRoot));
    if (!imported)
    {
        return {};
    }

    if (replaced && policy == StyleCopyConflictPolicy::Replace)
    {
        imported = openxmlelement_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Style>(
            replaced->ReplaceWith(imported));
        if (!imported)
        {
            return {};
        }
    }

    imported->SetStyleId(StringValue(targetId));
    return targetId;
}

Paragraph::Paragraph(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
                     const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_paragraph(paragraph), m_mainDocumentPart(mainDocumentPart)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph> Paragraph::GetLowLevelApi() const
{
    return m_paragraph;
}

Paragraph& Paragraph::AttachMainDocumentPart(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
{
    m_mainDocumentPart = mainDocumentPart;
    return *this;
}

std::shared_ptr<Run> Paragraph::AddRun()
{
    if (!m_paragraph)
    {
        return nullptr;
    }
    auto run = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    if (!run)
    {
        return nullptr;
    }
    return std::make_shared<Run>(run);
}

std::shared_ptr<Text> Paragraph::AddText(std::string_view text, bool preserveSpaces)
{
    auto run = AddRun();
    if (!run)
    {
        return nullptr;
    }
    return run->AddText(text, preserveSpaces);
}

std::shared_ptr<Run> Paragraph::AddBreak(BreakType type)
{
    auto run = AddRun();
    if (!run)
    {
        return nullptr;
    }
    run->AddBreak(type);
    return run;
}

std::shared_ptr<Run> Paragraph::AddRun(std::string_view text,
                                       const RunStyle& style,
                                       bool preserveSpaces)
{
    auto run = AddRun();
    if (!run)
    {
        return nullptr;
    }

    run->AddText(text, preserveSpaces);

    if (style.Bold)
    {
        run->SetBold(true);
    }
    if (style.Italic)
    {
        run->SetItalic(true);
    }
    if (style.Underline)
    {
        run->SetUnderline(true);
    }
    if (style.Strike)
    {
        run->SetStrike(true);
    }
    if (style.DoubleStrike)
    {
        run->SetDoubleStrike(true);
    }
    if (style.Caps)
    {
        run->SetCaps(true);
    }
    if (style.SmallCaps)
    {
        run->SetSmallCaps(true);
    }
    if (style.NoProof)
    {
        run->SetNoProof(true);
    }
    if (style.Color)
    {
        run->SetColor(*style.Color);
    }
    if (style.Highlight)
    {
        run->SetHighlight(*style.Highlight);
    }
    if (!style.AsciiFont.empty() || !style.HighAnsiFont.empty())
    {
        run->SetFont(style.AsciiFont, style.HighAnsiFont);
    }
    if (style.FontSize)
    {
        run->SetFontSize(*style.FontSize);
    }
    if (!style.StyleId.empty())
    {
        run->SetStyleId(style.StyleId);
    }

    return run;
}

std::vector<std::shared_ptr<Run>> Paragraph::Runs() const
{
    std::vector<std::shared_ptr<Run>> runs;
    if (!m_paragraph)
    {
        return runs;
    }

    for (const auto& run : m_paragraph->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
    {
        if (run)
        {
            runs.push_back(std::make_shared<Run>(run));
        }
    }
    return runs;
}

std::vector<std::shared_ptr<Text>> Paragraph::Texts() const
{
    std::vector<std::shared_ptr<Text>> texts;
    if (!m_paragraph)
    {
        return texts;
    }

    for (const auto& text : m_paragraph->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>())
    {
        if (text)
        {
            texts.push_back(std::make_shared<Text>(text));
        }
    }
    return texts;
}

std::vector<std::shared_ptr<Image>> Paragraph::Images() const
{
    std::vector<std::shared_ptr<Image>> images;
    if (!m_paragraph)
    {
        return images;
    }

    for (const auto& drawing : m_paragraph->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>())
    {
        if (drawing)
        {
            images.push_back(std::make_shared<Image>(drawing));
        }
    }
    return images;
}

std::string Paragraph::PlainText() const
{
    std::string result;
    for (const auto& text : Texts())
    {
        if (text)
        {
            result += text->GetText();
        }
    }
    return result;
}

Paragraph& Paragraph::SetStyleId(std::string_view styleId)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto style = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>(props);
    if (style)
    {
        style->SetVal(StringValue(std::string(styleId)));
    }
    return *this;
}

std::string Paragraph::GetStyleId() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    return WordPropertyReadHelper::GetStringChildValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>(props);
}

Paragraph& Paragraph::ClearStyleId()
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphStyleId>(props);
    return *this;
}

Paragraph& Paragraph::SetAlignment(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::JustificationValues alignment)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto justification = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Justification>(props);
    if (justification)
    {
        justification->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::JustificationValues>(alignment));
    }
    return *this;
}

std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::JustificationValues> Paragraph::GetAlignment() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    return WordPropertyReadHelper::ReadEnumValChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Justification,
                                                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::JustificationValues>(props);
}

Paragraph& Paragraph::ClearAlignment()
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Justification>(props);
    return *this;
}

Paragraph& Paragraph::SetSpacing(std::optional<ExyokiOffice::MeasuringUnits> before,
                                 std::optional<ExyokiOffice::MeasuringUnits> after,
                                 std::optional<ExyokiOffice::MeasuringUnits> line,
                                 ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues lineRule)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto spacing = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SpacingBetweenLines>(props);
    if (!spacing)
    {
        return *this;
    }

    if (before)
    {
        spacing->SetBefore(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*before))));
    }
    if (after)
    {
        spacing->SetAfter(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*after))));
    }
    if (line)
    {
        spacing->SetLine(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*line))));
        spacing->SetLineRule(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues>(lineRule));
    }
    return *this;
}

Paragraph& Paragraph::SetSpacingLines(std::optional<int> beforeLines,
                                      std::optional<int> afterLines,
                                      std::optional<int> lineLines,
                                      ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues lineRule)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto spacing = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SpacingBetweenLines>(props);
    if (!spacing)
    {
        return *this;
    }

    if (beforeLines)
    {
        spacing->SetBeforeLines(Int32Value(*beforeLines));
    }
    if (afterLines)
    {
        spacing->SetAfterLines(Int32Value(*afterLines));
    }
    if (lineLines)
    {
        spacing->SetLine(StringValue(std::to_string(*lineLines)));
        spacing->SetLineRule(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues>(lineRule));
    }
    return *this;
}

Paragraph& Paragraph::SetIndentation(std::optional<ExyokiOffice::MeasuringUnits> left,
                                     std::optional<ExyokiOffice::MeasuringUnits> right,
                                     std::optional<ExyokiOffice::MeasuringUnits> firstLine,
                                     std::optional<ExyokiOffice::MeasuringUnits> hanging)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto indentation = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Indentation>(props);
    if (!indentation)
    {
        return *this;
    }

    if (left)
    {
        indentation->SetLeft(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*left))));
    }
    if (right)
    {
        indentation->SetRight(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*right))));
    }
    if (firstLine)
    {
        indentation->SetFirstLine(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*firstLine))));
    }
    if (hanging)
    {
        indentation->SetHanging(StringValue(std::to_string(WordValueHelper::ToTwipsInt(*hanging))));
    }
    return *this;
}

bool Paragraph::TryGetSpacing(ParagraphSpacing& output) const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    auto spacing = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SpacingBetweenLines>() : nullptr;
    if (!spacing)
    {
        return false;
    }

    output = ParagraphSpacing{};
    output.Before = WordValueHelper::GetDefinedTwips(spacing->GetBefore());
    output.After = WordValueHelper::GetDefinedTwips(spacing->GetAfter());
    output.Line = WordValueHelper::GetDefinedTwips(spacing->GetLine());
    output.LineRule = spacing->GetLineRule().Value();
    return true;
}

bool Paragraph::TryGetSpacingLines(ParagraphSpacingLines& output) const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    auto spacing = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SpacingBetweenLines>() : nullptr;
    if (!spacing)
    {
        return false;
    }

    output = ParagraphSpacingLines{};
    output.BeforeLines = WordValueHelper::GetDefinedInt32(spacing->GetBeforeLines());
    output.AfterLines = WordValueHelper::GetDefinedInt32(spacing->GetAfterLines());
    output.LineLines = WordValueHelper::TryParseInt(spacing->GetLine().ToString());
    output.LineRule = spacing->GetLineRule().Value();
    return true;
}

bool Paragraph::TryGetIndentation(ParagraphIndentation& output) const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    auto indentation = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Indentation>() : nullptr;
    if (!indentation)
    {
        return false;
    }

    output = ParagraphIndentation{};
    output.Left = WordValueHelper::GetDefinedTwips(indentation->GetLeft());
    output.Right = WordValueHelper::GetDefinedTwips(indentation->GetRight());
    output.FirstLine = WordValueHelper::GetDefinedTwips(indentation->GetFirstLine());
    output.Hanging = WordValueHelper::GetDefinedTwips(indentation->GetHanging());
    return true;
}

Paragraph& Paragraph::ClearSpacing()
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SpacingBetweenLines>(props);
    return *this;
}

Paragraph& Paragraph::ClearIndentation()
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Indentation>(props);
    return *this;
}

Paragraph& Paragraph::SetNumbering(int numberingId, int level)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto numbering = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingProperties>(props);
    if (!numbering)
    {
        return *this;
    }
    auto numId = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingId>(numbering);
    auto lvl = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingLevelReference>(numbering);
    if (numId)
    {
        numId->SetVal(Int32Value(numberingId));
    }
    if (lvl)
    {
        lvl->SetVal(Int32Value(level));
    }
    return *this;
}

Paragraph& Paragraph::SetListStyle(const ListStyle& style)
{
    return SetNumbering(style.NumberingId, style.Level);
}

bool Paragraph::TryGetNumbering(ParagraphNumbering& output) const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    auto numbering = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingProperties>() : nullptr;
    if (!numbering)
    {
        return false;
    }

    output = ParagraphNumbering{};
    if (auto numId = numbering->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingId>())
    {
        output.NumberingId = numId->GetVal().Value();
    }
    if (auto level = numbering->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingLevelReference>())
    {
        output.Level = level->GetVal().Value();
    }
    return true;
}

Paragraph& Paragraph::ClearNumbering()
{
    if (!m_paragraph)
    {
        return *this;
    }
    auto props = m_paragraph->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties>();
    if (!props)
    {
        return *this;
    }
    if (auto numbering = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NumberingProperties>())
    {
        props->RemoveChild(numbering);
    }
    return *this;
}

Paragraph& Paragraph::AddTabStop(const ExyokiOffice::MeasuringUnits& position,
                                 ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TabStopValues alignment,
                                 ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TabStopLeaderCharValues leader)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto tabs = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Tabs>(props);
    if (!tabs)
    {
        return *this;
    }

    auto tab = tabs->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TabStop>();
    if (tab)
    {
        tab->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TabStopValues>(alignment));
        tab->SetLeader(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TabStopLeaderCharValues>(leader));
        tab->SetPosition(Int32Value(WordValueHelper::ToTwipsInt(position)));
    }
    return *this;
}

Paragraph& Paragraph::ClearTabStops()
{
    if (!m_paragraph)
    {
        return *this;
    }
    auto props = m_paragraph->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties>();
    if (!props)
    {
        return *this;
    }
    if (auto tabs = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Tabs>())
    {
        props->RemoveChild(tabs);
    }
    return *this;
}

Paragraph& Paragraph::SetBorders(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                                 UInt32 size,
                                 const ExyokiOffice::Color& color,
                                 std::optional<ExyokiOffice::MeasuringUnits> spacing)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto borders = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                                          ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders>(props);
    if (!borders)
    {
        return *this;
    }

    const auto spaceValue = spacing ? static_cast<UInt32>(std::max(0, WordValueHelper::ToTwipsInt(*spacing))) : 0u;
    const auto colorValue = color.ToHexString();

    auto applyBorder = [&](auto border)
    {
        if (!border)
        {
            return;
        }
        border->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues>(style));
        border->SetSize(UInt32Value(size));
        border->SetColor(StringValue(colorValue));
        border->SetSpace(UInt32Value(spaceValue));
    };

    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TopBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BottomBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LeftBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RightBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BetweenBorder>(borders));

    return *this;
}

Paragraph& Paragraph::SetBorders(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                                 const ExyokiOffice::MeasuringUnits& size,
                                 const ExyokiOffice::Color& color,
                                 std::optional<ExyokiOffice::MeasuringUnits> spacing)
{
    return SetBorders(style, WordValueHelper::ToBorderSizeUInt32(size), color, spacing);
}

Paragraph& Paragraph::ClearBorders()
{
    if (!m_paragraph)
    {
        return *this;
    }
    auto props = m_paragraph->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties>();
    if (!props)
    {
        return *this;
    }
    if (auto borders = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders>())
    {
        props->RemoveChild(borders);
    }
    return *this;
}

Paragraph& Paragraph::SetShading(const ExyokiOffice::Color& fill,
                                 ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues pattern,
                                 const ExyokiOffice::Color& patternColor)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto shading = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                                          ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Shading>(props);
    if (!shading)
    {
        return *this;
    }

    shading->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues>(pattern));
    shading->SetFill(StringValue(fill.ToHexString()));
    shading->SetColor(StringValue(patternColor.ToHexString()));
    return *this;
}

Paragraph& Paragraph::ClearShading()
{
    if (!m_paragraph)
    {
        return *this;
    }
    auto props = m_paragraph->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties>();
    if (!props)
    {
        return *this;
    }
    if (auto shading = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Shading>())
    {
        props->RemoveChild(shading);
    }
    return *this;
}

std::optional<ParagraphShading> Paragraph::GetShading() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    auto shading = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Shading>() : nullptr;
    if (!shading)
    {
        return std::nullopt;
    }

    ParagraphShading output;
    output = ParagraphShading{};
    output.FillColor = WordPropertyReadHelper::StringValueOrEmpty(shading->GetFill());
    output.PatternColor = WordPropertyReadHelper::StringValueOrEmpty(shading->GetColor());
    if (shading->GetVal().IsDefined())
    {
        output.Pattern = shading->GetVal().Value();
    }
    else if (auto parsed = WordValueHelper::TryParseEnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues>(
                 WordPropertyReadHelper::RawValAttributeOrEmpty(shading)))
    {
        output.Pattern = *parsed;
    }
    return output;
}

Paragraph& Paragraph::SetKeepWithNext(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto keep = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::KeepNext>(props);
    if (keep)
    {
        keep->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Paragraph& Paragraph::SetKeepLines(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto keep = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::KeepLines>(props);
    if (keep)
    {
        keep->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Paragraph& Paragraph::SetPageBreakBefore(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto pageBreak = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageBreakBefore>(props);
    if (pageBreak)
    {
        pageBreak->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Paragraph& Paragraph::SetWidowControl(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(m_paragraph);
    auto widow = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::WidowControl>(props);
    if (widow)
    {
        widow->SetVal(OnOffValue(enabled));
    }
    return *this;
}

std::optional<bool> Paragraph::GetKeepWithNext() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::KeepNext>(props);
}

std::optional<bool> Paragraph::GetKeepLines() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::KeepLines>(props);
}

std::optional<bool> Paragraph::GetPageBreakBefore() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageBreakBefore>(props);
}

std::optional<bool> Paragraph::GetWidowControl() const
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::WidowControl>(props);
}

Paragraph& Paragraph::ClearPagination()
{
    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::KeepNext>(props);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::KeepLines>(props);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageBreakBefore>(props);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::WidowControl>(props);
    return *this;
}

std::optional<ParagraphFormatting> Paragraph::GetFormatting() const
{
    if (!m_paragraph)
    {
        return std::nullopt;
    }

    ParagraphFormatting output;
    output.StyleId = GetStyleId();

    if (auto alignment = GetAlignment())
    {
        output.Alignment = *alignment;
    }

    ParagraphSpacing spacing;
    if (TryGetSpacing(spacing))
    {
        output.Spacing = spacing;
    }

    ParagraphSpacingLines spacingLines;
    if (TryGetSpacingLines(spacingLines))
    {
        output.SpacingLines = spacingLines;
    }

    ParagraphIndentation indentation;
    if (TryGetIndentation(indentation))
    {
        output.Indentation = indentation;
    }

    ParagraphNumbering numbering;
    if (TryGetNumbering(numbering))
    {
        output.Numbering = numbering;
    }

    if (auto shading = GetShading())
    {
        output.Shading = *shading;
    }

    auto props = WordPropertiesElementHelper::GetParagraphPropertiesElement(m_paragraph);
    output.HasBorders =
        WordPropertyReadHelper::GetFirstChildElementByTypeOrName<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphBorders>(props) != nullptr;

    if (auto flag = GetKeepWithNext())
    {
        output.KeepWithNext = *flag;
    }
    if (auto flag = GetKeepLines())
    {
        output.KeepLines = *flag;
    }
    if (auto flag = GetPageBreakBefore())
    {
        output.PageBreakBefore = *flag;
    }
    if (auto flag = GetWidowControl())
    {
        output.WidowControl = *flag;
    }
    return output;
}

Paragraph& Paragraph::ClearFormatting()
{
    return ClearStyleId()
        .ClearAlignment()
        .ClearSpacing()
        .ClearIndentation()
        .ClearNumbering()
        .ClearTabStops()
        .ClearBorders()
        .ClearShading()
        .ClearPagination();
}

std::shared_ptr<Hyperlink> Paragraph::AddHyperlink(std::string_view text,
                                                   std::string_view url,
                                                   std::string_view tooltip,
                                                   bool newWindow)
{
    if (!m_paragraph || url.empty())
    {
        return nullptr;
    }

    auto element = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Hyperlink>();
    if (!element)
    {
        return nullptr;
    }

    auto hyperlink = std::make_shared<Hyperlink>(element, m_mainDocumentPart);
    hyperlink->SetUrl(url);
    if (!tooltip.empty())
    {
        hyperlink->SetTooltip(tooltip);
    }
    if (newWindow)
    {
        hyperlink->SetNewWindow(true);
    }
    if (!text.empty())
    {
        hyperlink->AddText(text);
    }
    return hyperlink;
}

std::shared_ptr<Hyperlink> Paragraph::AddInternalHyperlink(std::string_view text,
                                                           std::string_view bookmarkName,
                                                           std::string_view tooltip)
{
    if (!m_paragraph || bookmarkName.empty())
    {
        return nullptr;
    }

    auto element = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Hyperlink>();
    if (!element)
    {
        return nullptr;
    }

    auto hyperlink = std::make_shared<Hyperlink>(element, m_mainDocumentPart);
    hyperlink->SetAnchor(bookmarkName);
    if (!tooltip.empty())
    {
        hyperlink->SetTooltip(tooltip);
    }
    if (!text.empty())
    {
        hyperlink->AddText(text);
    }
    return hyperlink;
}

std::vector<std::shared_ptr<Hyperlink>> Paragraph::Hyperlinks() const
{
    std::vector<std::shared_ptr<Hyperlink>> hyperlinks;
    if (!m_paragraph)
    {
        return hyperlinks;
    }

    // The generated element factory registers exactly one C++ type per element QName, but
    // "w:hyperlink" is shared by two distinct complex types in the schema: the normal
    // run-level hyperlink (CT_Hyperlink) and the restricted ruby-text hyperlink used inside
    // w:ruby (CT_HyperlinkRuby). That collision makes the factory resolve every "w:hyperlink"
    // node to whichever type survived generator-side deduplication, so Elements<Hyperlink>()
    // (which checks the resolved type against Hyperlink's metaclass) can silently reject valid
    // hyperlinks. Direct children of a paragraph named "hyperlink" are always the run-level
    // kind in practice (the ruby variant is only ever nested a few levels below a run, inside
    // w:ruby/w:rt), so matching by qualified name on direct children sidesteps the ambiguity.
    const ExyokiOffice::OpenXmlQualifiedName kHyperlinkName(kWordNamespace, "hyperlink");
    for (const auto& child : m_paragraph->Children())
    {
        if (child && child->QualifiedName() == kHyperlinkName)
        {
            auto hyperlink =
                std::static_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Hyperlink>(child);
            hyperlinks.push_back(std::make_shared<Hyperlink>(hyperlink, m_mainDocumentPart));
        }
    }
    return hyperlinks;
}

std::shared_ptr<Bookmark> Paragraph::AddBookmark(std::string_view name)
{
    if (!m_paragraph || name.empty())
    {
        return nullptr;
    }

    auto start = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>();
    if (!start)
    {
        return nullptr;
    }
    auto end = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>();
    if (!end)
    {
        m_paragraph->RemoveChild(start);
        return nullptr;
    }

    const auto id = WordIdHelper::NextBookmarkId(m_mainDocumentPart, m_paragraph);
    const auto idText = std::to_string(id);
    start->SetName(StringValue(std::string(name)));
    start->SetId(StringValue(idText));
    end->SetId(StringValue(idText));

    return std::make_shared<Bookmark>(start, end);
}

std::vector<std::shared_ptr<Bookmark>> Paragraph::Bookmarks() const
{
    std::vector<std::shared_ptr<Bookmark>> bookmarks;
    if (!m_paragraph)
    {
        return bookmarks;
    }

    auto localEnds = m_paragraph->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>();
    std::vector<std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>> documentEnds;
    if (m_mainDocumentPart)
    {
        if (auto root = m_mainDocumentPart->GetTypedRootElement())
        {
            documentEnds = root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>();
        }
    }

    for (const auto& start : m_paragraph->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>())
    {
        if (!start)
        {
            continue;
        }

        const auto id = start->GetId().ToString();
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd> matchedEnd;
        for (const auto& end : localEnds)
        {
            if (end && end->GetId().ToString() == id)
            {
                matchedEnd = end;
                break;
            }
        }
        if (!matchedEnd)
        {
            for (const auto& end : documentEnds)
            {
                if (end && end->GetId().ToString() == id)
                {
                    matchedEnd = end;
                    break;
                }
            }
        }
        bookmarks.push_back(std::make_shared<Bookmark>(start, matchedEnd));
    }
    return bookmarks;
}

std::shared_ptr<Field> Paragraph::AddField(std::string_view instruction,
                                           std::string_view cachedResult,
                                           bool preserveResultSpaces)
{
    if (!m_paragraph || instruction.empty())
    {
        return nullptr;
    }

    auto beginRun = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    auto codeRun = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    auto separatorRun = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    if (!beginRun || !codeRun || !separatorRun)
    {
        return nullptr;
    }

    auto begin = beginRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>();
    auto code = codeRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCode>();
    auto separator = separatorRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>();
    if (!begin || !code || !separator)
    {
        return nullptr;
    }

    begin->SetFieldCharType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues>(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::Begin));
    code->SetText(instruction);
    separator->SetFieldCharType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues>(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::Separate));

    if (!cachedResult.empty())
    {
        WordFieldHelper::InsertRunWithTextBefore(m_paragraph, nullptr, cachedResult, preserveResultSpaces);
    }

    auto endRun = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    if (!endRun)
    {
        return nullptr;
    }
    auto end = endRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>();
    if (!end)
    {
        return nullptr;
    }
    end->SetFieldCharType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues>(
        ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::End));

    auto field = std::make_shared<Field>(
        m_paragraph, begin, separator, end, beginRun, separatorRun, endRun, std::string(instruction), std::string(cachedResult));
    if (field->IsLayoutDependent())
    {
        field->InvalidateResult();
    }
    return field;
}

std::shared_ptr<Field> Paragraph::AddSimpleField(std::string_view instruction,
                                                 std::string_view cachedResult,
                                                 bool preserveResultSpaces)
{
    if (!m_paragraph || instruction.empty())
    {
        return nullptr;
    }

    auto simple = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SimpleField>();
    if (!simple)
    {
        return nullptr;
    }
    simple->SetInstruction(StringValue(std::string(instruction)));
    if (!cachedResult.empty())
    {
        WordFieldHelper::AppendRunWithText(simple, cachedResult, preserveResultSpaces);
    }

    auto field = std::make_shared<Field>(simple);
    if (field->IsLayoutDependent())
    {
        field->InvalidateResult();
    }
    return field;
}

std::vector<std::shared_ptr<Field>> Paragraph::Fields() const
{
    std::vector<std::shared_ptr<Field>> fields;
    if (!m_paragraph)
    {
        return fields;
    }

    struct PendingField
    {
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar> Begin;
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar> Separator;
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run> BeginRun;
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run> SeparatorRun;
        std::string Instruction;
        std::string Result;
    };

    const ExyokiOffice::OpenXmlQualifiedName simpleFieldName(kWordNamespace, "fldSimple");
    std::vector<PendingField> stack;

    for (const auto& child : m_paragraph->Children())
    {
        if (!child)
        {
            continue;
        }

        if (child->QualifiedName() == simpleFieldName)
        {
            auto simple =
                std::static_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SimpleField>(child);
            fields.push_back(std::make_shared<Field>(simple));
            continue;
        }

        auto run = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(child);
        if (!run)
        {
            continue;
        }

        const auto fieldChars = run->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>();
        if (fieldChars.empty() && !stack.empty())
        {
            if (stack.back().Separator)
            {
                stack.back().Result += WordValueHelper::DirectLeafTextByWordName(run, "t");
            }
            else
            {
                stack.back().Instruction += WordValueHelper::DirectLeafTextByWordName(run, "instrText");
            }
        }

        for (const auto& fieldChar : fieldChars)
        {
            switch (WordFieldHelper::ReadFieldCharType(fieldChar))
            {
                case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::Begin:
                    stack.push_back(PendingField{fieldChar, nullptr, run, nullptr, {}, {}});
                    break;
                case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::Separate:
                    if (!stack.empty())
                    {
                        stack.back().Separator = fieldChar;
                        stack.back().SeparatorRun = run;
                    }
                    break;
                case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::End:
                    if (!stack.empty())
                    {
                        auto pending = stack.back();
                        stack.pop_back();
                        fields.push_back(std::make_shared<Field>(
                            m_paragraph,
                            pending.Begin,
                            pending.Separator,
                            fieldChar,
                            pending.BeginRun,
                            pending.SeparatorRun,
                            run,
                            pending.Instruction,
                            pending.Result));
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return fields;
}

std::shared_ptr<Note> Paragraph::AddFootnote(std::string_view text, bool preserveSpaces)
{
    if (!m_paragraph || !m_mainDocumentPart)
    {
        return nullptr;
    }
    auto part = m_mainDocumentPart->GetFootnotesPart();
    if (!part)
    {
        part = m_mainDocumentPart->AddFootnotesPart();
    }
    return WordNoteHelper::AddNoteToDocument<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Footnote,
                                             ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteReferenceMark,
                                             ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteReference>(
        m_paragraph, m_mainDocumentPart, NoteKind::Footnote, part, text, preserveSpaces);
}

std::shared_ptr<Note> Paragraph::AddEndnote(std::string_view text, bool preserveSpaces)
{
    if (!m_paragraph || !m_mainDocumentPart)
    {
        return nullptr;
    }
    auto part = m_mainDocumentPart->GetEndnotesPart();
    if (!part)
    {
        part = m_mainDocumentPart->AddEndnotesPart();
    }
    return WordNoteHelper::AddNoteToDocument<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Endnote,
                                             ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::EndnoteReferenceMark,
                                             ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::EndnoteReference>(
        m_paragraph, m_mainDocumentPart, NoteKind::Endnote, part, text, preserveSpaces);
}

std::shared_ptr<Comment> Paragraph::AddComment(const std::vector<std::shared_ptr<Run>>& runs,
                                               std::string_view text,
                                               const CommentAuthor& author)
{
    if (!m_paragraph || !m_mainDocumentPart || runs.empty())
    {
        return nullptr;
    }

    auto firstRun = runs.front() ? runs.front()->GetLowLevelApi() : nullptr;
    auto lastRun = runs.back() ? runs.back()->GetLowLevelApi() : nullptr;
    if (!firstRun || !lastRun)
    {
        return nullptr;
    }

    auto commentsPart = m_mainDocumentPart->GetWordprocessingCommentsPart();
    if (!commentsPart)
    {
        commentsPart = m_mainDocumentPart->AddWordprocessingCommentsPart();
    }
    if (!commentsPart)
    {
        return nullptr;
    }
    auto commentsRoot = commentsPart->GetTypedRootElement();
    if (!commentsRoot)
    {
        return nullptr;
    }

    const int id = WordNoteHelper::NextCommentId(commentsPart);
    const std::string idText = std::to_string(id);

    auto rangeStart =
        m_paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentRangeStart>(firstRun);
    if (!rangeStart)
    {
        return nullptr;
    }
    rangeStart->SetId(StringValue(idText));

    auto insertBeforeForEnd = lastRun->NextSibling();
    auto rangeEnd = m_paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentRangeEnd>(
        insertBeforeForEnd);
    if (!rangeEnd)
    {
        m_paragraph->RemoveChild(rangeStart);
        return nullptr;
    }
    rangeEnd->SetId(StringValue(idText));

    auto refRun = m_paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(
        rangeEnd->NextSibling());
    if (!refRun)
    {
        m_paragraph->RemoveChild(rangeEnd);
        m_paragraph->RemoveChild(rangeStart);
        return nullptr;
    }
    auto reference = refRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentReference>();
    if (!reference)
    {
        m_paragraph->RemoveChild(refRun);
        m_paragraph->RemoveChild(rangeEnd);
        m_paragraph->RemoveChild(rangeStart);
        return nullptr;
    }
    reference->SetId(StringValue(idText));

    auto commentEntry = commentsRoot->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>();
    if (!commentEntry)
    {
        m_paragraph->RemoveChild(refRun);
        m_paragraph->RemoveChild(rangeEnd);
        m_paragraph->RemoveChild(rangeStart);
        return nullptr;
    }
    commentEntry->SetId(StringValue(idText));
    // w:author is required on w:comment, so an anonymous comment carries an
    // empty one rather than none; omitting it produced an invalid package.
    commentEntry->SetAuthor(StringValue(author.Name));
    if (!author.Initials.empty())
    {
        commentEntry->SetInitials(StringValue(author.Initials));
    }
    commentEntry->SetDate(DateTimeValue(std::chrono::system_clock::now()));

    auto commentWrapper = std::make_shared<Comment>(commentEntry, m_mainDocumentPart);
    if (!text.empty())
    {
        commentWrapper->AddParagraph(text);
    }

    // Every comment Word writes is a thread of one, complete with its
    // commentsExtended/commentsIds/commentsExtensible rows and its author entry
    // in the people part. Registering here is what lets Comment::AddReply() and
    // Comment::SetResolved() work on comments this library created.
    WordMergeHelper::CommentThreading::Register(m_mainDocumentPart, commentEntry, 0);
    return commentWrapper;
}

std::shared_ptr<Comment> Paragraph::AddCommentOnParagraph(std::string_view text, const CommentAuthor& author)
{
    return AddComment(Runs(), text, author);
}

std::vector<std::shared_ptr<Comment>> Paragraph::Comments() const
{
    std::vector<std::shared_ptr<Comment>> result;
    if (!m_paragraph || !m_mainDocumentPart)
    {
        return result;
    }

    auto commentsPart = m_mainDocumentPart->GetWordprocessingCommentsPart();
    if (!commentsPart)
    {
        return result;
    }
    auto commentsRoot = commentsPart->GetTypedRootElement();
    if (!commentsRoot)
    {
        return result;
    }

    for (const auto& child : m_paragraph->Children())
    {
        auto run = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(child);
        if (!run)
        {
            continue;
        }
        for (const auto& reference :
             run->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CommentReference>())
        {
            if (!reference)
            {
                continue;
            }
            const auto idText = reference->GetId().ToString();
            for (const auto& entry :
                 commentsRoot->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>())
            {
                if (entry && entry->GetId().ToString() == idText)
                {
                    result.push_back(std::make_shared<Comment>(entry, m_mainDocumentPart));
                    break;
                }
            }
        }
    }
    return result;
}

std::shared_ptr<ContentControl> Paragraph::AddInlineContentControl(std::string_view tag, std::string_view alias)
{
    if (!m_paragraph)
    {
        return nullptr;
    }
    auto sdt = m_paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtRun>();
    if (!sdt)
    {
        return nullptr;
    }
    auto control = std::make_shared<ContentControl>(sdt, ContentControlLevel::Inline, m_mainDocumentPart);
    control->EnsureId();
    if (!tag.empty())
    {
        control->SetTag(tag);
    }
    if (!alias.empty())
    {
        control->SetAlias(alias);
    }
    return control;
}

std::vector<std::shared_ptr<ContentControl>> Paragraph::ContentControls() const
{
    std::vector<std::shared_ptr<ContentControl>> result;
    if (!m_paragraph)
    {
        return result;
    }
    const ExyokiOffice::OpenXmlQualifiedName sdtName(kWordNamespace, "sdt");
    for (const auto& child : m_paragraph->Children())
    {
        if (child && child->QualifiedName() == sdtName)
        {
            result.push_back(
                std::make_shared<ContentControl>(child, ContentControlLevel::Inline, m_mainDocumentPart));
        }
    }
    return result;
}

std::optional<ContentRange> Paragraph::Find(std::string_view needle, Size searchFrom) const
{
    if (!m_paragraph || needle.empty())
    {
        return std::nullopt;
    }

    std::string text;
    for (const auto& run : Runs())
    {
        if (run)
        {
            text += run->PlainText();
        }
    }

    if (searchFrom > text.size())
    {
        return std::nullopt;
    }

    const auto pos = text.find(needle, searchFrom);
    if (pos == std::string::npos)
    {
        return std::nullopt;
    }

    return ContentRange{pos, pos + needle.size()};
}

std::vector<ContentRange> Paragraph::FindAll(std::string_view needle) const
{
    std::vector<ContentRange> ranges;
    if (needle.empty())
    {
        return ranges;
    }

    Size from = 0;
    while (auto range = Find(needle, from))
    {
        ranges.push_back(*range);
        from = range->End;
    }
    return ranges;
}

std::string Paragraph::GetText(const ContentRange& range) const
{
    if (!m_paragraph || range.Start >= range.End)
    {
        return {};
    }

    std::string text;
    for (const auto& run : Runs())
    {
        if (run)
        {
            text += run->PlainText();
        }
    }

    if (range.End > text.size())
    {
        return {};
    }
    return text.substr(range.Start, range.End - range.Start);
}

bool Paragraph::ReplaceText(const ContentRange& range, std::string_view replacement)
{
    if (!m_paragraph || range.Start > range.End)
    {
        return false;
    }

    auto runs = Runs();
    if (runs.empty())
    {
        if (range.Start != 0 || range.End != 0)
        {
            return false;
        }
        if (replacement.empty())
        {
            return true;
        }
        auto run = AddRun();
        if (!run)
        {
            return false;
        }
        if (auto text = run->AddText(replacement))
        {
            text->SetPreserveSpaces(true);
        }
        return true;
    }

    std::vector<Size> runStart(runs.size());
    std::vector<std::string> runText(runs.size());
    Size offset = 0;
    for (Size i = 0; i < runs.size(); ++i)
    {
        runText[i] = runs[i] ? runs[i]->PlainText() : std::string();
        runStart[i] = offset;
        offset += runText[i].size();
    }
    const Size totalLength = offset;

    if (range.End > totalLength)
    {
        return false;
    }

    // Locate the run containing the start offset: walk forward while the offset is at or
    // past the end of the current run (so a boundary offset belongs to the following run,
    // except when already at the last run).
    Size firstIndex = 0;
    while (firstIndex + 1 < runs.size() && range.Start >= runStart[firstIndex] + runText[firstIndex].size())
    {
        ++firstIndex;
    }

    Size lastIndex = firstIndex;
    while (lastIndex + 1 < runs.size() && range.End > runStart[lastIndex] + runText[lastIndex].size())
    {
        ++lastIndex;
    }

    const Size firstLocalStart = range.Start - runStart[firstIndex];
    const Size lastLocalEnd = range.End - runStart[lastIndex];

    const std::string prefix =
        runText[firstIndex].substr(0, std::min(firstLocalStart, runText[firstIndex].size()));
    const std::string suffix =
        runText[lastIndex].substr(std::min(lastLocalEnd, runText[lastIndex].size()));

    // Runs strictly between the boundary runs are fully covered by the range and are removed.
    for (Size i = firstIndex + 1; i < lastIndex; ++i)
    {
        if (runs[i])
        {
            m_paragraph->RemoveChild(runs[i]->GetLowLevelApi());
        }
    }

    if (firstIndex == lastIndex)
    {
        WordRunTextHelper::SetRunPlainText(runs[firstIndex], prefix + std::string(replacement) + suffix);
    }
    else
    {
        WordRunTextHelper::SetRunPlainText(runs[firstIndex], prefix + std::string(replacement));
        WordRunTextHelper::SetRunPlainText(runs[lastIndex], suffix);
    }

    return true;
}

Size Paragraph::ReplaceAll(std::string_view needle, std::string_view replacement)
{
    if (needle.empty())
    {
        return 0;
    }

    Size replacements = 0;
    Size from = 0;
    while (auto range = Find(needle, from))
    {
        if (!ReplaceText(*range, replacement))
        {
            break;
        }
        from = range->Start + replacement.size();
        ++replacements;
    }
    return replacements;
}

std::vector<ContentRange> Paragraph::FindAllRegex(const std::regex& pattern) const
{
    std::vector<ContentRange> ranges;
    if (!m_paragraph)
    {
        return ranges;
    }

    std::string text;
    for (const auto& run : Runs())
    {
        if (run)
        {
            text += run->PlainText();
        }
    }

    for (auto it = std::sregex_iterator(text.cbegin(), text.cend(), pattern); it != std::sregex_iterator(); ++it)
    {
        const auto& match = *it;
        const auto start = static_cast<Size>(match.position(0));
        ranges.push_back(ContentRange{start, start + static_cast<Size>(match.length(0))});
    }
    return ranges;
}

Size Paragraph::ReplaceAllRegex(const std::regex& pattern, std::string_view replacementFormat)
{
    const std::string format(replacementFormat);

    std::string text;
    for (const auto& run : Runs())
    {
        if (run)
        {
            text += run->PlainText();
        }
    }

    // Collect matches (and their formatted replacement text) against the original,
    // unmodified paragraph text first: std::sregex_iterator already guarantees
    // forward progress across zero-length matches, so there is no need to
    // re-derive that logic while mutating the DOM. Matches are then applied
    // back-to-front so earlier, not-yet-applied offsets stay valid.
    std::vector<std::pair<ContentRange, std::string>> pendingReplacements;
    for (auto it = std::sregex_iterator(text.cbegin(), text.cend(), pattern); it != std::sregex_iterator(); ++it)
    {
        const auto& match = *it;
        const auto start = static_cast<Size>(match.position(0));
        const auto end = start + static_cast<Size>(match.length(0));
        pendingReplacements.emplace_back(ContentRange{start, end}, match.format(format));
    }

    Size replacements = 0;
    for (auto it = pendingReplacements.rbegin(); it != pendingReplacements.rend(); ++it)
    {
        if (ReplaceText(it->first, it->second))
        {
            ++replacements;
        }
    }
    return replacements;
}

Run::Run(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>& run)
    : m_run(run)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run> Run::GetLowLevelApi() const
{
    return m_run;
}

std::shared_ptr<Text> Run::AddText(std::string_view text, bool preserveSpaces)
{
    if (!m_run)
    {
        return nullptr;
    }
    auto textElement = m_run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>();
    if (!textElement)
    {
        return nullptr;
    }
    textElement->SetText(text);
    if (preserveSpaces)
    {
        textElement->SetSpace(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues>(
            ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues::Preserve));
    }
    return std::make_shared<Text>(textElement);
}

Run& Run::AddBreak(BreakType type)
{
    if (!m_run)
    {
        return *this;
    }
    auto breakElement = m_run->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Break>();
    if (!breakElement)
    {
        return *this;
    }
    switch (type)
    {
        case BreakType::Page:
            breakElement->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BreakValues>(
                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BreakValues::Page));
            break;
        case BreakType::Column:
            breakElement->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BreakValues>(
                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BreakValues::Column));
            break;
        case BreakType::Line:
            // Text wrapping is the schema default; omit the attribute.
            break;
    }
    return *this;
}

std::vector<std::shared_ptr<Text>> Run::Texts() const
{
    std::vector<std::shared_ptr<Text>> texts;
    if (!m_run)
    {
        return texts;
    }

    for (const auto& text : m_run->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>())
    {
        if (text)
        {
            texts.push_back(std::make_shared<Text>(text));
        }
    }
    return texts;
}

std::vector<std::shared_ptr<Image>> Run::Images() const
{
    std::vector<std::shared_ptr<Image>> images;
    if (!m_run)
    {
        return images;
    }

    for (const auto& drawing : m_run->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>())
    {
        if (drawing)
        {
            images.push_back(std::make_shared<Image>(drawing));
        }
    }
    return images;
}

std::string Run::PlainText() const
{
    std::string result;
    for (const auto& text : Texts())
    {
        if (text)
        {
            result += text->GetText();
        }
    }
    return result;
}

Run& Run::SetBold(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto bold = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Bold>(props);
    if (bold)
    {
        bold->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetItalic(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto italic = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Italic>(props);
    if (italic)
    {
        italic->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetUnderline(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues style)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto underline = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Underline>(props);
    if (underline)
    {
        underline->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues>(style));
    }
    return *this;
}

Run& Run::SetUnderline(bool enabled)
{
    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues style = enabled ? ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues::Single : ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues::None;
    return SetUnderline(style);
}

Run& Run::SetStrike(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto strike = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Strike>(props);
    if (strike)
    {
        strike->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetDoubleStrike(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto strike = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DoubleStrike>(props);
    if (strike)
    {
        strike->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetCaps(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto caps = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Caps>(props);
    if (caps)
    {
        caps->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetSmallCaps(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto caps = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SmallCaps>(props);
    if (caps)
    {
        caps->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetNoProof(bool enabled)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto noProof = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NoProof>(props);
    if (noProof)
    {
        noProof->SetVal(OnOffValue(enabled));
    }
    return *this;
}

Run& Run::SetColor(const ExyokiOffice::Color& color)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto colorElement = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Color>(props);
    if (colorElement)
    {
        colorElement->SetVal(StringValue(color.ToHexString()));
    }
    return *this;
}

Run& Run::SetHighlight(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues color)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto highlight = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Highlight>(props);
    if (highlight)
    {
        highlight->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues>(color));
    }
    return *this;
}

Run& Run::SetFont(std::string_view asciiFont, std::string_view highAnsiFont)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto fonts = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunFonts>(props);
    if (fonts)
    {
        fonts->SetAscii(StringValue(std::string(asciiFont)));
        if (!highAnsiFont.empty())
        {
            fonts->SetHighAnsi(StringValue(std::string(highAnsiFont)));
        }
    }
    return *this;
}

Run& Run::SetLanguage(std::string_view latin, std::string_view eastAsia, std::string_view bidi)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto languages = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Languages>(props);
    if (languages)
    {
        if (!latin.empty())
        {
            languages->SetVal(StringValue(std::string(latin)));
        }
        if (!eastAsia.empty())
        {
            languages->SetEastAsia(StringValue(std::string(eastAsia)));
        }
        if (!bidi.empty())
        {
            languages->SetBidi(StringValue(std::string(bidi)));
        }
    }
    return *this;
}

Run& Run::SetKerning(const ExyokiOffice::MeasuringUnits& minimumSize)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto kern = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Kern>(props);
    if (kern)
    {
        auto halfPoints = std::llround(minimumSize.ToPt().GetValue() * 2.0);
        if (halfPoints < 0)
        {
            halfPoints = 0;
        }
        if (halfPoints > static_cast<Int64>(std::numeric_limits<UInt32>::max()))
        {
            halfPoints = static_cast<Int64>(std::numeric_limits<UInt32>::max());
        }
        kern->SetVal(UInt32Value(static_cast<UInt32>(halfPoints)));
    }
    return *this;
}

Run& Run::SetPosition(const ExyokiOffice::MeasuringUnits& offset)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto position = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Position>(props);
    if (position)
    {
        const auto halfPoints = static_cast<int>(std::lround(offset.ToPt().GetValue() * 2.0));
        position->SetVal(StringValue(std::to_string(halfPoints)));
    }
    return *this;
}

Run& Run::SetSpacing(const ExyokiOffice::MeasuringUnits& spacing)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto spacingElement = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Spacing>(props);
    if (spacingElement)
    {
        spacingElement->SetVal(Int32Value(WordValueHelper::ToTwipsInt(spacing)));
    }
    return *this;
}

Run& Run::SetFontSize(const ExyokiOffice::MeasuringUnits& size)
{
    const auto halfPoints = static_cast<int>(std::round(size.ToPt().GetValue() * 2.0));
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto fontSize = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FontSize>(props);
    if (fontSize)
    {
        fontSize->SetVal(StringValue(std::to_string(halfPoints)));
    }
    return *this;
}

Run& Run::SetTextEffect(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffectValues effect)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto textEffect = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffect>(props);
    if (textEffect)
    {
        textEffect->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffectValues>(effect));
    }
    return *this;
}

Run& Run::SetStyleId(std::string_view styleId)
{
    auto props = WordPropertiesElementHelper::EnsureRunProperties(m_run);
    auto style = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunStyle>(props);
    if (style)
    {
        style->SetVal(StringValue(std::string(styleId)));
    }
    return *this;
}

std::optional<bool> Run::GetBold() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Bold>(props);
}

Run& Run::ClearBold()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Bold>(props);
    return *this;
}

std::optional<bool> Run::GetItalic() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Italic>(props);
}

Run& Run::ClearItalic()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Italic>(props);
    return *this;
}

std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues> Run::GetUnderline() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadEnumValChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Underline,
                                                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::UnderlineValues>(props);
}

Run& Run::ClearUnderline()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Underline>(props);
    return *this;
}

std::optional<bool> Run::GetStrike() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Strike>(props);
}

Run& Run::ClearStrike()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Strike>(props);
    return *this;
}

std::optional<bool> Run::GetDoubleStrike() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DoubleStrike>(props);
}

Run& Run::ClearDoubleStrike()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DoubleStrike>(props);
    return *this;
}

std::optional<bool> Run::GetCaps() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Caps>(props);
}

Run& Run::ClearCaps()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Caps>(props);
    return *this;
}

std::optional<bool> Run::GetSmallCaps() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SmallCaps>(props);
}

Run& Run::ClearSmallCaps()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SmallCaps>(props);
    return *this;
}

std::optional<bool> Run::GetNoProof() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadOnOffChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NoProof>(props);
}

Run& Run::ClearNoProof()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::NoProof>(props);
    return *this;
}

std::string Run::GetColor() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto color = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Color>() : nullptr;
    auto value = color ? WordPropertyReadHelper::StringValueOrEmpty(color->GetVal()) : std::string{};
    return value.empty() ? WordPropertyReadHelper::RawValAttributeOrEmpty(color) : value;
}

Run& Run::ClearColor()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Color>(props);
    return *this;
}

std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues> Run::GetHighlight() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadEnumValChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Highlight,
                                                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues>(props);
}

Run& Run::ClearHighlight()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Highlight>(props);
    return *this;
}

std::optional<RunFonts> Run::GetFont() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto fonts = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunFonts>() : nullptr;
    if (!fonts)
    {
        return std::nullopt;
    }

    RunFonts output;
    output = RunFonts{};
    output.Ascii = WordPropertyReadHelper::StringValueOrEmpty(fonts->GetAscii());
    output.HighAnsi = WordPropertyReadHelper::StringValueOrEmpty(fonts->GetHighAnsi());
    output.EastAsia = WordPropertyReadHelper::StringValueOrEmpty(fonts->GetEastAsia());
    output.ComplexScript = WordPropertyReadHelper::StringValueOrEmpty(fonts->GetComplexScript());
    return output;
}

Run& Run::ClearFont()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunFonts>(props);
    return *this;
}

std::optional<RunLanguage> Run::GetLanguage() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto languages = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Languages>() : nullptr;
    if (!languages)
    {
        return std::nullopt;
    }

    RunLanguage output;
    output = RunLanguage{};
    output.Latin = WordPropertyReadHelper::StringValueOrEmpty(languages->GetVal());
    output.EastAsia = WordPropertyReadHelper::StringValueOrEmpty(languages->GetEastAsia());
    output.Bidi = WordPropertyReadHelper::StringValueOrEmpty(languages->GetBidi());
    return output;
}

Run& Run::ClearLanguage()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Languages>(props);
    return *this;
}

std::optional<ExyokiOffice::MeasuringUnits> Run::GetKerning() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto kern = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Kern>() : nullptr;
    return kern ? WordValueHelper::GetDefinedHalfPoints(kern->GetVal()) : std::nullopt;
}

Run& Run::ClearKerning()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Kern>(props);
    return *this;
}

std::optional<ExyokiOffice::MeasuringUnits> Run::GetPosition() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto position = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Position>() : nullptr;
    return position ? WordValueHelper::GetDefinedHalfPoints(position->GetVal()) : std::nullopt;
}

Run& Run::ClearPosition()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Position>(props);
    return *this;
}

std::optional<ExyokiOffice::MeasuringUnits> Run::GetSpacing() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto spacing = WordPropertyReadHelper::GetFirstChildElementByTypeOrName<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Spacing>(props);
    if (auto typed = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Spacing>(spacing))
    {
        return WordValueHelper::GetDefinedTwips(typed->GetVal());
    }
    if (auto raw = WordValueHelper::TryParseInt(WordPropertyReadHelper::RawValAttributeOrEmpty(spacing)))
    {
        return ExyokiOffice::MeasuringUnits(static_cast<Real>(*raw),
                                            ExyokiOffice::MeasurementUnit::Twip);
    }
    return std::nullopt;
}

Run& Run::ClearSpacing()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Spacing>(props);
    return *this;
}

std::optional<ExyokiOffice::MeasuringUnits> Run::GetFontSize() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    auto fontSize = props ? props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FontSize>() : nullptr;
    return fontSize ? WordValueHelper::GetDefinedHalfPoints(fontSize->GetVal()) : std::nullopt;
}

Run& Run::ClearFontSize()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FontSize>(props);
    return *this;
}

std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffectValues> Run::GetTextEffect() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::ReadEnumValChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffect,
                                                    ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffectValues>(props);
}

Run& Run::ClearTextEffect()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TextEffect>(props);
    return *this;
}

std::string Run::GetStyleId() const
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    return WordPropertyReadHelper::GetStringChildValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunStyle>(props);
}

Run& Run::ClearStyleId()
{
    auto props = WordPropertiesElementHelper::GetRunPropertiesElement(m_run);
    WordPropertyReadHelper::RemoveChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties,
                                              ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunStyle>(props);
    return *this;
}

std::optional<RunFormatting> Run::GetFormatting() const
{
    if (!m_run)
    {
        return std::nullopt;
    }

    RunFormatting output;
    output.StyleId = GetStyleId();
    output.Color = GetColor();

    if (auto flag = GetBold())
    {
        output.Bold = *flag;
    }
    if (auto flag = GetItalic())
    {
        output.Italic = *flag;
    }
    if (auto flag = GetStrike())
    {
        output.Strike = *flag;
    }
    if (auto flag = GetDoubleStrike())
    {
        output.DoubleStrike = *flag;
    }
    if (auto flag = GetCaps())
    {
        output.Caps = *flag;
    }
    if (auto flag = GetSmallCaps())
    {
        output.SmallCaps = *flag;
    }
    if (auto flag = GetNoProof())
    {
        output.NoProof = *flag;
    }

    if (auto underline = GetUnderline())
    {
        output.Underline = *underline;
    }

    if (auto highlight = GetHighlight())
    {
        output.Highlight = *highlight;
    }

    if (auto fonts = GetFont())
    {
        output.Fonts = *fonts;
    }

    if (auto language = GetLanguage())
    {
        output.Language = *language;
    }

    if (auto measurement = GetKerning())
    {
        output.Kerning = *measurement;
    }
    if (auto measurement = GetPosition())
    {
        output.Position = *measurement;
    }
    if (auto measurement = GetSpacing())
    {
        output.Spacing = *measurement;
    }
    if (auto measurement = GetFontSize())
    {
        output.FontSize = *measurement;
    }

    if (auto textEffect = GetTextEffect())
    {
        output.TextEffect = *textEffect;
    }
    return output;
}

Run& Run::ClearFormatting()
{
    return ClearStyleId()
        .ClearBold()
        .ClearItalic()
        .ClearUnderline()
        .ClearStrike()
        .ClearDoubleStrike()
        .ClearCaps()
        .ClearSmallCaps()
        .ClearNoProof()
        .ClearColor()
        .ClearHighlight()
        .ClearFont()
        .ClearLanguage()
        .ClearKerning()
        .ClearPosition()
        .ClearSpacing()
        .ClearFontSize()
        .ClearTextEffect();
}

Text::Text(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>& text)
    : m_text(text)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text> Text::GetLowLevelApi() const
{
    return m_text;
}

std::string Text::GetText() const
{
    if (!m_text)
    {
        return {};
    }
    return std::string(m_text->GetText());
}

Text& Text::SetText(std::string_view text)
{
    if (m_text)
    {
        m_text->SetText(text);
    }
    return *this;
}

Text& Text::SetPreserveSpaces(bool preserve)
{
    if (m_text)
    {
        if (preserve)
        {
            m_text->SetSpace(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues>(
                ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues::Preserve));
        }
        else
        {
            m_text->SetSpace(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues>(
                ExyokiOffice::DocumentFormat::OpenXml::SpaceProcessingModeValues::Default));
        }
    }
    return *this;
}

Hyperlink::Hyperlink(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Hyperlink>& hyperlink,
                     const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_hyperlink(hyperlink), m_mainDocumentPart(mainDocumentPart)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Hyperlink> Hyperlink::GetLowLevelApi() const
{
    return m_hyperlink;
}

Hyperlink& Hyperlink::AttachMainDocumentPart(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
{
    m_mainDocumentPart = mainDocumentPart;
    return *this;
}

std::shared_ptr<Run> Hyperlink::AddRun()
{
    if (!m_hyperlink)
    {
        return nullptr;
    }
    auto run = m_hyperlink->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    if (!run)
    {
        return nullptr;
    }
    return std::make_shared<Run>(run);
}

std::shared_ptr<Text> Hyperlink::AddText(std::string_view text, bool preserveSpaces)
{
    auto run = AddRun();
    if (!run)
    {
        return nullptr;
    }
    return run->AddText(text, preserveSpaces);
}

std::vector<std::shared_ptr<Run>> Hyperlink::Runs() const
{
    std::vector<std::shared_ptr<Run>> runs;
    if (!m_hyperlink)
    {
        return runs;
    }

    for (const auto& run : m_hyperlink->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
    {
        if (run)
        {
            runs.push_back(std::make_shared<Run>(run));
        }
    }
    return runs;
}

std::string Hyperlink::PlainText() const
{
    std::string result;
    for (const auto& run : Runs())
    {
        if (run)
        {
            result += run->PlainText();
        }
    }
    return result;
}

bool Hyperlink::IsExternal() const
{
    return m_hyperlink && !m_hyperlink->GetId().ToString().empty();
}

bool Hyperlink::IsInternal() const
{
    return m_hyperlink && !IsExternal() && !m_hyperlink->GetAnchor().ToString().empty();
}

void Hyperlink::RemoveExistingRelationship()
{
    if (!m_hyperlink || !m_mainDocumentPart)
    {
        return;
    }
    const auto existingId = m_hyperlink->GetId().ToString();
    if (!existingId.empty())
    {
        m_mainDocumentPart->RemoveExternalRelationship(existingId);
    }
}

Hyperlink& Hyperlink::SetUrl(std::string_view url)
{
    if (!m_hyperlink || !m_mainDocumentPart || url.empty())
    {
        return *this;
    }

    RemoveExistingRelationship();
    const auto relationshipId = m_mainDocumentPart->AddExternalRelationship(kHyperlinkRelationshipType, std::string(url));
    if (relationshipId.empty())
    {
        return *this;
    }

    m_hyperlink->SetAnchor(StringValue());
    m_hyperlink->SetId(StringValue(relationshipId));
    return *this;
}

std::string Hyperlink::GetUrl() const
{
    if (!m_hyperlink || !m_mainDocumentPart)
    {
        return {};
    }

    const auto relationshipId = m_hyperlink->GetId().ToString();
    if (relationshipId.empty())
    {
        return {};
    }

    // Hyperlinks are external relationships; RelationshipsByType() only returns internal
    // part-to-part relationships, so the full relationship list is searched here.
    for (const auto& relationship : m_mainDocumentPart->Relationships())
    {
        if (relationship.IsExternal && relationship.Type == kHyperlinkRelationshipType &&
            relationship.Id == relationshipId)
        {
            return relationship.Target;
        }
    }
    return {};
}

Hyperlink& Hyperlink::SetAnchor(std::string_view bookmarkName)
{
    if (!m_hyperlink || bookmarkName.empty())
    {
        return *this;
    }

    RemoveExistingRelationship();
    m_hyperlink->SetId(StringValue());
    m_hyperlink->SetAnchor(StringValue(std::string(bookmarkName)));
    return *this;
}

std::string Hyperlink::GetAnchor() const
{
    return m_hyperlink ? m_hyperlink->GetAnchor().ToString() : std::string();
}

Hyperlink& Hyperlink::SetTooltip(std::string_view tooltip)
{
    if (!m_hyperlink)
    {
        return *this;
    }
    m_hyperlink->SetTooltip(tooltip.empty() ? StringValue() : StringValue(std::string(tooltip)));
    return *this;
}

std::string Hyperlink::GetTooltip() const
{
    return m_hyperlink ? m_hyperlink->GetTooltip().ToString() : std::string();
}

Hyperlink& Hyperlink::SetNewWindow(bool enabled)
{
    if (!m_hyperlink)
    {
        return *this;
    }
    m_hyperlink->SetTargetFrame(enabled ? StringValue("_blank") : StringValue());
    return *this;
}

bool Hyperlink::GetNewWindow() const
{
    return m_hyperlink && m_hyperlink->GetTargetFrame().ToString() == "_blank";
}

void Hyperlink::Remove()
{
    if (!m_hyperlink)
    {
        return;
    }

    RemoveExistingRelationship();
    if (auto parent = m_hyperlink->Parent())
    {
        parent->RemoveChild(m_hyperlink);
    }
    m_hyperlink.reset();
}

Bookmark::Bookmark(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>& start,
                   const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>& end)
    : m_start(start), m_end(end)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkStart> Bookmark::GetStartElement() const
{
    return m_start;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd> Bookmark::GetEndElement() const
{
    return m_end;
}

std::string Bookmark::GetName() const
{
    return m_start ? m_start->GetName().ToString() : std::string();
}

int Bookmark::GetId() const
{
    if (!m_start)
    {
        return -1;
    }
    const auto idText = m_start->GetId().ToString();
    int id = 0;
    const auto result = std::from_chars(idText.data(), idText.data() + idText.size(), id);
    return result.ec == std::errc() ? id : -1;
}

void Bookmark::Remove()
{
    if (m_start)
    {
        if (auto parent = m_start->Parent())
        {
            parent->RemoveChild(m_start);
        }
    }
    if (m_end)
    {
        if (auto parent = m_end->Parent())
        {
            parent->RemoveChild(m_end);
        }
    }
}

Field::Field(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SimpleField>& simpleField)
    : m_simpleField(simpleField)
{
}

Field::Field(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
             const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>& begin,
             const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>& separator,
             const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>& end,
             const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>& beginRun,
             const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>& separatorRun,
             const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>& endRun,
             std::optional<std::string> instructionSnapshot,
             std::optional<std::string> resultSnapshot)
    : m_paragraph(paragraph), m_begin(begin), m_separator(separator), m_end(end), m_beginRun(beginRun), m_separatorRun(separatorRun), m_endRun(endRun), m_instructionSnapshot(std::move(instructionSnapshot)), m_resultSnapshot(std::move(resultSnapshot))
{
}

FieldKind Field::Kind() const noexcept
{
    return m_simpleField ? FieldKind::Simple : FieldKind::Complex;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SimpleField> Field::GetSimpleFieldElement() const
{
    return m_simpleField;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar> Field::GetBeginFieldCharElement() const
{
    return m_begin;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar> Field::GetSeparatorFieldCharElement() const
{
    return m_separator;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar> Field::GetEndFieldCharElement() const
{
    return m_end;
}

std::string Field::GetInstruction() const
{
    if (m_instructionSnapshot)
    {
        return *m_instructionSnapshot;
    }
    if (m_simpleField)
    {
        auto value = WordPropertyReadHelper::StringValueOrEmpty(m_simpleField->GetInstruction());
        return value.empty() ? WordRunTextHelper::WordAttributeOrEmpty(m_simpleField, "instr") : value;
    }

    auto beginRun = m_beginRun ? m_beginRun : WordFieldHelper::ParentRunOfFieldChar(m_begin);
    auto beforeRun = m_separator ? (m_separatorRun ? m_separatorRun : WordFieldHelper::ParentRunOfFieldChar(m_separator))
                                 : (m_endRun ? m_endRun : WordFieldHelper::ParentRunOfFieldChar(m_end));
    std::string instruction;
    for (const auto& child : WordFieldHelper::ParagraphChildrenBetween(m_paragraph, beginRun, beforeRun))
    {
        std::vector<std::shared_ptr<OpenXMLElement>> codes;
        WordBodyHelper::CollectDescendantsByName(child, ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "instrText"), codes);
        for (const auto& code : codes)
        {
            if (auto textElement = std::dynamic_pointer_cast<ExyokiOffice::OpenXmlLeafTextElement>(code))
            {
                instruction += std::string(textElement->GetText());
            }
        }
    }
    return instruction;
}

Field& Field::SetInstruction(std::string_view instruction)
{
    if (m_simpleField)
    {
        m_simpleField->SetInstruction(StringValue(std::string(instruction)));
        m_instructionSnapshot = std::string(instruction);
        return InvalidateResult();
    }

    auto beginRun = m_beginRun ? m_beginRun : WordFieldHelper::ParentRunOfFieldChar(m_begin);
    auto beforeRun = m_separator ? (m_separatorRun ? m_separatorRun : WordFieldHelper::ParentRunOfFieldChar(m_separator))
                                 : (m_endRun ? m_endRun : WordFieldHelper::ParentRunOfFieldChar(m_end));
    if (!m_paragraph || !beginRun || !beforeRun)
    {
        return *this;
    }

    WordFieldHelper::RemoveParagraphChildrenBetween(m_paragraph, beginRun, beforeRun);
    WordFieldHelper::AppendFieldCodeRun(m_paragraph, instruction, beforeRun);
    m_instructionSnapshot = std::string(instruction);
    return InvalidateResult();
}

std::string Field::GetResult() const
{
    if (m_resultSnapshot)
    {
        return *m_resultSnapshot;
    }
    std::string result;
    if (m_simpleField)
    {
        std::vector<std::shared_ptr<OpenXMLElement>> texts;
        WordBodyHelper::CollectDescendantsByName(m_simpleField, ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "t"), texts);
        for (const auto& text : texts)
        {
            if (auto textElement = std::dynamic_pointer_cast<ExyokiOffice::OpenXmlLeafTextElement>(text))
            {
                result += std::string(textElement->GetText());
            }
        }
        return result;
    }

    auto separatorRun = m_separatorRun ? m_separatorRun : WordFieldHelper::ParentRunOfFieldChar(m_separator);
    auto endRun = m_endRun ? m_endRun : WordFieldHelper::ParentRunOfFieldChar(m_end);
    for (const auto& child : WordFieldHelper::ParagraphChildrenBetween(m_paragraph, separatorRun, endRun))
    {
        std::vector<std::shared_ptr<OpenXMLElement>> texts;
        WordBodyHelper::CollectDescendantsByName(child, ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "t"), texts);
        for (const auto& text : texts)
        {
            if (auto textElement = std::dynamic_pointer_cast<ExyokiOffice::OpenXmlLeafTextElement>(text))
            {
                result += std::string(textElement->GetText());
            }
        }
    }
    return result;
}

bool Field::SetResult(std::string_view result, bool preserveSpaces)
{
    if (IsLayoutDependent())
    {
        InvalidateResult();
        return false;
    }

    if (m_simpleField)
    {
        for (const auto& child : m_simpleField->Children())
        {
            m_simpleField->RemoveChild(child);
        }
        if (!result.empty())
        {
            WordFieldHelper::AppendRunWithText(m_simpleField, result, preserveSpaces);
        }
        m_resultSnapshot = std::string(result);
        return true;
    }

    auto separatorRun = m_separatorRun ? m_separatorRun : WordFieldHelper::ParentRunOfFieldChar(m_separator);
    auto endRun = m_endRun ? m_endRun : WordFieldHelper::ParentRunOfFieldChar(m_end);
    if (!m_paragraph || !endRun)
    {
        return false;
    }
    if (!separatorRun)
    {
        separatorRun = m_paragraph->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>(endRun);
        if (!separatorRun)
        {
            return false;
        }
        m_separator = separatorRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldChar>();
        if (!m_separator)
        {
            return false;
        }
        m_separatorRun = separatorRun;
        m_separator->SetFieldCharType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FieldCharValues::Separate));
    }

    WordFieldHelper::RemoveParagraphChildrenBetween(m_paragraph, separatorRun, endRun);
    if (!result.empty())
    {
        WordFieldHelper::InsertRunWithTextBefore(m_paragraph, endRun, result, preserveSpaces);
    }
    m_resultSnapshot = std::string(result);
    return true;
}

Field& Field::InvalidateResult()
{
    if (m_simpleField)
    {
        m_simpleField->SetDirty(OnOffValue(true));
    }
    if (m_begin)
    {
        m_begin->SetDirty(OnOffValue(true));
    }
    return *this;
}

bool Field::IsDirty() const
{
    if (m_simpleField)
    {
        if (m_simpleField->GetDirty().IsDefined())
        {
            return m_simpleField->GetDirty().Value();
        }
        return WordPropertyReadHelper::RawOnOffOrEmpty(m_simpleField).value_or(false);
    }
    if (m_begin)
    {
        if (m_begin->GetDirty().IsDefined())
        {
            return m_begin->GetDirty().Value();
        }
        return WordPropertyReadHelper::RawOnOffOrEmpty(m_begin).value_or(false);
    }
    return false;
}

bool Field::IsLayoutDependent() const
{
    return WordFieldHelper::IsLayoutDependentFieldInstruction(GetInstruction());
}

Revision::Revision(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element)
    : m_element(element)
{
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> Revision::GetLowLevelApi() const
{
    return m_element;
}

RevisionType Revision::Type() const
{
    return m_element ? WordRevisionHelper::RevisionTypeFromName(m_element->QualifiedName()) : RevisionType::Unknown;
}

std::string Revision::GetId() const
{
    return WordRunTextHelper::WordAttributeOrEmpty(m_element, "id");
}

std::string Revision::GetAuthor() const
{
    return WordRunTextHelper::WordAttributeOrEmpty(m_element, "author");
}

std::string Revision::GetDate() const
{
    return WordRunTextHelper::WordAttributeOrEmpty(m_element, "date");
}

std::string Revision::Text() const
{
    std::string text;
    if (!m_element)
    {
        return text;
    }

    if (Type() == RevisionType::Deletion || Type() == RevisionType::MoveFrom)
    {
        for (const auto& deleted : m_element->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::DeletedText>())
        {
            if (deleted)
            {
                text += std::string(deleted->GetText());
            }
        }
        return text;
    }

    for (const auto& visible : m_element->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text>())
    {
        if (visible)
        {
            text += std::string(visible->GetText());
        }
    }
    return text;
}

bool Revision::Accept()
{
    switch (Type())
    {
        case RevisionType::Insertion:
        case RevisionType::MoveTo:
            return WordRevisionHelper::UnwrapRevisionElement(m_element, false);
        case RevisionType::Deletion:
        case RevisionType::MoveFrom:
            return WordRevisionHelper::RemoveRevisionElement(m_element, true);
        case RevisionType::Unknown:
        default:
            return WordRevisionHelper::RemoveRevisionElement(m_element);
    }
}

bool Revision::Reject()
{
    switch (Type())
    {
        case RevisionType::Insertion:
        case RevisionType::MoveTo:
            return WordRevisionHelper::RemoveRevisionElement(m_element, true);
        case RevisionType::Deletion:
        case RevisionType::MoveFrom:
            return WordRevisionHelper::UnwrapRevisionElement(m_element, true);
        case RevisionType::Unknown:
        default:
            return WordRevisionHelper::RemoveRevisionElement(m_element);
    }
}

Note::Note(NoteKind kind,
           const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
           const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_kind(kind), m_element(element), m_mainDocumentPart(mainDocumentPart)
{
}

NoteKind Note::Kind() const noexcept
{
    return m_kind;
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> Note::GetLowLevelApi() const
{
    return m_element;
}

int Note::GetId() const
{
    if (!m_element)
    {
        return -1;
    }
    const auto value =
        m_element->GetAttributeValue<IntegerValue>(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "id"));
    return value.IsDefined() ? static_cast<int>(value.Value()) : -1;
}

NoteEntryType Note::GetEntryType() const
{
    if (!m_element)
    {
        return NoteEntryType::Normal;
    }
    const auto type = m_element->GetAttributeValue<
        EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues>>(
        ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "type"));
    if (!type.IsDefined())
    {
        return NoteEntryType::Normal;
    }
    switch (type.Value())
    {
        case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues::Separator:
            return NoteEntryType::Separator;
        case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues::ContinuationSeparator:
            return NoteEntryType::ContinuationSeparator;
        case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues::ContinuationNotice:
            return NoteEntryType::ContinuationNotice;
        case ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteEndnoteValues::Normal:
        default:
            return NoteEntryType::Normal;
    }
}

std::vector<std::shared_ptr<Paragraph>> Note::Paragraphs() const
{
    std::vector<std::shared_ptr<Paragraph>> result;
    if (!m_element)
    {
        return result;
    }
    for (const auto& paragraph :
         m_element->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
    {
        if (paragraph)
        {
            result.push_back(std::make_shared<Paragraph>(paragraph, m_mainDocumentPart));
        }
    }
    return result;
}

std::shared_ptr<Paragraph> Note::AddParagraph(std::string_view text, bool preserveSpaces)
{
    if (!m_element)
    {
        return nullptr;
    }
    auto paragraph = m_element->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    if (!paragraph)
    {
        return nullptr;
    }
    auto wrapper = std::make_shared<Paragraph>(paragraph, m_mainDocumentPart);
    if (!text.empty())
    {
        wrapper->AddText(text, preserveSpaces);
    }
    return wrapper;
}

std::string Note::PlainText() const
{
    std::string text;
    for (const auto& paragraph : Paragraphs())
    {
        if (paragraph)
        {
            text += paragraph->PlainText();
        }
    }
    return text;
}

Note& Note::Clear()
{
    if (m_element)
    {
        for (const auto& child : m_element->Children())
        {
            if (child)
            {
                m_element->RemoveChild(child);
            }
        }
    }
    return *this;
}

Note& Note::SetText(std::string_view text, bool preserveSpaces)
{
    Clear();
    if (!m_element)
    {
        return *this;
    }

    auto paragraph = m_element->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    if (!paragraph)
    {
        return *this;
    }

    if (auto markRun = paragraph->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
    {
        if (m_kind == NoteKind::Footnote)
        {
            markRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteReferenceMark>();
        }
        else
        {
            markRun->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::EndnoteReferenceMark>();
        }
    }
    if (!text.empty())
    {
        WordFieldHelper::AppendRunWithText(paragraph, text, preserveSpaces);
    }
    return *this;
}

void Note::Remove()
{
    if (!m_element)
    {
        return;
    }

    const auto idText =
        m_element->GetAttributeValue<IntegerValue>(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "id"))
            .ToString();

    if (m_mainDocumentPart)
    {
        if (auto root = m_mainDocumentPart->GetTypedRootElement())
        {
            if (m_kind == NoteKind::Footnote)
            {
                for (const auto& reference :
                     root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FootnoteReference>())
                {
                    if (reference && reference->GetId().ToString() == idText)
                    {
                        WordStructureHelper::RemoveMarkerAndOwningRun(reference);
                    }
                }
            }
            else
            {
                for (const auto& reference :
                     root->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::EndnoteReference>())
                {
                    if (reference && reference->GetId().ToString() == idText)
                    {
                        WordStructureHelper::RemoveMarkerAndOwningRun(reference);
                    }
                }
            }
        }
    }

    if (auto parent = m_element->Parent())
    {
        parent->RemoveChild(m_element);
    }
    m_element = nullptr;
}

Comment::Comment(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>& comment,
                 const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_comment(comment), m_mainDocumentPart(mainDocumentPart)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment> Comment::GetLowLevelApi() const
{
    return m_comment;
}

int Comment::GetId() const
{
    if (!m_comment)
    {
        return -1;
    }
    auto parsed = WordValueHelper::TryParseInt(m_comment->GetId().ToString());
    return parsed ? *parsed : -1;
}

std::string Comment::GetAuthor() const
{
    return m_comment ? std::string(m_comment->GetAuthor().ToString()) : std::string();
}

Comment& Comment::SetAuthor(std::string_view author)
{
    if (m_comment)
    {
        m_comment->SetAuthor(StringValue(std::string(author)));
    }
    return *this;
}

std::string Comment::GetInitials() const
{
    return m_comment ? std::string(m_comment->GetInitials().ToString()) : std::string();
}

Comment& Comment::SetInitials(std::string_view initials)
{
    if (m_comment)
    {
        m_comment->SetInitials(StringValue(std::string(initials)));
    }
    return *this;
}

std::optional<std::chrono::system_clock::time_point> Comment::GetDate() const
{
    if (!m_comment)
    {
        return std::nullopt;
    }
    const auto value = m_comment->GetDate();
    if (!value.IsDefined())
    {
        return std::nullopt;
    }
    return value.Value();
}

Comment& Comment::SetDate(std::chrono::system_clock::time_point date)
{
    if (m_comment)
    {
        m_comment->SetDate(DateTimeValue(date));
    }
    return *this;
}

std::vector<std::shared_ptr<Paragraph>> Comment::Paragraphs() const
{
    std::vector<std::shared_ptr<Paragraph>> result;
    if (!m_comment)
    {
        return result;
    }
    for (const auto& paragraph :
         m_comment->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
    {
        if (paragraph)
        {
            result.push_back(std::make_shared<Paragraph>(paragraph, m_mainDocumentPart));
        }
    }
    return result;
}

std::shared_ptr<Paragraph> Comment::AddParagraph(std::string_view text, bool preserveSpaces)
{
    if (!m_comment)
    {
        return nullptr;
    }

    // A thread is keyed by the paraId of the comment's *last* paragraph, so the
    // key travels to the paragraph appended here and the former last paragraph
    // gets a fresh one. That keeps the commentsExtended and commentsIds rows and
    // every reply's w15:paraIdParent valid without rewriting any of them.
    auto previousLast = WordMergeHelper::CommentThreading::LastParagraph(m_comment);
    const auto threadParaId =
        previousLast ? WordMergeHelper::CommentThreading::ReadId(previousLast->GetParagraphId()) : UInt32{0};
    const bool threaded = WordMergeHelper::CommentThreading::FindCommentEx(m_mainDocumentPart, threadParaId) != nullptr;

    auto paragraph = m_comment->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    if (!paragraph)
    {
        return nullptr;
    }

    if (threaded)
    {
        auto used = WordMergeHelper::CommentThreading::CollectUsedIds(m_mainDocumentPart);
        if (const auto replacement = WordMergeHelper::CommentThreading::AllocateId(used); replacement != 0)
        {
            paragraph->SetParagraphId(WordMergeHelper::CommentThreading::MakeId(threadParaId));
            previousLast->SetParagraphId(WordMergeHelper::CommentThreading::MakeId(replacement));
        }
    }

    auto wrapper = std::make_shared<Paragraph>(paragraph, m_mainDocumentPart);
    if (!text.empty())
    {
        wrapper->AddText(text, preserveSpaces);
    }
    return wrapper;
}

std::string Comment::PlainText() const
{
    std::string text;
    for (const auto& paragraph : Paragraphs())
    {
        if (paragraph)
        {
            text += paragraph->PlainText();
        }
    }
    return text;
}

Comment& Comment::Clear()
{
    if (!m_comment)
    {
        return *this;
    }

    // A threaded comment cannot lose its last paragraph: the paraId on it is the
    // key its commentsExtended row, its replies and its resolution state hang
    // from. One empty paragraph is kept behind to carry that key. A comment that
    // takes no part in threading is emptied outright, as before.
    const auto threadParaId = WordMergeHelper::CommentThreading::GetThreadParaId(m_comment);
    const bool threaded = WordMergeHelper::CommentThreading::FindCommentEx(m_mainDocumentPart, threadParaId) != nullptr;

    for (const auto& child : m_comment->Children())
    {
        if (child)
        {
            m_comment->RemoveChild(child);
        }
    }

    if (threaded)
    {
        if (auto anchor =
                m_comment->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
        {
            anchor->SetParagraphId(WordMergeHelper::CommentThreading::MakeId(threadParaId));
        }
    }
    return *this;
}

Comment& Comment::SetText(std::string_view text, bool preserveSpaces)
{
    Clear();

    // Clear() leaves the thread anchor paragraph in place for a threaded comment;
    // filling it keeps SetText() a single-paragraph operation in both cases.
    if (auto paragraphs = Paragraphs(); !paragraphs.empty())
    {
        if (!text.empty() && paragraphs.front())
        {
            paragraphs.front()->AddText(text, preserveSpaces);
        }
        return *this;
    }

    AddParagraph(text, preserveSpaces);
    return *this;
}

std::shared_ptr<Comment> Comment::AddReply(std::string_view text, const CommentAuthor& author)
{
    if (!m_comment || !m_mainDocumentPart)
    {
        return nullptr;
    }

    auto commentsPart = m_mainDocumentPart->GetWordprocessingCommentsPart();
    auto commentsRoot = commentsPart ? commentsPart->GetTypedRootElement() : nullptr;
    if (!commentsRoot)
    {
        return nullptr;
    }

    // Threading is expressed only in commentsExtended, so this comment must own
    // its rows before anything can point at it.
    WordMergeHelper::CommentThreading::Register(m_mainDocumentPart, m_comment, 0);
    const auto parentParaId = WordMergeHelper::CommentThreading::GetThreadParaId(m_comment);
    if (parentParaId == 0)
    {
        return nullptr;
    }

    const std::string parentIdText = m_comment->GetId().ToString();
    const std::string idText = std::to_string(WordNoteHelper::NextCommentId(commentsPart));

    // The reply is a comment in its own right and needs its own markers around
    // the very same body span as the comment it answers.
    if (!WordMergeHelper::CommentThreading::AddReplyBodyMarkers(m_mainDocumentPart, parentIdText, idText))
    {
        return nullptr;
    }

    auto replyEntry = commentsRoot->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Comment>();
    if (!replyEntry)
    {
        WordMergeHelper::CommentThreading::RemoveBodyMarkers(m_mainDocumentPart, idText);
        return nullptr;
    }
    replyEntry->SetId(StringValue(idText));
    // w:author is required on w:comment; see Paragraph::AddComment.
    replyEntry->SetAuthor(StringValue(author.Name));
    if (!author.Initials.empty())
    {
        replyEntry->SetInitials(StringValue(author.Initials));
    }
    replyEntry->SetDate(DateTimeValue(std::chrono::system_clock::now()));

    auto replyWrapper = std::make_shared<Comment>(replyEntry, m_mainDocumentPart);
    if (!text.empty())
    {
        replyWrapper->AddParagraph(text);
    }
    WordMergeHelper::CommentThreading::Register(m_mainDocumentPart, replyEntry, parentParaId);
    return replyWrapper;
}

std::vector<std::shared_ptr<Comment>> Comment::Replies() const
{
    std::vector<std::shared_ptr<Comment>> result;
    if (!m_comment)
    {
        return result;
    }
    const auto paraId = WordMergeHelper::CommentThreading::GetThreadParaId(m_comment);
    for (const auto& entry : WordMergeHelper::CommentThreading::DirectReplies(m_mainDocumentPart, paraId))
    {
        result.push_back(std::make_shared<Comment>(entry, m_mainDocumentPart));
    }
    return result;
}

std::shared_ptr<Comment> Comment::GetParent() const
{
    if (!m_comment)
    {
        return nullptr;
    }
    auto row = WordMergeHelper::CommentThreading::FindCommentEx(m_mainDocumentPart, WordMergeHelper::CommentThreading::GetThreadParaId(m_comment));
    if (!row)
    {
        return nullptr;
    }
    auto parentEntry =
        WordMergeHelper::CommentThreading::FindByThreadParaId(m_mainDocumentPart, WordMergeHelper::CommentThreading::ReadId(row->GetParaIdParent()));
    return parentEntry ? std::make_shared<Comment>(parentEntry, m_mainDocumentPart) : nullptr;
}

bool Comment::IsResolved() const
{
    if (!m_comment)
    {
        return false;
    }
    auto row = WordMergeHelper::CommentThreading::FindCommentEx(m_mainDocumentPart, WordMergeHelper::CommentThreading::GetThreadParaId(m_comment));
    return row && row->GetDone().ValueOr(false);
}

Comment& Comment::SetResolved(bool resolved)
{
    if (!m_comment || !m_mainDocumentPart)
    {
        return *this;
    }

    // Word resolves whole threads, so the flag goes on the root and on every
    // reply below it no matter which member of the thread this wrapper points at.
    WordMergeHelper::CommentThreading::Register(m_mainDocumentPart, m_comment, 0);
    const auto paraId = WordMergeHelper::CommentThreading::GetThreadParaId(m_comment);
    const auto rootParaId = WordMergeHelper::CommentThreading::ThreadRootParaId(m_mainDocumentPart, paraId);
    WordMergeHelper::CommentThreading::SetDoneRecursive(m_mainDocumentPart, rootParaId, resolved, 0);
    return *this;
}

void Comment::Remove()
{
    if (!m_comment)
    {
        return;
    }

    const auto idText = m_comment->GetId().ToString();
    const auto paraId = WordMergeHelper::CommentThreading::GetThreadParaId(m_comment);

    // Dropping this comment's own rows first makes the recursion below immune to
    // a parent cycle in a hand-edited commentsExtended part.
    WordMergeHelper::CommentThreading::Unregister(m_mainDocumentPart, paraId);

    // A reply answers a comment that is about to disappear, so it goes with it,
    // together with its own markers, entry and rows.
    for (const auto& replyEntry : WordMergeHelper::CommentThreading::DirectReplies(m_mainDocumentPart, paraId))
    {
        Comment(replyEntry, m_mainDocumentPart).Remove();
    }

    WordMergeHelper::CommentThreading::RemoveBodyMarkers(m_mainDocumentPart, idText);

    if (auto parent = m_comment->Parent())
    {
        parent->RemoveChild(m_comment);
    }
    m_comment = nullptr;
}

ContentControl::ContentControl(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& sdt,
                               ContentControlLevel level,
                               const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_sdt(sdt), m_level(level), m_mainDocumentPart(mainDocumentPart)
{
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> ContentControl::GetLowLevelApi() const
{
    return m_sdt;
}

ContentControlLevel ContentControl::Level() const noexcept
{
    return m_level;
}

bool ContentControl::IsBlock() const noexcept
{
    return m_level == ContentControlLevel::Block;
}

bool ContentControl::IsInline() const noexcept
{
    return m_level == ContentControlLevel::Inline;
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> ContentControl::GetProperties() const
{
    if (!m_sdt)
    {
        return nullptr;
    }
    return m_sdt->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtProperties>();
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> ContentControl::EnsureProperties()
{
    if (!m_sdt)
    {
        return nullptr;
    }
    if (auto existing = GetProperties())
    {
        return existing;
    }

    // sdtPr must precede sdtContent/sdtEndPr per the CT_SdtBlock/CT_SdtRun content model.
    std::shared_ptr<OpenXMLElement> beforeAnchor =
        m_sdt->GetChild(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sdtContent"));
    if (!beforeAnchor)
    {
        beforeAnchor =
            m_sdt->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtEndCharProperties>();
    }
    return m_sdt->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtProperties>(beforeAnchor);
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> ContentControl::GetContent() const
{
    if (!m_sdt)
    {
        return nullptr;
    }
    return m_sdt->GetChild(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sdtContent"));
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> ContentControl::EnsureContent()
{
    if (!m_sdt)
    {
        return nullptr;
    }
    if (auto existing = GetContent())
    {
        return existing;
    }
    if (m_level == ContentControlLevel::Block)
    {
        return m_sdt->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtContentBlock>();
    }
    return m_sdt->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtContentRun>();
}

int ContentControl::GetId() const
{
    auto props = GetProperties();
    if (!props)
    {
        return -1;
    }
    auto idElement = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtId>();
    if (!idElement)
    {
        return -1;
    }
    const auto value = idElement->GetVal();
    return value.IsDefined() ? value.Value() : -1;
}

int ContentControl::EnsureId()
{
    auto props = EnsureProperties();
    if (!props)
    {
        return -1;
    }

    auto idElement = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtId>();
    if (idElement)
    {
        const auto value = idElement->GetVal();
        if (value.IsDefined())
        {
            return value.Value();
        }
    }
    else
    {
        idElement = props->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtId>();
        if (!idElement)
        {
            return -1;
        }
    }

    const int id = WordStructureHelper::NextSdtId(m_mainDocumentPart, m_sdt);
    idElement->SetVal(Int32Value(id));
    return id;
}

ContentControl& ContentControl::SetTag(std::string_view tag)
{
    if (auto props = EnsureProperties())
    {
        if (auto element =
                WordStructureHelper::EnsureChildOfType<OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Tag>(props))
        {
            element->SetVal(StringValue(std::string(tag)));
        }
    }
    return *this;
}

std::string ContentControl::GetTag() const
{
    auto props = GetProperties();
    if (!props)
    {
        return {};
    }
    auto element = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Tag>();
    return element ? std::string(element->GetVal().ToString()) : std::string();
}

ContentControl& ContentControl::SetAlias(std::string_view alias)
{
    if (auto props = EnsureProperties())
    {
        if (auto element =
                WordStructureHelper::EnsureChildOfType<OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtAlias>(
                    props))
        {
            element->SetVal(StringValue(std::string(alias)));
        }
    }
    return *this;
}

std::string ContentControl::GetAlias() const
{
    auto props = GetProperties();
    if (!props)
    {
        return {};
    }
    auto element = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SdtAlias>();
    return element ? std::string(element->GetVal().ToString()) : std::string();
}

ContentControl& ContentControl::SetLock(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LockingValues lock)
{
    if (auto props = EnsureProperties())
    {
        if (auto element =
                WordStructureHelper::EnsureChildOfType<OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Lock>(
                    props))
        {
            element->SetVal(
                EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LockingValues>(lock));
        }
    }
    return *this;
}

std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LockingValues> ContentControl::GetLock() const
{
    auto props = GetProperties();
    if (!props)
    {
        return std::nullopt;
    }
    auto element = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Lock>();
    if (!element)
    {
        return std::nullopt;
    }
    const auto value = element->GetVal();
    if (!value.IsDefined())
    {
        return std::nullopt;
    }
    return value.Value();
}

ContentControl& ContentControl::ClearLock()
{
    if (auto props = GetProperties())
    {
        if (auto element = props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Lock>())
        {
            props->RemoveChild(element);
        }
    }
    return *this;
}

ContentControl& ContentControl::SetShowingPlaceholder(bool enabled)
{
    if (auto props = EnsureProperties())
    {
        if (auto element = WordStructureHelper::EnsureChildOfType<OpenXMLElement,
                                                                  ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShowingPlaceholder>(
                props))
        {
            element->SetVal(OnOffValue(enabled));
        }
    }
    return *this;
}

bool ContentControl::IsShowingPlaceholder() const
{
    auto props = GetProperties();
    if (!props)
    {
        return false;
    }
    auto element =
        props->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShowingPlaceholder>();
    return element ? element->GetVal().ValueOr(false) : false;
}

std::vector<std::shared_ptr<Paragraph>> ContentControl::Paragraphs() const
{
    std::vector<std::shared_ptr<Paragraph>> result;
    if (m_level != ContentControlLevel::Block)
    {
        return result;
    }
    auto content = GetContent();
    if (!content)
    {
        return result;
    }
    for (const auto& paragraph :
         content->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
    {
        if (paragraph)
        {
            result.push_back(std::make_shared<Paragraph>(paragraph, m_mainDocumentPart));
        }
    }
    return result;
}

std::shared_ptr<Paragraph> ContentControl::AddParagraph(std::string_view text, bool preserveSpaces)
{
    if (m_level != ContentControlLevel::Block)
    {
        return nullptr;
    }
    auto content = EnsureContent();
    if (!content)
    {
        return nullptr;
    }
    auto paragraph = content->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    if (!paragraph)
    {
        return nullptr;
    }
    auto wrapper = std::make_shared<Paragraph>(paragraph, m_mainDocumentPart);
    if (!text.empty())
    {
        wrapper->AddText(text, preserveSpaces);
    }
    return wrapper;
}

std::vector<std::shared_ptr<Run>> ContentControl::Runs() const
{
    std::vector<std::shared_ptr<Run>> result;
    if (m_level != ContentControlLevel::Inline)
    {
        return result;
    }
    auto content = GetContent();
    if (!content)
    {
        return result;
    }
    for (const auto& run : content->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>())
    {
        if (run)
        {
            result.push_back(std::make_shared<Run>(run));
        }
    }
    return result;
}

std::shared_ptr<Run> ContentControl::AddRun()
{
    if (m_level != ContentControlLevel::Inline)
    {
        return nullptr;
    }
    auto content = EnsureContent();
    if (!content)
    {
        return nullptr;
    }
    auto run = content->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Run>();
    return run ? std::make_shared<Run>(run) : nullptr;
}

std::shared_ptr<Text> ContentControl::AddText(std::string_view text, bool preserveSpaces)
{
    auto run = AddRun();
    return run ? run->AddText(text, preserveSpaces) : nullptr;
}

std::string ContentControl::PlainText() const
{
    std::string text;
    if (m_level == ContentControlLevel::Block)
    {
        for (const auto& paragraph : Paragraphs())
        {
            if (paragraph)
            {
                text += paragraph->PlainText();
            }
        }
    }
    else
    {
        for (const auto& run : Runs())
        {
            if (run)
            {
                text += run->PlainText();
            }
        }
    }
    return text;
}

ContentControl& ContentControl::Clear()
{
    if (auto content = GetContent())
    {
        for (const auto& child : content->Children())
        {
            if (child)
            {
                content->RemoveChild(child);
            }
        }
    }
    return *this;
}

ContentControl& ContentControl::SetText(std::string_view text, bool preserveSpaces)
{
    Clear();
    if (m_level == ContentControlLevel::Block)
    {
        AddParagraph(text, preserveSpaces);
    }
    else if (!text.empty())
    {
        AddText(text, preserveSpaces);
    }
    return *this;
}

void ContentControl::Remove()
{
    if (!m_sdt)
    {
        return;
    }
    if (auto parent = m_sdt->Parent())
    {
        parent->RemoveChild(m_sdt);
    }
    m_sdt = nullptr;
}

Image::Image(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing,
             const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_drawing(drawing), m_mainDocumentPart(mainDocumentPart)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Drawing> Image::GetLowLevelApi() const
{
    return m_drawing;
}

ImageLayout Image::GetLayout() const
{
    if (!m_drawing)
    {
        return ImageLayout::Inline;
    }
    if (m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>())
    {
        return ImageLayout::Floating;
    }
    return ImageLayout::Inline;
}

Image& Image::SetLayout(ImageLayout layout)
{
    if (!m_drawing)
    {
        return *this;
    }

    const auto current = GetLayout();
    if (current == layout)
    {
        return *this;
    }

    auto info = WordDrawingHelper::ExtractDrawingInfo(m_drawing);
    auto layoutState = WordDrawingHelper::ExtractImageLayoutState(m_drawing);
    if (info.relationshipId.empty())
    {
        return *this;
    }

    if (auto inlineDrawing = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>())
    {
        m_drawing->RemoveChild(inlineDrawing);
    }
    if (auto anchorDrawing = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>())
    {
        m_drawing->RemoveChild(anchorDrawing);
    }

    if (!WordDrawingHelper::PopulateDrawingWithPicture(m_drawing,
                                                       info.relationshipId,
                                                       info.widthEmu,
                                                       info.heightEmu,
                                                       layout,
                                                       ImageWrap::Square,
                                                       info.docId,
                                                       info.name))
    {
        return *this;
    }

    if (layoutState.hasDistances)
    {
        SetDistanceFromText(ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.distanceLeft),
                                                         ExyokiOffice::MeasurementUnit::Emu),
                            ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.distanceTop),
                                                         ExyokiOffice::MeasurementUnit::Emu),
                            ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.distanceRight),
                                                         ExyokiOffice::MeasurementUnit::Emu),
                            ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.distanceBottom),
                                                         ExyokiOffice::MeasurementUnit::Emu));
    }

    if (layout == ImageLayout::Floating)
    {
        if (layoutState.hasWrap)
        {
            SetWrap(layoutState.wrap, layoutState.wrapText);
        }
        if (layoutState.hasHorizontalAlign && layoutState.hasVerticalAlign)
        {
            SetPositionAligned(layoutState.horizontalFrom,
                               layoutState.horizontalAlign,
                               layoutState.verticalFrom,
                               layoutState.verticalAlign);
        }
        else if (layoutState.hasHorizontalOffset && layoutState.hasVerticalOffset)
        {
            SetPosition(layoutState.horizontalFrom,
                        ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.horizontalOffsetEmu),
                                                     ExyokiOffice::MeasurementUnit::Emu),
                        layoutState.verticalFrom,
                        ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.verticalOffsetEmu),
                                                     ExyokiOffice::MeasurementUnit::Emu));
        }
        if (layoutState.hasAnchor)
        {
            SetBehindText(layoutState.behindText);
            SetAllowOverlap(layoutState.allowOverlap);
            SetAnchorLocked(layoutState.locked);
            SetLayoutInCell(layoutState.layoutInCell);
            SetRelativeHeight(layoutState.relativeHeight);
            SetSimplePositionEnabled(layoutState.simplePosEnabled);
            if (layoutState.hasSimplePosition)
            {
                SetSimplePosition(ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.simplePosX),
                                                               ExyokiOffice::MeasurementUnit::Emu),
                                  ExyokiOffice::MeasuringUnits(static_cast<Real>(layoutState.simplePosY),
                                                               ExyokiOffice::MeasurementUnit::Emu));
            }
        }
    }

    return *this;
}

Image& Image::SetSize(const ExyokiOffice::MeasuringUnits& width, const ExyokiOffice::MeasuringUnits& height)
{
    if (!m_drawing)
    {
        return *this;
    }

    const auto widthEmu = WordValueHelper::ToEmuInt64(width);
    const auto heightEmu = WordValueHelper::ToEmuInt64(height);

    std::shared_ptr<OpenXMLElement> container = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>();
    if (!container)
    {
        container = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    }
    if (container)
    {
        auto extent = WordStructureHelper::EnsureChildOfType<OpenXMLElement, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Extent>(container);
        if (extent)
        {
            extent->SetCx(Int64Value(widthEmu));
            extent->SetCy(Int64Value(heightEmu));
        }
    }

    auto graphic = container ? container->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>() : m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Graphic>();
    if (graphic)
    {
        auto graphicData = graphic->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::GraphicData>();
        if (graphicData)
        {
            auto picture = graphicData->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::Picture>();
            if (picture)
            {
                auto shapeProps = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::Picture, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties>(picture);
                auto transform = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D>(shapeProps);
                auto extents = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Extents>(transform);
                if (extents)
                {
                    extents->SetCx(Int64Value(widthEmu));
                    extents->SetCy(Int64Value(heightEmu));
                }
            }
        }
    }

    return *this;
}

bool Image::TryGetSize(ImageSize& output) const
{
    if (!m_drawing)
    {
        return false;
    }

    std::shared_ptr<OpenXMLElement> container = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>();
    if (!container)
    {
        container = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    }
    if (!container)
    {
        return false;
    }

    auto extent = container->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Extent>();
    if (!extent)
    {
        return false;
    }

    output.Width = ExyokiOffice::MeasuringUnits(static_cast<Real>(extent->GetCx().Value()),
                                                ExyokiOffice::MeasurementUnit::Emu);
    output.Height = ExyokiOffice::MeasuringUnits(static_cast<Real>(extent->GetCy().Value()),
                                                 ExyokiOffice::MeasurementUnit::Emu);
    return true;
}

Image& Image::SetWrap(ImageWrap wrap, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues wrapText)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return *this;
    }

    if (auto existing = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapNone>())
    {
        anchor->RemoveChild(existing);
    }
    if (auto existing = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapSquare>())
    {
        anchor->RemoveChild(existing);
    }
    if (auto existing = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTight>())
    {
        anchor->RemoveChild(existing);
    }
    if (auto existing = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapThrough>())
    {
        anchor->RemoveChild(existing);
    }
    if (auto existing = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTopBottom>())
    {
        anchor->RemoveChild(existing);
    }

    switch (wrap)
    {
        case ImageWrap::None:
            anchor->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapNone>();
            break;
        case ImageWrap::Square:
        {
            auto wrapSquare = anchor->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapSquare>();
            wrapSquare->SetWrapText(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues>(wrapText));
            break;
        }
        case ImageWrap::Tight:
        {
            auto wrapTight = anchor->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTight>();
            wrapTight->SetWrapText(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues>(wrapText));
            break;
        }
        case ImageWrap::Through:
        {
            auto wrapThrough = anchor->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapThrough>();
            wrapThrough->SetWrapText(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues>(wrapText));
            break;
        }
        case ImageWrap::TopAndBottom:
            anchor->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTopBottom>();
            break;
    }

    return *this;
}

bool Image::TryGetWrap(ImageWrapSettings& output) const
{
    if (!m_drawing)
    {
        return false;
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return false;
    }

    if (anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapNone>())
    {
        output.Wrap = ImageWrap::None;
        output.WrapText = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides;
        return true;
    }
    if (auto wrapSquare = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapSquare>())
    {
        output.Wrap = ImageWrap::Square;
        output.WrapText = wrapSquare->GetWrapText().ValueOr(
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
        return true;
    }
    if (auto wrapTight = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTight>())
    {
        output.Wrap = ImageWrap::Tight;
        output.WrapText = wrapTight->GetWrapText().ValueOr(
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
        return true;
    }
    if (auto wrapThrough = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapThrough>())
    {
        output.Wrap = ImageWrap::Through;
        output.WrapText = wrapThrough->GetWrapText().ValueOr(
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
        return true;
    }
    if (anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTopBottom>())
    {
        output.Wrap = ImageWrap::TopAndBottom;
        output.WrapText = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides;
        return true;
    }

    return false;
}

Image& Image::SetPosition(ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues horizontalFrom,
                          const ExyokiOffice::MeasuringUnits& horizontalOffset,
                          ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues verticalFrom,
                          const ExyokiOffice::MeasuringUnits& verticalOffset)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return *this;
    }

    const auto horizontalOffsetEmu = WordValueHelper::ToEmuInt64(horizontalOffset);
    const auto verticalOffsetEmu = WordValueHelper::ToEmuInt64(verticalOffset);

    auto positionH = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition>(anchor);
    if (positionH)
    {
        positionH->SetRelativeFrom(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues>(horizontalFrom));
        if (auto align = positionH->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignment>())
        {
            positionH->RemoveChild(align);
        }
        auto offset = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>(positionH);
        if (offset)
        {
            offset->SetText(std::to_string(horizontalOffsetEmu));
        }
    }

    auto positionV = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition>(anchor);
    if (positionV)
    {
        positionV->SetRelativeFrom(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues>(verticalFrom));
        if (auto align = positionV->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignment>())
        {
            positionV->RemoveChild(align);
        }
        auto offset = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition, ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>(positionV);
        if (offset)
        {
            offset->SetText(std::to_string(verticalOffsetEmu));
        }
    }

    return *this;
}

Image& Image::SetPositionAligned(
    ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues horizontalFrom,
    ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues horizontalAlign,
    ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues verticalFrom,
    ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues verticalAlign)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return *this;
    }

    auto positionH = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition>(anchor);
    if (positionH)
    {
        positionH->SetRelativeFrom(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues>(horizontalFrom));
        if (auto offset = positionH->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>())
        {
            positionH->RemoveChild(offset);
        }
        auto align = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignment>(positionH);
        if (align)
        {
            const auto text = EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues>(horizontalAlign).ToString();
            if (!text.empty())
            {
                align->SetText(text);
            }
        }
    }

    auto positionV = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition>(anchor);
    if (positionV)
    {
        positionV->SetRelativeFrom(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues>(verticalFrom));
        if (auto offset = positionV->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>())
        {
            positionV->RemoveChild(offset);
        }
        auto align = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignment>(positionV);
        if (align)
        {
            const auto text = EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues>(verticalAlign).ToString();
            if (!text.empty())
            {
                align->SetText(text);
            }
        }
    }

    return *this;
}

bool Image::TryGetPosition(ImagePosition& output) const
{
    if (!m_drawing)
    {
        return false;
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return false;
    }

    bool hasPosition = false;
    if (auto positionH = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalPosition>())
    {
        output.HorizontalFrom = positionH->GetRelativeFrom().ValueOr(
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::NotDefinedEnumValue);
        if (auto align = positionH->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignment>())
        {
            if (const auto parsed = WordValueHelper::TryParseEnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues>(align->GetText()))
            {
                output.HorizontalAlignment = *parsed;
            }
        }
        if (auto offset = positionH->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>())
        {
            const auto parsed = WordValueHelper::TryParseInt64(offset->GetText());
            if (parsed)
            {
                output.HorizontalOffset = ExyokiOffice::MeasuringUnits(static_cast<Real>(*parsed),
                                                                       ExyokiOffice::MeasurementUnit::Emu);
            }
        }
        hasPosition = true;
    }

    if (auto positionV = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalPosition>())
    {
        output.VerticalFrom = positionV->GetRelativeFrom().ValueOr(
            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::NotDefinedEnumValue);
        if (auto align = positionV->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignment>())
        {
            if (const auto parsed = WordValueHelper::TryParseEnumValue<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues>(align->GetText()))
            {
                output.VerticalAlignment = *parsed;
            }
        }
        if (auto offset = positionV->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::PositionOffset>())
        {
            const auto parsed = WordValueHelper::TryParseInt64(offset->GetText());
            if (parsed)
            {
                output.VerticalOffset = ExyokiOffice::MeasuringUnits(static_cast<Real>(*parsed),
                                                                     ExyokiOffice::MeasurementUnit::Emu);
            }
        }
        hasPosition = true;
    }

    return hasPosition;
}

Image& Image::SetDistanceFromText(const ExyokiOffice::MeasuringUnits& left,
                                  const ExyokiOffice::MeasuringUnits& top,
                                  const ExyokiOffice::MeasuringUnits& right,
                                  const ExyokiOffice::MeasuringUnits& bottom)
{
    if (!m_drawing)
    {
        return *this;
    }

    const auto leftEmu = WordValueHelper::ToEmuUInt32(left);
    const auto topEmu = WordValueHelper::ToEmuUInt32(top);
    const auto rightEmu = WordValueHelper::ToEmuUInt32(right);
    const auto bottomEmu = WordValueHelper::ToEmuUInt32(bottom);

    if (auto inlineDrawing = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>())
    {
        inlineDrawing->SetDistanceFromLeft(UInt32Value(leftEmu));
        inlineDrawing->SetDistanceFromTop(UInt32Value(topEmu));
        inlineDrawing->SetDistanceFromRight(UInt32Value(rightEmu));
        inlineDrawing->SetDistanceFromBottom(UInt32Value(bottomEmu));
    }
    if (auto anchorDrawing = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>())
    {
        anchorDrawing->SetDistanceFromLeft(UInt32Value(leftEmu));
        anchorDrawing->SetDistanceFromTop(UInt32Value(topEmu));
        anchorDrawing->SetDistanceFromRight(UInt32Value(rightEmu));
        anchorDrawing->SetDistanceFromBottom(UInt32Value(bottomEmu));
    }

    return *this;
}

bool Image::TryGetDistanceFromText(ImageDistanceFromText& output) const
{
    if (!m_drawing)
    {
        return false;
    }

    if (auto inlineDrawing = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Inline>())
    {
        output.Left = ExyokiOffice::MeasuringUnits(static_cast<Real>(inlineDrawing->GetDistanceFromLeft().ValueOr(0)),
                                                   ExyokiOffice::MeasurementUnit::Emu);
        output.Top = ExyokiOffice::MeasuringUnits(static_cast<Real>(inlineDrawing->GetDistanceFromTop().ValueOr(0)),
                                                  ExyokiOffice::MeasurementUnit::Emu);
        output.Right = ExyokiOffice::MeasuringUnits(static_cast<Real>(inlineDrawing->GetDistanceFromRight().ValueOr(0)),
                                                    ExyokiOffice::MeasurementUnit::Emu);
        output.Bottom = ExyokiOffice::MeasuringUnits(static_cast<Real>(inlineDrawing->GetDistanceFromBottom().ValueOr(0)),
                                                     ExyokiOffice::MeasurementUnit::Emu);
        return true;
    }

    if (auto anchorDrawing = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>())
    {
        output.Left = ExyokiOffice::MeasuringUnits(static_cast<Real>(anchorDrawing->GetDistanceFromLeft().ValueOr(0)),
                                                   ExyokiOffice::MeasurementUnit::Emu);
        output.Top = ExyokiOffice::MeasuringUnits(static_cast<Real>(anchorDrawing->GetDistanceFromTop().ValueOr(0)),
                                                  ExyokiOffice::MeasurementUnit::Emu);
        output.Right = ExyokiOffice::MeasuringUnits(static_cast<Real>(anchorDrawing->GetDistanceFromRight().ValueOr(0)),
                                                    ExyokiOffice::MeasurementUnit::Emu);
        output.Bottom = ExyokiOffice::MeasuringUnits(static_cast<Real>(anchorDrawing->GetDistanceFromBottom().ValueOr(0)),
                                                     ExyokiOffice::MeasurementUnit::Emu);
        return true;
    }

    return false;
}

Image& Image::SetBehindText(bool behindText)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (anchor)
    {
        anchor->SetBehindDoc(BooleanValue(behindText));
    }
    return *this;
}

Image& Image::SetAllowOverlap(bool allowOverlap)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (anchor)
    {
        anchor->SetAllowOverlap(BooleanValue(allowOverlap));
    }
    return *this;
}

Image& Image::SetAnchorLocked(bool locked)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (anchor)
    {
        anchor->SetLocked(BooleanValue(locked));
    }
    return *this;
}

Image& Image::SetRelativeHeight(UInt32 relativeHeight)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (anchor)
    {
        anchor->SetRelativeHeight(UInt32Value(relativeHeight));
    }
    return *this;
}

Image& Image::SetLayoutInCell(bool layoutInCell)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (anchor)
    {
        anchor->SetLayoutInCell(BooleanValue(layoutInCell));
    }
    return *this;
}

Image& Image::SetSimplePositionEnabled(bool enabled)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (anchor)
    {
        anchor->SetSimplePos(BooleanValue(enabled));
    }
    return *this;
}

Image& Image::SetSimplePosition(const ExyokiOffice::MeasuringUnits& x, const ExyokiOffice::MeasuringUnits& y)
{
    if (!m_drawing)
    {
        return *this;
    }

    if (GetLayout() != ImageLayout::Floating)
    {
        SetLayout(ImageLayout::Floating);
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return *this;
    }

    auto simplePos = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::SimplePosition>(anchor);
    if (simplePos)
    {
        simplePos->SetX(Int64Value(WordValueHelper::ToEmuInt64(x)));
        simplePos->SetY(Int64Value(WordValueHelper::ToEmuInt64(y)));
        anchor->SetSimplePos(BooleanValue(true));
    }

    return *this;
}

bool Image::TryGetAnchorOptions(ImageAnchorOptions& output) const
{
    if (!m_drawing)
    {
        return false;
    }

    auto anchor = m_drawing->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::Anchor>();
    if (!anchor)
    {
        return false;
    }

    output.BehindText = anchor->GetBehindDoc().ValueOr(false);
    output.AllowOverlap = anchor->GetAllowOverlap().ValueOr(true);
    output.AnchorLocked = anchor->GetLocked().ValueOr(false);
    output.LayoutInCell = anchor->GetLayoutInCell().ValueOr(true);
    output.SimplePositionEnabled = anchor->GetSimplePos().ValueOr(false);
    output.RelativeHeight = anchor->GetRelativeHeight().ValueOr(0);

    if (auto simplePos = anchor->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing::SimplePosition>())
    {
        output.SimplePosition = ImageSimplePosition{
            ExyokiOffice::MeasuringUnits(static_cast<Real>(simplePos->GetX().Value()), ExyokiOffice::MeasurementUnit::Emu),
            ExyokiOffice::MeasuringUnits(static_cast<Real>(simplePos->GetY().Value()), ExyokiOffice::MeasurementUnit::Emu)};
    }
    else
    {
        output.SimplePosition.reset();
    }

    return true;
}

Image& Image::AttachMainDocumentPart(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
{
    m_mainDocumentPart = mainDocumentPart;
    return *this;
}

Image& Image::SetCrop(Real left, Real top, Real right, Real bottom)
{
    auto blipFill = WordDrawingHelper::FindPictureBlipFill(m_drawing);
    if (!blipFill)
    {
        return *this;
    }

    auto srcRect = blipFill->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::SourceRectangle>();
    if (!srcRect)
    {
        std::shared_ptr<OpenXMLElement> before =
            blipFill->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Stretch>();
        if (!before)
        {
            before = blipFill->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Tile>();
        }
        srcRect = blipFill->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::SourceRectangle>(before);
    }
    if (!srcRect)
    {
        return *this;
    }

    const auto toPercentage = [](Real fraction)
    {
        return Int32Value(static_cast<Int32>(std::llround(fraction * 100000.0)));
    };
    srcRect->SetLeft(toPercentage(left));
    srcRect->SetTop(toPercentage(top));
    srcRect->SetRight(toPercentage(right));
    srcRect->SetBottom(toPercentage(bottom));
    return *this;
}

bool Image::TryGetCrop(ImageCrop& output) const
{
    auto blipFill = WordDrawingHelper::FindPictureBlipFill(m_drawing);
    if (!blipFill)
    {
        return false;
    }

    auto srcRect = blipFill->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::SourceRectangle>();
    if (!srcRect)
    {
        return false;
    }

    output.Left = static_cast<Real>(srcRect->GetLeft().ValueOr(0)) / 100000.0;
    output.Top = static_cast<Real>(srcRect->GetTop().ValueOr(0)) / 100000.0;
    output.Right = static_cast<Real>(srcRect->GetRight().ValueOr(0)) / 100000.0;
    output.Bottom = static_cast<Real>(srcRect->GetBottom().ValueOr(0)) / 100000.0;
    return true;
}

Image& Image::ClearCrop()
{
    auto blipFill = WordDrawingHelper::FindPictureBlipFill(m_drawing);
    if (!blipFill)
    {
        return *this;
    }

    if (auto srcRect = blipFill->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::SourceRectangle>())
    {
        blipFill->RemoveChild(srcRect);
    }
    return *this;
}

Image& Image::SetAltText(std::string_view title, std::string_view description)
{
    auto cNvPr = WordDrawingHelper::FindPictureNonVisualProperties(m_drawing);
    if (!cNvPr)
    {
        return *this;
    }

    cNvPr->SetTitle(StringValue(std::string(title)));
    cNvPr->SetDescription(StringValue(std::string(description)));
    return *this;
}

std::string Image::GetTitle() const
{
    auto cNvPr = WordDrawingHelper::FindPictureNonVisualProperties(m_drawing);
    return cNvPr ? cNvPr->GetTitle().ToString() : std::string();
}

std::string Image::GetDescription() const
{
    auto cNvPr = WordDrawingHelper::FindPictureNonVisualProperties(m_drawing);
    return cNvPr ? cNvPr->GetDescription().ToString() : std::string();
}

Image& Image::SetRotation(Real degrees)
{
    auto shapeProps = WordDrawingHelper::FindPictureShapeProperties(m_drawing);
    if (!shapeProps)
    {
        return *this;
    }

    auto transform = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D>(shapeProps);
    if (!transform)
    {
        return *this;
    }

    constexpr Int64 kFullTurn = static_cast<Int64>(360 * 60000);
    Int64 raw = static_cast<Int64>(std::llround(degrees * 60000.0)) % kFullTurn;
    if (raw < 0)
    {
        raw += kFullTurn;
    }
    transform->SetRotation(Int32Value(static_cast<Int32>(raw)));
    return *this;
}

Real Image::GetRotation() const
{
    auto shapeProps = WordDrawingHelper::FindPictureShapeProperties(m_drawing);
    auto transform = shapeProps
                         ? shapeProps->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D>()
                         : nullptr;
    if (!transform)
    {
        return 0.0;
    }
    return static_cast<Real>(transform->GetRotation().ValueOr(0)) / 60000.0;
}

Image& Image::SetFlip(bool horizontal, bool vertical)
{
    auto shapeProps = WordDrawingHelper::FindPictureShapeProperties(m_drawing);
    if (!shapeProps)
    {
        return *this;
    }

    auto transform = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Pictures::ShapeProperties,
                                                            ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D>(shapeProps);
    if (!transform)
    {
        return *this;
    }

    transform->SetHorizontalFlip(BooleanValue(horizontal));
    transform->SetVerticalFlip(BooleanValue(vertical));
    return *this;
}

bool Image::TryGetFlip(bool& horizontal, bool& vertical) const
{
    horizontal = false;
    vertical = false;

    auto shapeProps = WordDrawingHelper::FindPictureShapeProperties(m_drawing);
    auto transform = shapeProps
                         ? shapeProps->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::Transform2D>()
                         : nullptr;
    if (!transform)
    {
        return false;
    }

    horizontal = transform->GetHorizontalFlip().ValueOr(false);
    vertical = transform->GetVerticalFlip().ValueOr(false);
    return true;
}

Image& Image::SetHyperlink(std::string_view url, bool newWindow, std::string_view tooltip)
{
    if (!m_mainDocumentPart || url.empty())
    {
        return *this;
    }

    auto cNvPr = WordDrawingHelper::FindPictureNonVisualProperties(m_drawing);
    if (!cNvPr)
    {
        return *this;
    }

    if (auto existing = cNvPr->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::HyperlinkOnClick>())
    {
        const auto existingId = existing->GetId().ToString();
        if (!existingId.empty())
        {
            m_mainDocumentPart->RemoveExternalRelationship(existingId);
        }
        cNvPr->RemoveChild(existing);
    }

    const auto relationshipId = m_mainDocumentPart->AddExternalRelationship(kHyperlinkRelationshipType, std::string(url));
    if (relationshipId.empty())
    {
        return *this;
    }

    auto hyperlink = cNvPr->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Drawing::HyperlinkOnClick>();
    if (!hyperlink)
    {
        return *this;
    }

    hyperlink->SetId(StringValue(relationshipId));
    if (!tooltip.empty())
    {
        hyperlink->SetTooltip(StringValue(std::string(tooltip)));
    }
    if (newWindow)
    {
        hyperlink->SetTargetFrame(StringValue("_blank"));
    }
    return *this;
}

bool Image::TryGetHyperlink(ImageHyperlink& output) const
{
    auto cNvPr = WordDrawingHelper::FindPictureNonVisualProperties(m_drawing);
    if (!cNvPr)
    {
        return false;
    }

    auto hyperlink = cNvPr->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::HyperlinkOnClick>();
    if (!hyperlink)
    {
        return false;
    }

    output = ImageHyperlink{};
    output.Tooltip = hyperlink->GetTooltip().ToString();
    output.NewWindow = hyperlink->GetTargetFrame().ToString() == "_blank";

    const auto relationshipId = hyperlink->GetId().ToString();
    if (!relationshipId.empty() && m_mainDocumentPart)
    {
        // Hyperlinks are external relationships; RelationshipsByType() only returns
        // internal part-to-part relationships, so the full relationship list is searched here.
        for (const auto& relationship : m_mainDocumentPart->Relationships())
        {
            if (relationship.IsExternal && relationship.Type == kHyperlinkRelationshipType &&
                relationship.Id == relationshipId)
            {
                output.Url = relationship.Target;
                break;
            }
        }
    }
    return true;
}

Image& Image::RemoveHyperlink()
{
    auto cNvPr = WordDrawingHelper::FindPictureNonVisualProperties(m_drawing);
    if (!cNvPr)
    {
        return *this;
    }

    auto hyperlink = cNvPr->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Drawing::HyperlinkOnClick>();
    if (!hyperlink)
    {
        return *this;
    }

    if (m_mainDocumentPart)
    {
        const auto relationshipId = hyperlink->GetId().ToString();
        if (!relationshipId.empty())
        {
            m_mainDocumentPart->RemoveExternalRelationship(relationshipId);
        }
    }
    cNvPr->RemoveChild(hyperlink);
    return *this;
}

Table::Table(const std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>& table)
    : m_table(table)
{
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table> Table::GetLowLevelApi() const
{
    return m_table;
}

Size Table::GetRowCount() const
{
    if (!m_table)
    {
        return 0;
    }
    return m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>().size();
}

Size Table::GetColumnCount() const
{
    return GetLogicalColumnCount();
}

Size Table::GetLogicalColumnCount() const
{
    Size maxColumns = 0;
    for (const auto& row : GetLogicalGrid())
    {
        maxColumns = std::max(maxColumns, row.size());
    }
    return maxColumns;
}

std::vector<std::vector<TableGridCell>> Table::GetLogicalGrid() const
{
    std::vector<std::vector<TableGridCell>> grid;
    if (!m_table)
    {
        return grid;
    }

    struct ActiveVerticalMerge
    {
        Size OriginRow = 0;
        Size OriginColumn = 0;
        std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell> Cell;
    };

    std::vector<ActiveVerticalMerge> active;
    const auto rows = m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    grid.resize(rows.size());

    for (Size rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        Size logicalColumn = 0;
        for (const auto& cell : rows[rowIndex]->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>())
        {
            const auto columnSpan = static_cast<Size>(WordTableHelper::TableCellGridSpan(cell));
            const auto mergeValue = WordTableHelper::TableCellVerticalMergeValue(cell);
            const bool continuesVertical = mergeValue == "continue";

            if (active.size() < logicalColumn + columnSpan)
            {
                active.resize(logicalColumn + columnSpan);
            }
            if (grid[rowIndex].size() < logicalColumn + columnSpan)
            {
                grid[rowIndex].resize(logicalColumn + columnSpan);
            }

            Size originRow = rowIndex;
            Size originColumn = logicalColumn;
            auto originCell = cell;
            if (continuesVertical && logicalColumn < active.size() && active[logicalColumn].Cell)
            {
                originRow = active[logicalColumn].OriginRow;
                originColumn = active[logicalColumn].OriginColumn;
                originCell = active[logicalColumn].Cell;
            }

            for (Size offset = 0; offset < columnSpan; ++offset)
            {
                const auto column = logicalColumn + offset;
                auto& slot = grid[rowIndex][column];
                slot.Row = rowIndex;
                slot.Column = column;
                slot.IsOrigin = rowIndex == originRow && column == originColumn;
                slot.OriginRow = originRow;
                slot.OriginColumn = originColumn;
                slot.RowSpan = 1;
                slot.ColumnSpan = columnSpan;
                slot.Cell = originCell;

                if (mergeValue == "restart")
                {
                    active[column] = {rowIndex, logicalColumn, cell};
                }
                else if (!continuesVertical)
                {
                    active[column] = {};
                }
            }

            logicalColumn += columnSpan;
        }
    }

    for (auto& row : grid)
    {
        for (auto& slot : row)
        {
            if (!slot.Cell)
            {
                continue;
            }

            Size rowSpan = 0;
            Size columnSpan = 0;
            for (const auto& scanRow : grid)
            {
                for (const auto& scan : scanRow)
                {
                    if (scan.Cell == slot.Cell && scan.OriginRow == slot.OriginRow && scan.OriginColumn == slot.OriginColumn)
                    {
                        rowSpan = std::max(rowSpan, scan.Row + 1 - slot.OriginRow);
                        columnSpan = std::max(columnSpan, scan.Column + 1 - slot.OriginColumn);
                    }
                }
            }
            slot.RowSpan = std::max<Size>(1, rowSpan);
            slot.ColumnSpan = std::max<Size>(1, columnSpan);
        }
    }

    return grid;
}

std::vector<std::shared_ptr<Paragraph>> Table::Paragraphs() const
{
    std::vector<std::shared_ptr<Paragraph>> paragraphs;
    if (!m_table)
    {
        return paragraphs;
    }

    for (const auto& paragraph : m_table->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
    {
        if (paragraph)
        {
            paragraphs.push_back(std::make_shared<Paragraph>(paragraph));
        }
    }
    return paragraphs;
}

std::vector<std::shared_ptr<Table>> Table::Tables() const
{
    std::vector<std::shared_ptr<Table>> tables;
    if (!m_table)
    {
        return tables;
    }

    for (const auto& table : m_table->Descendants<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>())
    {
        if (table && table != m_table)
        {
            tables.push_back(std::make_shared<Table>(table));
        }
    }
    return tables;
}

Table& Table::SetWidth(const ExyokiOffice::MeasuringUnits& width)
{
    auto props = WordPropertiesElementHelper::EnsureTableProperties(m_table);
    auto tableWidth = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidth>(props);
    if (tableWidth)
    {
        tableWidth->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(width))));
        tableWidth->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    return *this;
}

Table& Table::SetAlignment(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowAlignmentValues alignment)
{
    auto props = WordPropertiesElementHelper::EnsureTableProperties(m_table);
    auto justification = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableJustification>(props);
    if (justification)
    {
        justification->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowAlignmentValues>(alignment));
    }
    return *this;
}

Table& Table::SetBorders(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                         UInt32 size,
                         const ExyokiOffice::Color& color)
{
    auto props = WordPropertiesElementHelper::EnsureTableProperties(m_table);
    auto borders = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders>(props);
    if (!borders)
    {
        return *this;
    }
    const auto colorValue = color.ToHexString();

    auto applyBorder = [&](auto border)
    {
        if (!border)
        {
            return;
        }
        border->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues>(style));
        border->SetSize(UInt32Value(size));
        border->SetColor(StringValue(colorValue));
        border->SetSpace(UInt32Value(0));
    };

    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TopBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BottomBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LeftBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RightBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::InsideHorizontalBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableBorders, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::InsideVerticalBorder>(borders));

    return *this;
}

Table& Table::SetBorders(ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                         const ExyokiOffice::MeasuringUnits& size,
                         const ExyokiOffice::Color& color)
{
    return SetBorders(style, WordValueHelper::ToBorderSizeUInt32(size), color);
}

Table& Table::SetDefaultCellMargins(const ExyokiOffice::MeasuringUnits& left,
                                    const ExyokiOffice::MeasuringUnits& top,
                                    const ExyokiOffice::MeasuringUnits& right,
                                    const ExyokiOffice::MeasuringUnits& bottom)
{
    auto props = WordPropertiesElementHelper::EnsureTableProperties(m_table);
    auto margins = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMarginDefault>(props);
    if (!margins)
    {
        return *this;
    }

    auto leftMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMarginDefault, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LeftMargin>(margins);
    auto rightMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMarginDefault, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RightMargin>(margins);
    auto topMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMarginDefault, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TopMargin>(margins);
    auto bottomMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMarginDefault, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BottomMargin>(margins);

    if (leftMargin)
    {
        leftMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(left))));
        leftMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    if (rightMargin)
    {
        rightMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(right))));
        rightMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    if (topMargin)
    {
        topMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(top))));
        topMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    if (bottomMargin)
    {
        bottomMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(bottom))));
        bottomMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }

    return *this;
}

Table& Table::AddRow(Size columns)
{
    if (!m_table)
    {
        return *this;
    }

    const auto columnCount = columns == 0 ? std::max<Size>(1, GetLogicalColumnCount()) : columns;
    auto row = m_table->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    if (!row)
    {
        return *this;
    }

    for (Size column = 0; column < columnCount; ++column)
    {
        WordTableHelper::AppendEmptyTableCell(row);
    }

    WordTableHelper::EnsureTableGridColumnCount(m_table, std::max(GetLogicalColumnCount(), columnCount));
    return *this;
}

Table& Table::AddColumn()
{
    return InsertColumn(GetLogicalColumnCount());
}

Table& Table::InsertRow(Size row)
{
    if (!m_table)
    {
        return *this;
    }

    SplitAllCells();
    const auto rows = m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    const auto columnCount = std::max<Size>(1, GetLogicalColumnCount());
    const auto before = row < rows.size() ? std::static_pointer_cast<ExyokiOffice::OpenXMLElement>(rows[row]) : nullptr;
    auto inserted = m_table->InsertChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>(before);
    if (!inserted)
    {
        return *this;
    }

    for (Size column = 0; column < columnCount; ++column)
    {
        WordTableHelper::AppendEmptyTableCell(inserted);
    }
    WordTableHelper::EnsureTableGridColumnCount(m_table, columnCount);
    return *this;
}

Table& Table::RemoveRow(Size row)
{
    if (!m_table)
    {
        return *this;
    }

    SplitAllCells();
    const auto rows = m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    if (row < rows.size())
    {
        m_table->RemoveChild(rows[row]);
    }
    WordTableHelper::EnsureTableGridColumnCount(m_table, GetLogicalColumnCount());
    return *this;
}

Table& Table::InsertColumn(Size column)
{
    if (!m_table)
    {
        return *this;
    }

    SplitAllCells();
    auto rows = m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    if (rows.empty())
    {
        AddRow(1);
        rows = m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    }

    for (const auto& row : rows)
    {
        auto cells = row ? row->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>() : std::vector<std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>>{};
        const auto before = column < cells.size() ? std::static_pointer_cast<ExyokiOffice::OpenXMLElement>(cells[column]) : nullptr;
        WordTableHelper::InsertEmptyTableCell(row, before);
    }

    WordTableHelper::EnsureTableGridColumnCount(m_table, GetLogicalColumnCount());
    return *this;
}

Table& Table::RemoveColumn(Size column)
{
    if (!m_table)
    {
        return *this;
    }

    SplitAllCells();
    for (const auto& row : m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>())
    {
        auto cells = row ? row->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>() : std::vector<std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>>{};
        if (column < cells.size())
        {
            row->RemoveChild(cells[column]);
        }
    }

    WordTableHelper::EnsureTableGridColumnCount(m_table, GetLogicalColumnCount());
    return *this;
}

Table& Table::SetCellText(Size row, Size column, std::string_view text, bool preserveSpaces)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    if (!cell)
    {
        return *this;
    }

    WordTableHelper::RemoveTableCellBlockContent(cell);
    auto paragraph = cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    if (!paragraph)
    {
        return *this;
    }

    WordTableHelper::AppendTextRunToTableCellParagraph(paragraph, text, preserveSpaces);
    return *this;
}

Table& Table::AppendCellText(Size row, Size column, std::string_view text, bool preserveSpaces)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    if (!cell)
    {
        return *this;
    }

    auto paragraphs = cell->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    auto paragraph = paragraphs.empty()
                         ? cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>()
                         : paragraphs.front();
    if (!paragraph)
    {
        return *this;
    }

    WordTableHelper::AppendTextRunToTableCellParagraph(paragraph, text, preserveSpaces);
    return *this;
}

Table& Table::SetCellMargins(Size row,
                             Size column,
                             const ExyokiOffice::MeasuringUnits& left,
                             const ExyokiOffice::MeasuringUnits& top,
                             const ExyokiOffice::MeasuringUnits& right,
                             const ExyokiOffice::MeasuringUnits& bottom)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
    auto margins = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMargin>(props);
    if (!margins)
    {
        return *this;
    }

    auto leftMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMargin, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LeftMargin>(margins);
    auto rightMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMargin, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RightMargin>(margins);
    auto topMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMargin, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TopMargin>(margins);
    auto bottomMargin = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellMargin, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BottomMargin>(margins);

    if (leftMargin)
    {
        leftMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(left))));
        leftMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    if (rightMargin)
    {
        rightMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(right))));
        rightMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    if (topMargin)
    {
        topMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(top))));
        topMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    if (bottomMargin)
    {
        bottomMargin->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(bottom))));
        bottomMargin->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }

    return *this;
}

Table& Table::SetCellHorizontalAlignment(Size row,
                                         Size column,
                                         ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::JustificationValues alignment)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    if (!cell)
    {
        return *this;
    }

    auto paragraphs = cell->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    auto paragraph = paragraphs.empty() ? cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>() : paragraphs.front();
    if (!paragraph)
    {
        return *this;
    }

    auto props = WordPropertiesElementHelper::EnsureParagraphProperties(paragraph);
    auto justification = WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                                                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Justification>(props);
    if (justification)
    {
        justification->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::JustificationValues>(alignment));
    }
    return *this;
}

Table& Table::SetCellBackgroundColor(Size row, Size column, const ExyokiOffice::Color& color)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
    auto shading = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties,
                                                          ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Shading>(props);
    if (shading)
    {
        shading->SetFill(StringValue(color.ToHexString()));
        shading->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues::Clear));
    }
    return *this;
}

Table& Table::SetCellVerticalAlignment(Size row,
                                       Size column,
                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableVerticalAlignmentValues alignment)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
    auto vAlign = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellVerticalAlignment>(props);
    if (vAlign)
    {
        vAlign->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableVerticalAlignmentValues>(alignment));
    }
    return *this;
}

Table& Table::SetCellWidth(Size row, Size column, const ExyokiOffice::MeasuringUnits& width)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
    auto widthElement = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties, ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellWidth>(props);
    if (widthElement)
    {
        widthElement->SetWidth(StringValue(std::to_string(WordValueHelper::ToTwipsInt(width))));
        widthElement->SetType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues>(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues::Dxa));
    }
    return *this;
}

Table& Table::SetCellBorders(Size row,
                             Size column,
                             ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                             UInt32 size,
                             const ExyokiOffice::Color& color)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    auto props = WordPropertiesElementHelper::EnsureTableCellProperties(cell);
    auto borders = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellProperties,
                                                          ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders>(props);
    if (!borders)
    {
        return *this;
    }
    const auto colorValue = color.ToHexString();

    auto applyBorder = [&](auto border)
    {
        if (!border)
        {
            return;
        }
        border->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues>(style));
        border->SetSize(UInt32Value(size));
        border->SetColor(StringValue(colorValue));
        border->SetSpace(UInt32Value(0));
    };

    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TopBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BottomBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::LeftBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RightBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::InsideHorizontalBorder>(borders));
    applyBorder(WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCellBorders,
                                                       ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::InsideVerticalBorder>(borders));

    return *this;
}
Table& Table::SetCellBorders(Size row,
                             Size column,
                             ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                             const ExyokiOffice::MeasuringUnits& size,
                             const ExyokiOffice::Color& color)
{
    return SetCellBorders(row, column, style, WordValueHelper::ToBorderSizeUInt32(size), color);
}

Table& Table::SetRowHeight(Size row,
                           const ExyokiOffice::MeasuringUnits& height,
                           ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeightRuleValues rule)
{
    auto rowElement = WordPropertiesElementHelper::EnsureTableRow(m_table, row);
    auto props = WordPropertiesElementHelper::EnsureTableRowProperties(rowElement);
    auto heightElement = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowProperties,
                                                                ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowHeight>(props);
    if (heightElement)
    {
        const auto twips = std::max(0, WordValueHelper::ToTwipsInt(height));
        heightElement->SetVal(UInt32Value(static_cast<UInt32>(twips)));
        heightElement->SetHeightType(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeightRuleValues>(rule));
    }
    return *this;
}

Table& Table::SetRowCantSplit(Size row, bool cantSplit)
{
    auto rowElement = WordPropertiesElementHelper::EnsureTableRow(m_table, row);
    auto props = WordPropertiesElementHelper::EnsureTableRowProperties(rowElement);
    auto element = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowProperties,
                                                          ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::CantSplit>(props);
    if (element)
    {
        element->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::OnOffOnlyValues>(
            cantSplit ? ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::OnOffOnlyValues::on
                      : ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::OnOffOnlyValues::off));
    }
    return *this;
}

Table& Table::SetRowHeader(Size row, bool isHeader)
{
    auto rowElement = WordPropertiesElementHelper::EnsureTableRow(m_table, row);
    auto props = WordPropertiesElementHelper::EnsureTableRowProperties(rowElement);
    auto element = WordStructureHelper::EnsureChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRowProperties,
                                                          ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableHeader>(props);
    if (element)
    {
        element->SetVal(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::OnOffOnlyValues>(
            isHeader ? ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::OnOffOnlyValues::on
                     : ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::OnOffOnlyValues::off));
    }
    return *this;
}

Table& Table::SetColumnWidth(Size column, const ExyokiOffice::MeasuringUnits& width)
{
    if (!m_table)
    {
        return *this;
    }

    auto grid = WordPropertiesElementHelper::EnsureTableGrid(m_table);
    if (!grid)
    {
        return *this;
    }

    auto columns = grid->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridColumn>();
    if (column < columns.size())
    {
        columns[column]->SetWidth(StringValue(std::to_string(std::max(0, WordValueHelper::ToTwipsInt(width)))));
        return *this;
    }

    for (Size i = columns.size(); i <= column; ++i)
    {
        auto gridCol = grid->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::GridColumn>();
        if (!gridCol)
        {
            return *this;
        }
        if (i == column)
        {
            gridCol->SetWidth(StringValue(std::to_string(std::max(0, WordValueHelper::ToTwipsInt(width)))));
        }
    }

    return *this;
}

Table& Table::MergeCells(Size row, Size column, Size rowSpan, Size columnSpan)
{
    if (rowSpan == 0 || columnSpan == 0)
    {
        return *this;
    }

    if (!m_table)
    {
        return *this;
    }

    SplitAllCells();
    for (Size rowIndex = row; rowIndex < row + rowSpan; ++rowIndex)
    {
        for (Size col = 0; col < column + columnSpan; ++col)
        {
            WordTableHelper::EnsureLogicalTableCell(m_table, rowIndex, col);
        }
    }
    WordTableHelper::EnsureTableGridColumnCount(m_table, std::max(GetLogicalColumnCount(), column + columnSpan));

    const auto rows = m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>();
    const auto endRow = std::min(row + rowSpan, rows.size());
    for (Size rowIndex = row; rowIndex < endRow; ++rowIndex)
    {
        auto rowElement = rows[rowIndex];
        auto cells = rowElement ? rowElement->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>() : std::vector<std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>>{};
        if (column >= cells.size())
        {
            continue;
        }

        auto origin = cells[column];
        WordTableHelper::SetTableCellGridSpan(origin, columnSpan);
        WordTableHelper::SetTableCellVerticalMerge(
            origin,
            rowSpan > 1
                ? std::optional<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::MergedCellValues>(
                      rowIndex == row
                          ? ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::MergedCellValues::Restart
                          : ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::MergedCellValues::Continue)
                : std::nullopt);

        const auto removeEnd = std::min(column + columnSpan, cells.size());
        for (Size physical = removeEnd; physical-- > column + 1;)
        {
            rowElement->RemoveChild(cells[physical]);
        }
    }

    return *this;
}

Table& Table::SplitCell(Size row, Size column)
{
    const auto grid = GetLogicalGrid();
    if (row >= grid.size() || column >= grid[row].size())
    {
        return *this;
    }

    const auto target = grid[row][column];
    if (target.RowSpan <= 1 && target.ColumnSpan <= 1)
    {
        WordTableHelper::RemoveTableCellMergeMarkup(target.Cell);
        return *this;
    }

    SplitAllCells();
    return *this;
}

Table& Table::SplitAllCells()
{
    if (!m_table)
    {
        return *this;
    }

    for (const auto& row : m_table->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableRow>())
    {
        if (!row)
        {
            continue;
        }

        auto cells = row->Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::TableCell>();
        for (Size i = 0; i < cells.size(); ++i)
        {
            const auto cell = cells[i];
            const auto span = static_cast<Size>(WordTableHelper::TableCellGridSpan(cell));
            auto before = cell ? cell->NextSibling() : nullptr;
            WordTableHelper::RemoveTableCellMergeMarkup(cell);
            for (Size extra = 1; extra < span; ++extra)
            {
                WordTableHelper::InsertEmptyTableCell(row, before);
            }
        }
    }

    WordTableHelper::EnsureTableGridColumnCount(m_table, GetLogicalColumnCount());
    return *this;
}

std::shared_ptr<Table> Table::AddNestedTable(Size row,
                                             Size column,
                                             Size rows,
                                             Size columns)
{
    auto cell = WordTableHelper::EnsureLogicalTableCell(m_table, row, column);
    if (!cell)
    {
        return nullptr;
    }

    auto nested = cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>();
    if (!nested)
    {
        return nullptr;
    }

    auto wrapper = std::make_shared<Table>(nested);
    for (Size rowIndex = 0; rowIndex < rows; ++rowIndex)
    {
        wrapper->AddRow(columns);
    }

    cell->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    return wrapper;
}

HeaderFooterContent::HeaderFooterContent(const std::shared_ptr<Packaging::HeaderPart>& part)
    : m_headerPart(part)
{
}

HeaderFooterContent::HeaderFooterContent(const std::shared_ptr<Packaging::FooterPart>& part)
    : m_footerPart(part)
{
}

bool HeaderFooterContent::IsHeader() const
{
    return m_headerPart != nullptr;
}

bool HeaderFooterContent::IsFooter() const
{
    return m_footerPart != nullptr;
}

std::shared_ptr<Packaging::HeaderPart> HeaderFooterContent::GetHeaderPart() const
{
    return m_headerPart;
}

std::shared_ptr<Packaging::FooterPart> HeaderFooterContent::GetFooterPart() const
{
    return m_footerPart;
}

std::string HeaderFooterContent::RelationshipId() const
{
    if (m_headerPart)
    {
        return m_headerPart->RelationshipId();
    }
    if (m_footerPart)
    {
        return m_footerPart->RelationshipId();
    }
    return {};
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Header>
HeaderFooterContent::HeaderRoot() const
{
    return m_headerPart ? m_headerPart->GetHeader() : nullptr;
}

std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Footer>
HeaderFooterContent::FooterRoot() const
{
    return m_footerPart ? m_footerPart->GetFooter() : nullptr;
}

std::vector<std::shared_ptr<Paragraph>> HeaderFooterContent::Paragraphs() const
{
    std::vector<std::shared_ptr<Paragraph>> paragraphs;
    auto appendParagraphs = [&paragraphs](const auto& root)
    {
        if (!root)
        {
            return;
        }
        for (const auto& paragraph :
             root->template Elements<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>())
        {
            if (paragraph)
            {
                paragraphs.push_back(std::make_shared<Paragraph>(paragraph));
            }
        }
    };

    appendParagraphs(HeaderRoot());
    appendParagraphs(FooterRoot());
    return paragraphs;
}

std::shared_ptr<Paragraph> HeaderFooterContent::AddParagraph(std::string_view text, bool preserveSpaces)
{
    std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph> paragraph;
    if (auto header = HeaderRoot())
    {
        paragraph = header->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    }
    else if (auto footer = FooterRoot())
    {
        paragraph = footer->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>();
    }

    if (!paragraph)
    {
        return nullptr;
    }

    auto wrapper = std::make_shared<Paragraph>(paragraph);
    if (!text.empty())
    {
        wrapper->AddText(text, preserveSpaces);
    }
    return wrapper;
}

std::string HeaderFooterContent::PlainText() const
{
    std::string text;
    for (const auto& paragraph : Paragraphs())
    {
        if (!paragraph)
        {
            continue;
        }
        text += paragraph->PlainText();
    }
    return text;
}

HeaderFooterContent& HeaderFooterContent::Clear()
{
    auto clearRoot = [](const auto& root)
    {
        if (!root)
        {
            return;
        }
        auto children = root->Children();
        for (const auto& child : children)
        {
            if (child)
            {
                root->RemoveChild(child);
            }
        }
    };

    clearRoot(HeaderRoot());
    clearRoot(FooterRoot());
    return *this;
}

HeaderFooterContent& HeaderFooterContent::SetText(std::string_view text, bool preserveSpaces)
{
    Clear();
    AddParagraph(text, preserveSpaces);
    return *this;
}

Section::Section(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& sectionProperties,
                 const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_sectionProperties(sectionProperties), m_mainDocumentPart(mainDocumentPart)
{
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> Section::GetLowLevelApi() const
{
    return m_sectionProperties;
}

bool Section::IsFinalBodySection() const
{
    if (!m_sectionProperties)
    {
        return false;
    }

    return std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body>(
               m_sectionProperties->Parent()) != nullptr;
}

std::optional<SectionStartType> Section::GetStartType() const
{
    if (!m_sectionProperties)
    {
        return std::nullopt;
    }

    auto type = m_sectionProperties
                    ->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionType>();
    if (type)
    {
        auto value = type->GetVal();
        if (value.IsDefined())
        {
            if (auto converted = WordValueHelper::FromDomSectionMark(value.Value()))
            {
                return converted;
            }
        }
    }

    auto typeElement = WordBodyHelper::FindFirstChildByName(m_sectionProperties,
                                                            ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "type"));
    if (!typeElement)
    {
        return std::nullopt;
    }

    EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionMarkValues> rawValue;
    if (!typeElement->TryGetAttributeValue(
            ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "val"),
            rawValue) ||
        !rawValue.IsDefined())
    {
        return std::nullopt;
    }
    return WordValueHelper::FromDomSectionMark(rawValue.Value());
}

Section& Section::SetStartType(SectionStartType startType)
{
    if (!m_sectionProperties)
    {
        return *this;
    }

    const auto value =
        EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionMarkValues>(
            WordValueHelper::ToDomSectionMark(startType));
    if (auto existingType = WordBodyHelper::FindFirstChildByName(m_sectionProperties,
                                                                 ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "type")))
    {
        existingType->SetAttributeValue(ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "val"), value);
        return *this;
    }

    auto type =
        WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionType>(
            m_sectionProperties);
    if (type)
    {
        type->SetVal(value);
    }
    return *this;
}

std::optional<SectionPageSize> Section::GetPageSize() const
{
    if (!m_sectionProperties)
    {
        return std::nullopt;
    }

    auto pageSize =
        m_sectionProperties
            ->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageSize>();
    if (!pageSize)
    {
        return std::nullopt;
    }

    const auto width = WordValueHelper::GetDefinedTwips(pageSize->GetWidth());
    const auto height = WordValueHelper::GetDefinedTwips(pageSize->GetHeight());
    if (!width || !height)
    {
        return std::nullopt;
    }

    SectionPageSize result;
    result.Width = *width;
    result.Height = *height;
    if (auto orientation = WordValueHelper::FromDomPageOrientation(pageSize->GetOrient().ValueOr(
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues::Portrait)))
    {
        result.Orientation = *orientation;
    }
    return result;
}

Section& Section::SetPageSize(const SectionPageSize& pageSize)
{
    if (!m_sectionProperties)
    {
        return *this;
    }

    auto element =
        WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageSize>(
            m_sectionProperties);
    if (element)
    {
        element->SetWidth(UInt32Value(WordValueHelper::ToTwipsUInt32(pageSize.Width)));
        element->SetHeight(UInt32Value(WordValueHelper::ToTwipsUInt32(pageSize.Height)));
        element->SetOrient(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues>(
            WordValueHelper::ToDomPageOrientation(pageSize.Orientation)));
    }
    return *this;
}

Section& Section::SetPageOrientation(PageOrientation orientation)
{
    if (!m_sectionProperties)
    {
        return *this;
    }

    auto element =
        WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageSize>(
            m_sectionProperties);
    if (element)
    {
        element->SetOrient(EnumValue<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageOrientationValues>(
            WordValueHelper::ToDomPageOrientation(orientation)));
    }
    return *this;
}

std::optional<SectionMargins> Section::GetMargins() const
{
    if (!m_sectionProperties)
    {
        return std::nullopt;
    }

    auto pageMargin =
        m_sectionProperties
            ->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageMargin>();
    if (!pageMargin)
    {
        return std::nullopt;
    }

    SectionMargins result;
    result.Top = WordValueHelper::GetDefinedTwips(pageMargin->GetTop()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Right = WordValueHelper::GetDefinedTwips(pageMargin->GetRight()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Bottom = WordValueHelper::GetDefinedTwips(pageMargin->GetBottom()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Left = WordValueHelper::GetDefinedTwips(pageMargin->GetLeft()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Header = WordValueHelper::GetDefinedTwips(pageMargin->GetHeader()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Footer = WordValueHelper::GetDefinedTwips(pageMargin->GetFooter()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Gutter = WordValueHelper::GetDefinedTwips(pageMargin->GetGutter()).value_or(ExyokiOffice::MeasuringUnits{});
    return result;
}

Section& Section::SetMargins(const SectionMargins& margins)
{
    if (!m_sectionProperties)
    {
        return *this;
    }

    auto pageMargin =
        WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::PageMargin>(
            m_sectionProperties);
    if (pageMargin)
    {
        pageMargin->SetTop(Int32Value(WordValueHelper::ToTwipsInt(margins.Top)));
        pageMargin->SetRight(UInt32Value(WordValueHelper::ToTwipsUInt32(margins.Right)));
        pageMargin->SetBottom(Int32Value(WordValueHelper::ToTwipsInt(margins.Bottom)));
        pageMargin->SetLeft(UInt32Value(WordValueHelper::ToTwipsUInt32(margins.Left)));
        pageMargin->SetHeader(UInt32Value(WordValueHelper::ToTwipsUInt32(margins.Header)));
        pageMargin->SetFooter(UInt32Value(WordValueHelper::ToTwipsUInt32(margins.Footer)));
        pageMargin->SetGutter(UInt32Value(WordValueHelper::ToTwipsUInt32(margins.Gutter)));
    }
    return *this;
}

std::optional<SectionColumns> Section::GetColumns() const
{
    if (!m_sectionProperties)
    {
        return std::nullopt;
    }

    auto columns =
        m_sectionProperties
            ->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Columns>();
    if (!columns)
    {
        return std::nullopt;
    }

    SectionColumns result;
    const auto count = columns->GetColumnCount();
    if (count.IsDefined() && count.Value() > 0)
    {
        result.Count = static_cast<UInt16>(count.Value());
    }
    result.Spacing = WordValueHelper::GetDefinedTwips(columns->GetSpace()).value_or(ExyokiOffice::MeasuringUnits{});
    result.Separator = columns->GetSeparator().ValueOr(false);
    return result;
}

Section& Section::SetColumns(const SectionColumns& columns)
{
    if (!m_sectionProperties)
    {
        return *this;
    }

    auto element =
        WordStructureHelper::EnsureChildOfType<ExyokiOffice::OpenXMLElement,
                                               ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Columns>(
            m_sectionProperties);
    if (element)
    {
        const auto clampedCount = std::max<UInt16>(1, columns.Count);
        element->SetEqualWidth(OnOffValue(true));
        element->SetColumnCount(Int16Value(static_cast<Int16>(
            std::min<UInt16>(clampedCount, static_cast<UInt16>(std::numeric_limits<Int16>::max())))));
        element->SetSpace(StringValue(std::to_string(WordValueHelper::ToTwipsUInt32(columns.Spacing))));
        element->SetSeparator(OnOffValue(columns.Separator));
    }
    return *this;
}

bool Section::HasHeader(HeaderFooterType type) const
{
    return WordHeaderFooterHelper::FindHeaderFooterReference(m_sectionProperties, true, type) != nullptr;
}

bool Section::HasFooter(HeaderFooterType type) const
{
    return WordHeaderFooterHelper::FindHeaderFooterReference(m_sectionProperties, false, type) != nullptr;
}

std::shared_ptr<HeaderFooterContent> Section::GetHeader(HeaderFooterType type) const
{
    auto reference = WordHeaderFooterHelper::FindHeaderFooterReference(m_sectionProperties, true, type);
    auto part = WordHeaderFooterHelper::FindHeaderPartByRelationshipId(m_mainDocumentPart, WordHeaderFooterHelper::GetRelationshipId(reference));
    return part ? std::make_shared<HeaderFooterContent>(part) : nullptr;
}

std::shared_ptr<HeaderFooterContent> Section::GetFooter(HeaderFooterType type) const
{
    auto reference = WordHeaderFooterHelper::FindHeaderFooterReference(m_sectionProperties, false, type);
    auto part = WordHeaderFooterHelper::FindFooterPartByRelationshipId(m_mainDocumentPart, WordHeaderFooterHelper::GetRelationshipId(reference));
    return part ? std::make_shared<HeaderFooterContent>(part) : nullptr;
}

std::shared_ptr<HeaderFooterContent> Section::EnsureHeader(HeaderFooterType type)
{
    if (!m_sectionProperties || !m_mainDocumentPart)
    {
        return nullptr;
    }

    if (auto existing = GetHeader(type))
    {
        return existing;
    }

    auto part = m_mainDocumentPart->AddHeaderPart();
    if (!part || part->RelationshipId().empty())
    {
        return nullptr;
    }
    const std::string relationshipId = part->RelationshipId();

    if (!WordHeaderFooterHelper::AppendHeaderFooterReference<
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::HeaderReference>(
            m_sectionProperties,
            type,
            relationshipId))
    {
        m_mainDocumentPart->RemoveHeaderPart(part);
        return nullptr;
    }
    return std::make_shared<HeaderFooterContent>(part);
}

std::shared_ptr<HeaderFooterContent> Section::EnsureFooter(HeaderFooterType type)
{
    if (!m_sectionProperties || !m_mainDocumentPart)
    {
        return nullptr;
    }

    if (auto existing = GetFooter(type))
    {
        return existing;
    }

    auto part = m_mainDocumentPart->AddFooterPart();
    if (!part || part->RelationshipId().empty())
    {
        return nullptr;
    }
    const std::string relationshipId = part->RelationshipId();

    if (!WordHeaderFooterHelper::AppendHeaderFooterReference<
            ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::FooterReference>(
            m_sectionProperties,
            type,
            relationshipId))
    {
        m_mainDocumentPart->RemoveFooterPart(part);
        return nullptr;
    }
    return std::make_shared<HeaderFooterContent>(part);
}

Section& Section::SetHeaderText(HeaderFooterType type, std::string_view text, bool preserveSpaces)
{
    if (auto header = EnsureHeader(type))
    {
        header->SetText(text, preserveSpaces);
    }
    return *this;
}

Section& Section::SetFooterText(HeaderFooterType type, std::string_view text, bool preserveSpaces)
{
    if (auto footer = EnsureFooter(type))
    {
        footer->SetText(text, preserveSpaces);
    }
    return *this;
}

bool Section::RemoveHeader(HeaderFooterType type)
{
    return WordHeaderFooterHelper::RemoveHeaderFooterReference(m_sectionProperties, m_mainDocumentPart, true, type);
}

bool Section::RemoveFooter(HeaderFooterType type)
{
    return WordHeaderFooterHelper::RemoveHeaderFooterReference(m_sectionProperties, m_mainDocumentPart, false, type);
}

bool Section::IsHeaderLinkedToPrevious(HeaderFooterType type) const
{
    return !HasHeader(type);
}

bool Section::IsFooterLinkedToPrevious(HeaderFooterType type) const
{
    return !HasFooter(type);
}

bool Section::LinkHeaderToPrevious(HeaderFooterType type)
{
    return RemoveHeader(type);
}

bool Section::LinkFooterToPrevious(HeaderFooterType type)
{
    return RemoveFooter(type);
}

BodyBlock::BodyBlock(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& lowLevel,
                     const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart)
    : m_lowLevel(lowLevel)
{
    if (!m_lowLevel)
    {
        return;
    }

    if (auto paragraph = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph>(
            m_lowLevel))
    {
        m_type = BodyBlockType::Paragraph;
        m_paragraph = std::make_shared<Paragraph>(paragraph, mainDocumentPart);
        return;
    }

    if (auto table = std::dynamic_pointer_cast<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table>(
            m_lowLevel))
    {
        m_type = BodyBlockType::Table;
        m_table = std::make_shared<Table>(table);
        return;
    }

    if (m_lowLevel->QualifiedName() == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sectPr"))
    {
        m_type = BodyBlockType::Section;
        m_section = std::make_shared<Section>(m_lowLevel, mainDocumentPart);
        return;
    }

    if (m_lowLevel->QualifiedName() == ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "sdt"))
    {
        m_type = BodyBlockType::ContentControl;
        m_contentControl = std::make_shared<ContentControl>(m_lowLevel, ContentControlLevel::Block, mainDocumentPart);
    }
}

BodyBlockType BodyBlock::Type() const
{
    return m_type;
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> BodyBlock::GetLowLevelApi() const
{
    return m_lowLevel;
}

std::shared_ptr<Paragraph> BodyBlock::AsParagraph() const
{
    return m_paragraph;
}

std::shared_ptr<Table> BodyBlock::AsTable() const
{
    return m_table;
}

std::shared_ptr<Section> BodyBlock::AsSection() const
{
    return m_section;
}

std::shared_ptr<ContentControl> BodyBlock::AsContentControl() const
{
    return m_contentControl;
}

} // namespace ExyokiOffice::Word
