// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "SharedToolset.hpp"

#include "Units.hpp"

#include "ExyokiOffice/ImageFormat.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/DocumentStats.hpp"
#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/DocumentTools.hpp"
#include "ExyokiOffice/Tools/MediaExporter.hpp"
#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Tools/XmlQueryTool.hpp"

#include "AsciiText.hpp"

#include "Base64.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace ExyokiOffice::Mcp
{

/// File-local helpers used by more than one shared tool.
class SharedToolsetHelper
{
public:
    /// Content types and URI shapes MediaExporter treats as media payloads.
    static bool IsMediaPart(const OpenXmlPackagePart& part)
    {
        // Binary payloads only: several XML parts carry "package" in their
        // content type, the core properties among them.
        if (!part.IsBinaryPart())
        {
            return false;
        }

        const std::string contentType(part.ContentType());
        if (contentType.rfind("image/", 0) == 0 || contentType.rfind("audio/", 0) == 0 ||
            contentType.rfind("video/", 0) == 0)
        {
            return true;
        }

        if (contentType.find("oleObject") != std::string::npos || contentType.find("package") != std::string::npos)
        {
            return true;
        }

        return part.Uri().find("/embeddings/") != std::string::npos;
    }

    /**
     * @brief The Office family a file extension names, Unknown for any other.
     *
     * Reuses the table the converter already keeps, so `.docm` and `.dotx`
     * count as Word here exactly as they do there; anything that is not an
     * Office document extension - `.md`, `.json`, no extension at all - is
     * deliberately Unknown rather than a mismatch.
     */
    static Tools::DocumentFamily FamilyOfExtension(const std::filesystem::path& path)
    {
        switch (Tools::InferConvertFormat(path))
        {
            case Tools::ConvertFormat::Docx:
                return Tools::DocumentFamily::Word;
            case Tools::ConvertFormat::Xlsx:
                return Tools::DocumentFamily::Excel;
            case Tools::ConvertFormat::Pptx:
                return Tools::DocumentFamily::PowerPoint;
            default:
                return Tools::DocumentFamily::Unknown;
        }
    }

    static std::vector<std::shared_ptr<OpenXmlPackagePart>> CollectMediaParts(const OpenXmlPackage& package)
    {
        std::vector<std::shared_ptr<OpenXmlPackagePart>> media;
        for (auto& part : Tools::CollectAllParts(package))
        {
            if (part && IsMediaPart(*part))
            {
                media.push_back(part);
            }
        }

        std::sort(media.begin(), media.end(),
                  [](const std::shared_ptr<OpenXmlPackagePart>& left,
                     const std::shared_ptr<OpenXmlPackagePart>& right)
                  { return left->Uri() < right->Uri(); });
        return media;
    }

    static nlohmann::json CorePropertiesToJson(const Tools::CoreProperties& properties)
    {
        nlohmann::json result = nlohmann::json::object();
        result["title"] = properties.Title;
        result["subject"] = properties.Subject;
        result["creator"] = properties.Creator;
        result["keywords"] = properties.Keywords;
        result["description"] = properties.Description;
        result["lastModifiedBy"] = properties.LastModifiedBy;
        result["category"] = properties.Category;
        result["contentStatus"] = properties.ContentStatus;
        result["created"] = properties.Created;
        result["modified"] = properties.Modified;
        result["application"] = properties.Application;
        result["appVersion"] = properties.AppVersion;
        result["company"] = properties.Company;
        return result;
    }

    static void AddOptionalCount(nlohmann::json& target, const char* name, const std::optional<UInt64>& value)
    {
        if (value.has_value())
        {
            target[name] = *value;
        }
    }

    static nlohmann::json StatsToJson(const Tools::DocumentStats& stats)
    {
        nlohmann::json result = nlohmann::json::object();
        AddOptionalCount(result, "wordCount", stats.WordCount);
        AddOptionalCount(result, "characterCount", stats.CharacterCount);
        AddOptionalCount(result, "paragraphCount", stats.ParagraphCount);
        AddOptionalCount(result, "headingCount", stats.HeadingCount);
        AddOptionalCount(result, "equationCount", stats.EquationCount);
        AddOptionalCount(result, "footnoteCount", stats.FootnoteCount);
        AddOptionalCount(result, "endnoteCount", stats.EndnoteCount);
        AddOptionalCount(result, "bookmarkCount", stats.BookmarkCount);
        AddOptionalCount(result, "sectionCount", stats.SectionCount);
        AddOptionalCount(result, "worksheetCount", stats.WorksheetCount);
        AddOptionalCount(result, "cellCount", stats.CellCount);
        AddOptionalCount(result, "formulaCount", stats.FormulaCount);
        AddOptionalCount(result, "mergedRangeCount", stats.MergedRangeCount);
        AddOptionalCount(result, "slideCount", stats.SlideCount);
        AddOptionalCount(result, "hiddenSlideCount", stats.HiddenSlideCount);
        AddOptionalCount(result, "slideWithNotesCount", stats.SlideWithNotesCount);
        AddOptionalCount(result, "shapeCount", stats.ShapeCount);
        AddOptionalCount(result, "chartCount", stats.ChartCount);
        AddOptionalCount(result, "tableCount", stats.TableCount);
        AddOptionalCount(result, "imageCount", stats.ImageCount);
        AddOptionalCount(result, "hyperlinkCount", stats.HyperlinkCount);
        AddOptionalCount(result, "commentCount", stats.CommentCount);
        if (stats.ReadingTimeMinutes.has_value())
        {
            result["readingTimeMinutes"] = *stats.ReadingTimeMinutes;
        }

        return result;
    }

    /**
     * @brief Narrows a model to the requested scope and page window.
     *
     * Scope keeps the reading tools usable on documents an agent could never
     * afford to read whole: a Word block range, one worksheet, or one slide.
     * The `offset`/`limit` pair then pages through whatever the scope left.
     */
    static void ApplyScope(Tools::DocumentModel& model, const nlohmann::json& arguments, Size& total, bool& truncated)
    {
        const auto scope = arguments.find("scope");
        const nlohmann::json scopeObject =
            (scope != arguments.end() && scope->is_object()) ? *scope : nlohmann::json::object();

        const Size offset = arguments.value("offset", static_cast<Size>(0));
        const Size limit = arguments.value("limit", static_cast<Size>(0));

        if (model.Word.has_value())
        {
            auto& blocks = model.Word->Body;
            const Size from = scopeObject.value("from", static_cast<Size>(1));
            const Size to = scopeObject.value("to", static_cast<Size>(0));
            if (from > 1 && from - 1 < blocks.size())
            {
                blocks.erase(blocks.begin(), blocks.begin() + static_cast<PtrDiff>(from - 1));
            }
            else if (from > 1)
            {
                blocks.clear();
            }

            if (to >= from && to > 0)
            {
                const Size keep = to - from + 1;
                if (blocks.size() > keep)
                {
                    blocks.resize(keep);
                }
            }

            total = blocks.size();
            truncated = PageItems(blocks, offset, limit);
            return;
        }

        if (model.Excel.has_value())
        {
            auto& sheets = model.Excel->Sheets;
            const auto sheetName = scopeObject.value("sheet", std::string());
            if (!sheetName.empty())
            {
                sheets.erase(std::remove_if(sheets.begin(), sheets.end(),
                                            [&sheetName](const Tools::ExcelSheetModel& sheet)
                                            { return !AsciiText::EqualsIgnoreCase(sheet.Name, sheetName); }),
                             sheets.end());
            }

            total = sheets.size();
            truncated = PageItems(sheets, offset, limit);
            return;
        }

        if (model.PowerPoint.has_value())
        {
            auto& slides = model.PowerPoint->Slides;
            const Size slide = scopeObject.value("slide", static_cast<Size>(0));
            if (slide > 0)
            {
                if (slide <= slides.size())
                {
                    const auto keep = slides[slide - 1];
                    slides.clear();
                    slides.push_back(keep);
                }
                else
                {
                    slides.clear();
                }
            }

            total = slides.size();
            truncated = PageItems(slides, offset, limit);
        }
    }

    template <typename Item>
    static bool PageItems(std::vector<Item>& items, Size offset, Size limit)
    {
        bool truncated = false;
        if (offset > 0)
        {
            if (offset < items.size())
            {
                items.erase(items.begin(), items.begin() + static_cast<PtrDiff>(offset));
            }
            else
            {
                items.clear();
            }
        }

        if (limit > 0 && items.size() > limit)
        {
            items.resize(limit);
            truncated = true;
        }

        return truncated;
    }

    /// Scope schema of the model readers, narrowed to what the family has.
    static nlohmann::json ScopeSchema(Tools::DocumentFamily family)
    {
        switch (family)
        {
            case Tools::DocumentFamily::Word:
                return Schema::Object(
                    "Restricts the result to a range of body blocks.", {},
                    nlohmann::json{{"from", Schema::Integer("First body block, 1-based.", 1)},
                                   {"to", Schema::Integer("Last body block, 1-based; omit for the end.", 1)}});
            case Tools::DocumentFamily::Excel:
                return Schema::Object("Restricts the result to one worksheet.", {},
                                      nlohmann::json{{"sheet", Schema::String("Worksheet name, case-insensitive.")}});
            case Tools::DocumentFamily::PowerPoint:
                return Schema::Object("Restricts the result to one slide.", {},
                                      nlohmann::json{{"slide", Schema::Integer("Slide index, 1-based.", 1)}});
            case Tools::DocumentFamily::Unknown:
                break;
        }

        return Schema::FreeObject("Restricts the result to part of the document.");
    }

    /// Conversion targets that make sense on a server of this family.
    static std::vector<std::string> ConversionFormats(Tools::DocumentFamily family)
    {
        std::vector<std::string> formats{"md", "json", "txt", "xml"};
        switch (family)
        {
            case Tools::DocumentFamily::Word:
                formats.emplace_back("docx");
                break;
            case Tools::DocumentFamily::Excel:
                formats.emplace_back("xlsx");
                formats.emplace_back("csv");
                break;
            case Tools::DocumentFamily::PowerPoint:
                formats.emplace_back("pptx");
                break;
            case Tools::DocumentFamily::Unknown:
                break;
        }

        std::sort(formats.begin(), formats.end());
        return formats;
    }

    /**
     * @brief Appends validation issues to @p target under a shared total cap.
     *
     * The cap counts everything already in @p target, so several issue lists
     * can be appended one after another without any of them being able to
     * exceed the caller's budget.
     *
     * @param maxTotal Largest number of entries @p target may hold; 0 is no limit.
     * @return True when the cap left issues out.
     */
    static bool AppendValidationIssues(nlohmann::json& target, const std::vector<ValidationIssue>& issues,
                                       bool errorsOnly, Size maxTotal)
    {
        for (const auto& issue : issues)
        {
            if (errorsOnly && issue.Severity != ValidationSeverity::Error)
            {
                continue;
            }

            if (maxTotal > 0 && target.size() >= maxTotal)
            {
                return true;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["severity"] = std::string(Tools::ToString(issue.Severity));
            entry["domain"] = std::string(Tools::ToString(issue.Domain));
            entry["id"] = std::string(Tools::ToString(issue.Id));
            entry["message"] = issue.Message;
            entry["partUri"] = issue.PartUri;
            target.push_back(std::move(entry));
        }

        return false;
    }

    /// Media types `get_media` can hand back inline, as an MCP content block.
    [[nodiscard]] static bool IsInlineMediaType(const std::string& contentType)
    {
        return contentType.rfind("image/", 0) == 0 || contentType.rfind("audio/", 0) == 0;
    }

    /**
     * @brief Builds the MCP content block that carries one media payload.
     *
     * Only `image` and `audio` exist as typed binary blocks, and the media rule
     * also admits video, OLE objects and embedded packages. Those used to travel
     * as an embedded `resource`, which the specification pairs with a `resources`
     * capability this server does not declare — and whose URI nothing here could
     * resolve, because there is no `resources/read`. They go through
     * `export_media` instead; IsInlineMediaType decides, and the caller refuses
     * what does not fit.
     */
    [[nodiscard]] static nlohmann::json MakeMediaContentBlock(const std::string& contentType,
                                                              std::span<const Byte> bytes)
    {
        nlohmann::json block = nlohmann::json::object();
        block["type"] = contentType.rfind("image/", 0) == 0 ? "image" : "audio";
        block["data"] = ToolSupport::EncodeBase64(bytes);
        block["mimeType"] = contentType;
        return block;
    }

    /// Logs when a path read shadows an open session that has unsaved changes.
    static void WarnAboutStaleSessionCopy(ToolContext& context, const std::filesystem::path& resolved)
    {
        const auto* session = context.Sessions().FindByPath(resolved);
        if (session != nullptr && session->IsDirty())
        {
            Log::Warn("Reading '" + resolved.generic_string() + "' from disk while session '" + session->Id() +
                      "' holds unsaved changes to the same file; those changes are not part of the result.");
        }
    }

    static nlohmann::json DiagnosticList(const std::vector<Tools::ToolDiagnostic>& diagnostics)
    {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& diagnostic : diagnostics)
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["severity"] = std::string(Tools::ToString(diagnostic.Severity));
            entry["message"] = diagnostic.Message;
            entry["context"] = diagnostic.Context;
            result.push_back(std::move(entry));
        }

        return result;
    }

    static bool HasErrorDiagnostic(const std::vector<Tools::ToolDiagnostic>& diagnostics)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Tools::ToolDiagnostic& diagnostic)
                           { return diagnostic.Severity == Tools::ToolSeverity::Error; });
    }
};

