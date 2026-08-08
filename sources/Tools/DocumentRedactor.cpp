// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "XmlNamespaceResolver.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

namespace
{

void AddError(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
{
    diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, std::move(message), std::move(context)});
}

void AddInfo(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
{
    diagnostics.push_back(ToolDiagnostic{ToolSeverity::Info, std::move(message), std::move(context)});
}

std::string_view LocalName(std::string_view qualifiedName)
{
    const auto colon = qualifiedName.find(':');
    return colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
}

/// True when the node's prefix resolves to the WordprocessingML namespace
/// (transitional or strict). Matching by namespace URI rather than by the
/// literal "w:" prefix keeps the scrub correct for documents that bind the
/// namespace to another prefix, and keeps it away from same-named elements in
/// foreign namespaces (mc:AlternateContent branches, custom XML, DrawingML).
[[nodiscard]] bool IsWordprocessingNode(const Pugi::xml_node& node)
{
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    const std::string_view prefix = colon == std::string_view::npos ? std::string_view()
                                                                    : name.substr(0, colon);
    const auto uri = ExyokiOffice::Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
    return uri && (*uri == "http://schemas.openxmlformats.org/wordprocessingml/2006/main" ||
                   *uri == "http://purl.oclc.org/ooxml/wordprocessingml/main");
}

/// Word content types whose XML carries comment markers and hidden runs.
[[nodiscard]] bool IsWordStoryContentType(const std::string& contentType)
{
    if (contentType.find("wordprocessingml") == std::string::npos &&
        contentType.find("ms-word") == std::string::npos)
    {
        return false;
    }
    return contentType.ends_with("main+xml") || contentType.ends_with("header+xml") ||
           contentType.ends_with("footer+xml") || contentType.ends_with("footnotes+xml") ||
           contentType.ends_with("endnotes+xml") || contentType.ends_with("glossary+xml");
}

/// Word parts that exist solely to store comments (and their author registry).
[[nodiscard]] bool IsWordCommentContentType(const std::string& contentType)
{
    if (contentType.find("wordprocessingml") == std::string::npos &&
        contentType.find("ms-word") == std::string::npos)
    {
        return false;
    }
    return contentType.ends_with(".comments+xml") || contentType.ends_with("commentsExtended+xml") ||
           contentType.ends_with("commentsIds+xml") || contentType.ends_with("commentsExtensible+xml") ||
           contentType.ends_with("people+xml");
}

/// Detaches every incoming relationship edge of the part, which removes the
/// part from the package once it is unreachable.
bool DetachPart(OpenXmlPackage& package, const PartRecord& record,
                std::vector<ToolDiagnostic>& diagnostics)
{
    auto part = package.GetPartByUri(record.Uri);
    if (!part)
    {
        return false;
    }
    for (const auto& edge : record.Incoming)
    {
        OpenXmlPartContainer* source = nullptr;
        if (edge.SourceUri == "/")
        {
            source = &package;
        }
        else
        {
            source = package.GetPartByUri(edge.SourceUri).get();
        }
        if (!source)
        {
            AddError(diagnostics, "Relationship source part not found", edge.SourceUri);
            return false;
        }
        while (source->RemovePartReference(part))
        {
        }
    }
    return true;
}

/// True when a `w:vanish` element actually hides the run (missing @w:val or a
/// value other than "false"/"0").
[[nodiscard]] bool VanishInEffect(const Pugi::xml_node& vanish)
{
    for (auto attribute = vanish.first_attribute(); attribute; attribute = attribute.next_attribute())
    {
        if (LocalName(attribute.name()) == "val")
        {
            const std::string_view value = attribute.value();
            return value != "false" && value != "0";
        }
    }
    return true;
}

[[nodiscard]] bool IsHiddenRun(const Pugi::xml_node& run)
{
    for (auto child = run.first_child(); child; child = child.next_sibling())
    {
        if (LocalName(child.name()) != "rPr")
        {
            continue;
        }
        for (auto property = child.first_child(); property; property = property.next_sibling())
        {
            if (LocalName(property.name()) == "vanish" && IsWordprocessingNode(property))
            {
                return VanishInEffect(property);
            }
        }
    }
    return false;
}

/// Recursively strips comment markers and/or hidden runs from one XML tree.
void ScrubWordNode(Pugi::xml_node node, bool removeCommentMarkers, bool removeHiddenRuns,
                   Size& commentMarkersRemoved, Size& hiddenRunsRemoved)
{
    auto child = node.first_child();
    while (child)
    {
        auto next = child.next_sibling();
        const auto localName = LocalName(child.name());
        if (removeCommentMarkers &&
            (localName == "commentRangeStart" || localName == "commentRangeEnd" ||
             localName == "commentReference") &&
            IsWordprocessingNode(child))
        {
            node.remove_child(child);
            ++commentMarkersRemoved;
        }
        else if (removeHiddenRuns && localName == "r" && IsWordprocessingNode(child) &&
                 IsHiddenRun(child))
        {
            node.remove_child(child);
            ++hiddenRunsRemoved;
        }
        else
        {
            ScrubWordNode(child, removeCommentMarkers, removeHiddenRuns, commentMarkersRemoved,
                          hiddenRunsRemoved);
        }
        child = next;
    }
}

/// Clears identity properties and drops the custom-properties part.
void RedactMetadata(OpenXmlPackage& package, RedactResult& result)
{
    for (const auto* name : {"Creator", "LastModifiedBy", "Company"})
    {
        if (WriteCoreProperty(package, name, ""))
        {
            ++result.MetadataFieldsCleared;
        }
    }

    for (const auto& record : ListParts(package))
    {
        if (record.ContentType == "application/vnd.openxmlformats-officedocument.custom-properties+xml")
        {
            if (DetachPart(package, record, result.Diagnostics))
            {
                AddInfo(result.Diagnostics, "Custom properties part removed", record.Uri);
                ++result.MetadataFieldsCleared;
            }
        }
    }
}

RedactResult RedactWord(Word::WordDocumentEditor& editor, const RedactOptions& options)
{
    RedactResult result;
    result.Family = DocumentFamily::Word;

    auto document = editor.GetDocument();
    if (!document)
    {
        AddError(result.Diagnostics, "Word document has no package");
        return result;
    }
    OpenXmlPackage& package = *document;

    if (options.RemoveComments)
    {
        result.CommentsRemoved = editor.Comments().size();
    }
    if (options.ResolveRevisions)
    {
        result.RevisionsResolved = editor.AcceptAllRevisions();
    }

    // Strip comment markers and hidden runs from every story part in one XML
    // pass per part. The typed editor is not used after this point, because
    // replacing part XML invalidates its wrappers.
    if (options.RemoveComments || options.RemoveHiddenText)
    {
        Size commentMarkersRemoved = 0;
        for (const auto& record : ListParts(package))
        {
            if (!IsWordStoryContentType(record.ContentType))
            {
                continue;
            }
            auto part = package.GetPartByUri(record.Uri);
            if (!part)
            {
                continue;
            }
            // load_buffer, not load_string: c_str() ends the document at the
            // first embedded NUL, and a redactor that only sees the prefix of a
            // part leaves whatever follows it in the published document.
            const auto partXml = part->GetXmlString();
            Pugi::xml_document xml;
            if (!xml.load_buffer(partXml.data(), partXml.size(), Xml::ParseOptions::Preserving))
            {
                AddError(result.Diagnostics, "Failed to parse part XML", record.Uri);
                continue;
            }
            Size markersBefore = commentMarkersRemoved;
            Size hiddenBefore = result.HiddenRunsRemoved;
            ScrubWordNode(xml, options.RemoveComments, options.RemoveHiddenText, commentMarkersRemoved,
                          result.HiddenRunsRemoved);
            if (commentMarkersRemoved != markersBefore || result.HiddenRunsRemoved != hiddenBefore)
            {
                std::ostringstream buffer;
                xml.save(buffer, "", Pugi::format_raw, Pugi::encoding_utf8);
                part->SetXmlString(buffer.str());
            }
        }
    }

    if (options.RemoveComments)
    {
        for (const auto& record : ListParts(package))
        {
            if (IsWordCommentContentType(record.ContentType))
            {
                DetachPart(package, record, result.Diagnostics);
            }
        }
    }

    if (options.RemovePersonalMetadata)
    {
        RedactMetadata(package, result);
    }

    result.Ok = true;
    return result;
}

RedactResult RedactExcel(Excel::ExcelDocumentEditor& editor, const RedactOptions& options)
{
    RedactResult result;
    result.Family = DocumentFamily::Excel;

    if (options.RemoveComments)
    {
        for (const auto& worksheet : editor.Worksheets())
        {
            for (const auto& comment : worksheet->Comments())
            {
                if (worksheet->RemoveComment(comment.Address))
                {
                    ++result.CommentsRemoved;
                }
            }
            for (const auto& threaded : worksheet->ThreadedComments())
            {
                if (worksheet->RemoveThreadedComment(threaded.Id))
                {
                    ++result.CommentsRemoved;
                }
            }
        }
    }

    if (options.RemovePersonalMetadata)
    {
        RedactMetadata(*editor.GetDocument(), result);
    }

    result.Ok = true;
    return result;
}

RedactResult RedactPowerPoint(PowerPoint::PowerPointDocumentEditor& editor, const RedactOptions& options)
{
    RedactResult result;
    result.Family = DocumentFamily::PowerPoint;

    if (options.RemoveComments)
    {
        for (const auto& slide : editor.Slides())
        {
            for (const auto& comment : slide->Comments())
            {
                if (slide->RemoveComment(comment.Id))
                {
                    ++result.CommentsRemoved;
                }
            }
        }
        // Authors are comment metadata; with the comments gone they are
        // removable, and each one names a person.
        for (const auto& author : editor.CommentAuthors())
        {
            editor.RemoveCommentAuthor(author.Id);
        }
    }

    if (options.RemovePersonalMetadata)
    {
        RedactMetadata(*editor.GetDocument(), result);
    }

    result.Ok = true;
    return result;
}

/// Writes a redaction result out, unless the redaction itself already failed.
RedactResult SaveRedacted(RedactResult result, OpenXmlPackage* package, const std::filesystem::path& destination,
                          const char* failureMessage)
{
    if (!result.Ok)
    {
        return result;
    }

    if (package == nullptr || !package->SaveToFile(destination))
    {
        result.Ok = false;
        AddError(result.Diagnostics, failureMessage, destination.string());
        return result;
    }

    result.Saved = true;
    return result;
}

} // namespace

