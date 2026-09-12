// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/ThemeService.hpp"
#include "WordToolset.hpp"

#include "SharedToolset.hpp"
#include "Units.hpp"
#include "WordAddressing.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Tools/WordAutomationTools.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ExyokiOffice::Mcp
{

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

/// Names a `w:documentProtection/@w:edit` value the way the tools publish it.
static const char* WordProtectionToken(Word::WordProtectionType editing)
{
    switch (editing)
    {
        case Word::WordProtectionType::ReadOnly:
            return "readOnly";
        case Word::WordProtectionType::Comments:
            return "comments";
        case Word::WordProtectionType::TrackedChanges:
            return "trackedChanges";
        case Word::WordProtectionType::Forms:
            return "forms";
        default:
            return "none";
    }
}

/// Open settings that carry the configured safety limits and nothing else.
static Packaging::OpenSettings SettingsWithLimits(const OpenXmlPackageLimits& limits)
{
    Packaging::OpenSettings settings;
    settings.PackageLimits = limits;
    return settings;
}

WordDocumentHandle::WordDocumentHandle(Word::WordDocumentEditor::Ptr editor, OpenXmlPackageLimits limits)
    : m_editor(std::move(editor)), m_packageLimits(limits)
{
}

Tools::DocumentFamily WordDocumentHandle::Family() const
{
    return Tools::DocumentFamily::Word;
}

bool WordDocumentHandle::SaveToFile(const std::filesystem::path& path)
{
    return m_editor && m_editor->SaveToFile(path);
}

std::vector<Byte> WordDocumentHandle::SaveToMemory()
{
    return m_editor ? m_editor->SaveToMemory() : std::vector<Byte>();
}

bool WordDocumentHandle::LoadFromMemory(std::span<const Byte> bytes)
{
    // Snapshot bytes come from this process, but they are a package all the
    // same: a document that was within the limits when it was opened stays
    // within them when it is restored, and a bug that made it grow past them
    // should surface here rather than be waved through.
    auto replacement = Word::WordDocumentEditor::Open(bytes, SettingsWithLimits(m_packageLimits));
    if (replacement == nullptr)
    {
        return false;
    }

    m_editor = std::move(replacement);
    return true;
}

std::shared_ptr<OpenXmlPackage> WordDocumentHandle::Package() const
{
    return m_editor ? m_editor->GetDocument() : nullptr;
}

nlohmann::json WordDocumentHandle::Protection() const
{
    const auto info = m_editor ? m_editor->GetDocumentProtection() : std::nullopt;
    if (!info.has_value())
    {
        return {};
    }

    nlohmann::json data = nlohmann::json::object();
    data["kind"] = "document";
    data["editing"] = WordProtectionToken(info->Options.Editing);
    data["enforced"] = info->Options.Enforce;
    data["restrictFormatting"] = info->Options.RestrictFormattingToUnlockedStyles;
    data["hasPassword"] = info->HasPassword;
    return data;
}

std::shared_ptr<Packaging::ThemePart> WordDocumentHandle::Theme() const
{
    const auto document = m_editor ? m_editor->GetDocument() : nullptr;
    const auto main = document ? document->GetMainDocumentPart() : nullptr;
    return main ? main->GetThemePart() : nullptr;
}

std::shared_ptr<Packaging::ThemePart> WordDocumentHandle::EnsureTheme()
{
    const auto document = m_editor ? m_editor->GetDocument() : nullptr;
    const auto main = document ? document->GetMainDocumentPart() : nullptr;
    if (!main)
    {
        return nullptr;
    }
    if (const auto existing = main->GetThemePart())
    {
        return existing;
    }
    const auto created = main->AddThemePart();
    return created && ThemeService::WriteDefaultTheme(created) ? created : nullptr;
}

nlohmann::json WordDocumentHandle::Summary() const
{
    nlohmann::json summary = nlohmann::json::object();
    if (m_editor == nullptr)
    {
        return summary;
    }

    summary["blockCount"] = static_cast<UInt64>(m_editor->BodyBlocks().size());
    summary["paragraphCount"] = static_cast<UInt64>(m_editor->Paragraphs().size());
    summary["tableCount"] = static_cast<UInt64>(m_editor->Tables().size());
    summary["sectionCount"] = static_cast<UInt64>(m_editor->Sections().size());
    summary["commentCount"] = static_cast<UInt64>(m_editor->Comments().size());
    summary["revisionCount"] = static_cast<UInt64>(m_editor->Revisions().size());
    return summary;
}

Tools::DocumentFamily WordFamilyAdapter::Family() const
{
    return Tools::DocumentFamily::Word;
}

std::string WordFamilyAdapter::FamilyName() const
{
    return "Word";
}

std::string WordFamilyAdapter::FileExtension() const
{
    return ".docx";
}

std::unique_ptr<DocumentHandle> WordFamilyAdapter::CreateNew() const
{
    auto editor = Word::WordDocumentEditor::CreateNew();
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<WordDocumentHandle>(std::move(editor), PackageLimits());
}

std::unique_ptr<DocumentHandle> WordFamilyAdapter::Open(const std::filesystem::path& path) const
{
    auto editor = Word::WordDocumentEditor::Open(path, SettingsWithLimits(PackageLimits()));
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<WordDocumentHandle>(std::move(editor), PackageLimits());
}

std::unique_ptr<DocumentHandle> WordFamilyAdapter::OpenFromMemory(std::span<const Byte> bytes) const
{
    auto editor = Word::WordDocumentEditor::Open(bytes, SettingsWithLimits(PackageLimits()));
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<WordDocumentHandle>(std::move(editor), PackageLimits());
}

/// The editor behind a handle this adapter produced.
static Word::WordDocumentEditor& EditorOf(DocumentHandle& document)
{
    // Every handle reaching a WordFamilyAdapter came out of its own CreateNew, Open or
    // OpenFromMemory, so the family is a fact here rather than a guess.
    return static_cast<WordDocumentHandle&>(document).Editor();
}

Tools::DocumentModel WordFamilyAdapter::ReadModel(DocumentHandle& document, const Tools::ModelReadOptions& options,
                                                  std::vector<Tools::ToolDiagnostic>& diagnostics) const
{
    return Tools::ReadWordModel(EditorOf(document), options, diagnostics);
}

Tools::DocumentStats WordFamilyAdapter::Stat(DocumentHandle& document) const
{
    return Tools::Stat(EditorOf(document));
}

Tools::ExtractedDocumentText WordFamilyAdapter::ExtractText(DocumentHandle& document) const
{
    return Tools::Extract(EditorOf(document));
}

Tools::DocumentSearchResult WordFamilyAdapter::SearchText(DocumentHandle& document, std::string_view needle, Size contextChars,
                                                          bool useRegex, bool ignoreCase) const
{
    return Tools::SearchDocumentText(EditorOf(document), needle, contextChars, useRegex, ignoreCase);
}

Tools::DocumentReplaceResult WordFamilyAdapter::ReplaceText(DocumentHandle& document, std::string_view needle,
                                                            std::string_view replacement, bool dryRun, bool useRegex,
                                                            bool ignoreCase) const
{
    return Tools::ReplaceDocumentText(EditorOf(document), needle, replacement, dryRun, useRegex, ignoreCase);
}

Tools::RedactResult WordFamilyAdapter::Redact(DocumentHandle& document, const Tools::RedactOptions& options) const
{
    return Tools::RedactDocument(EditorOf(document), options);
}

/// Implementation of the tools in §9 of the MCP server plan.
class WordTools
{
public:
    static void Register(ToolRegistry& registry)
    {
        RegisterGetOutline(registry);
        RegisterReadBlocks(registry);
        RegisterListStyles(registry);
        RegisterListCharts(registry);
        RegisterUpdateChart(registry);
        RegisterInsertParagraph(registry);
        RegisterInsertList(registry);
        RegisterEditParagraph(registry);
        RegisterDeleteBlocks(registry);
        RegisterApplyStyle(registry);
        RegisterInsertImage(registry);
        RegisterAddBookmark(registry);
        RegisterInsertTable(registry);
        RegisterEditTableCell(registry);
        RegisterModifyTable(registry);
        RegisterFormatTable(registry);
        RegisterSetHeaderFooter(registry);
        RegisterSetSection(registry);
        RegisterSetTrackedChanges(registry);
        RegisterListRevisions(registry);
        RegisterResolveRevisions(registry);
        RegisterListComments(registry);
        RegisterAddComment(registry);
        RegisterDeleteComment(registry);
        RegisterAddNote(registry);
        RegisterFillTemplate(registry);
        RegisterCompareDocuments(registry);
        RegisterSetProtection(registry);
        RegisterDefineStyle(registry);
        RegisterDeleteStyle(registry);
        RegisterListNumbering(registry);
        RegisterDefineList(registry);
    }

private:
    /// Resolves the session and its Word editor in one step.
    class WordSession
    {
    public:
        WordSession(ToolContext& context, const nlohmann::json& arguments)
        {
            m_session = ToolSupport::RequireSession(context, arguments, m_failure);
            if (m_session == nullptr)
            {
                return;
            }

            auto* handle = dynamic_cast<WordDocumentHandle*>(&m_session->Document());
            if (handle == nullptr)
            {
                m_failure = MakeError(ErrorCode::FamilyMismatch, "The document is not a Word document.",
                                      m_session->Id());
                return;
            }

            m_editor = &handle->Editor();
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_editor != nullptr; }
        [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_failure; }
        [[nodiscard]] DocumentSession& Session() const noexcept { return *m_session; }
        [[nodiscard]] Word::WordDocumentEditor& Editor() const noexcept { return *m_editor; }

    private:
        DocumentSession* m_session = nullptr;
        Word::WordDocumentEditor* m_editor = nullptr;
        ToolOutcome m_failure;
    };

    /// Reading tools take a document source; this resolves it to a Word editor.
    class WordReader
    {
    public:
        WordReader(ToolContext& context, const nlohmann::json& arguments)
            : m_access(context, arguments)
        {
            if (!m_access.IsValid())
            {
                return;
            }

            auto* handle = dynamic_cast<WordDocumentHandle*>(&m_access.Document());
            if (handle != nullptr)
            {
                m_editor = &handle->Editor();
            }
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_editor != nullptr; }
        [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_access.Failure(); }
        [[nodiscard]] Word::WordDocumentEditor& Editor() const noexcept { return *m_editor; }

    private:
        DocumentAccess m_access;
        Word::WordDocumentEditor* m_editor = nullptr;
    };

    static ToolDefinition MakeDefinition(std::string name, std::string title, std::string description,
                                         std::string group)
    {
        ToolDefinition definition;
        definition.Name = std::move(name);
        definition.Title = std::move(title);
        definition.Description = std::move(description);
        definition.Group = std::move(group);
        return definition;
    }

    // --- content ------------------------------------------------------------

    static void RegisterGetOutline(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json heading =
            Schema::Object("One heading.", {"block", "level", "text"},
                           nlohmann::json{{"block", Schema::Integer("1-based body block index.")},
                                          {"level", Schema::Integer("Heading level, 1 to 9.")},
                                          {"text", Schema::String("Heading text.")},
                                          {"styleId", Schema::String("Paragraph style identifier.")}});
        nlohmann::json bookmark =
            Schema::Object("One bookmark.", {"name"},
                           nlohmann::json{{"name", Schema::String("Bookmark name.")},
                                          {"id", Schema::Integer("Bookmark identifier.")}});

        auto definition = MakeDefinition("get_outline", "Get document outline",
                                         "List the headings and bookmarks of the document with their block "
                                         "indices. Use it to navigate before reading or editing content.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of get_outline.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Document outline.", {"headings"},
                           nlohmann::json{{"headings", Schema::Array("Headings in document order.", std::move(heading))},
                                          {"bookmarks", Schema::Array("Bookmarks.", std::move(bookmark))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return GetOutline(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome GetOutline(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        nlohmann::json headings = nlohmann::json::array();
        const auto blocks = reader.Editor().BodyBlocks();
        for (Size index = 0; index < blocks.size(); ++index)
        {
            auto paragraph = blocks[index].AsParagraph();
            if (paragraph == nullptr)
            {
                continue;
            }

            const auto styleId = paragraph->GetStyleId();
            const auto level = HeadingLevelOf(styleId);
            if (level == 0)
            {
                continue;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["block"] = static_cast<UInt64>(index + 1);
            entry["level"] = level;
            entry["text"] = paragraph->PlainText();
            entry["styleId"] = styleId;
            headings.push_back(std::move(entry));
        }

        nlohmann::json bookmarks = nlohmann::json::array();
        for (const auto& bookmark : reader.Editor().Bookmarks())
        {
            if (bookmark == nullptr)
            {
                continue;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = bookmark->GetName();
            entry["id"] = bookmark->GetId();
            bookmarks.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto headingCount = headings.size();
        data["headings"] = std::move(headings);
        data["bookmarks"] = std::move(bookmarks);

        return ResultBuilder("The document has " + std::to_string(headingCount) + " heading(s).")
            .WithData(std::move(data))
            .Build();
    }

    /**
     * @brief Heading level derived from the paragraph style, matching the model reader.
     *
     * A style identifier is arbitrary document data: "Heading99999999999999"
     * is a legal name, so the digits are parsed without a conversion that
     * throws and anything outside the nine Word heading levels is not a
     * heading.
     */
    static int HeadingLevelOf(const std::string& styleId)
    {
        if (styleId == "Title")
        {
            return 1;
        }

        constexpr std::string_view prefix = "Heading";
        if (!std::string_view(styleId).starts_with(prefix))
        {
            return 0;
        }

        const std::string_view digits = std::string_view(styleId).substr(prefix.size());
        int level = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), level);
        if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || level < 1 || level > 9)
        {
            return 0;
        }

        return level;
    }

    static void RegisterReadBlocks(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["from"] = Schema::IntegerWithDefault("First body block to read, 1-based.", 1, 1);
        properties["count"] = Schema::IntegerWithDefault("Number of blocks to read.", 50, 1, 1000);
        properties["format"] = Schema::EnumerationWithDefault("Shape of the returned blocks.",
                                                              {"model", "markdown", "text"}, "model");

        nlohmann::json block =
            Schema::Object("One body block.", {"block", "kind"},
                           nlohmann::json{{"block", Schema::Integer("1-based body block index.")},
                                          {"kind", Schema::String("paragraph, table, section, or contentControl.")},
                                          {"text", Schema::String("Plain text of a paragraph.")},
                                          {"styleId", Schema::String("Paragraph style identifier.")},
                                          {"rows", Schema::Integer("Row count of a table.")},
                                          {"columns", Schema::Integer("Logical column count of a table.")}});

        auto definition = MakeDefinition("read_blocks", "Read body blocks",
                                         "Read a window of body blocks with their indices. This is the cheap way "
                                         "to refresh the indices after a structural edit shifted them.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of read_blocks.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Block window.", {"blocks", "blockCount"},
                           nlohmann::json{{"blocks", Schema::Array("Blocks in document order.", std::move(block))},
                                          {"blockCount", Schema::Integer("Total number of body blocks.")},
                                          {"nextOffset", Schema::Integer("Value for 'from' on the next page; 0 at "
                                                                         "the end.")},
                                          {"markdown", Schema::String("Markdown rendering, when requested.")},
                                          {"text", Schema::String("Plain-text rendering, when requested.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"from", 1}, {"count", 20}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ReadBlocks(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ReadBlocks(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        const auto blocks = reader.Editor().BodyBlocks();
        const Size from = std::max<Size>(1, arguments.value("from", static_cast<Size>(1)));
        const Size count = arguments.value("count", static_cast<Size>(50));
        const Size last = std::min(blocks.size(), from + count - 1);

        nlohmann::json entries = nlohmann::json::array();
        std::string plainText;
        for (Size index = from; index <= last && index <= blocks.size(); ++index)
        {
            entries.push_back(WordAddressing::BlockToJson(blocks[index - 1], index));
            if (auto paragraph = blocks[index - 1].AsParagraph())
            {
                if (!plainText.empty())
                {
                    plainText.push_back('\n');
                }

                plainText.append(paragraph->PlainText());
            }
        }

        const auto format = arguments.value("format", std::string("model"));
        bool truncated = TruncateArrayToBudget(entries);

        nlohmann::json data = nlohmann::json::object();
        data["blocks"] = format == "model" ? std::move(entries) : nlohmann::json::array();
        data["blockCount"] = static_cast<UInt64>(blocks.size());
        data["nextOffset"] = last < blocks.size() ? static_cast<UInt64>(last + 1) : 0u;

        ResultBuilder builder("Read blocks " + std::to_string(from) + " to " + std::to_string(last) + " of " +
                              std::to_string(blocks.size()) + ".");

        if (format == "markdown" || format == "text")
        {
            std::string rendered;
            std::string problem;
            if (RenderModelWindow(context, arguments, blocks, from, last, format, rendered, problem))
            {
                truncated = TruncateTextToBudget(rendered) || truncated;
                data[format] = std::move(rendered);
            }
            else
            {
                data[format] = std::string();
                builder.WithWarning("render_unavailable", problem, format);
            }
        }
        else
        {
            data["markdown"] = std::string();
            data["text"] = std::move(plainText);
        }

        return builder.WithData(std::move(data)).WithTruncated(truncated).Build();
    }

    /**
     * @brief Number of document-model blocks one body block expands into.
     *
     * The model reader flattens the body: a section-only block carries no
     * content, unsupported markup is skipped, and a block-level content
     * control contributes one model block per paragraph it holds. The window
     * `read_blocks` reports and the window it renders would therefore drift
     * apart if the model were sliced by the body index itself.
     */
    static Size ModelBlockCount(const Word::BodyBlock& block)
    {
        switch (block.Type())
        {
            case Word::BodyBlockType::Paragraph:
            case Word::BodyBlockType::Table:
                return 1;
            case Word::BodyBlockType::ContentControl:
            {
                auto control = block.AsContentControl();
                return control == nullptr ? 0 : control->Paragraphs().size();
            }
            case Word::BodyBlockType::Section:
            case Word::BodyBlockType::Unsupported:
                break;
        }

        return 0;
    }

    /// Renders the addressed body-block window through the document-model bridge.
    static bool RenderModelWindow(ToolContext& context, const nlohmann::json& arguments,
                                  const std::vector<Word::BodyBlock>& blocks, Size from, Size last,
                                  const std::string& format, std::string& rendered, std::string& problem)
    {
        DocumentAccess access(context, arguments);
        if (!access.IsValid())
        {
            problem = "The document could not be read for rendering, so '" + format +
                      "' is empty; the reported block indices are still correct.";
            return false;
        }

        std::vector<Tools::ToolDiagnostic> diagnostics;
        Tools::ModelReadOptions options;
        options.IncludeMediaData = false;
        auto model = context.Adapter().ReadModel(access.Document(), options, diagnostics);
        if (!model.Word.has_value())
        {
            problem = "The document model could not be read, so '" + format +
                      "' is empty; the reported block indices are still correct.";
            return false;
        }

        Size skip = 0;
        Size take = 0;
        for (Size index = 1; index <= blocks.size(); ++index)
        {
            const auto produced = ModelBlockCount(blocks[index - 1]);
            if (index < from)
            {
                skip += produced;
            }
            else if (index <= last)
            {
                take += produced;
            }
        }

        auto& body = model.Word->Body;
        if (skip < body.size())
        {
            body.erase(body.begin(), body.begin() + static_cast<PtrDiff>(skip));
        }
        else
        {
            body.clear();
        }

        if (body.size() > take)
        {
            body.resize(take);
        }

        rendered = format == "markdown" ? Tools::SerializeModelMarkdown(model, diagnostics)
                                        : Tools::SerializeModelText(model);
        return true;
    }

    static std::string ChartTypeToken(Word::WordChartType type)
    {
        switch (type)
        {
            case Word::WordChartType::Column:
                return "column";
            case Word::WordChartType::Bar:
                return "bar";
            case Word::WordChartType::Line:
                return "line";
            case Word::WordChartType::Pie:
                return "pie";
            case Word::WordChartType::Area:
                return "area";
            case Word::WordChartType::XyScatter:
                return "scatter";
            case Word::WordChartType::Bubble:
                return "bubble";
            case Word::WordChartType::Unknown:
                break;
        }

        return "unknown";
    }

    static void RegisterListCharts(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json series =
            Schema::Object("One plotted series.", {"name", "values"},
                           nlohmann::json{{"name", Schema::String("Series name shown in the legend.")},
                                          {"values", Schema::Array("Numeric values in point order.",
                                                                   Schema::Number("One value."))},
                                          {"categories", Schema::Array("Category labels in point order.",
                                                                       Schema::String("One label."))}});

        nlohmann::json chart = Schema::Object(
            "One embedded chart.", {"chart", "type"},
            nlohmann::json{{"chart", Schema::Integer("1-based chart index, as update_chart takes it.")},
                           {"relationshipId", Schema::String("Relationship id of the chart part.")},
                           {"title", Schema::String("Chart title, or empty when it has none.")},
                           {"type", Schema::String("Plot type, or \"unknown\" for one this version does not "
                                                   "classify.")},
                           {"hasEmbeddedWorkbook", Schema::Boolean("True when the chart carries its own "
                                                                   "workbook.")},
                           {"series", Schema::Array("Series read from the chart's cached values.",
                                                    std::move(series))}});

        auto definition = MakeDefinition("list_charts", "List charts",
                                         "List the charts embedded in the document with their cached series, so "
                                         "update_chart can rewrite one. A chart whose plot type this version does "
                                         "not classify is listed as \"unknown\" with no series.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of list_charts.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Charts.", {"charts"},
                           nlohmann::json{{"charts", Schema::Array("Charts.", std::move(chart))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListCharts(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListCharts(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        nlohmann::json charts = nlohmann::json::array();
        const auto embedded = reader.Editor().Charts();
        for (Size index = 0; index < embedded.size(); ++index)
        {
            const auto& chart = embedded[index];

            nlohmann::json series = nlohmann::json::array();
            for (const auto& entry : chart.Series)
            {
                nlohmann::json one = nlohmann::json::object();
                one["name"] = entry.Name;
                one["values"] = entry.Values;
                if (entry.Categories.has_value())
                {
                    one["categories"] = *entry.Categories;
                }

                series.push_back(std::move(one));
            }

            nlohmann::json record = nlohmann::json::object();
            record["chart"] = static_cast<UInt64>(index + 1);
            record["relationshipId"] = chart.RelationshipId;
            record["title"] = chart.Title;
            record["type"] = ChartTypeToken(chart.Type);
            record["hasEmbeddedWorkbook"] = chart.HasEmbeddedWorkbook;
            record["series"] = std::move(series);
            charts.push_back(std::move(record));
        }

        const bool truncated = TruncateArrayToBudget(charts);

        nlohmann::json data = nlohmann::json::object();
        const auto count = charts.size();
        data["charts"] = std::move(charts);

        return ResultBuilder("The document holds " + std::to_string(count) + " chart(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterUpdateChart(ToolRegistry& registry)
    {
        nlohmann::json series =
            Schema::Object("One replacement series.", {"name", "values"},
                           nlohmann::json{{"name", Schema::String("Series name shown in the legend.")},
                                          {"values", Schema::Array("Numeric values in point order.",
                                                                   Schema::Number("One value."))},
                                          {"categories", Schema::Array("Category labels in point order; the X "
                                                                       "values of a scatter or bubble chart.",
                                                                       Schema::String("One label."))}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["chart"] = Schema::Integer("1-based chart index from list_charts.", 1);
        properties["relationship_id"] = Schema::String("Relationship id from list_charts; an alternative to "
                                                       "'chart'.");
        properties["title"] = Schema::String("Replacement chart title; an empty string removes the title.");
        properties["series"] = Schema::Array("Replacement series; the chart is rebuilt to match exactly.",
                                             std::move(series));

        auto definition = MakeDefinition(
            "update_chart", "Update chart",
            "Rewrite the cached series and the title of a chart already in the document. Series may be added, "
            "removed, or reordered. The chart's position, its plot type, and any embedded workbook are left "
            "alone, and per-series styling is not preserved. This version cannot create a new chart.",
            "content");
        definition.InputSchema =
            Schema::Object("Arguments of update_chart.", {"documentId", "series"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Updated chart.", {"chart"},
                           nlohmann::json{{"chart", Schema::Integer("1-based chart index.")},
                                          {"relationshipId", Schema::String("Relationship id of the chart "
                                                                            "part.")},
                                          {"seriesCount", Schema::Integer("Series the chart now plots.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"chart", 1},
            {"title", "Q3 results"},
            {"series", nlohmann::json::array({nlohmann::json{
                {"name", "Actuals"},
                {"values", nlohmann::json::array({12, 18, 9})},
                {"categories", nlohmann::json::array({"Jul", "Aug", "Sep"})}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return UpdateChart(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome UpdateChart(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto charts = session.Editor().Charts();
        if (charts.empty())
        {
            return MakeError(ErrorCode::Unsupported,
                             "The document holds no chart to update, and this version cannot create one.",
                             std::string(), "Start from a template that already carries the chart.");
        }

        // A chart is nameable either way: the index is what an agent reads off
        // list_charts, the relationship id is what survives a reordering.
        const auto relationshipId = arguments.value("relationship_id", std::string());
        Size position = 0;
        if (!relationshipId.empty())
        {
            const auto match = std::find_if(charts.begin(), charts.end(),
                                            [&relationshipId](const Word::WordChartInfo& info)
                                            { return info.RelationshipId == relationshipId; });
            if (match == charts.end())
            {
                return MakeError(ErrorCode::MediaNotFound, "No chart has that relationship id.", relationshipId);
            }

            position = static_cast<Size>(std::distance(charts.begin(), match));
        }
        else
        {
            const Size index = arguments.value("chart", static_cast<Size>(1));
            if (index == 0 || index > charts.size())
            {
                return MakeError(ErrorCode::MediaNotFound,
                                 "The document holds " + std::to_string(charts.size()) + " chart(s).",
                                 std::to_string(index), "Call list_charts to see them.");
            }

            position = index - 1;
        }

        const auto& target = charts[position];

        const auto series = arguments.find("series");
        if (series == arguments.end() || series->empty())
        {
            return MakeError(ErrorCode::InputInvalid, "A chart needs at least one series.", "series");
        }

        std::vector<Word::WordChartSeries> replacement;
        for (const auto& entry : *series)
        {
            Word::WordChartSeries one;
            one.Name = entry.value("name", std::string());
            one.Values = entry.value("values", std::vector<Real>());

            if (const auto categories = entry.find("categories"); categories != entry.end())
            {
                one.Categories = categories->get<std::vector<std::string>>();

                // A category list of a different length than the values would
                // silently mislabel points, which is worse than refusing it.
                if (one.Categories->size() != one.Values.size())
                {
                    return MakeError(ErrorCode::InputInvalid,
                                     "A series has " + std::to_string(one.Values.size()) + " value(s) but " +
                                         std::to_string(one.Categories->size()) + " category label(s).",
                                     one.Name);
                }
            }

            replacement.push_back(std::move(one));
        }

        std::optional<std::string> title;
        if (const auto value = arguments.find("title"); value != arguments.end())
        {
            title = value->get<std::string>();
        }

        MutationGuard guard(session.Session());

        if (!session.Editor().UpdateChartData(target.RelationshipId, replacement, title))
        {
            return MakeError(ErrorCode::OperationFailed,
                             "The chart could not be updated; its plot type may be one this version does not "
                             "classify.",
                             target.RelationshipId);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["chart"] = static_cast<UInt64>(position + 1);
        data["relationshipId"] = target.RelationshipId;
        data["seriesCount"] = static_cast<UInt64>(replacement.size());

        return ResultBuilder("Updated chart " + std::to_string(position + 1) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterListStyles(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["kind"] =
            Schema::Enumeration("Restricts the result to one style type.", {"paragraph", "character", "table",
                                                                            "numbering"});

        nlohmann::json style = Schema::Object("One style.", {"id", "name", "kind"},
                                              nlohmann::json{{"id", Schema::String("Style identifier.")},
                                                             {"name", Schema::String("User-visible style name.")},
                                                             {"kind", Schema::String("Style type.")},
                                                             {"builtIn", Schema::Boolean("True for built-in styles.")}});

        auto definition = MakeDefinition("list_styles", "List styles",
                                         "List the styles defined in the document. Use it to find a valid "
                                         "style_id before applying one.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of list_styles.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Style catalog.", {"styles"},
                           nlohmann::json{{"styles", Schema::Array("Defined styles.", std::move(style))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"kind", "paragraph"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListStyles(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::string StyleTypeToken(Word::StyleType type)
    {
        switch (type)
        {
            case Word::StyleType::Paragraph:
                return "paragraph";
            case Word::StyleType::Character:
                return "character";
            case Word::StyleType::Table:
                return "table";
            case Word::StyleType::Numbering:
                return "numbering";
        }

        return "paragraph";
    }

    static ToolOutcome ListStyles(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        const auto kind = arguments.value("kind", std::string());
        nlohmann::json styles = nlohmann::json::array();
        for (const auto& style : reader.Editor().Styles().Styles())
        {
            const auto token = StyleTypeToken(style.Type);
            if (!kind.empty() && token != kind)
            {
                continue;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = style.StyleId;
            entry["name"] = style.Name;
            entry["kind"] = token;
            entry["builtIn"] = !style.IsCustom;
            styles.push_back(std::move(entry));
        }

        const bool truncated = TruncateArrayToBudget(styles);

        nlohmann::json data = nlohmann::json::object();
        const auto count = styles.size();
        data["styles"] = std::move(styles);

        return ResultBuilder("The document defines " + std::to_string(count) + " style(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterInsertParagraph(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["anchor"] = WordAddressing::AnchorSchema();
        properties["text"] = WordAddressing::TextSchema();
        properties["inlines"] = Schema::Array("Formatted inline content.", WordAddressing::InlineSchema());
        properties["style_id"] = Schema::String("Paragraph style identifier, for example \"Heading1\".");
        properties["alignment"] = Schema::Enumeration("Paragraph alignment.", WordAddressing::AlignmentTokens());
        properties["heading_level"] =
            Schema::Integer("Applies the built-in HeadingN style; overrides style_id.", 1, 9);
        properties["list"] = Schema::Object("Attaches the paragraph to a numbering instance.", {"numberingId"},
                                            nlohmann::json{{"numberingId", Schema::Integer("Numbering identifier "
                                                                                           "from insert_list; it "
                                                                                           "must already exist.",
                                                                                           1)},
                                                           {"level", Schema::Integer("List level, 0-based.", 0)}});

        auto definition = MakeDefinition("insert_paragraph", "Insert paragraph",
                                         "Insert one paragraph at an anchor. Supply 'text' for plain content or "
                                         "'inlines' for mixed formatting, links, fields, and pictures. A 'list' "
                                         "numberingId that the document does not define is refused rather than "
                                         "written, because a dangling numbering reference makes the list "
                                         "disappear in Word.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of insert_paragraph.", {"documentId", "anchor"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Inserted paragraph.", {"block"},
                           nlohmann::json{{"block", Schema::Integer("1-based index of the new block.")},
                                          {"blockCount", Schema::Integer("Total number of body blocks.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"},
                                            {"anchor", nlohmann::json{{"position", "end"}}},
                                            {"text", "Quarterly summary"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return InsertParagraph(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome InsertParagraph(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        WordAnchor anchor;
        ToolOutcome failure;
        if (!WordAddressing::ParseAnchor(arguments, anchor, failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        Word::WordDocumentEditor::BodyCursor cursor;
        if (!WordAddressing::ResolveCursor(session.Editor(), anchor, cursor, failure))
        {
            return failure;
        }

        auto paragraph = cursor.InsertParagraph();
        if (paragraph == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The paragraph could not be inserted.");
        }

        if (!ApplyParagraphSettings(context, session.Editor(), *paragraph, arguments, failure))
        {
            return failure;
        }

        guard.Commit();

        const auto index = WordAddressing::IndexOfElement(session.Editor(), paragraph->GetLowLevelApi());

        nlohmann::json data = nlohmann::json::object();
        data["block"] = static_cast<UInt64>(index);
        data["blockCount"] = static_cast<UInt64>(session.Editor().BodyBlocks().size());

        return ResultBuilder("Inserted a paragraph as block " + std::to_string(index) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /// Applies content, style, alignment, heading level, and list membership.
    static bool ApplyParagraphSettings(ToolContext& context, Word::WordDocumentEditor& editor,
                                       Word::Paragraph& paragraph, const nlohmann::json& arguments,
                                       ToolOutcome& failure)
    {
        if (WordAddressing::HasContent(arguments))
        {
            if (!WordAddressing::ApplyContent(context, editor, paragraph, arguments, failure))
            {
                return false;
            }
        }

        const auto headingLevel = arguments.value("heading_level", 0);
        const auto styleId = headingLevel > 0 ? "Heading" + std::to_string(headingLevel)
                                              : arguments.value("style_id", std::string());
        if (!styleId.empty())
        {
            if (headingLevel == 0 && !editor.Styles().HasStyle(styleId))
            {
                failure = MakeError(ErrorCode::StyleNotFound, "The document defines no style '" + styleId + "'.",
                                    styleId, "Call list_styles to see the available style identifiers.");
                return false;
            }

            paragraph.SetStyleId(styleId);
        }

        const auto alignment = arguments.value("alignment", std::string());
        if (!alignment.empty())
        {
            const auto parsed = WordAddressing::ParseAlignment(alignment);
            if (!parsed.has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid, "Unknown alignment '" + alignment + "'.", alignment);
                return false;
            }

            paragraph.SetAlignment(*parsed);
        }

        const auto list = arguments.find("list");
        if (list != arguments.end() && list->is_object())
        {
            const auto numberingId = list->value("numberingId", 0);
            if (numberingId <= 0 || !editor.Numbering().GetInstance(numberingId).has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid,
                                    "The document defines no numbering instance " + std::to_string(numberingId) + ".",
                                    std::to_string(numberingId),
                                    "Call insert_list once and reuse the numberingId it reports.");
                return false;
            }

            paragraph.SetNumbering(numberingId, list->value("level", 0));
        }

        return true;
    }

    static void RegisterInsertList(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["anchor"] = WordAddressing::AnchorSchema();
        properties["items"] = Schema::Array(
            "List items in order.",
            Schema::Object("One list item.", {},
                           nlohmann::json{{"text", WordAddressing::TextSchema()},
                                          {"inlines", Schema::Array("Formatted inline content.",
                                                                    WordAddressing::InlineSchema())},
                                          {"level", Schema::Integer("List level, 0-based.", 0)}}));
        properties["kind"] = Schema::EnumerationWithDefault("List marker style.", {"bullet", "numbered"}, "bullet");
        properties["start"] = Schema::Integer("First number of a numbered list.", 1);
        properties["numbering_id"] = Schema::Integer(
            "Numbering instance the list joins, from a previous insert_list, define_list, or list_numbering. "
            "The list then continues that sequence and takes its shape; 'kind' and 'start' are not used.",
            1);

        auto definition = MakeDefinition("insert_list", "Insert list",
                                         "Insert a bulleted or numbered list as consecutive paragraphs. Pass the "
                                         "numberingId of an earlier list to continue its sequence, or one from "
                                         "define_list to lay out a multi-level definition.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of insert_list.", {"documentId", "anchor", "items"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Inserted list.", {"firstBlock", "lastBlock", "numberingId"},
                           nlohmann::json{{"firstBlock", Schema::Integer("Index of the first list paragraph.")},
                                          {"lastBlock", Schema::Integer("Index of the last list paragraph.")},
                                          {"numberingId", Schema::Integer("Numbering instance the list uses.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"anchor", nlohmann::json{{"position", "end"}}},
            {"items", nlohmann::json::array({nlohmann::json{{"text", "First"}}, nlohmann::json{{"text", "Second"}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return InsertList(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome InsertList(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        WordAnchor anchor;
        ToolOutcome failure;
        if (!WordAddressing::ParseAnchor(arguments, anchor, failure))
        {
            return failure;
        }

        const auto items = arguments.find("items");
        if (items == arguments.end() || !items->is_array() || items->empty())
        {
            return MakeError(ErrorCode::InputInvalid, "'items' must list at least one entry.");
        }

        MutationGuard guard(session.Session());

        Word::ListStyle listStyle;
        if (const auto numberingId = arguments.find("numbering_id");
            numberingId != arguments.end() && numberingId->is_number_integer())
        {
            // Joining an existing sequence is what makes the list continue
            // rather than start over, so the instance has to be the one that
            // already exists; a missing one is a caller mistake, not a reason
            // to invent a second list.
            listStyle = session.Editor().Numbering().ContinueList(numberingId->get<int>());
            if (listStyle.NumberingId == 0)
            {
                return MakeError(ErrorCode::InputInvalid,
                                 "The document has no numbering instance " + numberingId->dump() + ".",
                                 "numbering_id", "list_numbering reports the instances this document carries.");
            }
        }
        else
        {
            const auto kind = arguments.value("kind", std::string("bullet"));
            listStyle = kind == "numbered" ? session.Editor().EnsureNumberedListStyle()
                                           : session.Editor().EnsureBulletedListStyle();
            if (kind == "numbered")
            {
                const auto start = arguments.value("start", 1);
                if (start != 1)
                {
                    listStyle = session.Editor().Numbering().RestartList(
                        listStyle.NumberingId, {Word::NumberingLevelOverride{0, start}});
                }
            }
        }

        Size firstBlock = 0;
        Size lastBlock = 0;
        WordAnchor itemAnchor = anchor;
        for (const auto& item : *items)
        {
            Word::WordDocumentEditor::BodyCursor cursor;
            if (!WordAddressing::ResolveCursor(session.Editor(), itemAnchor, cursor, failure))
            {
                return failure;
            }

            auto paragraph = cursor.InsertParagraph();
            if (paragraph == nullptr)
            {
                return MakeError(ErrorCode::OperationFailed, "A list paragraph could not be inserted.");
            }

            if (!WordAddressing::ApplyContent(context, session.Editor(), *paragraph, item, failure))
            {
                return failure;
            }

            auto itemStyle = listStyle;
            itemStyle.Level = item.value("level", 0);
            paragraph->SetListStyle(itemStyle);

            const auto index = WordAddressing::IndexOfElement(session.Editor(), paragraph->GetLowLevelApi());
            if (firstBlock == 0)
            {
                firstBlock = index;
            }

            lastBlock = index;

            // Every following item lands after the one just inserted, so the
            // list keeps its order regardless of the original anchor.
            itemAnchor.Where = WordAnchor::Position::After;
            itemAnchor.Block = index;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["firstBlock"] = static_cast<UInt64>(firstBlock);
        data["lastBlock"] = static_cast<UInt64>(lastBlock);
        data["numberingId"] = listStyle.NumberingId;

        return ResultBuilder("Inserted a list as blocks " + std::to_string(firstBlock) + " to " +
                             std::to_string(lastBlock) + ", numbered as instance " +
                             std::to_string(listStyle.NumberingId) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterEditParagraph(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["block"] = Schema::Integer("1-based index of the paragraph to rewrite.", 1);
        properties["text"] = WordAddressing::TextSchema();
        properties["inlines"] = Schema::Array("Formatted inline content.", WordAddressing::InlineSchema());
        properties["style_id"] = Schema::String("Paragraph style identifier.");
        properties["alignment"] = Schema::Enumeration("Paragraph alignment.", WordAddressing::AlignmentTokens());

        auto definition = MakeDefinition("edit_paragraph", "Edit paragraph",
                                         "Replace the content and formatting of one paragraph. Omit 'text' and "
                                         "'inlines' to change only the style or alignment.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of edit_paragraph.", {"documentId", "block"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Edited paragraph.", {"block"},
                                            nlohmann::json{{"block", Schema::Integer("Index of the paragraph.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"block", 3}, {"text", "Revised paragraph text"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return EditParagraph(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome EditParagraph(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size block = arguments.value("block", static_cast<Size>(0));

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto paragraph = WordAddressing::ParagraphAt(session.Editor(), block, failure);
        if (paragraph == nullptr)
        {
            return failure;
        }

        if (!ApplyParagraphSettings(context, session.Editor(), *paragraph, arguments, failure))
        {
            return failure;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["block"] = static_cast<UInt64>(block);

        return ResultBuilder("Rewrote block " + std::to_string(block) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDeleteBlocks(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["from"] = Schema::Integer("First block to delete, 1-based.", 1);
        properties["to"] = Schema::Integer("Last block to delete; omit to delete only 'from'.", 1);

        auto definition = MakeDefinition("delete_blocks", "Delete body blocks",
                                         "Delete a range of body blocks. Block indices shift afterwards, so read "
                                         "them back before addressing content again.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of delete_blocks.", {"documentId", "from"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Deletion result.", {"deleted"},
                           nlohmann::json{{"deleted", Schema::Integer("Number of deleted blocks.")},
                                          {"blockCount", Schema::Integer("Remaining number of body blocks.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"from", 5}, {"to", 7}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteBlocks(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteBlocks(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size from = arguments.value("from", static_cast<Size>(0));
        const Size to = std::max(from, arguments.value("to", from));

        MutationGuard guard(session.Session());

        const auto blocks = session.Editor().BodyBlocks();
        if (from == 0 || from > blocks.size())
        {
            return MakeError(ErrorCode::BlockNotFound,
                             "The document has " + std::to_string(blocks.size()) + " body block(s); block " +
                                 std::to_string(from) + " does not exist.",
                             std::to_string(from), "Call read_blocks to see the current block indices.");
        }

        Size deleted = 0;
        for (Size index = from; index <= to && index <= blocks.size(); ++index)
        {
            auto element = blocks[index - 1].GetLowLevelApi();
            if (element && element->Remove())
            {
                ++deleted;
            }
        }

        if (deleted == 0)
        {
            return MakeError(ErrorCode::OperationFailed, "No block could be deleted.", std::to_string(from));
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["deleted"] = static_cast<UInt64>(deleted);
        data["blockCount"] = static_cast<UInt64>(session.Editor().BodyBlocks().size());

        return ResultBuilder("Deleted " + std::to_string(deleted) + " block(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterApplyStyle(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["blocks"] = Schema::Array("1-based block indices to restyle.",
                                             Schema::Integer("Body block index.", 1));
        properties["style_id"] = Schema::String("Paragraph style identifier to apply.");

        auto definition = MakeDefinition("apply_style", "Apply paragraph style",
                                         "Apply one paragraph style to several blocks at once.", "content");
        definition.InputSchema =
            Schema::Object("Arguments of apply_style.", {"documentId", "blocks", "style_id"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Restyled blocks.", {"styled"},
                           nlohmann::json{{"styled", Schema::Integer("Number of restyled paragraphs.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"}, {"blocks", nlohmann::json::array({2, 3})}, {"style_id", "Quote"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ApplyStyle(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ApplyStyle(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto styleId = arguments.value("style_id", std::string());
        if (!session.Editor().Styles().HasStyle(styleId))
        {
            return MakeError(ErrorCode::StyleNotFound, "The document defines no style '" + styleId + "'.", styleId,
                             "Call list_styles to see the available style identifiers.");
        }

        MutationGuard guard(session.Session());

        Size styled = 0;
        ToolOutcome failure;
        for (const auto& entry : arguments.at("blocks"))
        {
            auto paragraph = WordAddressing::ParagraphAt(session.Editor(), entry.get<Size>(), failure);
            if (paragraph == nullptr)
            {
                return failure;
            }

            paragraph->SetStyleId(styleId);
            ++styled;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["styled"] = static_cast<UInt64>(styled);

        return ResultBuilder("Applied '" + styleId + "' to " + std::to_string(styled) + " paragraph(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterInsertImage(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["anchor"] = WordAddressing::AnchorSchema();
        properties["path"] = Schema::String("Workspace-relative image file; mutually exclusive with dataBase64.");
        properties["dataBase64"] = Schema::String("Base64 image payload; mutually exclusive with path.");
        properties["contentType"] = Schema::String("Media type; detected from the payload when omitted.");
        properties["width"] =
            Schema::Length("Rendered width; the aspect ratio is kept when only one dimension is given.");
        properties["height"] =
            Schema::Length("Rendered height; the aspect ratio is kept when only one dimension is given.");
        properties["alt"] = Schema::String("Alternative text.");

        auto definition = MakeDefinition("insert_image", "Insert image",
                                         "Insert a picture as its own paragraph at an anchor. The image format is "
                                         "detected from the payload when contentType is omitted, and omitting both "
                                         "width and height keeps the image's own size.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of insert_image.", {"documentId", "anchor"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Inserted image.", {"block"},
                                            nlohmann::json{{"block", Schema::Integer("Index of the new block.")},
                                                           {"contentType", Schema::String("Media type stored.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"}, {"anchor", nlohmann::json{{"position", "end"}}}, {"path", "logo.png"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return InsertImage(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome InsertImage(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        WordAnchor anchor;
        ToolOutcome failure;
        if (!WordAddressing::ParseAnchor(arguments, anchor, failure))
        {
            return failure;
        }

        std::vector<Byte> bytes;
        std::string contentType;
        if (!ToolSupport::LoadImagePayload(context, arguments, bytes, contentType, failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        Word::WordDocumentEditor::BodyCursor cursor;
        if (!WordAddressing::ResolveCursor(session.Editor(), anchor, cursor, failure))
        {
            return failure;
        }

        auto paragraph = cursor.InsertParagraph();
        if (paragraph == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The image paragraph could not be inserted.");
        }

        if (!WordAddressing::AppendImage(session.Editor(), *paragraph, std::move(bytes), contentType, arguments,
                                         failure))
        {
            return failure;
        }

        guard.Commit();

        const auto index = WordAddressing::IndexOfElement(session.Editor(), paragraph->GetLowLevelApi());

        nlohmann::json data = nlohmann::json::object();
        data["block"] = static_cast<UInt64>(index);
        data["contentType"] = contentType;

        return ResultBuilder("Inserted an image as block " + std::to_string(index) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddBookmark(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["name"] = Schema::String("Bookmark name, unique within the document.");
        properties["block"] = Schema::Integer("1-based paragraph to bookmark.", 1);
        properties["end_block"] =
            Schema::Integer("1-based last paragraph of a range bookmark; defaults to 'block'.", 1);

        auto definition = MakeDefinition("add_bookmark", "Add bookmark",
                                         "Add a bookmark to a paragraph, or to the range from 'block' to "
                                         "'end_block', so internal hyperlinks and templates can address it by "
                                         "name.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of add_bookmark.", {"documentId", "name", "block"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Bookmark.", {"name"},
                                            nlohmann::json{{"name", Schema::String("Bookmark name.")},
                                                           {"id", Schema::Integer("Bookmark identifier.")},
                                                           {"block", Schema::Integer("First block the bookmark "
                                                                                     "covers.")},
                                                           {"endBlock", Schema::Integer("Last block the bookmark "
                                                                                        "covers.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"name", "summary"}, {"block", 2}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddBookmark(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddBookmark(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size block = arguments.value("block", static_cast<Size>(0));
        const Size endBlock = arguments.value("end_block", block);
        if (endBlock < block)
        {
            return MakeError(ErrorCode::AnchorInvalid, "'end_block' must not precede 'block'.",
                             std::to_string(endBlock), "Call read_blocks to see the current block indices.");
        }

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto paragraph = WordAddressing::ParagraphAt(session.Editor(), block, failure);
        if (paragraph == nullptr)
        {
            return failure;
        }

        std::shared_ptr<Word::Paragraph> endParagraph;
        if (endBlock > block)
        {
            endParagraph = WordAddressing::ParagraphAt(session.Editor(), endBlock, failure);
            if (endParagraph == nullptr)
            {
                return failure;
            }
        }

        const auto name = arguments.value("name", std::string());
        auto bookmark = paragraph->AddBookmark(name);
        if (bookmark == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The bookmark could not be added.", name);
        }

        // Paragraph::AddBookmark() marks one paragraph; a range bookmark is the
        // same pair of markers with the end moved to the closing paragraph.
        if (endParagraph != nullptr)
        {
            auto rangeEnd = bookmark->GetEndElement();
            auto lowEnd = endParagraph->GetLowLevelApi();
            if (!rangeEnd || !lowEnd || rangeEnd->MoveInto(lowEnd) == nullptr)
            {
                return MakeError(ErrorCode::OperationFailed, "The bookmark range could not be extended.", name);
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = bookmark->GetName();
        data["id"] = bookmark->GetId();
        data["block"] = static_cast<UInt64>(block);
        data["endBlock"] = static_cast<UInt64>(endBlock);

        return ResultBuilder("Added bookmark '" + name + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- tables -------------------------------------------------------------

    static void RegisterInsertTable(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["anchor"] = WordAddressing::AnchorSchema();
        properties["rows"] = Schema::Integer("Number of rows.", 1, 2000);
        properties["cols"] = Schema::Integer("Number of columns.", 1, 200);
        properties["data"] = Schema::Array("Cell texts, row by row.",
                                           Schema::Array("One row of cell texts.", Schema::String("Cell text.")));
        properties["header_row"] = Schema::BooleanWithDefault("Repeat the first row on every page.", false);
        properties["style_id"] = Schema::String("Table style identifier.");

        auto definition = MakeDefinition("insert_table", "Insert table",
                                         "Insert a table at an anchor, optionally filled from a two-dimensional "
                                         "array of cell texts.",
                                         "tables");
        definition.InputSchema = Schema::Object("Arguments of insert_table.", {"documentId", "anchor", "rows", "cols"},
                                                std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Inserted table.", {"block"},
                           nlohmann::json{{"block", Schema::Integer("Index of the table block.")},
                                          {"rows", Schema::Integer("Row count.")},
                                          {"columns", Schema::Integer("Column count.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"},
                                            {"anchor", nlohmann::json{{"position", "end"}}},
                                            {"rows", 2},
                                            {"cols", 2}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return InsertTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome InsertTable(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        WordAnchor anchor;
        ToolOutcome failure;
        if (!WordAddressing::ParseAnchor(arguments, anchor, failure))
        {
            return failure;
        }

        const Size rows = arguments.value("rows", static_cast<Size>(0));
        const Size columns = arguments.value("cols", static_cast<Size>(0));

        MutationGuard guard(session.Session());

        Word::WordDocumentEditor::BodyCursor cursor;
        if (!WordAddressing::ResolveCursor(session.Editor(), anchor, cursor, failure))
        {
            return failure;
        }

        auto table = cursor.InsertTable(rows, columns);
        if (table == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The table could not be inserted.");
        }

        const auto data = arguments.find("data");
        if (data != arguments.end() && data->is_array())
        {
            for (Size row = 0; row < data->size() && row < rows; ++row)
            {
                const auto& rowValues = (*data)[row];
                if (!rowValues.is_array())
                {
                    continue;
                }

                for (Size column = 0; column < rowValues.size() && column < columns; ++column)
                {
                    table->SetCellText(row, column, rowValues[column].get<std::string>(), true);
                }
            }
        }

        if (arguments.value("header_row", false) && rows > 0)
        {
            table->SetRowHeader(0, true);
        }

        guard.Commit();

        const auto index = WordAddressing::IndexOfElement(session.Editor(), table->GetLowLevelApi());

        nlohmann::json result = nlohmann::json::object();
        result["block"] = static_cast<UInt64>(index);
        result["rows"] = static_cast<UInt64>(table->GetRowCount());
        result["columns"] = static_cast<UInt64>(table->GetLogicalColumnCount());

        return ResultBuilder("Inserted a " + std::to_string(rows) + "x" + std::to_string(columns) +
                             " table as block " + std::to_string(index) + ".")
            .WithSession(session.Session())
            .WithData(std::move(result))
            .Build();
    }

    static void RegisterEditTableCell(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["block"] = Schema::Integer("1-based index of the table block.", 1);
        properties["row"] = Schema::Integer("1-based row in the logical grid.", 1);
        properties["col"] = Schema::Integer("1-based column in the logical grid.", 1);
        properties["text"] = WordAddressing::TextSchema();
        properties["inlines"] = Schema::Array("Formatted inline content.", WordAddressing::InlineSchema());

        auto definition = MakeDefinition("edit_table_cell", "Edit table cell",
                                         "Rewrite one table cell. Row and column address the logical grid, so a "
                                         "merged cell is addressed by its anchor position.",
                                         "tables");
        definition.InputSchema = Schema::Object("Arguments of edit_table_cell.",
                                                {"documentId", "block", "row", "col"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Edited cell.", {"block", "row", "col"},
                           nlohmann::json{{"block", Schema::Integer("Table block index.")},
                                          {"row", Schema::Integer("Row index.")},
                                          {"col", Schema::Integer("Column index.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"block", 4}, {"row", 1}, {"col", 2}, {"text", "42"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return EditTableCell(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome EditTableCell(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size block = arguments.value("block", static_cast<Size>(0));
        const Size row = arguments.value("row", static_cast<Size>(0));
        const Size column = arguments.value("col", static_cast<Size>(0));

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto table = WordAddressing::TableAt(session.Editor(), block, failure);
        if (table == nullptr)
        {
            return failure;
        }

        if (row == 0 || column == 0 || row > table->GetRowCount() || column > table->GetLogicalColumnCount())
        {
            return MakeError(ErrorCode::AnchorInvalid,
                             "The table has " + std::to_string(table->GetRowCount()) + " row(s) and " +
                                 std::to_string(table->GetLogicalColumnCount()) + " column(s).",
                             std::to_string(row) + "," + std::to_string(column),
                             "Call read_blocks to see the table dimensions.");
        }

        const auto grid = table->GetLogicalGrid();
        if (row - 1 < grid.size() && column - 1 < grid[row - 1].size() && !grid[row - 1][column - 1].IsOrigin)
        {
            return MakeError(ErrorCode::AnchorInvalid,
                             "The addressed cell is covered by a merge and holds no content of its own.",
                             std::to_string(row) + "," + std::to_string(column),
                             "Address the anchor cell of the merged region instead.");
        }

        table->SetCellText(row - 1, column - 1, arguments.value("text", std::string()), true);

        const auto inlines = arguments.find("inlines");
        if (inlines != arguments.end() && inlines->is_array())
        {
            const auto paragraphs = CellParagraphs(*table, row - 1, column - 1);
            if (paragraphs.empty())
            {
                return MakeError(ErrorCode::OperationFailed, "The cell has no paragraph to write into.");
            }

            if (!WordAddressing::ApplyContent(context, session.Editor(), *paragraphs.front(), arguments, failure))
            {
                return failure;
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["block"] = static_cast<UInt64>(block);
        data["row"] = static_cast<UInt64>(row);
        data["col"] = static_cast<UInt64>(column);

        return ResultBuilder("Rewrote cell " + std::to_string(row) + "," + std::to_string(column) + " of block " +
                             std::to_string(block) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /// Paragraphs of one logical grid cell, or an empty list when it is covered.
    static std::vector<std::shared_ptr<Word::Paragraph>> CellParagraphs(Word::Table& table, Size row, Size column)
    {
        const auto grid = table.GetLogicalGrid();
        if (row >= grid.size() || column >= grid[row].size())
        {
            return {};
        }

        auto cell = grid[row][column].Cell;
        if (!cell)
        {
            return {};
        }

        std::vector<std::shared_ptr<Word::Paragraph>> paragraphs;
        for (const auto& child : cell->Elements<W::Paragraph>())
        {
            paragraphs.push_back(std::make_shared<Word::Paragraph>(child));
        }

        return paragraphs;
    }

    /**
     * @brief The border styles a table is worth offering.
     *
     * `BorderValues` carries more than two hundred values, most of them the
     * decorative art borders Word draws around a page — apples, balloons,
     * pacifiers. Publishing those in the catalog would cost every client more
     * context than the rest of the tool, and none of them belongs on a table,
     * so the enumeration here is the set of line styles that does.
     */
    static nlohmann::json BorderStyleSchema(std::string description)
    {
        return Schema::Enumeration(std::move(description),
                                   {"none", "single", "thick", "double", "dotted", "dashed", "dotDash",
                                    "dotDotDash", "triple", "wave", "dashSmallGap", "threeDEmboss",
                                    "threeDEngrave", "outset", "inset"});
    }

    static W::BorderValues ParseBorderStyle(const std::string& token)
    {
        const auto* meta = W::BorderValues::GetMetaEnum();
        if (meta == nullptr)
        {
            return W::BorderValues::Single;
        }

        const auto raw = meta->FromString(token);
        return raw == W::BorderValues::NotDefinedEnumValue
                   ? W::BorderValues::Single
                   : static_cast<W::BorderValues::Value>(raw);
    }

    /// Schema of a border specification, shared by the table and the cells.
    static nlohmann::json BorderSchema(std::string description)
    {
        return Schema::Object(std::move(description), {"style"},
                              nlohmann::json{{"style", BorderStyleSchema("Line style.")},
                                             {"width", Schema::Length("Line width; defaults to half a point.")},
                                             {"color", Schema::String("Line color as \"#RRGGBB\".")}});
    }

    /// Reads one border specification into its three library arguments.
    static bool ReadBorder(const nlohmann::json& source, W::BorderValues& style,
                           MeasuringUnits& width, Color& color, ToolOutcome& failure)
    {
        style = ParseBorderStyle(source.value("style", std::string("single")));

        width = MeasuringUnits{0.5, MeasurementUnit::Point};
        if (const auto value = source.find("width"); value != source.end())
        {
            const auto parsed = ParseLength(*value);
            if (!parsed.has_value() || ToPointValue(*parsed) < 0.0)
            {
                failure = MakeError(ErrorCode::InputInvalid, "The border width is not a non-negative length.",
                                    "width");
                return false;
            }

            width = *parsed;
        }

        color = Color(0, 0, 0);
        if (const auto value = source.find("color"); value != source.end())
        {
            const auto parsed = ParseColor(value->get<std::string>());
            if (!parsed.has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid, "The border color is not \"#RRGGBB\".", "color");
                return false;
            }

            color = *parsed;
        }

        return true;
    }

    /// Reads the four members of a margin box, any of which may be absent.
    static bool ReadMarginBox(const nlohmann::json& source, MeasuringUnits& left, MeasuringUnits& top,
                              MeasuringUnits& right, MeasuringUnits& bottom, ToolOutcome& failure)
    {
        const auto read = [&source, &failure](const char* name, MeasuringUnits& target)
        {
            const auto value = source.find(name);
            if (value == source.end())
            {
                return true;
            }

            const auto parsed = ParseLength(*value);
            if (!parsed.has_value() || ToPointValue(*parsed) < 0.0)
            {
                failure = MakeError(ErrorCode::InputInvalid, "The margin is not a non-negative length.", name);
                return false;
            }

            target = *parsed;
            return true;
        };

        return read("left", left) && read("top", top) && read("right", right) && read("bottom", bottom);
    }

    static void RegisterFormatTable(ToolRegistry& registry)
    {
        nlohmann::json marginBox =
            Schema::Object("Cell padding; omitted sides keep their current value.", {},
                           nlohmann::json{{"left", Schema::Length("Left padding.")},
                                          {"top", Schema::Length("Top padding.")},
                                          {"right", Schema::Length("Right padding.")},
                                          {"bottom", Schema::Length("Bottom padding.")}});

        nlohmann::json cell = Schema::Object(
            "One cell to format, addressed in the logical grid.", {"row", "col"},
            nlohmann::json{{"row", Schema::Integer("1-based row.", 1)},
                           {"col", Schema::Integer("1-based column.", 1)},
                           {"background", Schema::String("Cell fill color as \"#RRGGBB\".")},
                           {"width", Schema::Length("Cell width.")},
                           {"align", Schema::Enumeration("Horizontal alignment of the cell text.",
                                                         {"left", "center", "right", "both"})},
                           {"valign", Schema::Enumeration("Vertical alignment of the cell content.",
                                                          {"top", "center", "bottom"})},
                           {"borders", BorderSchema("Borders of this cell.")},
                           {"margins", marginBox}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["block"] = Schema::Integer("1-based index of the table block.", 1);
        properties["width"] = Schema::Length("Total table width.");
        properties["alignment"] = Schema::Enumeration("Horizontal alignment of the table on the page.",
                                                      {"left", "center", "right"});
        properties["borders"] = BorderSchema("Borders applied to every side of the table.");
        properties["cell_margins"] = marginBox;
        properties["column_widths"] =
            Schema::Array("Width of each column, in order; shorter arrays leave the rest alone.",
                          Schema::Length("One column width."));
        properties["cells"] = Schema::Array("Individual cells to format.", std::move(cell));

        auto definition =
            MakeDefinition("format_table", "Format table",
                           "Set the width, alignment, borders, and cell padding of a table, the width of its "
                           "columns, and the shading, alignment, and borders of individual cells. Every member is "
                           "optional and the ones left out keep their current value.",
                           "tables");
        definition.InputSchema =
            Schema::Object("Arguments of format_table.", {"documentId", "block"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Formatted table.", {"block"},
                           nlohmann::json{{"block", Schema::Integer("Table block index.")},
                                          {"rows", Schema::Integer("Rows in the table.")},
                                          {"columns", Schema::Integer("Columns in the logical grid.")},
                                          {"cellsFormatted", Schema::Integer("Cells the call touched.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"},
                           {"block", 4},
                           {"borders", nlohmann::json{{"style", "single"}, {"color", "#808080"}}},
                           {"cells", nlohmann::json::array({nlohmann::json{
                               {"row", 1}, {"col", 1}, {"background", "#EFEFEF"}, {"align", "center"}}})}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return FormatTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome FormatTable(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size block = arguments.value("block", static_cast<Size>(0));

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto table = WordAddressing::TableAt(session.Editor(), block, failure);
        if (table == nullptr)
        {
            return failure;
        }

        const auto rows = table->GetRowCount();
        const auto columns = table->GetLogicalColumnCount();

        if (const auto width = arguments.find("width"); width != arguments.end())
        {
            const auto parsed = ParseLength(*width);
            if (!parsed.has_value() || ToPointValue(*parsed) <= 0.0)
            {
                return MakeError(ErrorCode::InputInvalid, "The table width is not a positive length.", "width");
            }

            table->SetWidth(*parsed);
        }

        if (const auto alignment = arguments.find("alignment"); alignment != arguments.end())
        {
            const auto token = alignment->get<std::string>();
            table->SetAlignment(token == "center"  ? W::TableRowAlignmentValues::center
                                : token == "right" ? W::TableRowAlignmentValues::right
                                                   : W::TableRowAlignmentValues::left);
        }

        if (const auto borders = arguments.find("borders"); borders != arguments.end())
        {
            W::BorderValues style{};
            MeasuringUnits width;
            Color color;
            if (!ReadBorder(*borders, style, width, color, failure))
            {
                return failure;
            }

            table->SetBorders(style, width, color);
        }

        if (const auto margins = arguments.find("cell_margins"); margins != arguments.end())
        {
            // The library writes all four at once, so the sides the caller left
            // out have to start from Word's own defaults rather than from zero.
            MeasuringUnits left{0.08, MeasurementUnit::Inch};
            MeasuringUnits top{0.0, MeasurementUnit::Inch};
            MeasuringUnits right{0.08, MeasurementUnit::Inch};
            MeasuringUnits bottom{0.0, MeasurementUnit::Inch};
            if (!ReadMarginBox(*margins, left, top, right, bottom, failure))
            {
                return failure;
            }

            table->SetDefaultCellMargins(left, top, right, bottom);
        }

        if (const auto widths = arguments.find("column_widths"); widths != arguments.end())
        {
            Size column = 0;
            for (const auto& entry : *widths)
            {
                if (column >= columns)
                {
                    break;
                }

                const auto parsed = ParseLength(entry);
                if (!parsed.has_value() || ToPointValue(*parsed) <= 0.0)
                {
                    return MakeError(ErrorCode::InputInvalid, "A column width is not a positive length.",
                                     "column_widths");
                }

                // A column width is a property of every cell in that column;
                // Word has no single place to record it.
                for (Size row = 0; row < rows; ++row)
                {
                    table->SetCellWidth(row, column, *parsed);
                }

                ++column;
            }
        }

        Size formatted = 0;
        if (const auto cells = arguments.find("cells"); cells != arguments.end())
        {
            const auto grid = table->GetLogicalGrid();
            for (const auto& entry : *cells)
            {
                const Size row = entry.value("row", static_cast<Size>(0));
                const Size column = entry.value("col", static_cast<Size>(0));
                if (row == 0 || column == 0 || row > rows || column > columns)
                {
                    return MakeError(ErrorCode::AnchorInvalid,
                                     "The table has " + std::to_string(rows) + " row(s) and " +
                                         std::to_string(columns) + " column(s).",
                                     std::to_string(row) + "," + std::to_string(column),
                                     "Call read_blocks to see the table dimensions.");
                }

                // Formatting a position a merge covers would write cell
                // properties nothing renders, so it is refused the same way
                // edit_table_cell refuses to write text there.
                if (row - 1 < grid.size() && column - 1 < grid[row - 1].size() &&
                    !grid[row - 1][column - 1].IsOrigin)
                {
                    return MakeError(ErrorCode::AnchorInvalid,
                                     "The addressed cell is covered by a merge and carries no formatting of its "
                                     "own.",
                                     std::to_string(row) + "," + std::to_string(column),
                                     "Address the anchor cell of the merged region instead.");
                }

                if (const auto background = entry.find("background"); background != entry.end())
                {
                    const auto color = ParseColor(background->get<std::string>());
                    if (!color.has_value())
                    {
                        return MakeError(ErrorCode::InputInvalid, "The cell background is not \"#RRGGBB\".",
                                         "background");
                    }

                    table->SetCellBackgroundColor(row - 1, column - 1, *color);
                }

                if (const auto width = entry.find("width"); width != entry.end())
                {
                    const auto parsed = ParseLength(*width);
                    if (!parsed.has_value() || ToPointValue(*parsed) <= 0.0)
                    {
                        return MakeError(ErrorCode::InputInvalid, "A cell width is not a positive length.",
                                         "width");
                    }

                    table->SetCellWidth(row - 1, column - 1, *parsed);
                }

                if (const auto align = entry.find("align"); align != entry.end())
                {
                    const auto token = align->get<std::string>();
                    table->SetCellHorizontalAlignment(
                        row - 1, column - 1,
                        token == "center" ? W::JustificationValues::Center
                        : token == "right" ? W::JustificationValues::Right
                        : token == "both"  ? W::JustificationValues::Both
                                           : W::JustificationValues::Left);
                }

                if (const auto valign = entry.find("valign"); valign != entry.end())
                {
                    const auto token = valign->get<std::string>();
                    table->SetCellVerticalAlignment(
                        row - 1, column - 1,
                        token == "center"   ? W::TableVerticalAlignmentValues::center
                        : token == "bottom" ? W::TableVerticalAlignmentValues::bottom
                                            : W::TableVerticalAlignmentValues::top);
                }

                if (const auto borders = entry.find("borders"); borders != entry.end())
                {
                    W::BorderValues style{};
                    MeasuringUnits width;
                    Color color;
                    if (!ReadBorder(*borders, style, width, color, failure))
                    {
                        return failure;
                    }

                    table->SetCellBorders(row - 1, column - 1, style, width, color);
                }

                if (const auto margins = entry.find("margins"); margins != entry.end())
                {
                    MeasuringUnits left{0.08, MeasurementUnit::Inch};
                    MeasuringUnits top{0.0, MeasurementUnit::Inch};
                    MeasuringUnits right{0.08, MeasurementUnit::Inch};
                    MeasuringUnits bottom{0.0, MeasurementUnit::Inch};
                    if (!ReadMarginBox(*margins, left, top, right, bottom, failure))
                    {
                        return failure;
                    }

                    table->SetCellMargins(row - 1, column - 1, left, top, right, bottom);
                }

                ++formatted;
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["block"] = static_cast<UInt64>(block);
        data["rows"] = static_cast<UInt64>(rows);
        data["columns"] = static_cast<UInt64>(columns);
        data["cellsFormatted"] = static_cast<UInt64>(formatted);

        return ResultBuilder("Formatted the table in block " + std::to_string(block) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterModifyTable(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["block"] = Schema::Integer("1-based index of the table block.", 1);
        properties["operation"] = Schema::Enumeration(
            "Structural change to apply.",
            {"add_row", "delete_row", "add_column", "delete_column", "merge_cells"});
        properties["at"] = Schema::Integer("1-based row or column the operation applies to.", 1);
        properties["count"] = Schema::IntegerWithDefault(
            "Number of rows or columns to add or delete; deleting stops at the last one that exists.", 1, 1, 200);
        properties["range"] = Schema::Object(
            "Region to merge, for the merge_cells operation.", {"row", "col", "rowSpan", "colSpan"},
            nlohmann::json{{"row", Schema::Integer("First row, 1-based.", 1)},
                           {"col", Schema::Integer("First column, 1-based.", 1)},
                           {"rowSpan", Schema::Integer("Number of rows to span.", 1)},
                           {"colSpan", Schema::Integer("Number of columns to span.", 1)}});

        auto definition = MakeDefinition("modify_table", "Modify table structure",
                                         "Add or delete table rows and columns, or merge a rectangular region of "
                                         "cells. A delete 'count' larger than what is left removes the rest and "
                                         "reports it; a merge range outside the table is refused.",
                                         "tables");
        definition.InputSchema = Schema::Object("Arguments of modify_table.", {"documentId", "block", "operation"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New table dimensions.", {"rows", "columns"},
                                            nlohmann::json{{"rows", Schema::Integer("Row count after the change.")},
                                                           {"columns", Schema::Integer("Column count after the "
                                                                                       "change.")},
                                                           {"removed", Schema::Integer("Rows or columns actually "
                                                                                       "removed.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"block", 4}, {"operation", "add_row"}, {"count", 2}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ModifyTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ModifyTable(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size block = arguments.value("block", static_cast<Size>(0));
        const auto operation = arguments.value("operation", std::string());
        const Size at = arguments.value("at", static_cast<Size>(0));
        const Size count = arguments.value("count", static_cast<Size>(1));

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto table = WordAddressing::TableAt(session.Editor(), block, failure);
        if (table == nullptr)
        {
            return failure;
        }

        Size removed = 0;
        if (operation == "add_row")
        {
            for (Size iteration = 0; iteration < count; ++iteration)
            {
                if (at == 0)
                {
                    table->AddRow();
                }
                else
                {
                    table->InsertRow(at - 1);
                }
            }
        }
        else if (operation == "delete_row")
        {
            const Size rows = table->GetRowCount();
            if (at == 0 || at > rows)
            {
                return MakeError(ErrorCode::AnchorInvalid,
                                 "The table has " + std::to_string(rows) + " row(s); row " + std::to_string(at) +
                                     " does not exist.",
                                 std::to_string(at), "Call read_blocks to see the table dimensions.");
            }

            // 'count' may reach past the last row; only what exists is removed
            // and the answer reports how many rows that was.
            removed = std::min(count, rows - at + 1);
            for (Size iteration = 0; iteration < removed; ++iteration)
            {
                table->RemoveRow(at - 1);
            }
        }
        else if (operation == "add_column")
        {
            for (Size iteration = 0; iteration < count; ++iteration)
            {
                if (at == 0)
                {
                    table->AddColumn();
                }
                else
                {
                    table->InsertColumn(at - 1);
                }
            }
        }
        else if (operation == "delete_column")
        {
            const Size columns = table->GetLogicalColumnCount();
            if (at == 0 || at > columns)
            {
                return MakeError(ErrorCode::AnchorInvalid,
                                 "The table has " + std::to_string(columns) + " column(s); column " +
                                     std::to_string(at) + " does not exist.",
                                 std::to_string(at), "Call read_blocks to see the table dimensions.");
            }

            removed = std::min(count, columns - at + 1);
            for (Size iteration = 0; iteration < removed; ++iteration)
            {
                table->RemoveColumn(at - 1);
            }
        }
        else if (operation == "merge_cells")
        {
            const auto range = arguments.find("range");
            if (range == arguments.end() || !range->is_object())
            {
                return MakeError(ErrorCode::InputInvalid, "The merge_cells operation needs a 'range' object.", {},
                                 "Pass {\"row\":1,\"col\":1,\"rowSpan\":2,\"colSpan\":2}.");
            }

            const Size rows = table->GetRowCount();
            const Size columns = table->GetLogicalColumnCount();
            const Size row = range->value("row", static_cast<Size>(1));
            const Size column = range->value("col", static_cast<Size>(1));
            const Size rowSpan = range->value("rowSpan", static_cast<Size>(1));
            const Size columnSpan = range->value("colSpan", static_cast<Size>(1));
            const std::string origin = std::to_string(row) + "," + std::to_string(column);
            if (row == 0 || column == 0 || row > rows || column > columns)
            {
                return MakeError(ErrorCode::AnchorInvalid,
                                 "The table has " + std::to_string(rows) + " row(s) and " + std::to_string(columns) +
                                     " column(s); cell " + origin + " does not exist.",
                                 origin, "Call read_blocks to see the table dimensions.");
            }

            if (rowSpan == 0 || columnSpan == 0 || row + rowSpan - 1 > rows || column + columnSpan - 1 > columns)
            {
                return MakeError(ErrorCode::RangeInvalid,
                                 "The merge range " + std::to_string(rowSpan) + "x" + std::to_string(columnSpan) +
                                     " from cell " + origin + " runs past the " + std::to_string(rows) + "x" +
                                     std::to_string(columns) + " table.",
                                 origin,
                                 "Reduce rowSpan and colSpan, or grow the table with add_row and add_column first.");
            }

            table->MergeCells(row - 1, column - 1, rowSpan, columnSpan);
        }
        else
        {
            return MakeError(ErrorCode::InputInvalid, "Unknown table operation '" + operation + "'.", operation);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["rows"] = static_cast<UInt64>(table->GetRowCount());
        data["columns"] = static_cast<UInt64>(table->GetLogicalColumnCount());
        data["removed"] = static_cast<UInt64>(removed);

        ResultBuilder builder("Applied '" + operation + "' to the table in block " + std::to_string(block) + ".");
        builder.WithSession(session.Session()).WithData(std::move(data));
        if (removed > 0 && removed < count)
        {
            builder.WithWarning("count_clamped",
                                "Only " + std::to_string(removed) + " of the requested " + std::to_string(count) +
                                    " were left to remove.",
                                "count");
        }

        return builder.Build();
    }

    // --- layout -------------------------------------------------------------

    static void RegisterSetHeaderFooter(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["target"] = Schema::Enumeration("Which running element to write.", {"header", "footer"});
        properties["kind"] = Schema::EnumerationWithDefault("Which page variant to write.",
                                                            {"default", "first", "even"}, "default");
        properties["section"] = Schema::Integer("1-based section; omit for the last section.", 1);
        properties["text"] = Schema::String("Plain text content.");
        properties["lines"] = Schema::Array("One plain-text paragraph per entry.", Schema::String("Paragraph text."));
        properties["inlines"] = Schema::Array("Formatted inline content of a single paragraph.",
                                              WordAddressing::InlineSchema());
        properties["blocks"] = Schema::Array(
            "One paragraph per entry, each with its own 'text' or 'inlines'.",
            Schema::Object("One header or footer paragraph.", {},
                           nlohmann::json{{"text", WordAddressing::TextSchema()},
                                          {"inlines", Schema::Array("Formatted inline content.",
                                                                    WordAddressing::InlineSchema())}}));

        auto definition = MakeDefinition("set_header_footer", "Set header or footer",
                                         "Replace the content of a header or footer. Existing paragraphs are "
                                         "discarded. Use 'inlines' or 'blocks' for a page-number field or other "
                                         "formatted content; a picture and an external link are not supported in "
                                         "a running element.",
                                         "layout");
        definition.InputSchema =
            Schema::Object("Arguments of set_header_footer.", {"documentId", "target"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Written running element.", {"target", "kind"},
                           nlohmann::json{{"target", Schema::String("header or footer.")},
                                          {"kind", Schema::String("default, first, or even.")},
                                          {"paragraphs", Schema::Integer("Number of written paragraphs.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"target", "footer"}, {"text", "Confidential"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetHeaderFooter(context, arguments); };
        registry.Add(std::move(definition));
    }

    /**
     * @brief Names the first inline a running element cannot carry.
     *
     * A picture and an external hyperlink are both backed by a relationship of
     * the part that holds them, and the editor only creates those on the main
     * document part, so a header or footer would end up referencing a
     * relationship it does not have. Text runs, breaks, bookmark links, and
     * fields such as PAGE are unaffected.
     *
     * @return "image", "link", or an empty string when everything is writable.
     */
    static std::string UnsupportedRunningInline(const nlohmann::json& holder)
    {
        const auto inlines = holder.find("inlines");
        if (inlines == holder.end() || !inlines->is_array())
        {
            return std::string();
        }

        for (const auto& item : *inlines)
        {
            if (!item.is_object())
            {
                continue;
            }

            if (item.contains("image"))
            {
                return "image";
            }

            const auto link = item.find("link");
            if (link != item.end() && link->is_object() && !link->value("target", std::string()).empty())
            {
                return "link";
            }
        }

        return std::string();
    }

    /// Refuses the inline kinds a header or footer cannot carry.
    static bool HeaderFooterContentIsSupported(const nlohmann::json& arguments, ToolOutcome& failure)
    {
        std::string unsupported = UnsupportedRunningInline(arguments);
        const auto blocks = arguments.find("blocks");
        if (unsupported.empty() && blocks != arguments.end() && blocks->is_array())
        {
            for (const auto& entry : *blocks)
            {
                if (!entry.is_object())
                {
                    continue;
                }

                unsupported = UnsupportedRunningInline(entry);
                if (!unsupported.empty())
                {
                    break;
                }
            }
        }

        if (!unsupported.empty())
        {
            const std::string hint =
                unsupported == "image"
                    ? std::string("Keep the picture in the body with insert_image.")
                    : std::string("Link to a bookmark with 'anchor' instead of an external 'target'.");
            failure = MakeError(ErrorCode::Unsupported,
                                "A header or footer cannot hold a '" + unsupported +
                                    "' inline; its relationship would belong to the main document part.",
                                unsupported, hint);
            return false;
        }

        return true;
    }

    static Word::HeaderFooterType ParseHeaderFooterType(const std::string& token)
    {
        if (token == "first")
        {
            return Word::HeaderFooterType::First;
        }

        if (token == "even")
        {
            return Word::HeaderFooterType::Even;
        }

        return Word::HeaderFooterType::Default;
    }

    static ToolOutcome SetHeaderFooter(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        if (!HeaderFooterContentIsSupported(arguments, failure))
        {
            return failure;
        }

        const auto blocks = arguments.find("blocks");
        MutationGuard guard(session.Session());

        auto section = ResolveSection(session.Editor(), arguments, failure);
        if (section == nullptr)
        {
            return failure;
        }

        const auto target = arguments.value("target", std::string());
        const auto kind = ParseHeaderFooterType(arguments.value("kind", std::string("default")));
        auto content = target == "header" ? section->EnsureHeader(kind) : section->EnsureFooter(kind);
        if (content == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The running element could not be created.", target);
        }

        content->Clear();

        Size paragraphs = 0;
        const auto lines = arguments.find("lines");
        if (blocks != arguments.end() && blocks->is_array())
        {
            for (const auto& entry : *blocks)
            {
                if (!entry.is_object())
                {
                    return MakeError(ErrorCode::InputInvalid, "Every entry of 'blocks' must be an object.", "blocks",
                                     "Pass [{\"text\": \"...\"}] or [{\"inlines\": [...]}].");
                }

                auto paragraph = content->AddParagraph();
                if (paragraph == nullptr)
                {
                    return MakeError(ErrorCode::OperationFailed, "A header or footer paragraph could not be created.",
                                     target);
                }

                if (!WordAddressing::ApplyContent(context, session.Editor(), *paragraph, entry, failure))
                {
                    return failure;
                }

                ++paragraphs;
            }
        }
        else if (arguments.contains("inlines"))
        {
            auto paragraph = content->AddParagraph();
            if (paragraph == nullptr)
            {
                return MakeError(ErrorCode::OperationFailed, "A header or footer paragraph could not be created.",
                                 target);
            }

            if (!WordAddressing::ApplyContent(context, session.Editor(), *paragraph, arguments, failure))
            {
                return failure;
            }

            paragraphs = 1;
        }
        else if (lines != arguments.end() && lines->is_array())
        {
            for (const auto& line : *lines)
            {
                content->AddParagraph(line.get<std::string>(), true);
                ++paragraphs;
            }
        }
        else
        {
            content->AddParagraph(arguments.value("text", std::string()), true);
            paragraphs = 1;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["target"] = target;
        data["kind"] = arguments.value("kind", std::string("default"));
        data["paragraphs"] = static_cast<UInt64>(paragraphs);

        return ResultBuilder("Wrote the " + target + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /**
     * @brief Resolves the `section` argument, or fills @p failure.
     *
     * An index the document does not have is refused instead of being clamped
     * to the last section: silently editing a different section than the one
     * addressed is worse than a failure the caller can react to.
     */
    static std::shared_ptr<Word::Section> ResolveSection(Word::WordDocumentEditor& editor,
                                                         const nlohmann::json& arguments, ToolOutcome& failure)
    {
        const auto sections = editor.Sections();
        const Size index = arguments.value("section", static_cast<Size>(0));
        if (index > 0)
        {
            if (index > sections.size())
            {
                failure = MakeError(ErrorCode::AnchorInvalid,
                                    "The document has " + std::to_string(sections.size()) + " section(s); section " +
                                        std::to_string(index) + " does not exist.",
                                    std::to_string(index),
                                    "Omit 'section' to address the last section; open_document reports "
                                    "sectionCount in its summary.");
                return nullptr;
            }

            return sections[index - 1];
        }

        if (!sections.empty())
        {
            return sections.back();
        }

        auto ensured = editor.EnsureFinalSection();
        if (ensured == nullptr)
        {
            failure = MakeError(ErrorCode::OperationFailed, "The document has no section properties to modify.", {},
                                "Insert at least one paragraph before writing the page setup.");
        }

        return ensured;
    }

    static void RegisterSetSection(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["section"] = Schema::Integer("1-based section; omit for the last section.", 1);
        properties["page_size"] = Schema::Object(
            "Page dimensions; use either 'preset' or both 'width' and 'height'.", {},
            nlohmann::json{{"preset", Schema::Enumeration("Named paper size.", {"A3", "A4", "A5", "Letter", "Legal"})},
                           {"width", Schema::Length("Page width.")},
                           {"height", Schema::Length("Page height.")}});
        properties["orientation"] = Schema::Enumeration("Page orientation.", {"portrait", "landscape"});
        properties["margins"] = Schema::Object("Page margins; omitted members keep their value.", {},
                                               nlohmann::json{{"top", Schema::Length("Top margin.")},
                                                              {"right", Schema::Length("Right margin.")},
                                                              {"bottom", Schema::Length("Bottom margin.")},
                                                              {"left", Schema::Length("Left margin.")},
                                                              {"header", Schema::Length("Header distance.")},
                                                              {"footer", Schema::Length("Footer distance.")},
                                                              {"gutter", Schema::Length("Gutter.")}});

        auto definition = MakeDefinition("set_section", "Set page setup",
                                         "Set the page size, orientation, and margins of a section.", "layout");
        definition.InputSchema = Schema::Object("Arguments of set_section.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Applied page setup.", {},
                           nlohmann::json{{"widthPt", Schema::Number("Page width in points.")},
                                          {"heightPt", Schema::Number("Page height in points.")},
                                          {"orientation", Schema::String("portrait or landscape.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"}, {"page_size", nlohmann::json{{"preset", "A4"}}}, {"orientation", "landscape"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetSection(context, arguments); };
        registry.Add(std::move(definition));
    }

    /// Paper presets in the units the page-size element stores.
    static bool PresetPageSize(const std::string& preset, MeasuringUnits& width, MeasuringUnits& height)
    {
        if (preset == "A3")
        {
            width = MeasuringUnits(297.0, MeasurementUnit::Millimeter);
            height = MeasuringUnits(420.0, MeasurementUnit::Millimeter);
            return true;
        }

        if (preset == "A4")
        {
            width = MeasuringUnits(210.0, MeasurementUnit::Millimeter);
            height = MeasuringUnits(297.0, MeasurementUnit::Millimeter);
            return true;
        }

        if (preset == "A5")
        {
            width = MeasuringUnits(148.0, MeasurementUnit::Millimeter);
            height = MeasuringUnits(210.0, MeasurementUnit::Millimeter);
            return true;
        }

        if (preset == "Letter")
        {
            width = MeasuringUnits(8.5, MeasurementUnit::Inch);
            height = MeasuringUnits(11.0, MeasurementUnit::Inch);
            return true;
        }

        if (preset == "Legal")
        {
            width = MeasuringUnits(8.5, MeasurementUnit::Inch);
            height = MeasuringUnits(14.0, MeasurementUnit::Inch);
            return true;
        }

        return false;
    }

    static ToolOutcome SetSection(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto section = ResolveSection(session.Editor(), arguments, failure);
        if (section == nullptr)
        {
            return failure;
        }

        auto pageSize = section->GetPageSize().value_or(Word::SectionPageSize{
            MeasuringUnits(210.0, MeasurementUnit::Millimeter), MeasuringUnits(297.0, MeasurementUnit::Millimeter),
            Word::PageOrientation::Portrait});

        const auto size = arguments.find("page_size");
        if (size != arguments.end() && size->is_object())
        {
            const auto preset = size->value("preset", std::string());
            if (!preset.empty())
            {
                if (!PresetPageSize(preset, pageSize.Width, pageSize.Height))
                {
                    return MakeError(ErrorCode::InputInvalid, "Unknown page size preset '" + preset + "'.", preset);
                }
            }
            else
            {
                const auto width = size->find("width");
                const auto height = size->find("height");
                if (width == size->end() || height == size->end())
                {
                    return MakeError(ErrorCode::InputInvalid,
                                     "'page_size' needs either 'preset' or both 'width' and 'height'.");
                }

                const auto parsedWidth = ParseLength(*width);
                const auto parsedHeight = ParseLength(*height);
                if (!parsedWidth.has_value() || !parsedHeight.has_value())
                {
                    return MakeError(ErrorCode::InputInvalid, "The page width or height is not a valid length.");
                }

                pageSize.Width = *parsedWidth;
                pageSize.Height = *parsedHeight;
            }
        }

        const auto orientation = arguments.value("orientation", std::string());
        if (!orientation.empty())
        {
            pageSize.Orientation =
                orientation == "landscape" ? Word::PageOrientation::Landscape : Word::PageOrientation::Portrait;
            // Landscape stores the wider edge as the width, so the two are
            // swapped when the requested orientation contradicts the values.
            const bool wide = pageSize.Width.ToEmu().GetValue() >= pageSize.Height.ToEmu().GetValue();
            if ((pageSize.Orientation == Word::PageOrientation::Landscape) != wide)
            {
                std::swap(pageSize.Width, pageSize.Height);
            }
        }

        section->SetPageSize(pageSize);

        const auto margins = arguments.find("margins");
        if (margins != arguments.end() && margins->is_object())
        {
            auto values = section->GetMargins().value_or(Word::SectionMargins{});
            ApplyMargin(*margins, "top", values.Top);
            ApplyMargin(*margins, "right", values.Right);
            ApplyMargin(*margins, "bottom", values.Bottom);
            ApplyMargin(*margins, "left", values.Left);
            ApplyMargin(*margins, "header", values.Header);
            ApplyMargin(*margins, "footer", values.Footer);
            ApplyMargin(*margins, "gutter", values.Gutter);
            section->SetMargins(values);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["widthPt"] = ToPointValue(pageSize.Width);
        data["heightPt"] = ToPointValue(pageSize.Height);
        data["orientation"] =
            pageSize.Orientation == Word::PageOrientation::Landscape ? "landscape" : "portrait";

        return ResultBuilder("Updated the section page setup.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void ApplyMargin(const nlohmann::json& margins, const char* name, MeasuringUnits& target)
    {
        const auto member = margins.find(name);
        if (member == margins.end())
        {
            return;
        }

        const auto parsed = ParseLength(*member);
        if (parsed.has_value())
        {
            target = *parsed;
        }
    }

    // --- review -------------------------------------------------------------

    static void RegisterSetTrackedChanges(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["enabled"] = Schema::Boolean("Turn revision tracking on or off.");
        properties["author"] = Schema::String("Author recorded on generated revisions.");

        auto definition = MakeDefinition(
            "set_tracked_changes", "Set revision tracking",
            "Turn the document's revision tracking flag on or off. The flag governs editors that open the "
            "document; the tools of this server write content directly and do not generate revisions themselves, "
            "so use compare_documents when you need tracked differences.",
            "review");
        definition.InputSchema =
            Schema::Object("Arguments of set_tracked_changes.", {"documentId", "enabled"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Tracking state.", {"enabled"},
                                            nlohmann::json{{"enabled", Schema::Boolean("Resulting state.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"enabled", true}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetTrackedChanges(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetTrackedChanges(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        auto document = session.Editor().GetDocument();
        auto mainPart = document ? document->GetMainDocumentPart() : nullptr;
        if (mainPart == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The document has no main part.", session.Session().Id());
        }

        MutationGuard guard(session.Session());

        auto settingsPart = mainPart->GetDocumentSettingsPart();
        if (settingsPart == nullptr)
        {
            settingsPart = mainPart->AddDocumentSettingsPart();
        }

        auto settings = settingsPart ? settingsPart->GetSettings() : nullptr;
        if (settings == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The document settings part could not be created.",
                             session.Session().Id());
        }

        const bool enabled = arguments.value("enabled", false);
        auto existing = settings->GetFirstChildOfType<W::TrackRevisions>();
        if (enabled && existing == nullptr)
        {
            settings->AppendChild<W::TrackRevisions>();
        }
        else if (!enabled && existing != nullptr)
        {
            existing->Remove();
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["enabled"] = enabled;

        ResultBuilder builder(enabled ? "Revision tracking is now on." : "Revision tracking is now off.");
        builder.WithSession(session.Session()).WithData(std::move(data));
        if (arguments.contains("author"))
        {
            builder.WithWarning("author_ignored",
                                "The tracking flag carries no author; pass 'author' to compare_documents instead.",
                                "author");
        }

        return builder.Build();
    }

    static void RegisterListRevisions(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json revision =
            Schema::Object("One tracked revision.", {"id", "kind"},
                           nlohmann::json{{"id", Schema::String("Revision identifier.")},
                                          {"kind", Schema::String("insertion, deletion, moveFrom, or moveTo.")},
                                          {"author", Schema::String("Revision author.")},
                                          {"date", Schema::String("Revision timestamp.")},
                                          {"text", Schema::String("Text the revision covers.")}});

        auto definition = MakeDefinition("list_revisions", "List tracked revisions",
                                         "List the tracked revisions of the document so they can be reviewed "
                                         "before resolve_revisions accepts or rejects them.",
                                         "review");
        definition.InputSchema = Schema::Object("Arguments of list_revisions.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Revisions.", {"revisions"},
                           nlohmann::json{{"revisions", Schema::Array("Tracked revisions.", std::move(revision))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListRevisions(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::string RevisionTypeToken(Word::RevisionType type)
    {
        switch (type)
        {
            case Word::RevisionType::Insertion:
                return "insertion";
            case Word::RevisionType::Deletion:
                return "deletion";
            case Word::RevisionType::MoveFrom:
                return "moveFrom";
            case Word::RevisionType::MoveTo:
                return "moveTo";
            case Word::RevisionType::Unknown:
                break;
        }

        return "unknown";
    }

    static ToolOutcome ListRevisions(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        nlohmann::json revisions = nlohmann::json::array();
        for (const auto& revision : reader.Editor().Revisions())
        {
            if (revision == nullptr)
            {
                continue;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = revision->GetId();
            entry["kind"] = RevisionTypeToken(revision->Type());
            entry["author"] = revision->GetAuthor();
            entry["date"] = revision->GetDate();
            entry["text"] = revision->Text();
            revisions.push_back(std::move(entry));
        }

        const bool truncated = TruncateArrayToBudget(revisions);

        nlohmann::json data = nlohmann::json::object();
        const auto count = revisions.size();
        data["revisions"] = std::move(revisions);

        return ResultBuilder("The document holds " + std::to_string(count) + " tracked revision(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterResolveRevisions(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["mode"] = Schema::Enumeration("What to do with the revisions.", {"accept", "reject"});
        properties["ids"] = Schema::Array("Revision identifiers; omit to resolve every revision.",
                                          Schema::String("Revision identifier."));

        auto definition = MakeDefinition("resolve_revisions", "Accept or reject revisions",
                                         "Accept or reject tracked revisions. Omit 'ids' to resolve the whole "
                                         "document at once.",
                                         "review");
        definition.InputSchema =
            Schema::Object("Arguments of resolve_revisions.", {"documentId", "mode"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Resolution result.", {"resolved"},
                                            nlohmann::json{{"resolved", Schema::Integer("Revisions resolved.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"mode", "accept"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ResolveRevisions(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ResolveRevisions(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto mode = arguments.value("mode", std::string());
        const bool accept = mode == "accept";

        MutationGuard guard(session.Session());

        Size resolved = 0;
        const auto ids = arguments.find("ids");
        if (ids == arguments.end() || !ids->is_array())
        {
            resolved = accept ? session.Editor().AcceptAllRevisions() : session.Editor().RejectAllRevisions();
        }
        else
        {
            std::vector<std::string> wanted;
            for (const auto& id : *ids)
            {
                wanted.push_back(id.get<std::string>());
            }

            for (const auto& revision : session.Editor().Revisions())
            {
                if (revision == nullptr ||
                    std::find(wanted.begin(), wanted.end(), revision->GetId()) == wanted.end())
                {
                    continue;
                }

                if (accept ? revision->Accept() : revision->Reject())
                {
                    ++resolved;
                }
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["resolved"] = static_cast<UInt64>(resolved);

        return ResultBuilder((accept ? "Accepted " : "Rejected ") + std::to_string(resolved) + " revision(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterListComments(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json comment = Schema::Object(
            "One comment.", {"id", "text"},
            nlohmann::json{{"id", Schema::Integer("Comment identifier.")},
                           {"author", Schema::String("Comment author.")},
                           {"initials", Schema::String("Author initials.")},
                           {"date", Schema::String("Authoring timestamp as \"2026-01-31T09:30:00Z\"; empty when "
                                                   "the comment carries none.")},
                           {"text", Schema::String("Comment text.")},
                           {"blocks", Schema::Array("1-based body blocks the comment covers.",
                                                    Schema::Integer("Body block index."))},
                           {"parentId", Schema::Integer("Comment this one replies to; 0 for a thread root.")},
                           {"resolved", Schema::Boolean("True when the comment's thread is marked resolved.")}});

        auto definition = MakeDefinition("list_comments", "List comments",
                                         "List the comments of the document with their identifiers, timestamps, "
                                         "thread structure, and the body blocks they cover.",
                                         "review");
        definition.InputSchema = Schema::Object("Arguments of list_comments.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Comments.", {"comments"},
                           nlohmann::json{{"comments", Schema::Array("Comments.", std::move(comment))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListComments(context, arguments); };
        registry.Add(std::move(definition));
    }

    /// Adds the comment identifier of every marker to @p ids.
    template <typename TElement>
    static void CollectCommentIds(const std::vector<std::shared_ptr<TElement>>& markers, std::set<int>& ids)
    {
        for (const auto& marker : markers)
        {
            if (!marker)
            {
                continue;
            }

            // GetId() answers with a temporary whose View() would dangle, so
            // the identifier is materialized before it is parsed.
            const std::string text = marker->GetId().ToString();
            int value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size())
            {
                ids.insert(value);
            }
        }
    }

    /**
     * @brief Body block indices each comment covers, keyed by comment id.
     *
     * The markers are searched among the descendants of a block, so a comment
     * inside a table cell is reported on the table's block. A comment whose
     * markers live outside the body — in a header or a note — is absent from
     * the map and reported with an empty list.
     */
    static std::map<int, std::vector<Size>> CommentCoverage(Word::WordDocumentEditor& editor)
    {
        std::map<int, std::pair<Size, Size>> spans;
        const auto blocks = editor.BodyBlocks();
        for (Size index = 0; index < blocks.size(); ++index)
        {
            auto element = blocks[index].GetLowLevelApi();
            if (!element)
            {
                continue;
            }

            std::set<int> found;
            CollectCommentIds(element->Descendants<W::CommentRangeStart>(), found);
            CollectCommentIds(element->Descendants<W::CommentRangeEnd>(), found);
            CollectCommentIds(element->Descendants<W::CommentReference>(), found);

            const Size blockIndex = index + 1;
            for (const auto id : found)
            {
                auto span = spans.find(id);
                if (span == spans.end())
                {
                    spans.emplace(id, std::make_pair(blockIndex, blockIndex));
                }
                else
                {
                    span->second.second = blockIndex;
                }
            }
        }

        // A comment range is contiguous, so the blocks between its first and
        // last marker are covered even though they carry no marker themselves.
        std::map<int, std::vector<Size>> coverage;
        for (const auto& [id, span] : spans)
        {
            std::vector<Size> indices;
            for (Size index = span.first; index <= span.second; ++index)
            {
                indices.push_back(index);
            }

            coverage.emplace(id, std::move(indices));
        }

        return coverage;
    }

    static ToolOutcome ListComments(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        const auto coverage = CommentCoverage(reader.Editor());

        nlohmann::json comments = nlohmann::json::array();
        for (const auto& comment : reader.Editor().Comments())
        {
            if (comment == nullptr)
            {
                continue;
            }

            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = comment->GetId();
            entry["author"] = comment->GetAuthor();
            entry["initials"] = comment->GetInitials();
            const auto date = comment->GetDate();
            entry["date"] =
                date.has_value() ? Packaging::DocumentProperties::FormatW3cDateTime(*date) : std::string();
            entry["text"] = comment->PlainText();

            nlohmann::json blocks = nlohmann::json::array();
            const auto covered = coverage.find(comment->GetId());
            if (covered != coverage.end())
            {
                for (const auto index : covered->second)
                {
                    blocks.push_back(static_cast<UInt64>(index));
                }
            }

            entry["blocks"] = std::move(blocks);

            // Thread shape: without it an agent cannot tell a reply from a
            // top-level comment, nor which comment to answer. Identifiers
            // start at 0, so a thread root omits the member rather than
            // reporting a sentinel that is also a valid id.
            if (const auto parent = comment->GetParent())
            {
                entry["parentId"] = parent->GetId();
            }

            entry["resolved"] = comment->IsResolved();

            comments.push_back(std::move(entry));
        }

        const bool truncated = TruncateArrayToBudget(comments);

        nlohmann::json data = nlohmann::json::object();
        const auto count = comments.size();
        data["comments"] = std::move(comments);

        return ResultBuilder("The document holds " + std::to_string(count) + " comment(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterAddComment(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["block"] = Schema::Integer("1-based paragraph the comment refers to.", 1);
        properties["end_block"] =
            Schema::Integer("1-based last paragraph of a comment covering several blocks; defaults to 'block'.", 1);
        properties["text"] = Schema::String("Comment text.");
        properties["author"] = Schema::String("Comment author.");
        properties["initials"] = Schema::String("Author initials.");
        // Comment identifiers start at 0, so no minimum is imposed here.
        properties["parent_id"] =
            Schema::Integer("Comment this one replies to. Pass it instead of 'block': a reply joins the parent's "
                            "thread and shares the range the parent already marks.");

        auto definition = MakeDefinition("add_comment", "Add comment",
                                         "Attach a comment to a paragraph, to the range from 'block' to "
                                         "'end_block', or, with 'parent_id', as a threaded reply to an existing "
                                         "comment.",
                                         "review");
        definition.InputSchema =
            Schema::Object("Arguments of add_comment.", {"documentId", "text"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("New comment.", {"commentId"},
                           nlohmann::json{{"commentId", Schema::Integer("Comment identifier.")},
                                          {"block", Schema::Integer("First block the comment covers.")},
                                          {"endBlock", Schema::Integer("Last block the comment covers.")},
                                          {"parentId", Schema::Integer("Comment this one replies to.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"block", 2}, {"text", "Please verify this figure."}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddComment(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddComment(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        // A reply inherits the range its parent already marks, so a caller
        // addresses either a paragraph or a parent comment, never both.
        const bool hasParent = arguments.contains("parent_id");
        if (hasParent == arguments.contains("block"))
        {
            return MakeError(ErrorCode::InputInvalid,
                             "Pass exactly one of 'block' (a comment on that paragraph) or 'parent_id' (a reply "
                             "sharing its parent's range).",
                             {}, "Call list_comments to see the comment ids you can reply to.");
        }

        if (hasParent)
        {
            if (arguments.contains("end_block"))
            {
                return MakeError(ErrorCode::InputInvalid,
                                 "'end_block' does not apply to a reply; a reply covers the same range as the "
                                 "comment it answers.",
                                 "end_block", "Drop 'end_block' and keep 'parent_id'.");
            }

            return AddReply(session, arguments);
        }

        const Size block = arguments.value("block", static_cast<Size>(0));
        const Size endBlock = arguments.value("end_block", block);
        if (endBlock < block)
        {
            return MakeError(ErrorCode::AnchorInvalid, "'end_block' must not precede 'block'.",
                             std::to_string(endBlock), "Call read_blocks to see the current block indices.");
        }

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto paragraph = WordAddressing::ParagraphAt(session.Editor(), block, failure);
        if (paragraph == nullptr)
        {
            return failure;
        }

        std::shared_ptr<Word::Paragraph> endParagraph;
        if (endBlock > block)
        {
            endParagraph = WordAddressing::ParagraphAt(session.Editor(), endBlock, failure);
            if (endParagraph == nullptr)
            {
                return failure;
            }
        }

        Word::CommentAuthor author;
        author.Name = arguments.value("author", std::string("ExyokiOffice"));
        author.Initials = arguments.value("initials", std::string());

        auto comment = paragraph->AddCommentOnParagraph(arguments.value("text", std::string()), author);
        if (comment == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The comment could not be added.");
        }

        if (endParagraph != nullptr &&
            !ExtendCommentRange(*paragraph, *endParagraph, comment->GetId(), failure))
        {
            return failure;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["commentId"] = comment->GetId();
        data["block"] = static_cast<UInt64>(block);
        data["endBlock"] = static_cast<UInt64>(endBlock);

        return ResultBuilder("Added comment " + std::to_string(comment->GetId()) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /**
     * @brief Adds a threaded reply to an existing comment.
     *
     * The reply gets its own comment id and its own range markers, but Word
     * shows it under its parent and resolves the two together.
     */
    static ToolOutcome AddReply(WordSession& session, const nlohmann::json& arguments)
    {
        const auto parentId = arguments.value("parent_id", 0);

        MutationGuard guard(session.Session());

        auto parent = session.Editor().FindComment(parentId);
        if (parent == nullptr)
        {
            return MakeError(ErrorCode::CommentNotFound,
                             "The document has no comment with id " + std::to_string(parentId) + ".",
                             std::to_string(parentId), "Call list_comments to see the available identifiers.");
        }

        Word::CommentAuthor author;
        author.Name = arguments.value("author", std::string("ExyokiOffice"));
        author.Initials = arguments.value("initials", std::string());

        auto reply = parent->AddReply(arguments.value("text", std::string()), author);
        if (reply == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The reply could not be added.",
                             std::to_string(parentId));
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["commentId"] = reply->GetId();
        data["parentId"] = parentId;

        return ResultBuilder("Added reply " + std::to_string(reply->GetId()) + " to comment " +
                             std::to_string(parentId) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /**
     * @brief Moves the end of a paragraph-local comment into a later paragraph.
     *
     * Paragraph::AddCommentOnParagraph() marks one paragraph, and the library
     * offers no range form, so the closing marker and the reference run are
     * relocated instead. That is exactly the markup Word writes for a comment
     * spanning several paragraphs: the range start stays where it is, the
     * range end and the `w:commentReference` run move to the last paragraph.
     */
    static bool ExtendCommentRange(Word::Paragraph& start, Word::Paragraph& end, int commentId,
                                   ToolOutcome& failure)
    {
        auto lowStart = start.GetLowLevelApi();
        auto lowEnd = end.GetLowLevelApi();
        if (!lowStart || !lowEnd)
        {
            failure = MakeError(ErrorCode::OperationFailed, "The comment range could not be extended.");
            return false;
        }

        const std::string id = std::to_string(commentId);
        std::shared_ptr<OpenXMLElement> rangeEnd;
        for (const auto& candidate : lowStart->Elements<W::CommentRangeEnd>())
        {
            if (candidate && candidate->GetId().ToString() == id)
            {
                rangeEnd = candidate;
                break;
            }
        }

        std::shared_ptr<OpenXMLElement> referenceRun;
        for (const auto& run : lowStart->Elements<W::Run>())
        {
            if (!run)
            {
                continue;
            }

            for (const auto& reference : run->Elements<W::CommentReference>())
            {
                if (reference && reference->GetId().ToString() == id)
                {
                    referenceRun = run;
                    break;
                }
            }

            if (referenceRun)
            {
                break;
            }
        }

        if (!rangeEnd || !referenceRun)
        {
            failure = MakeError(ErrorCode::OperationFailed, "The comment markers could not be located.", id);
            return false;
        }

        auto movedEnd = rangeEnd->MoveInto(lowEnd);
        if (movedEnd == nullptr || referenceRun->MoveAfter(movedEnd) == nullptr)
        {
            failure = MakeError(ErrorCode::OperationFailed, "The comment range could not be extended.", id);
            return false;
        }

        return true;
    }

    static void RegisterDeleteComment(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["comment_id"] = Schema::Integer("Identifier reported by list_comments.");

        auto definition = MakeDefinition("delete_comment", "Delete comment", "Remove one comment from the document.",
                                         "review");
        definition.InputSchema =
            Schema::Object("Arguments of delete_comment.", {"documentId", "comment_id"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Deleted comment.", {"commentId"},
                           nlohmann::json{{"commentId", Schema::Integer("Identifier of the removed comment.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"comment_id", 1}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteComment(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteComment(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto commentId = arguments.value("comment_id", 0);

        MutationGuard guard(session.Session());

        auto comment = session.Editor().FindComment(commentId);
        if (comment == nullptr)
        {
            return MakeError(ErrorCode::CommentNotFound,
                             "The document has no comment with id " + std::to_string(commentId) + ".",
                             std::to_string(commentId), "Call list_comments to see the available identifiers.");
        }

        comment->Remove();
        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["commentId"] = commentId;

        return ResultBuilder("Deleted comment " + std::to_string(commentId) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddNote(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["block"] = Schema::Integer("1-based paragraph the note is anchored to.", 1);
        properties["kind"] = Schema::EnumerationWithDefault("Note kind.", {"footnote", "endnote"}, "footnote");
        properties["text"] = Schema::String("Note text.");

        auto definition = MakeDefinition("add_note", "Add footnote or endnote",
                                         "Append a footnote or endnote reference to a paragraph.", "review");
        definition.InputSchema =
            Schema::Object("Arguments of add_note.", {"documentId", "block", "text"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New note.", {"noteId"},
                                            nlohmann::json{{"noteId", Schema::Integer("Note identifier.")},
                                                           {"kind", Schema::String("footnote or endnote.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"block", 2}, {"text", "Source: internal analysis."}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddNote(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddNote(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        MutationGuard guard(session.Session());

        ToolOutcome failure;
        auto paragraph =
            WordAddressing::ParagraphAt(session.Editor(), arguments.value("block", static_cast<Size>(0)), failure);
        if (paragraph == nullptr)
        {
            return failure;
        }

        const auto kind = arguments.value("kind", std::string("footnote"));
        const auto text = arguments.value("text", std::string());
        auto note = kind == "endnote" ? paragraph->AddEndnote(text, true) : paragraph->AddFootnote(text, true);
        if (note == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The note could not be added.", kind);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["noteId"] = note->GetId();
        data["kind"] = kind;

        return ResultBuilder("Added a " + kind + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- automation ---------------------------------------------------------

    static void RegisterFillTemplate(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["documentId"] =
            Schema::String("Identifier of the open template to fill; mutually exclusive with input_path.");
        properties["input_path"] =
            Schema::String("Workspace-relative template file; mutually exclusive with documentId.");
        properties["output_path"] =
            Schema::String("Workspace-relative destination for the filled document; defaults to input_path.");
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing an existing output file.", false);
        properties["data"] = Schema::FreeObject(
            "Merge data. A string, number, or boolean member fills the matching MERGEFIELD and same-paragraph "
            "bookmark; an array of objects drives a TableStart/TableEnd repeating region.");

        auto definition = MakeDefinition(
            "fill_template", "Fill mail-merge template",
            "Fill MERGEFIELD placeholders, same-paragraph bookmarks, and TableStart/TableEnd repeating regions of "
            "a template. Address either an open session with documentId or a file with input_path. The merge is "
            "literal: no expression language is evaluated.",
            "automation");
        definition.InputSchema = Schema::Object("Arguments of fill_template.", {"data"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Merge counts.", {"fieldsMerged"},
                           nlohmann::json{{"fieldsMerged", Schema::Integer("Scalar fields merged.")},
                                          {"bookmarksMerged", Schema::Integer("Bookmarks merged.")},
                                          {"regionsMerged", Schema::Integer("Repeating regions expanded.")},
                                          {"regionRowsInserted", Schema::Integer("Row copies inserted.")},
                                          {"outputPath", Schema::String("Workspace-relative output file, for the "
                                                                        "path-based form.")},
                                          {"skippedFields", Schema::Array("Members whose shape was not usable.",
                                                                          Schema::String("Member name."))}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"data", nlohmann::json{{"Customer", "Acme Corp."}}}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return FillTemplate(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome FillTemplate(ToolContext& context, const nlohmann::json& arguments)
    {
        const auto inputPath = arguments.value("input_path", std::string());
        const auto documentId = arguments.value("documentId", std::string());
        if (inputPath.empty() == documentId.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "Pass exactly one of 'documentId' and 'input_path'.", {},
                             "Use 'documentId' for an open session, or 'input_path' for a file in the workspace.");
        }

        const auto data = arguments.find("data");
        if (data == arguments.end() || !data->is_object())
        {
            return MakeError(ErrorCode::InputInvalid, "'data' must be a JSON object.");
        }

        Word::TemplateMergeData merge;
        nlohmann::json skipped = nlohmann::json::array();
        ReadMergeData(*data, merge, skipped);

        if (!inputPath.empty())
        {
            return FillTemplateFile(context, arguments, merge, std::move(skipped));
        }

        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        MutationGuard guard(session.Session());
        const auto result = session.Editor().MergeTemplate(merge, true);

        if (result.FieldsMerged == 0 && result.BookmarksMerged == 0 && result.RegionsMerged == 0)
        {
            return MakeError(ErrorCode::TemplateFieldMissing,
                             "The document contains no MERGEFIELD, bookmark, or repeating region matching the "
                             "supplied data.",
                             {}, "Call get_outline to see the bookmarks, or query_xml for the field codes.");
        }

        guard.Commit();

        auto resultData = MergeResultToJson(result, std::move(skipped));

        return ResultBuilder("Merged " + std::to_string(result.FieldsMerged) + " field(s) and " +
                             std::to_string(result.BookmarksMerged) + " bookmark(s).")
            .WithSession(session.Session())
            .WithData(std::move(resultData))
            .Build();
    }

    /// Runs the merge over a workspace file and writes the filled document.
    static ToolOutcome FillTemplateFile(ToolContext& context, const nlohmann::json& arguments,
                                        const Word::TemplateMergeData& merge, nlohmann::json skipped)
    {
        ToolOutcome failure;
        auto input = ToolSupport::ResolveExistingFile(context, arguments.value("input_path", std::string()), failure);
        if (!input.has_value())
        {
            return failure;
        }

        const auto outputArgument = arguments.value("output_path", std::string());
        std::filesystem::path output = *input;
        if (!outputArgument.empty())
        {
            auto resolved =
                ToolSupport::ResolveOutputPath(context, outputArgument, arguments.value("overwrite", false), failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            output = *resolved;
        }

        auto editor =
            Word::WordDocumentEditor::Open(*input, SettingsWithLimits(context.Adapter().PackageLimits()));
        if (editor == nullptr)
        {
            return MakeError(ErrorCode::PackageLoadFailed, "The template could not be opened as a Word document.",
                             arguments.value("input_path", std::string()));
        }

        const auto result = editor->MergeTemplate(merge, true);
        if (result.FieldsMerged == 0 && result.BookmarksMerged == 0 && result.RegionsMerged == 0)
        {
            return MakeError(ErrorCode::TemplateFieldMissing,
                             "The template contains no MERGEFIELD, bookmark, or repeating region matching the "
                             "supplied data.",
                             {}, "Call get_outline on the template to see its bookmarks and fields.");
        }

        if (!editor->SaveToFile(output))
        {
            return MakeError(ErrorCode::OperationFailed, "The filled document could not be written.",
                             context.GetWorkspace().Relativize(output));
        }

        auto resultData = MergeResultToJson(result, std::move(skipped));
        resultData["outputPath"] = context.GetWorkspace().Relativize(output);

        return ResultBuilder("Merged " + std::to_string(result.FieldsMerged) + " field(s) and " +
                             std::to_string(result.BookmarksMerged) + " bookmark(s) into " +
                             context.GetWorkspace().Relativize(output) + ".")
            .WithData(std::move(resultData))
            .Build();
    }

    /// Renders the merge counts and the skipped members as the tool's data block.
    static nlohmann::json MergeResultToJson(const Word::TemplateMergeResult& result, nlohmann::json skipped)
    {
        nlohmann::json data = nlohmann::json::object();
        data["fieldsMerged"] = static_cast<UInt64>(result.FieldsMerged);
        data["bookmarksMerged"] = static_cast<UInt64>(result.BookmarksMerged);
        data["regionsMerged"] = static_cast<UInt64>(result.RegionsMerged);
        data["regionRowsInserted"] = static_cast<UInt64>(result.RegionRowsInserted);
        data["skippedFields"] = std::move(skipped);
        return data;
    }

    /// Translates the `data` object into the library's merge model.
    static void ReadMergeData(const nlohmann::json& data, Word::TemplateMergeData& merge, nlohmann::json& skipped)
    {
        for (const auto& [key, value] : data.items())
        {
            if (value.is_string())
            {
                merge.Values.emplace(key, value.get<std::string>());
            }
            else if (value.is_boolean())
            {
                merge.Values.emplace(key, value.get<bool>() ? "true" : "false");
            }
            else if (value.is_number())
            {
                merge.Values.emplace(key, value.dump());
            }
            else if (value.is_null())
            {
                merge.Values.emplace(key, std::string());
            }
            else if (value.is_array())
            {
                std::vector<std::unordered_map<std::string, std::string>> rows;
                bool usable = true;
                for (const auto& row : value)
                {
                    if (!row.is_object())
                    {
                        usable = false;
                        break;
                    }

                    std::unordered_map<std::string, std::string> entry;
                    for (const auto& [name, cell] : row.items())
                    {
                        entry.emplace(name, cell.is_string() ? cell.get<std::string>() : cell.dump());
                    }

                    rows.push_back(std::move(entry));
                }

                if (usable)
                {
                    merge.Regions.emplace(key, std::move(rows));
                }
                else
                {
                    skipped.push_back(key);
                }
            }
            else
            {
                skipped.push_back(key);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Document protection
    //
    // This is a restriction a word processor honours, not encryption: every
    // part of the package stays plain, readable OOXML, and a tool that ignores
    // the setting can still rewrite the whole document. The password is stored
    // as a verifier, which lets Word recognise the right password and nothing
    // more.
    // -----------------------------------------------------------------------

    static void RegisterSetProtection(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["protect"] =
            Schema::BooleanWithDefault("True to apply protection, false to remove it.", true);
        properties["editing"] = Schema::EnumerationWithDefault(
            "What a reader may still change: nothing at all, comments only, anything as long as revision "
            "tracking stays on, or only form fields and unlocked regions. \"none\" records the restriction "
            "without limiting editing, which is what leaves formatting restrictions on their own.",
            {"readOnly", "comments", "trackedChanges", "forms", "none"}, "readOnly");
        properties["restrict_formatting"] =
            Schema::BooleanWithDefault("Also restrict direct formatting to styles that are not locked.", false);
        properties["enforce"] = Schema::BooleanWithDefault(
            "Enforce the restriction rather than only recording it; a recorded but unenforced restriction is "
            "what Word shows as available protection the reader can switch on.",
            true);
        properties["password"] =
            Schema::String("Password required to remove the protection again; omit for none.");

        auto definition = MakeDefinition(
            "set_protection", "Set document protection",
            "Restrict how a word processor lets a reader edit the document, or remove that restriction. This is "
            "not encryption: every part stays readable and any tool that ignores the setting can still rewrite "
            "the document, so use it to state intent rather than to keep a secret. Removing protection needs "
            "the password it was applied with.",
            "review");
        definition.InputSchema =
            Schema::Object("Arguments of set_protection.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Protection state.", {"protected"},
                           nlohmann::json{{"protected", Schema::Boolean("True when a restriction is now "
                                                                        "recorded.")},
                                          {"editing", Schema::String("Editing restriction now in force.")},
                                          {"hasPassword", Schema::Boolean("Removing it requires a password.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"editing", "comments"}, {"password", "secret"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetProtection(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::optional<Word::WordProtectionType> ParseProtectionType(const std::string& token)
    {
        if (token == "readOnly")
        {
            return Word::WordProtectionType::ReadOnly;
        }

        if (token == "comments")
        {
            return Word::WordProtectionType::Comments;
        }

        if (token == "trackedChanges")
        {
            return Word::WordProtectionType::TrackedChanges;
        }

        if (token == "forms")
        {
            return Word::WordProtectionType::Forms;
        }

        if (token == "none")
        {
            return Word::WordProtectionType::None;
        }

        return std::nullopt;
    }

    static ToolOutcome SetProtection(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const bool protect = arguments.value("protect", true);
        const auto password = arguments.value("password", std::string());

        Word::WordProtectionOptions options;
        const auto editing = arguments.value("editing", std::string("readOnly"));
        if (const auto parsed = ParseProtectionType(editing); parsed.has_value())
        {
            options.Editing = *parsed;
        }
        else
        {
            return MakeError(ErrorCode::InputInvalid, "'" + editing + "' is not an editing restriction.",
                             "editing");
        }
        options.RestrictFormattingToUnlockedStyles = arguments.value("restrict_formatting", false);
        options.Enforce = arguments.value("enforce", true);

        MutationGuard guard(session.Session());

        const auto result = protect ? session.Editor().ProtectDocument(options, password)
                                    : session.Editor().UnprotectDocument(password);
        if (!result.Succeeded())
        {
            switch (result.Error)
            {
                case Word::WordProtectionError::PasswordMismatch:
                    return MakeError(ErrorCode::InputInvalid, result.Message, "password",
                                     "Removing protection needs the password it was applied with.");
                case Word::WordProtectionError::UnsupportedVerifier:
                    return MakeError(ErrorCode::Unsupported, result.Message, session.Session().Id(),
                                     "The stored verifier uses an algorithm this server cannot compute, so it "
                                     "cannot tell a right password from a wrong one; remove the protection in "
                                     "Word.");
                default:
                    return MakeError(ErrorCode::OperationFailed, result.Message, session.Session().Id());
            }
        }

        guard.Commit();

        const auto state = session.Editor().GetDocumentProtection();

        nlohmann::json data = nlohmann::json::object();
        data["protected"] = state.has_value();
        data["editing"] = state.has_value() ? WordProtectionToken(state->Options.Editing) : "none";
        data["hasPassword"] = state.has_value() && state->HasPassword;

        return ResultBuilder(protect ? "Protected the document." : "Removed the document protection.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // -----------------------------------------------------------------------
    // Style definitions
    //
    // A style is a name content can point at, which is why defining one is
    // worth a tool of its own: a document whose paragraphs carry meaningful
    // style names can be restyled by changing a few definitions, while a
    // document that repeats direct formatting on every paragraph cannot.
    // -----------------------------------------------------------------------

    /// The style formatting members define_style publishes.
    static nlohmann::json StyleFormattingProperties()
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["font"] = Schema::Object(
            "Run formatting the style carries. A boolean passed as false is written as an explicit \"off\", "
            "which is how a style overrides the one it is based on; a member left out is not touched at all.",
            {},
            nlohmann::json{{"name", Schema::String("Typeface.")},
                           {"sizePt", Schema::Number("Font size in points, from 1 to 1638.")},
                           {"bold", Schema::Boolean("Bold text.")},
                           {"italic", Schema::Boolean("Italic text.")},
                           {"underline", Schema::Boolean("Single underline.")},
                           {"strike", Schema::Boolean("Struck-through text.")},
                           {"allCaps", Schema::Boolean("Render lowercase letters as capitals.")},
                           {"smallCaps", Schema::Boolean("Render lowercase letters as small capitals.")},
                           {"color", Schema::String("Text color as \"#RRGGBB\".")}});
        properties["paragraph"] = Schema::Object(
            "Paragraph formatting the style carries; not accepted by a character style, which cannot hold any.",
            {},
            nlohmann::json{
                {"alignment", Schema::Enumeration("Horizontal alignment.", {"left", "center", "right", "both"})},
                {"space_before", Schema::Length("Space above the paragraph.")},
                {"space_after", Schema::Length("Space below the paragraph.")},
                {"line_spacing", Schema::Number("Line spacing as a multiple of single spacing.")},
                {"indent_left", Schema::Length("Left indent.")},
                {"indent_right", Schema::Length("Right indent.")},
                {"indent_first_line", Schema::Length("First-line indent; a negative value hangs the first line.")},
                {"keep_next", Schema::Boolean("Keep the paragraph on the same page as the next one.")},
                {"keep_lines", Schema::Boolean("Keep every line of the paragraph on one page.")},
                {"page_break_before", Schema::Boolean("Start the paragraph on a new page.")},
                {"outline_level", Schema::Integer("Outline level, 0 for a top-level heading and 9 for body text.",
                                                  0, 9)}});
        return properties;
    }

    static void RegisterDefineStyle(ToolRegistry& registry)
    {
        nlohmann::json properties = StyleFormattingProperties();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["style_id"] = Schema::String(
            "Identifier content refers to. Reusing an existing one updates that style rather than adding a "
            "second definition.");
        properties["kind"] = Schema::EnumerationWithDefault(
            "Which family the style belongs to, and therefore what can refer to it: paragraphs, runs, tables, "
            "or numbering definitions. Ignored when the style already exists.",
            {"paragraph", "character", "table", "numbering"}, "paragraph");
        properties["name"] = Schema::String("Name Word shows in its style gallery; defaults to the identifier.");
        properties["based_on"] = Schema::String("Identifier of the style this one inherits from.");
        properties["next"] = Schema::String(
            "Identifier of the style Word applies to the paragraph created by pressing Enter at the end of one "
            "carrying this style. A heading usually names the body style here.");
        properties["linked"] = Schema::String(
            "Identifier of the character style that pairs with this paragraph style, or the other way round.");
        properties["aliases"] = Schema::String("Comma-separated alternative names.");
        properties["ui_priority"] = Schema::Integer("Sort order in Word's style gallery; lower comes first.", 0,
                                                    99);
        properties["quick_style"] =
            Schema::Boolean("Show the style in Word's quick style gallery rather than only in the full list.");
        properties["hidden"] = Schema::Boolean("Hide the style from the gallery until it is used.");
        properties["default"] =
            Schema::Boolean("Make this the default style of its family; any previous default loses the flag.");
        properties["built_in"] = Schema::BooleanWithDefault(
            "Define the built-in style of this name rather than a new one of your own. Word reserves the "
            "built-in names - \"Normal\", \"heading 1\", \"Title\", \"caption\" and the rest - so a custom "
            "style given one of them is renamed on open: \"heading 1\" becomes \"Heading 11\". Set this when "
            "you mean to redefine what the document's Heading 1 looks like, and leave it alone for a style of "
            "your own.",
            false);

        auto definition = MakeDefinition(
            "define_style", "Define style",
            "Create a style definition or change an existing one. Only the members you pass take part, so a "
            "second call adds to a definition rather than replacing it. Formatting the schema does not publish "
            "is preserved: this rewrites the members it is given and leaves the rest of the definition alone.",
            "content");
        definition.InputSchema =
            Schema::Object("Arguments of define_style.", {"documentId", "style_id"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Style definition.", {"styleId", "kind", "created"},
                           nlohmann::json{{"styleId", Schema::String("Identifier of the style.")},
                                          {"kind", Schema::String("Family the style belongs to.")},
                                          {"created", Schema::Boolean("False when an existing style was "
                                                                      "changed.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"style_id", "ReportHeading"},
            {"name", "Report Heading"},
            {"based_on", "Heading1"},
            {"next", "Normal"},
            {"font", nlohmann::json{{"sizePt", 16}, {"bold", true}, {"color", "#1F4E79"}}},
            {"paragraph", nlohmann::json{{"space_before", 12}, {"keep_next", true}}}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DefineStyle(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::optional<Word::StyleType> ParseStyleType(const std::string& token)
    {
        if (token == "paragraph")
        {
            return Word::StyleType::Paragraph;
        }

        if (token == "character")
        {
            return Word::StyleType::Character;
        }

        if (token == "table")
        {
            return Word::StyleType::Table;
        }

        if (token == "numbering")
        {
            return Word::StyleType::Numbering;
        }

        return std::nullopt;
    }

    /**
     * @brief Sets or clears one `w:val`-carrying on/off property of a style.
     *
     * An absent member leaves whatever the definition already had. `false` is
     * not absence: it writes the element with an explicit off value, which is
     * the only way a style can cancel something the style it is based on turns
     * on.
     */
    template <typename TProperty, typename TParent>
    static void ApplyStyleFlag(const std::shared_ptr<TParent>& parent, const nlohmann::json& owner,
                               const char* name)
    {
        const auto member = owner.find(name);
        if (member == owner.end() || !member->is_boolean())
        {
            return;
        }

        auto node = parent->template GetFirstChildOfType<TProperty>();
        if (node == nullptr)
        {
            node = parent->template AppendChild<TProperty>();
        }
        if (node != nullptr)
        {
            node->SetVal(OnOffValue(member->get<bool>()));
        }
    }

    /// Returns @p parent's child of type @p TChild, creating it when absent.
    template <typename TChild, typename TParent>
    static std::shared_ptr<TChild> EnsureChild(const std::shared_ptr<TParent>& parent)
    {
        auto child = parent->template GetFirstChildOfType<TChild>();
        return child != nullptr ? child : parent->template AppendChild<TChild>();
    }

    static bool ApplyStyleFont(const std::shared_ptr<W::Style>& style, const nlohmann::json& font,
                               ToolOutcome& failure)
    {
        auto properties = EnsureChild<W::StyleRunProperties>(style);
        if (properties == nullptr)
        {
            failure = MakeError(ErrorCode::OperationFailed, "The style could not carry run formatting.");
            return false;
        }

        ApplyStyleFlag<W::Bold>(properties, font, "bold");
        ApplyStyleFlag<W::Italic>(properties, font, "italic");
        ApplyStyleFlag<W::Strike>(properties, font, "strike");
        ApplyStyleFlag<W::Caps>(properties, font, "allCaps");
        ApplyStyleFlag<W::SmallCaps>(properties, font, "smallCaps");

        if (const auto underline = font.find("underline");
            underline != font.end() && underline->is_boolean())
        {
            auto node = EnsureChild<W::Underline>(properties);
            if (node != nullptr)
            {
                node->SetVal(EnumValue<W::UnderlineValues>(underline->get<bool>() ? W::UnderlineValues::Single
                                                                                  : W::UnderlineValues::None));
            }
        }

        const auto name = font.value("name", std::string());
        if (!name.empty())
        {
            auto fonts = EnsureChild<W::RunFonts>(properties);
            if (fonts != nullptr)
            {
                fonts->SetAscii(StringValue(name));
                fonts->SetHighAnsi(StringValue(name));
                fonts->SetComplexScript(StringValue(name));
            }
        }

        if (const auto size = font.find("sizePt"); size != font.end() && size->is_number())
        {
            // Word stores this in half-points, so an odd half-point is the
            // smallest step a font size can take.
            const auto halfPoints = static_cast<Int64>(std::lround(size->get<Real>() * 2.0));
            auto node = EnsureChild<W::FontSize>(properties);
            if (node != nullptr)
            {
                node->SetVal(StringValue(std::to_string(halfPoints)));
            }
        }

        const auto color = font.value("color", std::string());
        if (!color.empty())
        {
            const auto parsed = ParseColor(color);
            if (!parsed.has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid, "The font color is not a valid \"#RRGGBB\" value.",
                                    color, "Use a hexadecimal color such as \"#1F4E79\".");
                return false;
            }

            auto node = EnsureChild<W::Color>(properties);
            if (node != nullptr)
            {
                auto hex = parsed->ToHexString();
                if (!hex.empty() && hex.front() == '#')
                {
                    hex.erase(hex.begin());
                }
                node->SetVal(StringValue(hex));
            }
        }

        return true;
    }

    /// Reads a length argument as whole twips, the unit WordprocessingML stores.
    static bool ReadTwips(const nlohmann::json& owner, const char* name, std::optional<Int64>& result,
                          ToolOutcome& failure)
    {
        result.reset();

        const auto member = owner.find(name);
        if (member == owner.end())
        {
            return true;
        }

        const auto length = ParseLength(*member);
        if (!length.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid, "'" + std::string(name) + "' is not a length.",
                                member->dump(), "Pass a number of points, or a string such as \"1.5cm\".");
            return false;
        }

        result = static_cast<Int64>(std::llround(length->ToTw().GetValue()));
        return true;
    }

    static bool ApplyStyleParagraph(const std::shared_ptr<W::Style>& style, const nlohmann::json& paragraph,
                                    ToolOutcome& failure)
    {
        auto properties = EnsureChild<W::StyleParagraphProperties>(style);
        if (properties == nullptr)
        {
            failure = MakeError(ErrorCode::OperationFailed, "The style could not carry paragraph formatting.");
            return false;
        }

        ApplyStyleFlag<W::KeepNext>(properties, paragraph, "keep_next");
        ApplyStyleFlag<W::KeepLines>(properties, paragraph, "keep_lines");
        ApplyStyleFlag<W::PageBreakBefore>(properties, paragraph, "page_break_before");

        const auto alignment = paragraph.value("alignment", std::string());
        if (!alignment.empty())
        {
            const auto parsed = WordAddressing::ParseAlignment(alignment);
            if (!parsed.has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid, "'" + alignment + "' is not an alignment.",
                                    "alignment");
                return false;
            }

            auto node = EnsureChild<W::Justification>(properties);
            if (node != nullptr)
            {
                node->SetVal(EnumValue<W::JustificationValues>(*parsed));
            }
        }

        std::optional<Int64> before;
        std::optional<Int64> after;
        if (!ReadTwips(paragraph, "space_before", before, failure) ||
            !ReadTwips(paragraph, "space_after", after, failure))
        {
            return false;
        }

        const auto lineSpacing = paragraph.find("line_spacing");
        const bool hasLine = lineSpacing != paragraph.end() && lineSpacing->is_number();
        if (before.has_value() || after.has_value() || hasLine)
        {
            auto node = EnsureChild<W::SpacingBetweenLines>(properties);
            if (node != nullptr)
            {
                if (before.has_value())
                {
                    node->SetBefore(StringValue(std::to_string(*before)));
                }
                if (after.has_value())
                {
                    node->SetAfter(StringValue(std::to_string(*after)));
                }
                if (hasLine)
                {
                    // Word expresses a multiple of single spacing as 240ths of
                    // a line, so single is 240 and double is 480.
                    const auto line = static_cast<Int64>(std::lround(lineSpacing->get<Real>() * 240.0));
                    node->SetLine(StringValue(std::to_string(line)));
                    node->SetLineRule(EnumValue<W::LineSpacingRuleValues>(W::LineSpacingRuleValues::Auto));
                }
            }
        }

        std::optional<Int64> left;
        std::optional<Int64> right;
        std::optional<Int64> firstLine;
        if (!ReadTwips(paragraph, "indent_left", left, failure) ||
            !ReadTwips(paragraph, "indent_right", right, failure) ||
            !ReadTwips(paragraph, "indent_first_line", firstLine, failure))
        {
            return false;
        }

        if (left.has_value() || right.has_value() || firstLine.has_value())
        {
            auto node = EnsureChild<W::Indentation>(properties);
            if (node != nullptr)
            {
                if (left.has_value())
                {
                    node->SetLeft(StringValue(std::to_string(*left)));
                }
                if (right.has_value())
                {
                    node->SetRight(StringValue(std::to_string(*right)));
                }
                if (firstLine.has_value())
                {
                    // A negative first-line indent is a hanging indent, which
                    // WordprocessingML spells as its own positive attribute.
                    if (*firstLine < 0)
                    {
                        node->SetHanging(StringValue(std::to_string(-*firstLine)));
                    }
                    else
                    {
                        node->SetFirstLine(StringValue(std::to_string(*firstLine)));
                    }
                }
            }
        }

        if (const auto outline = paragraph.find("outline_level");
            outline != paragraph.end() && outline->is_number_integer())
        {
            auto node = EnsureChild<W::OutlineLevel>(properties);
            if (node != nullptr)
            {
                node->SetVal(Int32Value(outline->get<Int32>()));
            }
        }

        return true;
    }

    static ToolOutcome DefineStyle(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto styleId = arguments.value("style_id", std::string());
        if (styleId.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "A style needs an identifier.", "style_id");
        }

        auto styles = session.Editor().Styles();
        const auto existing = styles.GetStyle(styleId);
        const bool created = !existing.has_value();

        auto definition = existing.value_or(Word::StyleDefinition{});
        definition.StyleId = styleId;
        if (created)
        {
            const auto kind = arguments.value("kind", std::string("paragraph"));
            const auto parsed = ParseStyleType(kind);
            if (!parsed.has_value())
            {
                return MakeError(ErrorCode::InputInvalid, "'" + kind + "' is not a style family.", "kind");
            }

            definition.Type = *parsed;
            definition.Name = styleId;
        }

        if (arguments.contains("built_in"))
        {
            definition.IsCustom = !arguments.value("built_in", false);
        }

        if (const auto name = arguments.value("name", std::string()); !name.empty())
        {
            definition.Name = name;
        }
        if (arguments.contains("based_on"))
        {
            definition.BasedOnStyleId = arguments.value("based_on", std::string());
        }
        if (arguments.contains("next"))
        {
            definition.NextStyleId = arguments.value("next", std::string());
        }
        if (arguments.contains("linked"))
        {
            definition.LinkedStyleId = arguments.value("linked", std::string());
        }
        if (arguments.contains("aliases"))
        {
            definition.Aliases = arguments.value("aliases", std::string());
        }
        if (const auto priority = arguments.find("ui_priority");
            priority != arguments.end() && priority->is_number_integer())
        {
            definition.UiPriority = priority->get<int>();
        }
        if (arguments.contains("quick_style"))
        {
            definition.IsPrimary = arguments.value("quick_style", false);
        }
        if (arguments.contains("hidden"))
        {
            definition.IsSemiHidden = arguments.value("hidden", false);
            definition.IsUnhideWhenUsed = definition.IsSemiHidden;
        }

        const auto paragraph = arguments.find("paragraph");
        const bool wantsParagraph = paragraph != arguments.end() && paragraph->is_object();
        if (wantsParagraph && definition.Type == Word::StyleType::Character)
        {
            return MakeError(ErrorCode::InputInvalid,
                             "A character style cannot carry paragraph formatting.", "paragraph",
                             "A character style formats runs inside a paragraph; define a paragraph style for "
                             "the paragraph itself and link the two.");
        }

        MutationGuard guard(session.Session());

        if (!styles.UpsertStyle(definition))
        {
            return MakeError(ErrorCode::OperationFailed, "The style definition could not be written.", styleId);
        }

        if (arguments.value("default", false) && !styles.SetDefaultStyle(definition.Type, styleId))
        {
            return MakeError(ErrorCode::OperationFailed, "The style could not be made the default.", styleId);
        }

        const auto style = styles.GetLowLevelStyle(styleId);
        if (style == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The style could not be read back.", styleId);
        }

        ToolOutcome failure;
        if (const auto font = arguments.find("font");
            font != arguments.end() && font->is_object() && !ApplyStyleFont(style, *font, failure))
        {
            return failure;
        }

        if (wantsParagraph && !ApplyStyleParagraph(style, *paragraph, failure))
        {
            return failure;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["styleId"] = styleId;
        data["kind"] = StyleTypeToken(definition.Type);
        data["created"] = created;

        return ResultBuilder((created ? "Defined the style '" : "Changed the style '") + styleId + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDeleteStyle(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["style_id"] = Schema::String("Identifier of the style definition to remove.");

        auto definition = MakeDefinition(
            "delete_style", "Delete style",
            "Remove a style definition. Content that still names the style keeps the reference and falls back "
            "to the document defaults, so the answer reports how many blocks are in that position; move them "
            "to another style with apply_style first if that is not what you want.",
            "content");
        definition.InputSchema =
            Schema::Object("Arguments of delete_style.", {"documentId", "style_id"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Removal.", {"styleId", "removed"},
                           nlohmann::json{{"styleId", Schema::String("Identifier that was removed.")},
                                          {"removed", Schema::Boolean("A definition was there and is gone.")},
                                          {"danglingBlocks",
                                           Schema::Array("1-based indices of body blocks that still name the "
                                                         "removed style.",
                                                         Schema::Integer("Block index."))}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"style_id", "ReportHeading"}};
        definition.Annotations.Destructive = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteStyle(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteStyle(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto styleId = arguments.value("style_id", std::string());
        auto styles = session.Editor().Styles();
        if (!styles.HasStyle(styleId))
        {
            return MakeError(ErrorCode::StyleNotFound, "No style '" + styleId + "' is defined.", styleId,
                             "list_styles reports the identifiers this document defines.");
        }

        MutationGuard guard(session.Session());

        if (!styles.RemoveStyle(styleId))
        {
            return MakeError(ErrorCode::OperationFailed, "The style definition could not be removed.", styleId);
        }

        guard.Commit();

        // Reported rather than repaired: which style the orphaned blocks should
        // carry instead is a decision this tool has no way to make.
        nlohmann::json dangling = nlohmann::json::array();
        const auto blocks = session.Editor().BodyBlocks();
        for (Size index = 1; index <= blocks.size(); ++index)
        {
            const auto paragraph = blocks[index - 1].AsParagraph();
            if (paragraph != nullptr && paragraph->GetStyleId() == styleId)
            {
                dangling.push_back(static_cast<UInt64>(index));
            }
        }

        nlohmann::json data = nlohmann::json::object();
        data["styleId"] = styleId;
        data["removed"] = true;
        data["danglingBlocks"] = std::move(dangling);

        return ResultBuilder("Removed the style '" + styleId + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // -----------------------------------------------------------------------
    // Numbering definitions
    //
    // WordprocessingML separates the shape of a list from its sequence: an
    // abstract definition says what each level looks like, and an instance is
    // what paragraphs point at. Two paragraphs numbered in the same sequence
    // share an instance; a list that starts over gets a new one over the same
    // definition. Both halves are reported and both can be created here.
    // -----------------------------------------------------------------------

    static void RegisterListNumbering(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json level = Schema::Object(
            "One level of a definition.", {"level"},
            nlohmann::json{{"level", Schema::Integer("Zero-based level.")},
                           {"format", Schema::String("Numbering format of the level.")},
                           {"text", Schema::String("Pattern of the marker, where %1 is this level's number.")},
                           {"start", Schema::Integer("First value of the level.")}});

        nlohmann::json definitionSchema = Schema::Object(
            "One abstract definition.", {"definitionId"},
            nlohmann::json{{"definitionId", Schema::Integer("Identifier instances refer to.")},
                           {"name", Schema::String("Name of the definition.")},
                           {"levels", Schema::Array("Levels the definition declares.", std::move(level))}});

        nlohmann::json override = Schema::Object(
            "One restart override.", {"level", "start"},
            nlohmann::json{{"level", Schema::Integer("Zero-based level that restarts.")},
                           {"start", Schema::Integer("Value the level restarts at.")}});

        nlohmann::json instance = Schema::Object(
            "One numbering instance.", {"numberingId", "definitionId"},
            nlohmann::json{{"numberingId", Schema::Integer("Identifier a paragraph names.")},
                           {"definitionId", Schema::Integer("Definition the instance uses.")},
                           {"overrides", Schema::Array("Levels this instance restarts.", std::move(override))}});

        auto definition = MakeDefinition(
            "list_numbering", "List numbering",
            "Report the list definitions a document carries and the instances paragraphs point at. Use it to "
            "find the numberingId that continues an existing list, or the name of a definition to reuse.",
            "content");
        definition.InputSchema = Schema::Object("Arguments of list_numbering.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Numbering.", {"definitions", "instances"},
                           nlohmann::json{{"definitions", Schema::Array("Abstract definitions.",
                                                                        std::move(definitionSchema))},
                                          {"instances", Schema::Array("Instances over them.",
                                                                      std::move(instance))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListNumbering(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListNumbering(ToolContext& context, const nlohmann::json& arguments)
    {
        WordReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        auto numbering = reader.Editor().Numbering();

        nlohmann::json instances = nlohmann::json::array();
        std::set<int> definitionIds;
        for (const auto& instance : numbering.Instances())
        {
            definitionIds.insert(instance.AbstractNumberingId);

            nlohmann::json overrides = nlohmann::json::array();
            for (const auto& value : instance.Overrides)
            {
                overrides.push_back(nlohmann::json{{"level", value.Level}, {"start", value.Start}});
            }

            instances.push_back(nlohmann::json{{"numberingId", instance.NumberingId},
                                               {"definitionId", instance.AbstractNumberingId},
                                               {"overrides", std::move(overrides)}});
        }

        // Only the definitions something points at are reported: an abstract
        // definition nothing instantiates cannot be reached from a paragraph,
        // and listing it would offer an identifier that leads nowhere.
        nlohmann::json definitions = nlohmann::json::array();
        for (const auto id : definitionIds)
        {
            const auto found = numbering.GetDefinition(id);
            if (!found.has_value())
            {
                continue;
            }

            nlohmann::json levels = nlohmann::json::array();
            for (const auto& level : found->Levels)
            {
                levels.push_back(nlohmann::json{{"level", level.Level},
                                                {"format", WordAddressing::NumberFormatToken(level.Format)},
                                                {"text", level.LevelText},
                                                {"start", level.Start}});
            }

            definitions.push_back(
                nlohmann::json{{"definitionId", id}, {"name", found->Name}, {"levels", std::move(levels)}});
        }

        nlohmann::json data = nlohmann::json::object();
        data["definitions"] = std::move(definitions);
        data["instances"] = std::move(instances);

        return ResultBuilder("The document carries " + std::to_string(data["definitions"].size()) +
                             " list definition(s) and " + std::to_string(data["instances"].size()) +
                             " instance(s).")
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDefineList(ToolRegistry& registry)
    {
        nlohmann::json level = Schema::Object(
            "One level of the definition.", {"level"},
            nlohmann::json{
                {"level", Schema::Integer("Zero-based level; Word allows 0 through 8.", 0, 8)},
                {"format", Schema::EnumerationWithDefault(
                               "How the number of this level is rendered. These are the common members of "
                               "ST_NumberFormat; the full list is larger.",
                               {"decimal", "decimalZero", "upperRoman", "lowerRoman", "upperLetter",
                                "lowerLetter", "ordinal", "ordinalText", "cardinalText", "decimalEnclosedCircle",
                                "bullet", "none"},
                               "decimal")},
                {"text", Schema::String(
                             "Pattern of the marker, where %1 is the number of level 0 and %2 of level 1, so "
                             "\"%1.%2.\" renders as \"2.3.\". A bullet level puts the bullet character here.")},
                {"start", Schema::IntegerWithDefault("First value of the level.", 1, 0)},
                {"suffix", Schema::EnumerationWithDefault("What separates the marker from the text.",
                                                          {"tab", "space", "nothing"}, "tab")},
                {"alignment", Schema::EnumerationWithDefault("Alignment of the marker itself.",
                                                             {"left", "center", "right"}, "left")},
                {"style_id", Schema::String("Paragraph style this level applies.")},
                {"indent_left", Schema::Length("Left indent of paragraphs at this level.")},
                {"indent_hanging", Schema::Length("How far the marker hangs left of the text.")},
                {"restart_after", Schema::Integer("Restart this level whenever that lower level advances.", 0, 8)},
                {"legal", Schema::Boolean("Render every inherited level as a decimal number.")}});

        nlohmann::json restart = Schema::Object(
            "One level to restart.", {"level", "start"},
            nlohmann::json{{"level", Schema::Integer("Zero-based level to restart.", 0, 8)},
                           {"start", Schema::Integer("Value it restarts at.", 0)}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["name"] = Schema::String(
            "Name of the definition. An existing definition with this name is reused rather than duplicated, "
            "which is what lets several lists share one shape.");
        properties["levels"] = Schema::Array(
            "Levels to write, in any order. Levels you leave out keep Word's defaults for them. Ignored when "
            "the named definition already exists.",
            std::move(level));
        properties["restart"] = Schema::Array(
            "Restart the returned instance at these values instead of continuing the sequence. Without it the "
            "returned instance continues wherever the definition left off.",
            std::move(restart));

        auto definition = MakeDefinition(
            "define_list", "Define list",
            "Create a multi-level list definition, or reuse one by name, and return the numbering instance "
            "paragraphs point at. Hand the returned numberingId to insert_list to lay the list out.",
            "content");
        definition.InputSchema =
            Schema::Object("Arguments of define_list.", {"documentId", "name"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("List definition.", {"numberingId", "definitionId"},
                           nlohmann::json{{"numberingId", Schema::Integer("Instance a paragraph names.")},
                                          {"definitionId", Schema::Integer("Abstract definition behind it.")},
                                          {"created", Schema::Boolean("False when an existing definition of "
                                                                      "that name was reused.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"name", "Outline"},
            {"levels", nlohmann::json::array(
                           {nlohmann::json{{"level", 0}, {"format", "upperRoman"}, {"text", "%1."}},
                            nlohmann::json{{"level", 1}, {"format", "lowerLetter"}, {"text", "%2)"}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DefineList(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DefineList(ToolContext& context, const nlohmann::json& arguments)
    {
        WordSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto name = arguments.value("name", std::string());
        if (name.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "A list definition needs a name.", "name");
        }

        auto numbering = session.Editor().Numbering();

        // Whether the name is new decides what the answer reports, and it has
        // to be read before anything is written.
        bool existed = false;
        for (const auto& instance : numbering.Instances())
        {
            const auto found = numbering.GetDefinition(instance.AbstractNumberingId);
            if (found.has_value() && found->Name == name)
            {
                existed = true;
                break;
            }
        }

        Word::NumberingDefinition definition;
        definition.Name = name;

        const auto levels = arguments.find("levels");
        if (levels != arguments.end() && levels->is_array())
        {
            std::set<int> seen;
            for (const auto& entry : *levels)
            {
                Word::NumberingLevelDefinition level;
                level.Level = entry.value("level", 0);
                if (!seen.insert(level.Level).second)
                {
                    return MakeError(ErrorCode::InputInvalid,
                                     "Level " + std::to_string(level.Level) + " is declared twice.", "levels");
                }

                const auto format = entry.value("format", std::string("decimal"));
                const auto parsedFormat = WordAddressing::ParseNumberFormat(format);
                if (!parsedFormat.has_value())
                {
                    return MakeError(ErrorCode::InputInvalid, "'" + format + "' is not a numbering format.",
                                     "format");
                }
                level.Format = *parsedFormat;

                level.LevelText = entry.value("text", std::string());
                if (level.LevelText.empty())
                {
                    // A level with no pattern renders no marker at all, which
                    // reads as a broken list rather than as a choice.
                    level.LevelText = level.Format == W::NumberFormatValues::Bullet
                                          ? std::string("\xEF\x82\xB7")
                                          : "%" + std::to_string(level.Level + 1) + ".";
                }

                level.Start = entry.value("start", 1);
                level.LegalNumbering = entry.value("legal", false);
                level.ParagraphStyleId = entry.value("style_id", std::string());

                const auto suffix = entry.value("suffix", std::string("tab"));
                level.Suffix = suffix == "space"     ? W::LevelSuffixValues::Space
                               : suffix == "nothing" ? W::LevelSuffixValues::Nothing
                                                     : W::LevelSuffixValues::Tab;

                const auto alignment = entry.value("alignment", std::string("left"));
                level.Justification = alignment == "center"  ? W::LevelJustificationValues::center
                                      : alignment == "right" ? W::LevelJustificationValues::right
                                                             : W::LevelJustificationValues::left;

                ToolOutcome failure;
                std::optional<Int64> indentLeft;
                std::optional<Int64> indentHanging;
                if (!ReadTwips(entry, "indent_left", indentLeft, failure) ||
                    !ReadTwips(entry, "indent_hanging", indentHanging, failure))
                {
                    return failure;
                }

                if (indentLeft.has_value())
                {
                    level.LeftIndent = MeasuringUnits(static_cast<Real>(*indentLeft), MeasurementUnit::Twip);
                }
                if (indentHanging.has_value())
                {
                    level.HangingIndent = MeasuringUnits(static_cast<Real>(*indentHanging), MeasurementUnit::Twip);
                }

                if (const auto restartAfter = entry.find("restart_after");
                    restartAfter != entry.end() && restartAfter->is_number_integer())
                {
                    level.RestartAfterLevel = restartAfter->get<int>();
                }

                definition.Levels.push_back(level);
            }
        }

        if (!existed && definition.Levels.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "A new list definition needs at least one level.", "levels",
                             "Pass 'levels', or name a definition list_numbering already reports.");
        }

        MutationGuard guard(session.Session());

        auto listStyle = numbering.EnsureMultilevelList(definition);
        if (listStyle.NumberingId == 0)
        {
            return MakeError(ErrorCode::OperationFailed, "The list definition could not be written.", name);
        }

        const auto restart = arguments.find("restart");
        if (restart != arguments.end() && restart->is_array() && !restart->empty())
        {
            std::vector<Word::NumberingLevelOverride> overrides;
            for (const auto& entry : *restart)
            {
                overrides.push_back(
                    Word::NumberingLevelOverride{entry.value("level", 0), entry.value("start", 1)});
            }

            listStyle = numbering.RestartList(listStyle.NumberingId, overrides);
            if (listStyle.NumberingId == 0)
            {
                return MakeError(ErrorCode::OperationFailed, "The restarted instance could not be created.", name);
            }
        }

        const auto instance = numbering.GetInstance(listStyle.NumberingId);

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["numberingId"] = listStyle.NumberingId;
        data["definitionId"] = instance.has_value() ? instance->AbstractNumberingId : 0;
        data["created"] = !existed;

        return ResultBuilder((existed ? "Reused the list definition '" : "Defined the list '") + name +
                             "' as numbering instance " + std::to_string(listStyle.NumberingId) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterCompareDocuments(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        properties["original_path"] = Schema::String("Workspace-relative baseline document.");
        properties["revised_path"] = Schema::String("Workspace-relative revised document.");
        properties["output_path"] = Schema::String("Workspace-relative destination for the annotated result.");
        properties["overwrite"] = Schema::BooleanWithDefault("Permit replacing an existing output file.", false);
        properties["author"] = Schema::StringWithDefault("Author recorded on the generated revisions.", "exyoki");

        auto definition = MakeDefinition(
            "compare_documents", "Compare documents",
            "Compare two documents and write the baseline annotated with tracked insertions and deletions. The "
            "comparison is paragraph-level plain text, not Word's full diff engine.",
            "automation");
        definition.InputSchema = Schema::Object("Arguments of compare_documents.",
                                                {"original_path", "revised_path", "output_path"},
                                                std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Comparison result.", {"identical", "outputPath"},
                           nlohmann::json{{"identical", Schema::Boolean("True when no revision was produced.")},
                                          {"revisionsCreated", Schema::Integer("Revisions written.")},
                                          {"outputPath", Schema::String("Workspace-relative output file.")}}),
            false);
        definition.Example = nlohmann::json{
            {"original_path", "v1.docx"}, {"revised_path", "v2.docx"}, {"output_path", "compared.docx"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return CompareDocuments(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome CompareDocuments(ToolContext& context, const nlohmann::json& arguments)
    {
        ToolOutcome failure;
        auto original =
            ToolSupport::ResolveExistingFile(context, arguments.value("original_path", std::string()), failure);
        if (!original.has_value())
        {
            return failure;
        }

        auto revised =
            ToolSupport::ResolveExistingFile(context, arguments.value("revised_path", std::string()), failure);
        if (!revised.has_value())
        {
            return failure;
        }

        auto output = ToolSupport::ResolveOutputPath(context, arguments.value("output_path", std::string()),
                                                     arguments.value("overwrite", false), failure);
        if (!output.has_value())
        {
            return failure;
        }

        const auto result = Tools::CompareWordDocuments(*original, *revised, *output,
                                                        arguments.value("author", std::string("exyoki")));
        if (!result.Ok)
        {
            return MakeErrorFromDiagnostics(ErrorCode::OperationFailed, "The documents could not be compared.",
                                            result.Diagnostics);
        }

        nlohmann::json data = nlohmann::json::object();
        data["identical"] = result.Identical;
        data["revisionsCreated"] = static_cast<UInt64>(result.RevisionsCreated);
        data["outputPath"] = context.GetWorkspace().Relativize(*output);

        return ResultBuilder(result.Identical ? "The two documents are identical."
                                              : "Wrote " + std::to_string(result.RevisionsCreated) +
                                                    " tracked revision(s).")
            .WithData(std::move(data))
            .WithDiagnostics(result.Diagnostics)
            .Build();
    }
};

void RegisterWordToolset(ToolRegistry& registry)
{
    WordTools::Register(registry);
}

} // namespace ExyokiOffice::Mcp