DocumentAccess::DocumentAccess(ToolContext& context, const nlohmann::json& arguments)
{
    const auto documentId = arguments.value("documentId", std::string());
    const auto path = arguments.value("path", std::string());

    if (documentId.empty() == path.empty())
    {
        m_failure = MakeError(ErrorCode::InputInvalid,
                              "Pass exactly one of 'documentId' (an open session) or 'path' (a workspace file).",
                              {}, "Call list_documents to see the open sessions.");
        return;
    }

    if (!documentId.empty())
    {
        m_session = context.Sessions().Find(documentId);
        if (m_session == nullptr)
        {
            m_failure = MakeError(ErrorCode::DocumentNotFound, "No open document has the id '" + documentId + "'.",
                                  documentId, "Call list_documents to see the open sessions.");
            return;
        }

        m_document = &m_session->Document();
        m_valid = true;
        return;
    }

    auto resolved = ToolSupport::ResolveExistingFile(context, path, m_failure);
    if (!resolved.has_value())
    {
        return;
    }

    SharedToolsetHelper::WarnAboutStaleSessionCopy(context, *resolved);
    m_transient = context.Adapter().Open(*resolved);
    if (m_transient == nullptr)
    {
        m_failure = MakeError(ErrorCode::PackageLoadFailed, "The file could not be opened as a " + context.Adapter().FamilyName() + " document.",
                              path, "Verify the file is a valid " + context.Adapter().FileExtension() + " package.");
        return;
    }

    m_document = m_transient.get();
    m_valid = true;
}

MutationGuard::MutationGuard(DocumentSession& session)
    : m_session(&session)
{
    m_snapshotTaken = m_session->PushSnapshot();
}

MutationGuard::~MutationGuard()
{
    if (m_committed || m_session == nullptr)
    {
        return;
    }

    if (!m_snapshotTaken)
    {
        // Not flagged as a broken session: most handlers fail their checks
        // before touching anything, and with undo switched off there is no way
        // to tell those apart from a partial edit. Running without snapshots
        // is an explicit choice, documented at --snapshot-depth.
        Log::Warn("A failed edit on '" + m_session->Id() +
                  "' was not rolled back because no snapshot was taken; run with --snapshot-depth greater than "
                  "zero to make failed edits recoverable.");
        return;
    }

    if (!m_session->RollbackToNewestSnapshot())
    {
        Log::Error("Rolling back a failed edit on '" + m_session->Id() +
                   "' failed; the session may hold a partially edited document.");
        m_session->MarkRollbackFailed();
    }
}

void MutationGuard::Commit()
{
    m_committed = true;
    m_session->MarkMutated();
}

void ToolSupport::AddDocumentSourceProperties(nlohmann::json& properties)
{
    properties["documentId"] = Schema::String("Identifier of an open document; mutually exclusive with 'path'.");
    properties["path"] =
        Schema::String("Workspace-relative file to read without opening a session; mutually exclusive with "
                       "'documentId'. Reads the file as it is on disk, so unsaved changes of a session holding "
                       "the same file are not included.");
}

void ToolSupport::AddDocumentIdProperty(nlohmann::json& properties)
{
    properties["documentId"] = Schema::String("Identifier of the open document to modify.");
}

DocumentSession* ToolSupport::RequireSession(ToolContext& context, const nlohmann::json& arguments,
                                             ToolOutcome& failure)
{
    const auto documentId = arguments.value("documentId", std::string());
    if (documentId.empty())
    {
        failure = MakeError(ErrorCode::InputInvalid, "This tool requires the 'documentId' of an open document.", {},
                            "Call open_document or create_document first.");
        return nullptr;
    }

    auto* session = context.Sessions().Find(documentId);
    if (session == nullptr)
    {
        failure = MakeError(ErrorCode::DocumentNotFound, "No open document has the id '" + documentId + "'.",
                            documentId, "Call list_documents to see the open sessions.");
    }

    return session;
}

std::optional<std::filesystem::path> ToolSupport::ResolvePath(ToolContext& context, std::string_view path,
                                                              ToolOutcome& failure)
{
    if (path.empty())
    {
        failure = MakeError(ErrorCode::InputInvalid, "An empty path cannot be resolved.");
        return std::nullopt;
    }

    if (Workspace::IsRefusedForm(path))
    {
        // Reported apart from path_outside_workspace: such a path does not leave
        // the workspace, it is one the server will not name at all, and the way
        // an agent repairs it differs.
        failure = MakeError(ErrorCode::PathInvalid, "The path uses a form this server refuses.",
                            std::string(path),
                            "Pass a plain workspace-relative file name, without a device name, a stream "
                            "suffix, or an extended-length prefix.");
        return std::nullopt;
    }

    auto resolved = context.GetWorkspace().Resolve(path);
    if (!resolved.has_value())
    {
        failure = MakeError(ErrorCode::PathOutsideWorkspace,
                            "The path leaves the configured workspace and was rejected.", std::string(path),
                            "Use a path inside the workspace; call list_workspace to see what is reachable.");
    }

    return resolved;
}

std::optional<std::filesystem::path> ToolSupport::ResolveExistingFile(ToolContext& context, std::string_view path,
                                                                      ToolOutcome& failure)
{
    auto resolved = ResolvePath(context, path, failure);
    if (!resolved.has_value())
    {
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(*resolved, error) || error)
    {
        failure = MakeError(ErrorCode::FileNotFound, "No file exists at the requested path.", std::string(path),
                            "Call list_workspace to see the available files.");
        return std::nullopt;
    }

    return resolved;
}

std::optional<std::filesystem::path> ToolSupport::ResolveOutputPath(ToolContext& context, std::string_view path,
                                                                    bool overwrite, ToolOutcome& failure)
{
    auto resolved = ResolvePath(context, path, failure);
    if (!resolved.has_value())
    {
        return std::nullopt;
    }

    std::error_code error;
    if (!overwrite && std::filesystem::exists(*resolved, error) && !error)
    {
        failure = MakeError(ErrorCode::FileExists, "A file already exists at the destination.", std::string(path),
                            "Pass overwrite: true to replace it, or choose another path.");
        return std::nullopt;
    }

    return resolved;
}

bool ToolSupport::CheckDestinationFamily(ToolContext& context, std::string_view path, ToolOutcome& failure)
{
    const auto named = SharedToolsetHelper::FamilyOfExtension(std::filesystem::path(path));
    if (named == Tools::DocumentFamily::Unknown || named == context.Adapter().Family())
    {
        return true;
    }

    const std::string namedFamily(Tools::ToString(named));
    failure = MakeError(ErrorCode::FamilyMismatch,
                        "This server writes " + context.Adapter().FamilyName() +
                            " documents, but the destination names a " + namedFamily + " file.",
                        std::string(path),
                        "Use the " + context.Adapter().FileExtension() + " extension, or the MCP server for " +
                            namedFamily + " documents.");
    return false;
}

std::optional<std::vector<Byte>> ToolSupport::DecodeBase64(std::string_view text)
{
    // A tool argument is typed by an agent, not produced by a writer, so a
    // final group left unpadded still names the bytes it means.
    std::vector<Byte> bytes;
    if (!Base64::Decode(text, bytes, Base64::Padding::Optional))
    {
        return std::nullopt;
    }
    return bytes;
}

std::string ToolSupport::EncodeBase64(std::span<const Byte> bytes)
{
    return Base64::Encode(bytes);
}

bool ToolSupport::LoadImagePayload(ToolContext& context, const nlohmann::json& arguments, std::vector<Byte>& bytes,
                                   std::string& contentType, ToolOutcome& failure)
{
    const auto path = arguments.value("path", std::string());
    const auto data = arguments.value("dataBase64", std::string());

    if (path.empty() == data.empty())
    {
        failure = MakeError(ErrorCode::InputInvalid,
                            "Pass exactly one of 'path' (a workspace image file) or 'dataBase64'.");
        return false;
    }

    if (!path.empty())
    {
        auto resolved = ResolveExistingFile(context, path, failure);
        if (!resolved.has_value())
        {
            return false;
        }

        if (!ReadFileBytes(*resolved, bytes))
        {
            failure = MakeError(ErrorCode::OperationFailed, "The image file could not be read.", path);
            return false;
        }
    }
    else
    {
        if (data.size() > context.MaxMediaBytes() * 4u / 3u)
        {
            failure = MakeError(ErrorCode::InputInvalid, "The base64 payload exceeds the configured media limit.", {},
                                "Use a workspace file path instead of an inline payload.");
            return false;
        }

        auto decoded = DecodeBase64(data);
        if (!decoded.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid, "'dataBase64' is not valid base64 data.");
            return false;
        }

        bytes = std::move(*decoded);
    }

    if (bytes.empty())
    {
        failure = MakeError(ErrorCode::InputInvalid, "The image payload is empty.");
        return false;
    }

    if (bytes.size() > context.MaxMediaBytes())
    {
        failure = MakeError(ErrorCode::InputInvalid, "The image exceeds the configured media limit.", {},
                            "Use a workspace file path instead of an inline payload, or raise --max-media-size.");
        return false;
    }

    contentType = arguments.value("contentType", std::string());
    if (contentType.empty())
    {
        const auto detected = DetectImageFormat(bytes);
        if (!detected.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid, "The image format could not be detected.", {},
                                "Pass 'contentType' explicitly, for example \"image/png\".");
            return false;
        }

        contentType = detected->ContentType;
    }

    return true;
}

std::string ToolSupport::FamilyToken(Tools::DocumentFamily family)
{
    return std::string(Tools::ToString(family));
}

bool ToolSupport::ReadFileBytes(const std::filesystem::path& path, std::vector<Byte>& bytes)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
    {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    bytes.resize(static_cast<Size>(size));
    if (size > 0)
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    return static_cast<bool>(input) || input.eof();
}

