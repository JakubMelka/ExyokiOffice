// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

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

        auto definition = MakeDefinition("insert_list", "Insert list",
                                         "Insert a bulleted or numbered list as consecutive paragraphs. Reuse the "
                                         "returned numberingId to continue the same list later.",
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

        const auto kind = arguments.value("kind", std::string("bullet"));
        auto listStyle = kind == "numbered" ? session.Editor().EnsureNumberedListStyle()
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

        return ResultBuilder("Inserted a " + kind + " list as blocks " + std::to_string(firstBlock) + " to " +
                             std::to_string(lastBlock) + ".")
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