RedactResult RedactDocument(const std::filesystem::path& path, const std::filesystem::path& outputPath,
                            const RedactOptions& options)
{
    RedactResult result;

    OpenXmlPackage probe;
    ApplyDefaultPackageLimits(probe);
    if (!probe.LoadFromFile(path))
    {
        AddError(result.Diagnostics, "Failed to open package", path.string());
        return result;
    }

    const auto destination = outputPath.empty() ? path : outputPath;
    const auto info = GetInfo(probe);
    switch (info.Family)
    {
        case DocumentFamily::Word:
        {
            auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                AddError(result.Diagnostics, "Failed to open Word document", path.string());
                return result;
            }
            result = RedactWord(*editor, options);
            // Saved through the package: RedactWord works over the parts
            // directly once the typed edits are done, so the editor's own
            // save would not see them.
            return SaveRedacted(result, editor->GetDocument().get(), destination, "Failed to save document");
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                AddError(result.Diagnostics, "Failed to open Excel document", path.string());
                return result;
            }
            result = RedactExcel(*editor, options);
            return SaveRedacted(result, editor->GetDocument().get(), destination, "Failed to save workbook");
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                AddError(result.Diagnostics, "Failed to open PowerPoint document", path.string());
                return result;
            }
            result = RedactPowerPoint(*editor, options);
            return SaveRedacted(result, editor->GetDocument().get(), destination, "Failed to save presentation");
        }
        case DocumentFamily::Unknown:
            break;
    }

    AddError(result.Diagnostics, DescribeUnknownFamily(info), path.string());
    return result;
}

RedactResult RedactDocument(Word::WordDocumentEditor& editor, const RedactOptions& options)
{
    return RedactWord(editor, options);
}

RedactResult RedactDocument(Excel::ExcelDocumentEditor& editor, const RedactOptions& options)
{
    return RedactExcel(editor, options);
}

RedactResult RedactDocument(PowerPoint::PowerPointDocumentEditor& editor, const RedactOptions& options)
{
    return RedactPowerPoint(editor, options);
}

} // namespace ExyokiOffice::Tools