/// Registration of the `lifecycle` group.
class LifecycleTools
{
public:
    static void Register(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        RegisterCreateDocument(registry, adapter);
        RegisterOpenDocument(registry, adapter);
        RegisterSaveDocument(registry, adapter);
        RegisterCloseDocument(registry);
        RegisterListDocuments(registry);
        RegisterUndo(registry);
    }

private:
    static void RegisterCreateDocument(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        const auto extension = adapter.FileExtension();

        nlohmann::json properties = nlohmann::json::object();
        properties["path"] = Schema::String("Workspace-relative destination, for example \"report" + extension +
                                            "\"; an extension of another Office family is refused. The file is "
                                            "written by save_document, not here.");
        properties["overwrite"] =
            Schema::BooleanWithDefault("Permit an existing file at 'path' to be replaced on save.", false);
        properties["template_path"] =
            Schema::String("Workspace-relative template opened as the starting point of the new document.");

        nlohmann::json data = Schema::Object("New session.", {"documentId", "family"},
                                             nlohmann::json{{"documentId", Schema::String("Session identifier.")},
                                                            {"family", Schema::String("Document family.")},
                                                            {"path", Schema::String("Destination path, if given.")}});

        ToolDefinition definition;
        definition.Name = "create_document";
        definition.Title = "Create document";
        definition.Description = "Create a new " + adapter.FamilyName() +
                                 " document and open it as a session. Pass 'template_path' to start from an "
                                 "existing document. Nothing is written until you call save_document.";
        definition.Group = "lifecycle";
        definition.InputSchema =
            Schema::Object("Arguments of create_document.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(std::move(data), true);
        definition.Example = nlohmann::json{{"path", "report" + extension}};
        definition.Annotations.ReadOnly = false;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Create(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Create(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        std::filesystem::path destination;
        const auto path = arguments.value("path", std::string());
        if (!path.empty())
        {
            auto resolved =
                ToolSupport::ResolveOutputPath(context, path, arguments.value("overwrite", false), failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            // Checked here rather than in save_document alone: the session
            // carries this path from now on, so the mismatch would otherwise
            // only surface once the work is done and cannot be written.
            if (!ToolSupport::CheckDestinationFamily(context, path, failure))
            {
                return failure;
            }

            destination = *resolved;
        }

        std::unique_ptr<DocumentHandle> document;
        const auto templatePath = arguments.value("template_path", std::string());
        if (!templatePath.empty())
        {
            auto resolvedTemplate = ToolSupport::ResolveExistingFile(context, templatePath, failure);
            if (!resolvedTemplate.has_value())
            {
                return failure;
            }

            document = context.Adapter().Open(*resolvedTemplate);
            if (document == nullptr)
            {
                return MakeError(ErrorCode::PackageLoadFailed, "The template could not be opened.", templatePath,
                                 "Verify the template is a valid " + context.Adapter().FileExtension() + " package.");
            }
        }
        else
        {
            document = context.Adapter().CreateNew();
            if (document == nullptr)
            {
                return MakeError(ErrorCode::OperationFailed, "A new " + context.Adapter().FamilyName() +
                                                                 " document could not be created.");
            }
        }

        auto* session = context.Sessions().Create(std::move(document), destination);
        if (session == nullptr)
        {
            return MakeError(ErrorCode::DocumentLimitReached,
                             "The server already holds the maximum number of open documents.", {},
                             "Call close_document on a session you no longer need.");
        }

        nlohmann::json data = nlohmann::json::object();
        data["documentId"] = session->Id();
        data["family"] = ToolSupport::FamilyToken(context.Adapter().Family());
        data["path"] = destination.empty() ? std::string() : context.GetWorkspace().Relativize(destination);

        return ResultBuilder("Created a new " + context.Adapter().FamilyName() + " document as " + session->Id() +
                             ".")
            .WithSession(*session)
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterOpenDocument(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["path"] = Schema::String("Workspace-relative " + adapter.FileExtension() + " file to open.");

        nlohmann::json data =
            Schema::Object("Opened session.", {"documentId", "family", "path"},
                           nlohmann::json{{"documentId", Schema::String("Session identifier.")},
                                          {"family", Schema::String("Document family.")},
                                          {"path", Schema::String("Workspace-relative path of the file.")},
                                          {"summary", Schema::FreeObject("Family-specific structural counts.")},
                                          {"properties", Schema::FreeObject("Core document properties.")}});

        ToolDefinition definition;
        definition.Name = "open_document";
        definition.Title = "Open document";
        definition.Description = "Open an existing " + adapter.FamilyName() +
                                 " document as a session and report its structure. Use the returned documentId for "
                                 "every later call on this document.";
        definition.Group = "lifecycle";
        definition.InputSchema = Schema::Object("Arguments of open_document.", {"path"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(std::move(data), true);
        definition.Example = nlohmann::json{{"path", "report" + adapter.FileExtension()}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Open(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Open(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        const auto path = arguments.value("path", std::string());
        auto resolved = ToolSupport::ResolveExistingFile(context, path, failure);
        if (!resolved.has_value())
        {
            return failure;
        }

        auto document = context.Adapter().Open(*resolved);
        if (document == nullptr)
        {
            return MakeError(ErrorCode::PackageLoadFailed, "The file could not be opened as an OPC package.", path,
                             "Verify the file is a valid " + context.Adapter().FileExtension() + " document.");
        }

        auto package = document->Package();
        const auto info = package ? Tools::GetInfo(*package) : Tools::PackageInfo{};
        if (info.Family != context.Adapter().Family())
        {
            return MakeError(ErrorCode::FamilyMismatch,
                             "This server handles " + context.Adapter().FamilyName() + " documents, but the file is " +
                                 std::string(Tools::ToString(info.Family)) + ".",
                             path, "Use the MCP server matching the document family.");
        }

        nlohmann::json summary = document->Summary();
        nlohmann::json coreProperties = SharedToolsetHelper::CorePropertiesToJson(info.Properties);

        auto* session = context.Sessions().Create(std::move(document), *resolved);
        if (session == nullptr)
        {
            return MakeError(ErrorCode::DocumentLimitReached,
                             "The server already holds the maximum number of open documents.", {},
                             "Call close_document on a session you no longer need.");
        }

        nlohmann::json data = nlohmann::json::object();
        data["documentId"] = session->Id();
        data["family"] = ToolSupport::FamilyToken(info.Family);
        data["path"] = context.GetWorkspace().Relativize(*resolved);
        data["summary"] = std::move(summary);
        data["properties"] = std::move(coreProperties);

        return ResultBuilder("Opened " + context.GetWorkspace().Relativize(*resolved) + " as " + session->Id() + ".")
            .WithSession(*session)
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSaveDocument(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["path"] = Schema::String("Workspace-relative destination; omit to save back to the opened file. "
                                            "An extension of another Office family is refused.");
        properties["overwrite"] =
            Schema::BooleanWithDefault("Permit replacing an existing file at a new destination.", false);
        properties["force"] =
            Schema::BooleanWithDefault("Save even when the file changed on disk since it was opened.", false);

        nlohmann::json data = Schema::Object("Saved file.", {"path", "bytesWritten"},
                                             nlohmann::json{{"path", Schema::String("Workspace-relative path written.")},
                                                            {"bytesWritten", Schema::Integer("Size of the file.")}});

        ToolDefinition definition;
        definition.Name = "save_document";
        definition.Title = "Save document";
        definition.Description =
            "Write an open document to disk atomically. Saving back to the source file fails when it changed on "
            "disk since it was opened, unless you pass force.";
        definition.Group = "lifecycle";
        definition.InputSchema =
            Schema::Object("Arguments of save_document.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(std::move(data), true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"path", "report" + adapter.FileExtension()}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Save(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Save(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto* session = ToolSupport::RequireSession(context, arguments, failure);
        if (session == nullptr)
        {
            return failure;
        }

        const auto requestedPath = arguments.value("path", std::string());
        std::filesystem::path destination = session->Path();
        bool savingInPlace = true;

        if (!requestedPath.empty())
        {
            auto resolved = ToolSupport::ResolvePath(context, requestedPath, failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            if (!ToolSupport::CheckDestinationFamily(context, requestedPath, failure))
            {
                return failure;
            }

            savingInPlace = *resolved == session->Path();
            destination = *resolved;
        }

        if (destination.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "This document has no path yet.", session->Id(),
                             "Pass 'path' to choose where the document is written.");
        }

        std::error_code error;
        if (!savingInPlace && !arguments.value("overwrite", false) && std::filesystem::exists(destination, error) &&
            !error)
        {
            return MakeError(ErrorCode::FileExists, "A file already exists at the destination.", requestedPath,
                             "Pass overwrite: true to replace it, or choose another path.");
        }

        if (savingInPlace && !arguments.value("force", false) && session->HasFileChangedOnDisk())
        {
            return MakeError(ErrorCode::FileChangedOnDisk,
                             "The file changed on disk after this session opened it; saving would discard those "
                             "changes.",
                             context.GetWorkspace().Relativize(destination),
                             "Pass force: true to overwrite, or save to a different path.");
        }

        if (destination.has_parent_path())
        {
            std::filesystem::create_directories(destination.parent_path(), error);
        }

        if (!session->Document().SaveToFile(destination))
        {
            return MakeError(ErrorCode::OperationFailed, "The document could not be written.",
                             context.GetWorkspace().Relativize(destination));
        }

        session->MarkSaved(destination);

        const auto written = std::filesystem::file_size(destination, error);
        nlohmann::json data = nlohmann::json::object();
        data["path"] = context.GetWorkspace().Relativize(destination);
        data["bytesWritten"] = error ? 0 : static_cast<UInt64>(written);

        return ResultBuilder("Saved " + session->Id() + " to " + context.GetWorkspace().Relativize(destination) + ".")
            .WithSession(*session)
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterCloseDocument(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["discard"] = Schema::BooleanWithDefault("Close even when the document has unsaved changes.", false);

        ToolDefinition definition;
        definition.Name = "close_document";
        definition.Title = "Close document";
        definition.Description =
            "Release an open document and its undo history. Closing a document with unsaved changes requires "
            "discard: true.";
        definition.Group = "lifecycle";
        definition.InputSchema =
            Schema::Object("Arguments of close_document.", {"documentId"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Closed session.", {"documentId"},
                                            nlohmann::json{{"documentId", Schema::String("Closed session id.")}}),
                             false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Close(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Close(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto* session = ToolSupport::RequireSession(context, arguments, failure);
        if (session == nullptr)
        {
            return failure;
        }

        if (session->IsDirty() && !arguments.value("discard", false))
        {
            return MakeError(ErrorCode::OperationFailed, "The document has unsaved changes.", session->Id(),
                             "Call save_document first, or close with discard: true.");
        }

        const auto id = session->Id();
        context.Sessions().Close(id);

        nlohmann::json data = nlohmann::json::object();
        data["documentId"] = id;
        return ResultBuilder("Closed " + id + ".").WithData(std::move(data)).Build();
    }

    static void RegisterListDocuments(ToolRegistry& registry)
    {
        nlohmann::json entry =
            Schema::Object("One open document.", {"documentId", "family", "dirty", "revision"},
                           nlohmann::json{{"documentId", Schema::String("Session identifier.")},
                                          {"path", Schema::String("Workspace-relative path, empty when unsaved.")},
                                          {"family", Schema::String("Document family.")},
                                          {"dirty", Schema::Boolean("True when changes are unsaved.")},
                                          {"revision", Schema::Integer("Number of applied mutations.")}});

        ToolDefinition definition;
        definition.Name = "list_documents";
        definition.Title = "List open documents";
        definition.Description = "List every document this server currently holds open, with its save state.";
        definition.Group = "lifecycle";
        definition.InputSchema = Schema::Object("This tool takes no arguments.", {}, nlohmann::json::object());
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Open sessions.", {"documents"},
                           nlohmann::json{{"documents", Schema::Array("Open documents.", std::move(entry))}}),
            false);
        definition.Example = nlohmann::json::object();
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json&)
        { return List(context); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome List(ToolContext& context)
    {
        nlohmann::json documents = nlohmann::json::array();
        for (const auto& session : context.Sessions().Sessions())
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["documentId"] = session->Id();
            entry["path"] =
                session->Path().empty() ? std::string() : context.GetWorkspace().Relativize(session->Path());
            entry["family"] = ToolSupport::FamilyToken(session->Family());
            entry["dirty"] = session->IsDirty();
            entry["revision"] = session->Revision();
            documents.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto count = documents.size();
        data["documents"] = std::move(documents);

        return ResultBuilder(std::to_string(count) + " document(s) open.").WithData(std::move(data)).Build();
    }

    static void RegisterUndo(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);

        ToolDefinition definition;
        definition.Name = "undo";
        definition.Title = "Undo last change";
        definition.Description =
            "Restore the document to the state before its most recent mutation. Undo itself counts as a mutation, "
            "so the revision counter keeps increasing.";
        definition.Group = "lifecycle";
        definition.InputSchema = Schema::Object("Arguments of undo.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Undo result.", {"restoredRevision"},
                           nlohmann::json{{"restoredRevision",
                                           Schema::Integer("Revision the document content was restored to.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Undo(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Undo(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto* session = ToolSupport::RequireSession(context, arguments, failure);
        if (session == nullptr)
        {
            return failure;
        }

        if (!session->HasSnapshots())
        {
            return MakeError(ErrorCode::SnapshotUnavailable, "No undo snapshot is available for this document.",
                             session->Id(),
                             "Undo is limited by --snapshot-depth and covers only mutations made in this session.");
        }

        const auto restored = session->Undo();
        if (!restored.has_value())
        {
            return MakeError(ErrorCode::SnapshotUnavailable, "The snapshot could not be restored.", session->Id());
        }

        session->MarkMutated();

        nlohmann::json data = nlohmann::json::object();
        data["restoredRevision"] = *restored;

        return ResultBuilder("Undid the last change to " + session->Id() + ".")
            .WithSession(*session)
            .WithData(std::move(data))
            .Build();
    }
};

/// Registration of the `read` group; every tool accepts `documentId` or `path`.
class ReadTools
{
public:
    static void Register(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        RegisterGetDocumentInfo(registry);
        RegisterGetDocumentModel(registry, adapter);
        RegisterGetDocumentMarkdown(registry, adapter);
        RegisterGetDocumentText(registry);
        RegisterSearchText(registry);
        RegisterValidateDocument(registry);
        RegisterQueryXml(registry);
        RegisterGetProperties(registry);
        RegisterListMedia(registry);
        RegisterGetMedia(registry);
    }

private:
    /// Base descriptor shared by every reading tool.
    static ToolDefinition MakeReadDefinition(std::string name, std::string title, std::string description)
    {
        ToolDefinition definition;
        definition.Name = std::move(name);
        definition.Title = std::move(title);
        definition.Description = std::move(description);
        definition.Group = "read";
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        return definition;
    }

    static void RegisterGetDocumentInfo(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json data =
            Schema::Object("Document overview.", {"family"},
                           nlohmann::json{{"family", Schema::String("Detected document family.")},
                                          {"documentType", Schema::String("Package document type name.")},
                                          {"mainPartUri", Schema::String("URI of the main document part.")},
                                          {"partCount", Schema::Integer("Number of package parts.")},
                                          {"relationshipCount", Schema::Integer("Number of relationships.")},
                                          {"totalPartSize", Schema::Integer("Uncompressed size of all parts.")},
                                          {"properties", Schema::FreeObject("Core and extended properties.")},
                                          {"statistics", Schema::FreeObject("Family-specific content statistics.")}});

        auto definition = MakeReadDefinition(
            "get_document_info", "Get document info",
            "Report the package overview, core properties, and content statistics of a document. Use it as the "
            "cheap first look before reading content.");
        definition.InputSchema = Schema::Object("Arguments of get_document_info.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(std::move(data), false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Info(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Info(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The document has no package.");
        }

        const auto info = Tools::GetInfo(*package);

        nlohmann::json data = nlohmann::json::object();
        data["family"] = ToolSupport::FamilyToken(info.Family);
        data["documentType"] = info.DocumentTypeName;
        data["mainPartUri"] = info.MainPartUri;
        data["partCount"] = static_cast<UInt64>(info.PartCount);
        data["relationshipCount"] = static_cast<UInt64>(info.RelationshipCount);
        data["totalPartSize"] = info.TotalPartSize;
        data["properties"] = SharedToolsetHelper::CorePropertiesToJson(info.Properties);

        DocumentAccess statistics(context, arguments);
        data["statistics"] = statistics.IsValid()
                                 ? SharedToolsetHelper::StatsToJson(
                                       context.Adapter().Stat(statistics.Document()))
                                 : nlohmann::json::object();

        return ResultBuilder("Read the overview of a " + std::string(Tools::ToString(info.Family)) + " document.")
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterGetDocumentModel(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["scope"] = SharedToolsetHelper::ScopeSchema(adapter.Family());
        properties["offset"] = Schema::IntegerWithDefault("Number of leading top-level items to skip.", 0, 0);
        properties["limit"] = Schema::IntegerWithDefault("Maximum number of top-level items to return; 0 means all.",
                                                         0, 0);

        nlohmann::json data =
            Schema::Object("Semantic document model.", {"document"},
                           nlohmann::json{{"document", Schema::FreeObject("The exyokioffice-document envelope.")},
                                          {"itemCount", Schema::Integer("Top-level items the scope selected.")},
                                          {"nextOffset", Schema::Integer("Offset to pass to read the next page.")}});

        auto definition = MakeReadDefinition(
            "get_document_model", "Get document model",
            "Read the document as the semantic exyokioffice-document JSON model, optionally narrowed to a scope "
            "and paged. Media payloads are never embedded; use list_media and get_media for those.");
        definition.InputSchema = Schema::Object("Arguments of get_document_model.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(std::move(data), false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"limit", 20}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Model(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Model(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        std::vector<Tools::ToolDiagnostic> diagnostics;
        Tools::ModelReadOptions options;
        options.IncludeMediaData = false;
        auto model = context.Adapter().ReadModel(access.Document(), options, diagnostics);
        if (model.Family == Tools::DocumentFamily::Unknown)
        {
            return MakeErrorFromDiagnostics(ErrorCode::PackageLoadFailed, "The document model could not be read.",
                                            diagnostics);
        }

        Size total = 0;
        bool paged = false;
        SharedToolsetHelper::ApplyScope(model, arguments, total, paged);

        auto serialized = Tools::SerializeModelJson(model, false);
        auto document = nlohmann::json::parse(serialized, nullptr, false);
        if (document.is_discarded())
        {
            return MakeError(ErrorCode::InternalError, "The serialized document model is not valid JSON.");
        }

        const Size offset = arguments.value("offset", static_cast<Size>(0));
        const Size limit = arguments.value("limit", static_cast<Size>(0));

        nlohmann::json data = nlohmann::json::object();
        data["document"] = std::move(document);
        data["itemCount"] = static_cast<UInt64>(total);
        data["nextOffset"] = paged ? static_cast<UInt64>(offset + limit) : 0u;

        bool truncated = paged;
        bool overBudget = false;
        if (SerializeJson(data).size() > ResponseBudgetBytes)
        {
            // Handing back part of the tree is not an option: the payload is
            // published under a document schema that a fragment would not
            // satisfy. The model is dropped and the caller told to narrow it.
            data["document"] = nlohmann::json::object();
            truncated = true;
            overBudget = true;
        }

        ResultBuilder builder("Read the semantic model of the document.");
        builder.WithData(std::move(data)).WithDiagnostics(diagnostics).WithTruncated(truncated);
        if (overBudget)
        {
            builder.WithWarning("response_budget_exceeded",
                                "The model did not fit the response budget and was omitted. Narrow 'scope' or "
                                "lower 'limit' and read the document in pages.");
        }

        return builder.Build();
    }

    static void RegisterGetDocumentMarkdown(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["scope"] = SharedToolsetHelper::ScopeSchema(adapter.Family());

        auto definition = MakeReadDefinition(
            "get_document_markdown", "Get document as Markdown",
            "Render the document as structure-preserving Markdown. This is the cheapest way to check that an edit "
            "produced what you intended.");
        definition.InputSchema = Schema::Object("Arguments of get_document_markdown.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Rendered document.", {"markdown"},
                           nlohmann::json{{"markdown", Schema::String("Markdown rendering of the document.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Markdown(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Markdown(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        std::vector<Tools::ToolDiagnostic> diagnostics;
        Tools::ModelReadOptions options;
        options.IncludeMediaData = false;
        auto model = context.Adapter().ReadModel(access.Document(), options, diagnostics);
        if (model.Family == Tools::DocumentFamily::Unknown)
        {
            return MakeErrorFromDiagnostics(ErrorCode::PackageLoadFailed, "The document model could not be read.",
                                            diagnostics);
        }

        Size total = 0;
        bool paged = false;
        SharedToolsetHelper::ApplyScope(model, arguments, total, paged);

        auto markdown = Tools::SerializeModelMarkdown(model, diagnostics);
        const bool truncated = TruncateTextToBudget(markdown);

        nlohmann::json data = nlohmann::json::object();
        data["markdown"] = std::move(markdown);

        return ResultBuilder("Rendered the document as Markdown.")
            .WithData(std::move(data))
            .WithDiagnostics(diagnostics)
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterGetDocumentText(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        auto definition = MakeReadDefinition(
            "get_document_text", "Get document text",
            "Extract every readable text block of the document as plain text, including headers, notes, and "
            "comments. Use it when you only need the words, not the structure.");
        definition.InputSchema = Schema::Object("Arguments of get_document_text.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Extracted text.", {"text"},
                           nlohmann::json{{"text", Schema::String("Extracted text, one block per line.")},
                                          {"blockCount", Schema::Integer("Number of extracted blocks.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Text(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Text(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        const auto extracted = context.Adapter().ExtractText(access.Document());
        if (!extracted.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The document text could not be extracted.",
                                            extracted.Diagnostics);
        }

        std::string text;
        for (const auto& block : extracted.Blocks)
        {
            if (block.Text.empty())
            {
                continue;
            }

            if (!text.empty())
            {
                text.push_back('\n');
            }

            text.append(block.Text);
        }

        const bool truncated = TruncateTextToBudget(text);

        nlohmann::json data = nlohmann::json::object();
        data["text"] = std::move(text);
        data["blockCount"] = static_cast<UInt64>(extracted.Blocks.size());

        return ResultBuilder("Extracted " + std::to_string(extracted.Blocks.size()) + " text block(s).")
            .WithData(std::move(data))
            .WithDiagnostics(extracted.Diagnostics)
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterSearchText(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["needle"] = Schema::String("Text or regular expression to look for.");
        properties["regex"] =
            Schema::BooleanWithDefault("Treat 'needle' as an ECMAScript regular expression.", false);
        properties["ignore_case"] = Schema::BooleanWithDefault("Match case-insensitively.", false);
        properties["context"] =
            Schema::IntegerWithDefault("Characters of surrounding text to include on each side.", 40, 0, 500);
        properties["max_matches"] = Schema::IntegerWithDefault("Maximum number of matches to return.", 200, 1, 5000);

        nlohmann::json match =
            Schema::Object("One search hit.", {"label", "matchText"},
                           nlohmann::json{{"label", Schema::String("Location, such as \"Sheet1!B2\".")},
                                          {"offset", Schema::Integer("Offset inside the containing text unit.")},
                                          {"length", Schema::Integer("Length of the match.")},
                                          {"matchText", Schema::String("The matched text.")},
                                          {"context", Schema::String("Surrounding text.")}});

        auto definition = MakeReadDefinition("search_text", "Search document text",
                                             "Find text anywhere in the document and report where each hit sits. "
                                             "Run it before replace_text to see what would change.");
        definition.InputSchema = Schema::Object("Arguments of search_text.", {"needle"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Search result.", {"matches"},
                           nlohmann::json{{"matches", Schema::Array("Matches in document order.", std::move(match))},
                                          {"matchCount", Schema::Integer("Number of returned matches.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"needle", "quarterly"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Search(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Search(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        const auto needle = arguments.value("needle", std::string());
        const auto result = context.Adapter().SearchText(access.Document(), needle,
                                                         arguments.value("context", static_cast<Size>(40)),
                                                         arguments.value("regex", false),
                                                         arguments.value("ignore_case", false));
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The search could not run.",
                                            result.Diagnostics,
                                            "Check that 'needle' is not empty and that a regex is well formed.");
        }

        const Size maximum = arguments.value("max_matches", static_cast<Size>(200));
        nlohmann::json matches = nlohmann::json::array();
        for (const auto& match : result.Matches)
        {
            if (matches.size() >= maximum)
            {
                break;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["label"] = match.Label;
            entry["offset"] = static_cast<UInt64>(match.Offset);
            entry["length"] = static_cast<UInt64>(match.Length);
            entry["matchText"] = match.MatchText;
            entry["context"] = match.Context;
            matches.push_back(std::move(entry));
        }

        bool truncated = matches.size() < result.Matches.size();
        truncated = TruncateArrayToBudget(matches) || truncated;

        nlohmann::json data = nlohmann::json::object();
        const auto returned = matches.size();
        data["matches"] = std::move(matches);
        data["matchCount"] = static_cast<UInt64>(returned);

        return ResultBuilder("Found " + std::to_string(result.Matches.size()) + " match(es); returned " +
                             std::to_string(returned) + ".")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterValidateDocument(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["office_version"] = Schema::EnumerationWithDefault(
            "Office generation the schema availability is checked against.",
            {"2007", "2010", "2013", "2016", "2019", "2021", "365"}, "365");
        properties["errors_only"] = Schema::BooleanWithDefault("Report errors and drop warnings.", false);
        properties["max_issues"] = Schema::IntegerWithDefault("Maximum number of issues to return.", 100, 1, 5000);

        nlohmann::json issue =
            Schema::Object("One validation issue.", {"severity", "message"},
                           nlohmann::json{{"severity", Schema::String("\"error\" or \"warning\".")},
                                          {"domain", Schema::String("Validation domain, such as \"opc\".")},
                                          {"id", Schema::String("Stable identifier of the rule.")},
                                          {"message", Schema::String("Human-readable description.")},
                                          {"partUri", Schema::String("Package part the issue belongs to.")}});

        auto definition = MakeReadDefinition(
            "validate_document", "Validate document",
            "Run OPC and schema validation over the document. This checks package and markup correctness, not full "
            "Microsoft Office compatibility; call it before saving a document you built with many edits.");
        definition.InputSchema = Schema::Object("Arguments of validate_document.", {}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Validation report.", {"errorCount", "warningCount", "issues"},
                                            nlohmann::json{{"errorCount", Schema::Integer("Number of errors.")},
                                                           {"warningCount", Schema::Integer("Number of warnings.")},
                                                           {"issues", Schema::Array("Reported issues.", std::move(issue))}}),
                             false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Validate(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Validate(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        Tools::ValidationRunOptions options;
        const auto version = Tools::ParseFileFormatVersion(arguments.value("office_version", std::string("365")));
        if (version.has_value())
        {
            options.TargetVersion = *version;
        }

        // Validated as it stands, not as it was last written: for an open
        // session those are different documents, and the one the agent is
        // asking about is the one in front of it.
        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::PackageLoadFailed, "The package could not be loaded for validation.");
        }

        const auto report = Tools::Run(*package, options);

        const bool errorsOnly = arguments.value("errors_only", false);
        const Size maximum = arguments.value("max_issues", static_cast<Size>(100));

        nlohmann::json issues = nlohmann::json::array();
        const bool loadCapped =
            SharedToolsetHelper::AppendValidationIssues(issues, report.LoadIssues, errorsOnly, maximum);
        const bool validationCapped =
            SharedToolsetHelper::AppendValidationIssues(issues, report.ValidationIssues, errorsOnly, maximum);

        const bool truncated = TruncateArrayToBudget(issues) || loadCapped || validationCapped;

        nlohmann::json data = nlohmann::json::object();
        data["errorCount"] = static_cast<UInt64>(report.ErrorCount);
        data["warningCount"] = static_cast<UInt64>(report.WarningCount);
        data["issues"] = std::move(issues);

        return ResultBuilder("Validation found " + std::to_string(report.ErrorCount) + " error(s) and " +
                             std::to_string(report.WarningCount) + " warning(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterQueryXml(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["xpath"] = Schema::String("XPath expression evaluated over the selected part.");
        properties["part"] = Schema::String("Part URI to query; omit for the main document part.");
        properties["namespaces"] = Schema::Array(
            "Extra prefix bindings layered over the well-known Open XML prefixes.",
            Schema::Object("One namespace binding.", {"prefix", "uri"},
                           nlohmann::json{{"prefix", Schema::String("Prefix used in the expression.")},
                                          {"uri", Schema::String("Namespace URI it binds to.")}}));
        properties["max"] = Schema::IntegerWithDefault("Maximum number of matches to return.", 100, 1, 5000);

        nlohmann::json match =
            Schema::Object("One matched element.", {"path", "name"},
                           nlohmann::json{{"path", Schema::String("Location of the element inside its part.")},
                                          {"name", Schema::String("Prefixed element name as written.")},
                                          {"text", Schema::String("Aggregated text of the element subtree.")},
                                          {"attributes", Schema::FreeObject("Attributes as name/value pairs.")}});

        auto definition = MakeReadDefinition(
            "query_xml", "Query document XML",
            "Run an XPath query over one XML part of the package. This is a read-only escape hatch for markup the "
            "typed tools do not expose; raw XML writing is deliberately not offered.");
        definition.InputSchema = Schema::Object("Arguments of query_xml.", {"xpath"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Query result.", {"matches"},
                           nlohmann::json{{"partName", Schema::String("Part that was queried.")},
                                          {"matches", Schema::Array("Matched elements.", std::move(match))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"xpath", "//w:p"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Query(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Query(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::PackageLoadFailed, "The package could not be read.");
        }

        Tools::QueryOptions options;
        options.PartName = arguments.value("part", std::string());
        options.MaxMatches = arguments.value("max", static_cast<Size>(100));

        const auto namespaces = arguments.find("namespaces");
        if (namespaces != arguments.end() && namespaces->is_array())
        {
            for (const auto& binding : *namespaces)
            {
                options.NamespaceBindings.emplace_back(binding.value("prefix", std::string()),
                                                       binding.value("uri", std::string()));
            }
        }

        const auto result = Tools::Query(*package, arguments.value("xpath", std::string()), options);
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The XPath query could not run.",
                                            result.Diagnostics,
                                            "Check the expression and that every prefix is bound.");
        }

        nlohmann::json matches = nlohmann::json::array();
        for (const auto& match : result.Matches)
        {
            nlohmann::json attributes = nlohmann::json::object();
            for (const auto& attribute : match.Attributes)
            {
                attributes[attribute.first] = attribute.second;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["path"] = match.Location;
            entry["name"] = match.Name;
            entry["text"] = match.Text;
            entry["attributes"] = std::move(attributes);
            matches.push_back(std::move(entry));
        }

        const bool truncated = TruncateArrayToBudget(matches);

        nlohmann::json data = nlohmann::json::object();
        const auto returned = matches.size();
        data["partName"] = result.PartName;
        data["matches"] = std::move(matches);

        return ResultBuilder("Matched " + std::to_string(returned) + " element(s) in " + result.PartName + ".")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterGetProperties(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        auto definition = MakeReadDefinition("get_properties", "Get document properties",
                                             "Read the core, extended, and custom document properties.");
        definition.InputSchema = Schema::Object("Arguments of get_properties.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Document properties.", {"core"},
                           nlohmann::json{{"core", Schema::FreeObject("Core and extended properties.")},
                                          {"custom", Schema::FreeObject("Custom properties by name.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Properties(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Properties(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The document has no package.");
        }

        nlohmann::json custom = nlohmann::json::object();
        Packaging::DocumentProperties documentProperties(*package);
        for (const auto& property : documentProperties.GetCustomProperties())
        {
            std::visit(
                [&custom, &property](const auto& value)
                {
                    using ValueType = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<ValueType, std::chrono::system_clock::time_point>)
                    {
                        custom[property.Name] = Packaging::DocumentProperties::FormatW3cDateTime(value);
                    }
                    else
                    {
                        custom[property.Name] = value;
                    }
                },
                property.Value);
        }

        nlohmann::json data = nlohmann::json::object();
        data["core"] = SharedToolsetHelper::CorePropertiesToJson(Tools::ReadCoreProperties(*package));
        data["custom"] = std::move(custom);

        return ResultBuilder("Read the document properties.").WithData(std::move(data)).Build();
    }

    static void RegisterListMedia(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json entry =
            Schema::Object("One media part.", {"id", "partUri", "contentType", "bytes"},
                           nlohmann::json{{"id", Schema::String("Identifier to pass to get_media.")},
                                          {"partUri", Schema::String("Part URI inside the package.")},
                                          {"contentType", Schema::String("Media type of the payload.")},
                                          {"bytes", Schema::Integer("Payload size in bytes.")}});

        auto definition = MakeReadDefinition("list_media", "List media",
                                             "List every image, audio, video, and embedded-object part of the "
                                             "document without transferring any payload.");
        definition.InputSchema = Schema::Object("Arguments of list_media.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Media inventory.", {"media"},
                           nlohmann::json{{"media", Schema::Array("Media parts.", std::move(entry))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListMedia(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListMedia(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The document has no package.");
        }

        nlohmann::json media = nlohmann::json::array();
        Size index = 0;
        for (const auto& part : SharedToolsetHelper::CollectMediaParts(*package))
        {
            ++index;
            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = "media" + std::to_string(index);
            entry["partUri"] = part->Uri();
            entry["contentType"] = std::string(part->ContentType());
            entry["bytes"] = static_cast<UInt64>(part->GetBinaryData().size());
            media.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto count = media.size();
        data["media"] = std::move(media);

        return ResultBuilder("The document holds " + std::to_string(count) + " media part(s).")
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterGetMedia(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["media_id"] = Schema::String("Identifier or part URI reported by list_media.");

        auto definition =
            MakeReadDefinition("get_media", "Get media payload",
                               "Return one image or audio payload as a content block plus its metadata. Other media, "
                               "such as video, OLE objects and embedded packages, only travel through export_media. "
                               "Payloads larger than the configured media limit are refused.");
        definition.InputSchema = Schema::Object("Arguments of get_media.", {"media_id"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Media payload.", {"contentType", "bytes"},
                           nlohmann::json{{"id", Schema::String("Identifier of the returned part.")},
                                          {"partUri", Schema::String("Part URI inside the package.")},
                                          {"contentType", Schema::String("Media type of the payload.")},
                                          {"bytes", Schema::Integer("Payload size in bytes.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"media_id", "media1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return GetMedia(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome GetMedia(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The document has no package.");
        }

        const auto mediaId = arguments.value("media_id", std::string());
        const auto parts = SharedToolsetHelper::CollectMediaParts(*package);

        std::shared_ptr<OpenXmlPackagePart> selected;
        Size selectedIndex = 0;
        for (Size index = 0; index < parts.size(); ++index)
        {
            const auto synthetic = "media" + std::to_string(index + 1);
            if (mediaId == synthetic || mediaId == parts[index]->Uri())
            {
                selected = parts[index];
                selectedIndex = index + 1;
                break;
            }
        }

        if (selected == nullptr)
        {
            return MakeError(ErrorCode::MediaNotFound, "No media part matches '" + mediaId + "'.", mediaId,
                             "Call list_media to see the available identifiers.");
        }

        const std::string contentType(selected->ContentType());
        if (!SharedToolsetHelper::IsInlineMediaType(contentType))
        {
            // Video, OLE objects and embedded packages have no typed content
            // block. Handing them over as an embedded resource would promise a
            // `resources` capability this server does not have. Decided before
            // the payload is read, because it is refused whatever its size.
            return MakeError(ErrorCode::Unsupported,
                             "A payload of type '" + contentType + "' cannot travel in a content block.", mediaId,
                             "Use export_media to write the payload to a workspace file instead.");
        }

        auto bytes = selected->GetBinaryData();
        if (bytes.size() > context.MaxMediaBytes())
        {
            return MakeError(ErrorCode::OperationFailed,
                             "The media payload exceeds the configured media limit of " +
                                 std::to_string(context.MaxMediaBytes()) + " bytes.",
                             mediaId, "Use export_media to write the payload to a workspace file instead.");
        }

        nlohmann::json data = nlohmann::json::object();
        data["id"] = "media" + std::to_string(selectedIndex);
        data["partUri"] = selected->Uri();
        data["contentType"] = contentType;
        data["bytes"] = static_cast<UInt64>(bytes.size());

        auto outcome = ResultBuilder("Returned " + selected->Uri() + ".").WithData(std::move(data)).Build();
        outcome.ExtraContent.push_back(SharedToolsetHelper::MakeMediaContentBlock(contentType, bytes));
        return outcome;
    }
};

/// Registration of the `edit` group plus the deliberately transactional `batch`.
class EditTools
{
public:
    static void Register(ToolRegistry& registry)
    {
        RegisterReplaceText(registry);
        RegisterSetProperties(registry);
        RegisterBatch(registry);
    }

    /**
     * @brief Decides whether a tool may appear inside a batch.
     *
     * A batch is a single transaction over one open document. Lifecycle tools
     * would change which document that is, file utilities work outside the
     * session entirely, reading tools add nothing, and a nested batch would
     * make the rollback boundary ambiguous.
     */
    static bool IsBatchable(const ToolDefinition& definition)
    {
        if (definition.Annotations.ReadOnly || definition.Name == "batch")
        {
            return false;
        }

        return definition.Group != "lifecycle" && definition.Group != "files" && definition.Group != "read";
    }

private:
    static void RegisterReplaceText(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["needle"] = Schema::String("Text or regular expression to replace.");
        properties["replacement"] =
            Schema::String("Replacement text; with regex enabled it may reference groups as $1, $2, ...");
        properties["regex"] =
            Schema::BooleanWithDefault("Treat 'needle' as an ECMAScript regular expression.", false);
        properties["ignore_case"] = Schema::BooleanWithDefault("Match case-insensitively.", false);
        properties["dry_run"] =
            Schema::BooleanWithDefault("Count the matches without changing the document.", false);

        nlohmann::json data =
            Schema::Object("Replacement result.", {"replacementCount"},
                           nlohmann::json{{"replacementCount", Schema::Integer("Number of replacements applied.")},
                                          {"skippedMatches",
                                           Schema::Integer("Matches deliberately left alone, such as Excel matches "
                                                           "inside non-text cells.")},
                                          {"dryRun", Schema::Boolean("True when nothing was changed.")}});

        ToolDefinition definition;
        definition.Name = "replace_text";
        definition.Title = "Replace text";
        definition.Description =
            "Replace text throughout the document, including tables, headers, notes, and comments. Run it with "
            "dry_run first to see how many matches it would touch.";
        definition.Group = "edit";
        definition.InputSchema =
            Schema::Object("Arguments of replace_text.", {"documentId", "needle", "replacement"},
                           std::move(properties));
        definition.OutputSchema = Schema::Envelope(std::move(data), true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"needle", "2025"}, {"replacement", "2026"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ReplaceText(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ReplaceText(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto* session = ToolSupport::RequireSession(context, arguments, failure);
        if (session == nullptr)
        {
            return failure;
        }

        const bool dryRun = arguments.value("dry_run", false);

        // The replacement now runs on the session's own document rather than on
        // an exported copy, so the guard has to be in place before it starts: it
        // is the only thing that puts the document back if the replacement fails
        // partway. A dry run changes nothing and pays for no snapshot.
        std::optional<MutationGuard> guard;
        if (!dryRun)
        {
            guard.emplace(*session);
        }

        const auto result = context.Adapter().ReplaceText(
            session->Document(), arguments.value("needle", std::string()),
            arguments.value("replacement", std::string()), dryRun, arguments.value("regex", false),
            arguments.value("ignore_case", false));
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The replacement could not run.",
                                            result.Diagnostics,
                                            "Check that 'needle' is not empty and that a regex is well formed.");
        }

        if (guard.has_value())
        {
            guard->Commit();
        }

        nlohmann::json data = nlohmann::json::object();
        data["replacementCount"] = static_cast<UInt64>(result.ReplacementCount);
        data["skippedMatches"] = static_cast<UInt64>(result.SkippedMatches);
        data["dryRun"] = dryRun;

        return ResultBuilder((dryRun ? "Would replace " : "Replaced ") +
                             std::to_string(result.ReplacementCount) + " occurrence(s).")
            .WithSession(*session)
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }

    static void RegisterSetProperties(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["title"] = Schema::String("Core property dc:title.");
        properties["creator"] = Schema::String("Core property dc:creator.");
        properties["subject"] = Schema::String("Core property dc:subject.");
        properties["keywords"] = Schema::String("Core property cp:keywords.");
        properties["description"] = Schema::String("Core property dc:description.");
        properties["category"] = Schema::String("Core property cp:category.");
        properties["company"] = Schema::String("Extended property Company.");
        properties["custom"] = Schema::FreeObject("Custom properties as a name to string/number/boolean map.");

        ToolDefinition definition;
        definition.Name = "set_properties";
        definition.Title = "Set document properties";
        definition.Description =
            "Write core, extended, and custom document properties. Only the members you pass are changed.";
        definition.Group = "edit";
        definition.InputSchema =
            Schema::Object("Arguments of set_properties.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Written properties.", {"written"},
                           nlohmann::json{{"written", Schema::Array("Names of the properties that were written.",
                                                                    Schema::String("Property name."))}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"title", "Quarterly report"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetProperties(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetProperties(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto* session = ToolSupport::RequireSession(context, arguments, failure);
        if (session == nullptr)
        {
            return failure;
        }

        auto package = session->Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The document has no package.", session->Id());
        }

        MutationGuard guard(*session);
        Packaging::DocumentProperties documentProperties(*package);

        nlohmann::json written = nlohmann::json::array();
        static constexpr std::string_view coreNames[] = {"title", "creator", "subject", "keywords",
                                                         "description", "category", "company"};
        for (const auto name : coreNames)
        {
            const std::string key(name);
            const auto member = arguments.find(key);
            if (member == arguments.end() || !member->is_string())
            {
                continue;
            }

            if (Tools::WriteCoreProperty(*package, key, member->get<std::string>()))
            {
                written.push_back(key);
            }
        }

        const auto custom = arguments.find("custom");
        if (custom != arguments.end() && custom->is_object())
        {
            for (const auto& [key, value] : custom->items())
            {
                bool ok = false;
                if (value.is_string())
                {
                    ok = documentProperties.SetCustomProperty(key, value.get<std::string>());
                }
                else if (value.is_boolean())
                {
                    ok = documentProperties.SetCustomProperty(key, value.get<bool>());
                }
                else if (value.is_number_integer())
                {
                    ok = documentProperties.SetCustomProperty(key, static_cast<Int32>(value.get<Int64>()));
                }
                else if (value.is_number())
                {
                    ok = documentProperties.SetCustomProperty(key, value.get<Real>());
                }

                if (ok)
                {
                    written.push_back("custom." + key);
                }
            }
        }

        if (written.empty())
        {
            return MakeError(ErrorCode::OperationFailed, "No property could be written.", session->Id(),
                             "The package may have no properties part; check get_properties.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        const auto count = written.size();
        data["written"] = std::move(written);

        return ResultBuilder("Wrote " + std::to_string(count) + " property value(s).")
            .WithSession(*session)
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterBatch(ToolRegistry& registry)
    {
        nlohmann::json operation =
            Schema::Object("One queued tool call.", {"tool", "arguments"},
                           nlohmann::json{{"tool", Schema::String("Name of a mutating session tool.")},
                                          {"arguments", Schema::FreeObject("Arguments without 'documentId'.")}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["operations"] =
            Schema::Array("Operations applied in order, all or nothing.", std::move(operation), 50);

        nlohmann::json result =
            Schema::Object("One applied operation.", {"tool", "summary"},
                           nlohmann::json{{"tool", Schema::String("Tool that ran.")},
                                          {"summary", Schema::String("Summary the tool reported.")},
                                          {"data", Schema::FreeObject("Key indices or addresses it returned.")}});

        ToolDefinition definition;
        definition.Name = "batch";
        definition.Title = "Apply a batch of edits";
        definition.Description =
            "Apply several mutating tools to one document as a single transaction. The first failure rolls the "
            "document back completely, and a successful batch counts as one undo step.";
        definition.Group = "edit";
        definition.InputSchema =
            Schema::Object("Arguments of batch.", {"documentId", "operations"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Batch result.", {"applied", "results"},
                           nlohmann::json{{"applied", Schema::Integer("Number of applied operations.")},
                                          {"results", Schema::Array("Per-operation summaries.", std::move(result))}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"operations", nlohmann::json::array({nlohmann::json{{"tool", "set_properties"},
                                                                 {"arguments", nlohmann::json{{"title", "Report"}}}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Batch(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Batch(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto* session = ToolSupport::RequireSession(context, arguments, failure);
        if (session == nullptr)
        {
            return failure;
        }

        const auto* registry = context.Registry();
        if (registry == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The tool registry is unavailable.");
        }

        const auto operations = arguments.find("operations");
        if (operations == arguments.end() || !operations->is_array() || operations->empty())
        {
            return MakeError(ErrorCode::InputInvalid, "'operations' must be a non-empty array.");
        }

        // Every operation is checked before the first one runs, so an
        // unsupported call never leaves the document half-edited.
        for (Size index = 0; index < operations->size(); ++index)
        {
            const auto& operation = (*operations)[index];
            const auto toolName = operation.value("tool", std::string());
            const auto* tool = registry->Find(toolName);
            if (tool == nullptr)
            {
                return MakeError(ErrorCode::InputInvalid, "Unknown tool '" + toolName + "' in operation " + std::to_string(index + 1) + ".",
                                 toolName, "Call tools/list to see what this server offers.");
            }

            if (!IsBatchable(tool->Definition))
            {
                return MakeError(ErrorCode::InputInvalid,
                                 "'" + toolName + "' cannot run inside a batch; only mutating session tools can.",
                                 toolName, "Call the tool on its own instead.");
            }
        }

        auto marker = session->BeginBatch();
        if (!marker.has_value())
        {
            return MakeError(ErrorCode::SnapshotUnavailable,
                             "The document could not be captured for a transactional batch.", session->Id());
        }

        nlohmann::json results = nlohmann::json::array();
        for (Size index = 0; index < operations->size(); ++index)
        {
            const auto& operation = (*operations)[index];
            const auto toolName = operation.value("tool", std::string());
            const auto* tool = registry->Find(toolName);

            nlohmann::json callArguments = nlohmann::json::object();
            const auto supplied = operation.find("arguments");
            if (supplied != operation.end() && supplied->is_object())
            {
                callArguments = *supplied;
            }

            callArguments["documentId"] = session->Id();

            auto outcome = registry->Call(context, *tool, callArguments);
            if (outcome.IsError)
            {
                const bool restored = session->RollbackBatch(*marker);
                if (!restored)
                {
                    Log::Error("Rolling back the batch on '" + session->Id() +
                               "' failed; the session may hold a partially applied batch.");
                    session->MarkRollbackFailed();
                }

                nlohmann::json details = nlohmann::json::object();
                details["failedIndex"] = static_cast<UInt64>(index);
                details["tool"] = toolName;
                details["error"] = outcome.Structured.value("error", nlohmann::json::object());
                details["restored"] = restored;

                const std::string outcomeText =
                    restored ? "the document was restored."
                             : "the document could NOT be restored and may hold partial changes.";

                return MakeError(ErrorCode::BatchAborted,
                                 "Operation " + std::to_string(index + 1) + " ('" + toolName + "') failed; " +
                                     outcomeText,
                                 toolName,
                                 restored ? "Fix that operation and resend the batch."
                                          : "Close the document with discard and reopen it before editing further.",
                                 nlohmann::json::array({std::move(details)}));
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["tool"] = toolName;
            entry["summary"] = outcome.Structured.value("summary", std::string());
            entry["data"] = outcome.Structured.value("data", nlohmann::json::object());
            results.push_back(std::move(entry));
        }

        session->CommitBatch(std::move(*marker));

        nlohmann::json data = nlohmann::json::object();
        const auto applied = results.size();
        data["applied"] = static_cast<UInt64>(applied);
        data["results"] = std::move(results);

        return ResultBuilder("Applied " + std::to_string(applied) + " operation(s) as one change.")
            .WithSession(*session)
            .WithData(std::move(data))
            .Build();
    }
};

/// Registration of the `files` group: path-to-path utilities that open no session.
class FileTools
{
public:
    static void Register(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        RegisterListWorkspace(registry, adapter);
        RegisterConvertDocument(registry, adapter);
        RegisterDiffDocuments(registry);
        RegisterMergeDocuments(registry, adapter);
        RegisterSplitDocument(registry, adapter);
        RegisterRedactDocument(registry, adapter);
        RegisterExportMedia(registry);
    }

private:
    static void RegisterListWorkspace(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        const auto defaultGlob = "*" + adapter.FileExtension();

        nlohmann::json properties = nlohmann::json::object();
        properties["glob"] = Schema::StringWithDefault(
            "Case-insensitive glob over workspace-relative paths; supports *, **, and ?.", defaultGlob);
        properties["limit"] = Schema::IntegerWithDefault("Maximum number of files to return.", 200, 1, 5000);

        nlohmann::json entry =
            Schema::Object("One workspace file.", {"path", "bytes"},
                           nlohmann::json{{"path", Schema::String("Workspace-relative path.")},
                                          {"bytes", Schema::Integer("File size in bytes.")},
                                          {"modified", Schema::String("Last write time, ISO 8601 UTC.")}});

        ToolDefinition definition;
        definition.Name = "list_workspace";
        definition.Title = "List workspace files";
        definition.Description = "List files inside the configured workspace. The default glob shows only " +
                                 adapter.FileExtension() + " documents.";
        definition.Group = "files";
        definition.InputSchema = Schema::Object("Arguments of list_workspace.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Workspace listing.", {"files"},
                           nlohmann::json{{"files", Schema::Array("Matching files.", std::move(entry))},
                                          {"roots", Schema::Array("Configured workspace roots.",
                                                                  Schema::String("Absolute root path."))}}),
            false);
        definition.Example = nlohmann::json{{"glob", defaultGlob}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [defaultGlob](ToolContext& context, const nlohmann::json& arguments)
        { return ListWorkspace(context, arguments, defaultGlob); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListWorkspace(ToolContext& context, const nlohmann::json& arguments,
                                     const std::string& defaultGlob)
    {
        const auto glob = arguments.value("glob", defaultGlob);
        const Size limit = arguments.value("limit", static_cast<Size>(200));

        const auto listing = context.GetWorkspace().List(glob, limit);

        nlohmann::json files = nlohmann::json::array();
        for (const auto& file : listing.Files)
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["path"] = file.Path;
            entry["bytes"] = file.Bytes;
            entry["modified"] = file.Modified;
            files.push_back(std::move(entry));
        }

        nlohmann::json roots = nlohmann::json::array();
        for (const auto& root : context.GetWorkspace().Roots())
        {
            roots.push_back(root.generic_string());
        }

        const bool truncated = TruncateArrayToBudget(files) || listing.Truncated;

        nlohmann::json data = nlohmann::json::object();
        const auto count = files.size();
        data["files"] = std::move(files);
        data["roots"] = std::move(roots);

        return ResultBuilder("Found " + std::to_string(count) + " file(s) matching '" + glob + "'.")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterConvertDocument(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        const auto formats = SharedToolsetHelper::ConversionFormats(adapter.Family());

        nlohmann::json properties = nlohmann::json::object();
        properties["input_path"] = Schema::String("Workspace-relative source file.");
        properties["output_path"] = Schema::String("Workspace-relative destination file.");
        properties["from"] = Schema::Enumeration("Input format; omit to infer it from the extension.", formats);
        properties["to"] = Schema::Enumeration("Output format; omit to infer it from the extension.", formats);
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing existing output files.", false);
        properties["media_dir"] =
            Schema::String("Directory for external media when converting to a text format.");
        properties["embed_media"] =
            Schema::BooleanWithDefault("Embed media as base64 into JSON or XML output.", false);
        properties["no_media"] = Schema::BooleanWithDefault("Drop media entirely.", false);
        if (adapter.Family() == Tools::DocumentFamily::Excel)
        {
            properties["csv_separator"] = Schema::StringWithDefault("Field separator for CSV input or output.", ",");
            // A name, never an index: this tool works file to file and holds no
            // open workbook to resolve a position against.
            properties["sheet"] = Schema::String(
                "Name of the worksheet exported to CSV, or the name given to a worksheet imported from CSV. "
                "Unlike the session tools, this one does not accept a sheet index.");
        }

        ToolDefinition definition;
        definition.Name = "convert_document";
        definition.Title = "Convert document";
        definition.Description =
            "Convert between " + adapter.FamilyName() +
            " packages and the text formats Markdown, JSON, plain text, and semantic XML. Package to package "
            "conversion is rejected; use merge_documents or split_document for that.";
        definition.Group = "files";
        definition.InputSchema = Schema::Object("Arguments of convert_document.", {"input_path", "output_path"},
                                                std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Conversion result.", {"outputPath"},
                           nlohmann::json{{"outputPath", Schema::String("Workspace-relative output file.")},
                                          {"family", Schema::String("Detected document family.")},
                                          {"from", Schema::String("Effective input format.")},
                                          {"to", Schema::String("Effective output format.")},
                                          {"mediaFiles", Schema::Array("External media files written.",
                                                                       Schema::String("Workspace-relative path."))}}),
            false);
        definition.Example = nlohmann::json{{"input_path", "report" + adapter.FileExtension()},
                                            {"output_path", "report.md"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Convert(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Convert(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto input = ToolSupport::ResolveExistingFile(context, arguments.value("input_path", std::string()), failure);
        if (!input.has_value())
        {
            return failure;
        }

        const bool overwrite = arguments.value("overwrite", false);
        auto output =
            ToolSupport::ResolveOutputPath(context, arguments.value("output_path", std::string()), overwrite, failure);
        if (!output.has_value())
        {
            return failure;
        }

        Tools::ConvertOptions options;
        options.From = Tools::ParseConvertFormat(arguments.value("from", std::string()));
        options.To = Tools::ParseConvertFormat(arguments.value("to", std::string()));
        options.EmbedMedia = arguments.value("embed_media", false);
        options.NoMedia = arguments.value("no_media", false);
        options.Overwrite = overwrite;
        options.CsvSeparator = arguments.value("csv_separator", std::string(","));
        options.SheetName = arguments.value("sheet", std::string());

        const auto mediaDir = arguments.value("media_dir", std::string());
        if (!mediaDir.empty())
        {
            auto resolved = ToolSupport::ResolvePath(context, mediaDir, failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            options.MediaDir = *resolved;
        }

        std::error_code error;
        if (output->has_parent_path())
        {
            std::filesystem::create_directories(output->parent_path(), error);
        }

        const auto result = Tools::ConvertDocument(*input, *output, options);
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(result.UsageOk ? ErrorCode::OperationFailed : ErrorCode::InputInvalid,
                                            "The conversion failed.", result.Diagnostics,
                                            "Check that the format pair is supported for this family.");
        }

        nlohmann::json mediaFiles = nlohmann::json::array();
        for (const auto& item : result.MediaItems)
        {
            mediaFiles.push_back(context.GetWorkspace().Relativize(item.OutputPath));
        }

        nlohmann::json data = nlohmann::json::object();
        data["outputPath"] = context.GetWorkspace().Relativize(*output);
        data["family"] = ToolSupport::FamilyToken(result.Family);
        data["from"] = std::string(Tools::ToString(result.From));
        data["to"] = std::string(Tools::ToString(result.To));
        data["mediaFiles"] = std::move(mediaFiles);

        return ResultBuilder("Converted to " + context.GetWorkspace().Relativize(*output) + ".")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }

    static void RegisterDiffDocuments(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["left_path"] = Schema::String("Workspace-relative first package.");
        properties["right_path"] = Schema::String("Workspace-relative second package.");
        properties["parts_only"] =
            Schema::BooleanWithDefault("Report only part changes, omitting relationship changes.", false);

        nlohmann::json partChange =
            Schema::Object("One changed part.", {"uri", "kind"},
                           nlohmann::json{{"uri", Schema::String("Part URI.")},
                                          {"kind", Schema::String("added, removed, changed, or contentType.")},
                                          {"firstDifference", Schema::String("Approximate path of the first XML "
                                                                             "difference.")}});
        nlohmann::json relationshipChange =
            Schema::Object("One changed relationship.", {"sourceUri", "relationshipId", "kind"},
                           nlohmann::json{{"sourceUri", Schema::String("Container URI.")},
                                          {"relationshipId", Schema::String("Relationship identifier.")},
                                          {"kind", Schema::String("added, removed, or changed.")}});

        ToolDefinition definition;
        definition.Name = "diff_documents";
        definition.Title = "Diff documents";
        definition.Description =
            "Compare two packages part by part and relationship by relationship. Use it to see exactly what an "
            "edit changed inside the package.";
        definition.Group = "files";
        definition.InputSchema =
            Schema::Object("Arguments of diff_documents.", {"left_path", "right_path"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Diff result.", {"identical"},
                                            nlohmann::json{{"identical", Schema::Boolean("True when nothing differs.")},
                                                           {"partChanges", Schema::Array("Changed parts.",
                                                                                         std::move(partChange))},
                                                           {"relationshipChanges",
                                                            Schema::Array("Changed relationships.",
                                                                          std::move(relationshipChange))}}),
                             false);
        definition.Example = nlohmann::json{{"left_path", "before.docx"}, {"right_path", "after.docx"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Diff(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::string PartChangeToken(Tools::PartChangeKind kind)
    {
        switch (kind)
        {
            case Tools::PartChangeKind::Added:
                return "added";
            case Tools::PartChangeKind::Removed:
                return "removed";
            case Tools::PartChangeKind::ChangedXml:
                return "changedXml";
            case Tools::PartChangeKind::ChangedBinary:
                return "changedBinary";
            case Tools::PartChangeKind::ContentTypeChanged:
                return "contentTypeChanged";
        }

        return "changed";
    }

    static std::string RelationshipChangeToken(Tools::RelationshipChangeKind kind)
    {
        switch (kind)
        {
            case Tools::RelationshipChangeKind::Added:
                return "added";
            case Tools::RelationshipChangeKind::Removed:
                return "removed";
            case Tools::RelationshipChangeKind::Changed:
                return "changed";
        }

        return "changed";
    }

    static ToolOutcome Diff(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto left = ToolSupport::ResolveExistingFile(context, arguments.value("left_path", std::string()), failure);
        if (!left.has_value())
        {
            return failure;
        }

        auto right = ToolSupport::ResolveExistingFile(context, arguments.value("right_path", std::string()), failure);
        if (!right.has_value())
        {
            return failure;
        }

        const auto result = Tools::Compare(*left, *right);
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The packages could not be compared.",
                                            result.Diagnostics);
        }

        nlohmann::json partChanges = nlohmann::json::array();
        for (const auto& change : result.PartChanges)
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["uri"] = change.Uri;
            entry["kind"] = PartChangeToken(change.Kind);
            entry["firstDifference"] = change.FirstDifferencePath;
            partChanges.push_back(std::move(entry));
        }

        nlohmann::json relationshipChanges = nlohmann::json::array();
        if (!arguments.value("parts_only", false))
        {
            for (const auto& change : result.RelationshipChanges)
            {
                nlohmann::json entry = nlohmann::json::object();
                entry["sourceUri"] = change.SourceUri;
                entry["relationshipId"] = change.RelationshipId;
                entry["kind"] = RelationshipChangeToken(change.Kind);
                relationshipChanges.push_back(std::move(entry));
            }
        }

        bool truncated = TruncateArrayToBudget(partChanges, ResponseBudgetBytes / 2);
        truncated = TruncateArrayToBudget(relationshipChanges, ResponseBudgetBytes / 2) || truncated;

        nlohmann::json data = nlohmann::json::object();
        data["identical"] = result.Identical;
        data["partChanges"] = std::move(partChanges);
        data["relationshipChanges"] = std::move(relationshipChanges);

        return ResultBuilder(result.Identical ? "The two packages are identical."
                                              : "The two packages differ in " +
                                                    std::to_string(result.PartChanges.size()) + " part(s).")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterMergeDocuments(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["input_paths"] = Schema::Array("Workspace-relative source packages, in merge order.",
                                                  Schema::String("Workspace-relative package."));
        properties["output_path"] =
            Schema::String("Workspace-relative destination package; an extension of another Office family is "
                           "refused.");
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing an existing output file.", false);
        if (adapter.Family() == Tools::DocumentFamily::Word)
        {
            properties["insert_page_breaks"] =
                Schema::BooleanWithDefault("Insert a page break between consecutive documents.", true);
            properties["style_conflict"] = Schema::EnumerationWithDefault(
                "How a colliding style identifier is imported.", {"rename", "keep", "replace"}, "rename");
        }

        ToolDefinition definition;
        definition.Name = "merge_documents";
        definition.Title = "Merge documents";
        definition.Description = "Merge several " + adapter.FamilyName() +
                                 " packages into one new file, importing styles, media, and every other "
                                 "dependency. The inputs are never modified.";
        definition.Group = "files";
        definition.InputSchema =
            Schema::Object("Arguments of merge_documents.", {"input_paths", "output_path"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Merge result.", {"outputPath", "documentsMerged"},
                           nlohmann::json{{"outputPath", Schema::String("Workspace-relative output package.")},
                                          {"documentsMerged", Schema::Integer("Number of merged inputs.")},
                                          {"itemsMerged", Schema::Integer("Bodies, worksheets, or slides appended.")}}),
            false);
        definition.Example =
            nlohmann::json{{"input_paths", nlohmann::json::array({"a" + adapter.FileExtension(),
                                                                  "b" + adapter.FileExtension()})},
                           {"output_path", "merged" + adapter.FileExtension()}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Merge(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Merge(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        std::vector<std::filesystem::path> inputs;
        const auto inputPaths = arguments.find("input_paths");
        if (inputPaths == arguments.end() || !inputPaths->is_array() || inputPaths->empty())
        {
            return MakeError(ErrorCode::InputInvalid, "'input_paths' must list at least one package.");
        }

        for (const auto& entry : *inputPaths)
        {
            auto resolved = ToolSupport::ResolveExistingFile(context, entry.get<std::string>(), failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            inputs.push_back(*resolved);
        }

        const bool overwrite = arguments.value("overwrite", false);
        const auto outputPath = arguments.value("output_path", std::string());
        auto output = ToolSupport::ResolveOutputPath(context, outputPath, overwrite, failure);
        if (!output.has_value())
        {
            return failure;
        }

        if (!ToolSupport::CheckDestinationFamily(context, outputPath, failure))
        {
            return failure;
        }

        Tools::DocumentMergeOptions options;
        options.Overwrite = overwrite;
        options.InsertWordPageBreaks = arguments.value("insert_page_breaks", true);
        const auto conflict = arguments.value("style_conflict", std::string("rename"));
        if (conflict == "keep")
        {
            options.WordStyleConflictPolicy = Word::StyleCopyConflictPolicy::KeepExisting;
        }
        else if (conflict == "replace")
        {
            options.WordStyleConflictPolicy = Word::StyleCopyConflictPolicy::Replace;
        }

        std::error_code error;
        if (output->has_parent_path())
        {
            std::filesystem::create_directories(output->parent_path(), error);
        }

        const auto result = Tools::MergeDocuments(inputs, *output, options);
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The documents could not be merged.",
                                            result.Diagnostics,
                                            "Every input must belong to the same family as this server.");
        }

        nlohmann::json data = nlohmann::json::object();
        data["outputPath"] = context.GetWorkspace().Relativize(*output);
        data["documentsMerged"] = static_cast<UInt64>(result.DocumentsMerged);
        data["itemsMerged"] = static_cast<UInt64>(result.ItemsMerged);

        return ResultBuilder("Merged " + std::to_string(result.DocumentsMerged) + " document(s) into " +
                             context.GetWorkspace().Relativize(*output) + ".")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }

    /// Split strategies that mean something for the server's family.
    static std::vector<std::string> SplitStrategies(Tools::DocumentFamily family)
    {
        switch (family)
        {
            case Tools::DocumentFamily::Word:
                return {"auto", "sections", "pages", "paragraphs", "marker"};
            case Tools::DocumentFamily::Excel:
                return {"auto", "worksheets"};
            case Tools::DocumentFamily::PowerPoint:
                return {"auto", "slides"};
            case Tools::DocumentFamily::Unknown:
                break;
        }

        return {"auto"};
    }

    static Tools::DocumentSplitStrategy ParseSplitStrategy(const std::string& token)
    {
        if (token == "sections")
        {
            return Tools::DocumentSplitStrategy::SectionBreaks;
        }

        if (token == "pages")
        {
            return Tools::DocumentSplitStrategy::PageBreaks;
        }

        if (token == "paragraphs")
        {
            return Tools::DocumentSplitStrategy::ParagraphCount;
        }

        if (token == "marker")
        {
            return Tools::DocumentSplitStrategy::Marker;
        }

        if (token == "worksheets")
        {
            return Tools::DocumentSplitStrategy::Worksheets;
        }

        if (token == "slides")
        {
            return Tools::DocumentSplitStrategy::Slides;
        }

        return Tools::DocumentSplitStrategy::Auto;
    }

    static void RegisterSplitDocument(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["input_path"] = Schema::String("Workspace-relative package to split.");
        properties["output_dir"] = Schema::String("Workspace-relative directory for the numbered outputs.");
        properties["by"] = Schema::EnumerationWithDefault("Boundary the split follows.",
                                                          SplitStrategies(adapter.Family()), "auto");
        properties["count"] =
            Schema::Integer("Items per output package; paragraphs, worksheets, or slides, depending on 'by'.", 1);
        properties["marker"] = Schema::String("Exact paragraph text that starts a new part (Word 'marker' only).");
        properties["prefix"] =
            Schema::StringWithDefault("File name prefix of the outputs; a plain name, no directory.", "part");
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing existing numbered outputs.", false);

        ToolDefinition definition;
        definition.Name = "split_document";
        definition.Title = "Split document";
        definition.Description = "Split one " + adapter.FamilyName() +
                                 " package into several numbered files along a structural boundary. The source is "
                                 "opened read-only and never modified.";
        definition.Group = "files";
        definition.InputSchema =
            Schema::Object("Arguments of split_document.", {"input_path", "output_dir"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Split result.", {"outputFiles"},
                           nlohmann::json{{"outputFiles", Schema::Array("Written files in content order.",
                                                                        Schema::String("Workspace-relative path."))},
                                          {"family", Schema::String("Detected document family.")}}),
            false);
        definition.Example =
            nlohmann::json{{"input_path", "report" + adapter.FileExtension()}, {"output_dir", "parts"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Split(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Split(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto input = ToolSupport::ResolveExistingFile(context, arguments.value("input_path", std::string()), failure);
        if (!input.has_value())
        {
            return failure;
        }

        auto outputDirectory = ToolSupport::ResolvePath(context, arguments.value("output_dir", std::string()), failure);
        if (!outputDirectory.has_value())
        {
            return failure;
        }

        // The prefix is concatenated into the output file names, so it is a path
        // fragment the workspace never sees: a prefix of "../../evil" would place
        // the numbered files outside the directory that was resolved above.
        const auto prefix = arguments.value("prefix", std::string("part"));
        if (!Workspace::IsAcceptableName(prefix))
        {
            return MakeError(ErrorCode::InputInvalid,
                             "The prefix names the output files and must be a plain file-name fragment.", prefix,
                             "Pass a name without a path separator or '..'; output_dir chooses the directory.");
        }

        Tools::DocumentSplitOptions options;
        options.Strategy = ParseSplitStrategy(arguments.value("by", std::string("auto")));
        options.ItemCount = arguments.value("count", static_cast<Size>(0));
        options.Marker = arguments.value("marker", std::string());
        options.OutputPrefix = prefix;
        options.Overwrite = arguments.value("overwrite", false);

        const auto result = Tools::SplitDocument(*input, *outputDirectory, options);
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The document could not be split.",
                                            result.Diagnostics,
                                            "Check that 'by' matches the family and that 'count' or 'marker' is set "
                                            "where the strategy needs it.");
        }

        nlohmann::json outputFiles = nlohmann::json::array();
        for (const auto& file : result.OutputFiles)
        {
            outputFiles.push_back(context.GetWorkspace().Relativize(file));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto count = outputFiles.size();
        data["outputFiles"] = std::move(outputFiles);
        data["family"] = ToolSupport::FamilyToken(result.Family);

        return ResultBuilder("Wrote " + std::to_string(count) + " part(s).")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }

    static void RegisterRedactDocument(ToolRegistry& registry, const FamilyAdapter& adapter)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["input_path"] =
            Schema::String("Workspace-relative source package; mutually exclusive with 'documentId'.");
        properties["output_path"] =
            Schema::String("Workspace-relative destination; with 'input_path' it defaults to overwriting the "
                           "source. An extension of another Office family is refused.");
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing an existing output file.", false);
        properties["keep_comments"] = Schema::BooleanWithDefault("Leave comments in place.", false);
        properties["keep_revisions"] = Schema::BooleanWithDefault("Leave tracked revisions unresolved.", false);
        properties["keep_hidden_text"] = Schema::BooleanWithDefault("Leave hidden-text runs in place.", false);
        properties["keep_personal_metadata"] =
            Schema::BooleanWithDefault("Leave identity properties and custom properties in place.", false);

        ToolDefinition definition;
        definition.Name = "redact_document";
        definition.Title = "Redact document";
        definition.Description =
            "Remove review and identity artifacts before publication: comments, tracked revisions, hidden text, "
            "and personal metadata. This is structural scrubbing, not content search; combine it with "
            "replace_text for sensitive words.";
        definition.Group = "files";
        definition.InputSchema = Schema::Object("Arguments of redact_document.", {}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Redaction result.", {"commentsRemoved"},
                                            nlohmann::json{{"commentsRemoved", Schema::Integer("Comments removed.")},
                                                           {"revisionsResolved",
                                                            Schema::Integer("Tracked revisions accepted.")},
                                                           {"hiddenRunsRemoved",
                                                            Schema::Integer("Hidden-text runs deleted.")},
                                                           {"metadataFieldsCleared",
                                                            Schema::Integer("Identity fields cleared.")},
                                                           {"outputPath",
                                                            Schema::String("File written, when working on paths.")}}),
                             false);
        definition.Example = nlohmann::json{{"input_path", "draft" + adapter.FileExtension()},
                                            {"output_path", "public" + adapter.FileExtension()}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Redact(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Redact(ToolContext& context, const nlohmann::json& arguments)
    {
        const auto documentId = arguments.value("documentId", std::string());
        const auto inputPath = arguments.value("input_path", std::string());
        if (documentId.empty() == inputPath.empty())
        {
            return MakeError(ErrorCode::InputInvalid,
                             "Pass exactly one of 'documentId' (an open session) or 'input_path' (a file).");
        }

        Tools::RedactOptions options;
        options.RemoveComments = !arguments.value("keep_comments", false);
        options.ResolveRevisions = !arguments.value("keep_revisions", false);
        options.RemoveHiddenText = !arguments.value("keep_hidden_text", false);
        options.RemovePersonalMetadata = !arguments.value("keep_personal_metadata", false);

        ToolOutcome failure;
        if (!documentId.empty())
        {
            auto* session = ToolSupport::RequireSession(context, arguments, failure);
            if (session == nullptr)
            {
                return failure;
            }

            // Redacts the session's own document, so the guard is what undoes a
            // redaction that fails partway rather than one that fails to reload.
            MutationGuard guard(*session);
            const auto result = context.Adapter().Redact(session->Document(), options);
            if (!result.Ok)
            {
                return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The document could not be redacted.",
                                                result.Diagnostics);
            }

            guard.Commit();

            return ResultBuilder("Redacted " + session->Id() + ".")
                .WithSession(*session)
                .WithData(RedactionData(result, {}))
                .WithDiagnostics(result.Diagnostics)
                .Build();
        }

        auto input = ToolSupport::ResolveExistingFile(context, inputPath, failure);
        if (!input.has_value())
        {
            return failure;
        }

        std::filesystem::path output;
        const auto outputPath = arguments.value("output_path", std::string());
        if (!outputPath.empty())
        {
            auto resolved =
                ToolSupport::ResolveOutputPath(context, outputPath, arguments.value("overwrite", false), failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            if (!ToolSupport::CheckDestinationFamily(context, outputPath, failure))
            {
                return failure;
            }

            output = *resolved;
        }

        const auto result = Tools::RedactDocument(*input, output, options);
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The document could not be redacted.",
                                            result.Diagnostics);
        }

        const auto written = output.empty() ? *input : output;
        return ResultBuilder("Redacted " + context.GetWorkspace().Relativize(written) + ".")
            .WithData(RedactionData(result, context.GetWorkspace().Relativize(written)))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }

    static nlohmann::json RedactionData(const Tools::RedactResult& result, std::string outputPath)
    {
        nlohmann::json data = nlohmann::json::object();
        data["commentsRemoved"] = static_cast<UInt64>(result.CommentsRemoved);
        data["revisionsResolved"] = static_cast<UInt64>(result.RevisionsResolved);
        data["hiddenRunsRemoved"] = static_cast<UInt64>(result.HiddenRunsRemoved);
        data["metadataFieldsCleared"] = static_cast<UInt64>(result.MetadataFieldsCleared);
        data["outputPath"] = std::move(outputPath);
        return data;
    }

    static void RegisterExportMedia(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["output_dir"] = Schema::String("Workspace-relative directory the media files are written to.");
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing existing media files.", false);

        nlohmann::json entry =
            Schema::Object("One exported media file.", {"path", "contentType"},
                           nlohmann::json{{"partUri", Schema::String("Source part URI.")},
                                          {"path", Schema::String("Workspace-relative file written.")},
                                          {"contentType", Schema::String("Media type of the payload.")},
                                          {"bytes", Schema::Integer("Payload size in bytes.")}});

        ToolDefinition definition;
        definition.Name = "export_media";
        definition.Title = "Export media";
        definition.Description =
            "Write every image, audio, video, and embedded object of the document into a directory. Use this "
            "instead of get_media when the payloads are large.";
        definition.Group = "files";
        definition.InputSchema = Schema::Object("Arguments of export_media.", {"output_dir"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Export result.", {"files"},
                           nlohmann::json{{"files", Schema::Array("Written files.", std::move(entry))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"output_dir", "media"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ExportMedia(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ExportMedia(ToolContext& context, const nlohmann::json& arguments)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            return access.Failure();
        }

        ToolOutcome failure;
        auto outputDirectory = ToolSupport::ResolvePath(context, arguments.value("output_dir", std::string()), failure);
        if (!outputDirectory.has_value())
        {
            return failure;
        }

        auto package = access.Document().Package();
        if (package == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The document has no package.");
        }

        const auto result = Tools::ExportMedia(*package, *outputDirectory, arguments.value("overwrite", false));
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The media could not be exported.",
                                            result.Diagnostics,
                                            "Pass overwrite: true when the destination already holds files.");
        }

        nlohmann::json files = nlohmann::json::array();
        for (const auto& item : result.Items)
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["partUri"] = item.PartUri;
            entry["path"] = context.GetWorkspace().Relativize(item.OutputPath);
            entry["contentType"] = item.ContentType;
            entry["bytes"] = item.Size;
            files.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto count = files.size();
        data["files"] = std::move(files);

        return ResultBuilder("Exported " + std::to_string(count) + " media file(s).")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }
};

void RegisterSharedToolset(ToolRegistry& registry, const FamilyAdapter& adapter)
{
    LifecycleTools::Register(registry, adapter);
    ReadTools::Register(registry, adapter);
    EditTools::Register(registry);
    FileTools::Register(registry, adapter);
}

} // namespace ExyokiOffice::Mcp
