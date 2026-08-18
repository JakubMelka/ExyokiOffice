// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "PowerPointToolset.hpp"

#include "PptAddressing.hpp"
#include "SharedToolset.hpp"
#include "Units.hpp"

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Guid.hpp"

#include <algorithm>
#include <utility>

namespace ExyokiOffice::Mcp
{

namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

/// Open settings that carry the configured safety limits and nothing else.
static Packaging::OpenSettings SettingsWithLimits(const OpenXmlPackageLimits& limits)
{
    Packaging::OpenSettings settings;
    settings.PackageLimits = limits;
    return settings;
}

PowerPointDocumentHandle::PowerPointDocumentHandle(PowerPoint::PowerPointDocumentEditor::Ptr editor,
                                                   OpenXmlPackageLimits limits)
    : m_editor(std::move(editor)), m_packageLimits(limits)
{
}

Tools::DocumentFamily PowerPointDocumentHandle::Family() const
{
    return Tools::DocumentFamily::PowerPoint;
}

bool PowerPointDocumentHandle::SaveToFile(const std::filesystem::path& path)
{
    return m_editor && m_editor->SaveToFile(path);
}

std::vector<Byte> PowerPointDocumentHandle::SaveToMemory()
{
    return m_editor ? m_editor->SaveToMemory() : std::vector<Byte>();
}

bool PowerPointDocumentHandle::LoadFromMemory(std::span<const Byte> bytes)
{
    // Snapshot bytes come from this process, but they are a package all the
    // same: a document that was within the limits when it was opened stays
    // within them when it is restored, and a bug that made it grow past them
    // should surface here rather than be waved through.
    auto replacement = PowerPoint::PowerPointDocumentEditor::Open(bytes, SettingsWithLimits(m_packageLimits));
    if (replacement == nullptr)
    {
        return false;
    }

    m_editor = std::move(replacement);
    return true;
}

std::shared_ptr<OpenXmlPackage> PowerPointDocumentHandle::Package() const
{
    return m_editor ? m_editor->GetDocument() : nullptr;
}

nlohmann::json PowerPointDocumentHandle::Summary() const
{
    nlohmann::json summary = nlohmann::json::object();
    if (m_editor == nullptr)
    {
        return summary;
    }

    UInt64 hidden = 0;
    for (const auto& slide : m_editor->Slides())
    {
        if (slide != nullptr && slide->IsHidden())
        {
            ++hidden;
        }
    }

    summary["slideCount"] = static_cast<UInt64>(m_editor->SlideCount());
    summary["hiddenSlideCount"] = hidden;
    summary["layoutCount"] = static_cast<UInt64>(m_editor->SlideLayouts().size());
    summary["masterCount"] = static_cast<UInt64>(m_editor->SlideMasters().size());
    return summary;
}

Tools::DocumentFamily PowerPointFamilyAdapter::Family() const
{
    return Tools::DocumentFamily::PowerPoint;
}

std::string PowerPointFamilyAdapter::FamilyName() const
{
    return "PowerPoint";
}

std::string PowerPointFamilyAdapter::FileExtension() const
{
    return ".pptx";
}

std::unique_ptr<DocumentHandle> PowerPointFamilyAdapter::CreateNew() const
{
    auto editor = PowerPoint::PowerPointDocumentEditor::CreateNew();
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<PowerPointDocumentHandle>(std::move(editor), PackageLimits());
}

std::unique_ptr<DocumentHandle> PowerPointFamilyAdapter::Open(const std::filesystem::path& path) const
{
    auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, SettingsWithLimits(PackageLimits()));
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<PowerPointDocumentHandle>(std::move(editor), PackageLimits());
}

std::unique_ptr<DocumentHandle> PowerPointFamilyAdapter::OpenFromMemory(std::span<const Byte> bytes) const
{
    auto editor = PowerPoint::PowerPointDocumentEditor::Open(bytes, SettingsWithLimits(PackageLimits()));
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<PowerPointDocumentHandle>(std::move(editor), PackageLimits());
}

/// The editor behind a handle this adapter produced.
static PowerPoint::PowerPointDocumentEditor& EditorOf(DocumentHandle& document)
{
    // Every handle reaching a PowerPointFamilyAdapter came out of its own CreateNew, Open or
    // OpenFromMemory, so the family is a fact here rather than a guess.
    return static_cast<PowerPointDocumentHandle&>(document).Editor();
}

Tools::DocumentModel PowerPointFamilyAdapter::ReadModel(DocumentHandle& document, const Tools::ModelReadOptions& options,
                                                        std::vector<Tools::ToolDiagnostic>& diagnostics) const
{
    return Tools::ReadPowerPointModel(EditorOf(document), options, diagnostics);
}

Tools::DocumentStats PowerPointFamilyAdapter::Stat(DocumentHandle& document) const
{
    return Tools::Stat(EditorOf(document));
}

Tools::ExtractedDocumentText PowerPointFamilyAdapter::ExtractText(DocumentHandle& document) const
{
    return Tools::Extract(EditorOf(document));
}

Tools::DocumentSearchResult PowerPointFamilyAdapter::SearchText(DocumentHandle& document, std::string_view needle, Size contextChars,
                                                                bool useRegex, bool ignoreCase) const
{
    return Tools::SearchDocumentText(EditorOf(document), needle, contextChars, useRegex, ignoreCase);
}

Tools::DocumentReplaceResult PowerPointFamilyAdapter::ReplaceText(DocumentHandle& document, std::string_view needle,
                                                                  std::string_view replacement, bool dryRun, bool useRegex,
                                                                  bool ignoreCase) const
{
    return Tools::ReplaceDocumentText(EditorOf(document), needle, replacement, dryRun, useRegex, ignoreCase);
}

Tools::RedactResult PowerPointFamilyAdapter::Redact(DocumentHandle& document, const Tools::RedactOptions& options) const
{
    return Tools::RedactDocument(EditorOf(document), options);
}

/// Implementation of the tools in §11 of the MCP server plan.
class PowerPointTools
{
public:
    static void Register(ToolRegistry& registry)
    {
        RegisterListSlides(registry);
        RegisterGetSlide(registry);
        RegisterListLayouts(registry);
        RegisterAddSlide(registry);
        RegisterDeleteSlide(registry);
        RegisterMoveSlide(registry);
        RegisterDuplicateSlide(registry);
        RegisterCopySlideFrom(registry);
        RegisterSetSlideHidden(registry);
        RegisterSetPlaceholderText(registry);
        RegisterAddTextBox(registry);
        RegisterEditTextFrame(registry);
        RegisterDeleteShape(registry);
        RegisterSetShapeTransform(registry);
        RegisterAddImage(registry);
        RegisterAddTable(registry);
        RegisterEditTableCell(registry);
        RegisterAddChart(registry);
        RegisterSetNotes(registry);
        RegisterListComments(registry);
        RegisterAddComment(registry);
        RegisterSetTransition(registry);
        RegisterAddSection(registry);
        RegisterSetSlideSize(registry);
    }

private:
    /// Resolves the session and its PowerPoint editor in one step.
    class PptSession
    {
    public:
        PptSession(ToolContext& context, const nlohmann::json& arguments)
        {
            m_session = ToolSupport::RequireSession(context, arguments, m_failure);
            if (m_session == nullptr)
            {
                return;
            }

            auto* handle = dynamic_cast<PowerPointDocumentHandle*>(&m_session->Document());
            if (handle == nullptr)
            {
                m_failure =
                    MakeError(ErrorCode::FamilyMismatch, "The document is not a presentation.", m_session->Id());
                return;
            }

            m_editor = &handle->Editor();
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_editor != nullptr; }
        [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_failure; }
        [[nodiscard]] DocumentSession& Session() const noexcept { return *m_session; }
        [[nodiscard]] PowerPoint::PowerPointDocumentEditor& Editor() const noexcept { return *m_editor; }

    private:
        DocumentSession* m_session = nullptr;
        PowerPoint::PowerPointDocumentEditor* m_editor = nullptr;
        ToolOutcome m_failure;
    };

    /// Reading tools take a document source; this resolves it to a PowerPoint editor.
    class PptReader
    {
    public:
        PptReader(ToolContext& context, const nlohmann::json& arguments)
            : m_access(context, arguments)
        {
            if (!m_access.IsValid())
            {
                return;
            }

            auto* handle = dynamic_cast<PowerPointDocumentHandle*>(&m_access.Document());
            if (handle != nullptr)
            {
                m_editor = &handle->Editor();
            }
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_editor != nullptr; }
        [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_access.Failure(); }
        [[nodiscard]] PowerPoint::PowerPointDocumentEditor& Editor() const noexcept { return *m_editor; }

    private:
        DocumentAccess m_access;
        PowerPoint::PowerPointDocumentEditor* m_editor = nullptr;
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

    /// True when two element wrappers view the same XML node.
    static bool SameElement(const std::shared_ptr<OpenXMLElement>& left,
                            const std::shared_ptr<OpenXMLElement>& right)
    {
        return left != nullptr && right != nullptr && left->IsSameNode(right);
    }

    static nlohmann::json SlideProperty()
    {
        return Schema::Integer("1-based slide index.", 1);
    }

    /// Title text of a slide, taken from its title placeholder when present.
    static std::string SlideTitle(const PowerPoint::PresentationSlide& slide)
    {
        for (const auto& placeholder : slide.Placeholders(false))
        {
            if (placeholder == nullptr)
            {
                continue;
            }

            const auto type = placeholder->Type();
            if (type != P::PlaceholderValues::Title && type != P::PlaceholderValues::CenteredTitle)
            {
                continue;
            }

            auto shape = placeholder->GetShape();
            if (shape == nullptr)
            {
                continue;
            }

            for (const auto& candidate : ShapesOf(slide))
            {
                if (SameElement(candidate.first->GetElement(), placeholder->GetElement()))
                {
                    const auto frame = candidate.first->GetTextFrame();
                    if (frame.has_value())
                    {
                        return PptAddressing::TextFrameToText(*frame);
                    }
                }
            }
        }

        return {};
    }

    /// Flattens the shape tree into (shape, path) pairs in document order.
    static std::vector<std::pair<PowerPoint::PresentationShape::Ptr, std::string>> ShapesOf(
        const PowerPoint::PresentationSlide& slide)
    {
        std::vector<std::pair<PowerPoint::PresentationShape::Ptr, std::string>> shapes;
        auto tree = slide.ShapeTree();
        if (tree == nullptr)
        {
            return shapes;
        }

        CollectShapes(tree->Shapes(), {}, shapes);
        return shapes;
    }

    static void CollectShapes(const std::vector<PowerPoint::PresentationShape::Ptr>& level, const std::string& prefix,
                              std::vector<std::pair<PowerPoint::PresentationShape::Ptr, std::string>>& shapes)
    {
        for (Size index = 0; index < level.size(); ++index)
        {
            const auto& shape = level[index];
            if (shape == nullptr)
            {
                continue;
            }

            const auto path = prefix.empty() ? std::to_string(index + 1)
                                             : prefix + "/" + std::to_string(index + 1);
            shapes.emplace_back(shape, path);
            if (shape->IsGroup())
            {
                CollectShapes(shape->Children(), path, shapes);
            }
        }
    }

    /**
     * @brief Validates a 1-based insertion position against the deck.
     *
     * A position may address any existing slide or the one place past the end,
     * which appends. Anything beyond that is rejected rather than clamped: an
     * index of 99 on a two-slide deck is an agent mistake, and silently
     * appending hides it until someone reads the deck back.
     */
    static bool CheckInsertPosition(const nlohmann::json& arguments, const std::string& name, Size slideCount,
                                    ToolOutcome& failure)
    {
        const Size requested = arguments.value(name, static_cast<Size>(0));
        if (requested == 0 || requested <= slideCount + 1)
        {
            return true;
        }

        failure = MakeError(ErrorCode::SlideNotFound,
                            "The presentation has " + std::to_string(slideCount) + " slide(s), so '" + name +
                                "' must be between 1 and " + std::to_string(slideCount + 1) + ".",
                            std::to_string(requested),
                            "Omit '" + name + "' to append at the end, or call list_slides to see the positions.");
        return false;
    }

    /// Describes an insertion position, including what happens when it is omitted.
    static nlohmann::json InsertPositionProperty(const std::string& subject)
    {
        return Schema::Integer("1-based position of " + subject +
                                   "; omit to append at the end. A position past the end of the presentation is "
                                   "rejected, never clamped.",
                               1);
    }

    /// Placeholder type of a shape, or an empty string when it is not one.
    static std::string PlaceholderTypeOf(const PowerPoint::PresentationSlide& slide,
                                         const PowerPoint::PresentationShape& shape)
    {
        for (const auto& placeholder : slide.Placeholders(false))
        {
            if (placeholder != nullptr && SameElement(placeholder->GetElement(), shape.GetElement()))
            {
                return PptAddressing::PlaceholderToken(placeholder->Type());
            }
        }

        return {};
    }

    // --- slides -------------------------------------------------------------

    static void RegisterListSlides(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json slide =
            Schema::Object("One slide.", {"index"},
                           nlohmann::json{{"index", Schema::Integer("1-based slide index.")},
                                          {"layout", Schema::String("Name of the slide's layout.")},
                                          {"title", Schema::String("Text of the title placeholder.")},
                                          {"hidden", Schema::Boolean("True when the slide is skipped in a show.")},
                                          {"shapeCount", Schema::Integer("Number of shapes on the slide.")},
                                          {"hasNotes", Schema::Boolean("True when the slide has speaker notes.")}});

        auto definition = MakeDefinition("list_slides", "List slides",
                                         "List the slides of the presentation with their layouts and titles. Call "
                                         "it first to learn the slide indices.",
                                         "slides");
        definition.InputSchema = Schema::Object("Arguments of list_slides.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Slides.", {"slides"},
                           nlohmann::json{{"slides", Schema::Array("Slides in presentation order.", std::move(slide))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListSlides(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListSlides(ToolContext& context, const nlohmann::json& arguments)
    {
        PptReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        nlohmann::json slides = nlohmann::json::array();
        const auto items = reader.Editor().Slides();
        for (Size index = 0; index < items.size(); ++index)
        {
            const auto& slide = items[index];
            if (slide == nullptr)
            {
                continue;
            }

            auto layout = slide->Layout();

            nlohmann::json entry = nlohmann::json::object();
            entry["index"] = static_cast<UInt64>(index + 1);
            entry["layout"] = layout != nullptr ? layout->Name() : std::string();
            entry["title"] = SlideTitle(*slide);
            entry["hidden"] = slide->IsHidden();
            entry["shapeCount"] = static_cast<UInt64>(ShapesOf(*slide).size());
            entry["hasNotes"] = !slide->NotesText().empty();
            slides.push_back(std::move(entry));
        }

        const bool truncated = TruncateArrayToBudget(slides);

        nlohmann::json data = nlohmann::json::object();
        const auto count = slides.size();
        data["slides"] = std::move(slides);

        return ResultBuilder("The presentation has " + std::to_string(count) + " slide(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterGetSlide(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["slide"] = SlideProperty();
        properties["include_notes"] = Schema::BooleanWithDefault("Include the speaker notes.", true);
        properties["format"] = Schema::EnumerationWithDefault(
            "How the slide is rendered: 'shapes' lists every shape with the path the editing tools accept, "
            "'model' returns the semantic exyokioffice-document model of the slide, 'markdown' renders it as "
            "Markdown, and 'text' extracts its plain text.",
            {"shapes", "model", "markdown", "text"}, "shapes");

        nlohmann::json shape =
            Schema::Object("One shape.", {"path"},
                           nlohmann::json{{"path", Schema::String("Shape path, for example \"2\" or \"2/1\".")},
                                          {"id", Schema::Integer("Shape identifier.")},
                                          {"kind", Schema::String("textBox, picture, table, chart, group, or "
                                                                  "shape.")},
                                          {"placeholderType", Schema::String("Placeholder type, when the shape is "
                                                                             "one.")},
                                          {"isGroup", Schema::Boolean("True for group shapes.")},
                                          {"transform", Schema::FreeObject("Position and size in points and EMU, "
                                                                           "and 'rotation' in degrees.")},
                                          {"text", Schema::String("Text of the shape's text frame.")}});

        auto definition = MakeDefinition("get_slide", "Get slide contents",
                                         "Read one slide, either as the shape list the editing tools address or as "
                                         "the semantic model, Markdown, or plain text of that slide alone.",
                                         "slides");
        definition.InputSchema = Schema::Object("Arguments of get_slide.", {"slide"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object(
                "Slide contents; which members are present depends on 'format'.", {"slide", "format"},
                nlohmann::json{
                    {"slide", Schema::Integer("1-based slide index.")},
                    {"format", Schema::String("The rendering that was produced.")},
                    {"layout", Schema::String("Layout name; 'shapes' only.")},
                    {"hidden", Schema::Boolean("Hidden slide flag; 'shapes' only.")},
                    {"notes", Schema::String("Speaker notes; 'shapes' only.")},
                    {"shapes", Schema::Array("Shapes in document order; 'shapes' only.", std::move(shape))},
                    {"document", Schema::FreeObject("The exyokioffice-document envelope holding this slide "
                                                    "alone; 'model' only.")},
                    {"markdown", Schema::String("Markdown rendering of the slide; 'markdown' only.")},
                    {"text", Schema::String("Plain text of the slide; 'text' only.")},
                    {"note", Schema::String("Why a payload was dropped, when one had to be.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return GetSlide(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome GetSlide(ToolContext& context, const nlohmann::json& arguments)
    {
        const auto format = arguments.value("format", std::string("shapes"));
        if (format != "shapes")
        {
            return GetSlideRendering(context, arguments, format);
        }

        PptReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(reader.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        nlohmann::json shapes = nlohmann::json::array();
        for (const auto& [shape, path] : ShapesOf(*slide))
        {
            auto entry = PptAddressing::ShapeToJson(*shape, path);
            entry["placeholderType"] = PlaceholderTypeOf(*slide, *shape);
            shapes.push_back(std::move(entry));
        }

        const bool truncated = TruncateArrayToBudget(shapes);
        auto layout = slide->Layout();

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = arguments.value("slide", static_cast<UInt64>(0));
        data["format"] = format;
        data["layout"] = layout != nullptr ? layout->Name() : std::string();
        data["hidden"] = slide->IsHidden();
        data["notes"] = arguments.value("include_notes", true) ? slide->NotesText() : std::string();
        data["shapes"] = std::move(shapes);

        return ResultBuilder("Read slide " + std::to_string(arguments.value("slide", 0)) + ".")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    /**
     * @brief Answers get_slide for the model, Markdown, and text formats.
     *
     * These reuse the same path get_document_model and get_document_markdown
     * take — the family adapter reads the semantic model, the scope narrows it
     * to the addressed slide, and the shared serializers render it — so a slide
     * looks the same whether it is read alone or as part of the deck.
     */
    static ToolOutcome GetSlideRendering(ToolContext& context, const nlohmann::json& arguments,
                                         const std::string& format)
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
        if (model.Family == Tools::DocumentFamily::Unknown || !model.PowerPoint.has_value())
        {
            return MakeErrorFromDiagnostics(ErrorCode::PackageLoadFailed, "The document model could not be read.",
                                            diagnostics);
        }

        auto& slides = model.PowerPoint->Slides;
        const Size wanted = arguments.value("slide", static_cast<Size>(0));
        if (wanted == 0 || wanted > slides.size())
        {
            return MakeError(ErrorCode::SlideNotFound,
                             "The presentation has " + std::to_string(slides.size()) + " slide(s); slide " +
                                 std::to_string(wanted) + " does not exist.",
                             std::to_string(wanted), "Call list_slides to see the slide indices.");
        }

        auto selected = slides[wanted - 1];
        if (!arguments.value("include_notes", true))
        {
            selected.NotesText.clear();
        }

        slides.clear();
        slides.push_back(std::move(selected));

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = static_cast<UInt64>(wanted);
        data["format"] = format;
        bool truncated = false;

        if (format == "model")
        {
            // The serialized model is measured before it is parsed: it is the
            // payload, so its length is the budget, and an oversized slide then
            // costs no parse at all.
            const auto serialized = Tools::SerializeModelJson(model, false);
            if (serialized.size() > ResponseBudgetBytes)
            {
                data["document"] = nlohmann::json::object();
                data["note"] = "The model exceeded the response budget; read the slide as 'markdown' or 'text' "
                               "instead.";
                truncated = true;
            }
            else
            {
                auto document = nlohmann::json::parse(serialized, nullptr, false);
                if (document.is_discarded())
                {
                    return MakeError(ErrorCode::InternalError, "The serialized document model is not valid JSON.");
                }

                data["document"] = std::move(document);
            }
        }
        else if (format == "markdown")
        {
            auto markdown = Tools::SerializeModelMarkdown(model, diagnostics);
            truncated = TruncateTextToBudget(markdown);
            data["markdown"] = std::move(markdown);
        }
        else
        {
            auto text = Tools::SerializeModelText(model);
            truncated = TruncateTextToBudget(text);
            data["text"] = std::move(text);
        }

        return ResultBuilder("Read slide " + std::to_string(wanted) + " as " + format + ".")
            .WithData(std::move(data))
            .WithDiagnostics(diagnostics)
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterListLayouts(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json layout =
            Schema::Object("One slide layout.", {"name"},
                           nlohmann::json{{"name", Schema::String("Layout name, as accepted by add_slide.")},
                                          {"master", Schema::String("Name of the owning slide master.")},
                                          {"placeholders", Schema::Array("Placeholder types the layout offers.",
                                                                         Schema::String("Placeholder type."))}});

        auto definition = MakeDefinition("list_layouts", "List slide layouts",
                                         "List the slide layouts of the presentation and the placeholders each "
                                         "offers. Pass one of these names to add_slide.",
                                         "slides");
        definition.InputSchema = Schema::Object("Arguments of list_layouts.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Layouts.", {"layouts"},
                           nlohmann::json{{"layouts", Schema::Array("Available layouts.", std::move(layout))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListLayouts(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListLayouts(ToolContext& context, const nlohmann::json& arguments)
    {
        PptReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        nlohmann::json layouts = nlohmann::json::array();
        for (const auto& layout : reader.Editor().SlideLayouts())
        {
            if (layout == nullptr)
            {
                continue;
            }

            nlohmann::json placeholders = nlohmann::json::array();
            for (const auto& placeholder : layout->Placeholders(true))
            {
                if (placeholder == nullptr)
                {
                    continue;
                }

                const auto token = PptAddressing::PlaceholderToken(placeholder->Type());
                if (!token.empty())
                {
                    placeholders.push_back(token);
                }
            }

            auto master = layout->Master();

            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = layout->Name();
            entry["master"] = master != nullptr ? master->Name() : std::string();
            entry["placeholders"] = std::move(placeholders);
            layouts.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto count = layouts.size();
        data["layouts"] = std::move(layouts);

        return ResultBuilder("The presentation offers " + std::to_string(count) + " layout(s).")
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddSlide(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["layout"] = Schema::String("Layout name from list_layouts; the first layout is used when "
                                              "omitted.");
        properties["index"] = InsertPositionProperty("the new slide");
        properties["title"] = Schema::String("Text written into the slide's title placeholder.");
        properties["bullets"] = Schema::Array("Body placeholder bullets.",
                                              Schema::String("One bullet line."));
        properties["notes"] = Schema::String("Speaker notes for the new slide.");

        auto definition = MakeDefinition(
            "add_slide", "Add slide",
            "Add a slide built from a layout. The title and body are written into real placeholders, so the "
            "layout's formatting applies and the outline view shows the slide.",
            "slides");
        definition.InputSchema = Schema::Object("Arguments of add_slide.", {"documentId"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New slide.", {"slide"},
                                            nlohmann::json{{"slide", Schema::Integer("1-based index of the slide.")},
                                                           {"layout", Schema::String("Layout that was used.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"title", "Results"}, {"bullets", nlohmann::json::array({"Up 12%"})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddSlide(context, arguments); };
        registry.Add(std::move(definition));
    }

    /**
     * @brief Writes text into a placeholder shape, creating the placeholder if needed.
     *
     * @param writtenPath Receives the shape path of the placeholder that was
     *        written. The caller reports that path, so it must be the shape the
     *        write landed on rather than the first shape of the same type.
     */
    static bool WritePlaceholderText(PowerPoint::PresentationSlide& slide, P::PlaceholderValues::Value type,
                                     const PowerPoint::PresentationTextFrame& frame, std::string& writtenPath)
    {
        PowerPoint::PresentationPlaceholder::Ptr placeholder;
        for (const auto& candidate : slide.Placeholders(false))
        {
            if (candidate != nullptr && candidate->Type() == type)
            {
                placeholder = candidate;
                break;
            }
        }

        if (placeholder == nullptr)
        {
            placeholder = slide.AddPlaceholder(type);
        }

        if (placeholder == nullptr)
        {
            return false;
        }

        for (const auto& [shape, path] : ShapesOf(slide))
        {
            if (SameElement(shape->GetElement(), placeholder->GetElement()))
            {
                if (!shape->SetTextFrame(frame))
                {
                    return false;
                }

                writtenPath = path;
                return true;
            }
        }

        return false;
    }

    /// Writes a placeholder when the caller has no use for the resulting path.
    static bool WritePlaceholderText(PowerPoint::PresentationSlide& slide, P::PlaceholderValues::Value type,
                                     const PowerPoint::PresentationTextFrame& frame)
    {
        std::string ignored;
        return WritePlaceholderText(slide, type, frame, ignored);
    }

    static ToolOutcome AddSlide(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        if (!CheckInsertPosition(arguments, "index", session.Editor().SlideCount(), failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        const auto layoutName = arguments.value("layout", std::string());
        auto layouts = session.Editor().SlideLayouts();
        if (layouts.empty() && layoutName.empty())
        {
            // A freshly created presentation has no master and no layout, and
            // a slide without a layout cannot carry real placeholders. One
            // default pair is created so the very first add_slide works.
            auto master = session.Editor().AddSlideMaster("ExyokiOffice");
            if (master != nullptr)
            {
                session.Editor().AddSlideLayout(master, "Title and Content",
                                                P::SlideLayoutValues::Object);
                layouts = session.Editor().SlideLayouts();
            }
        }

        PowerPoint::PresentationSlideLayout::Ptr layout;
        if (!layoutName.empty())
        {
            const auto match = std::find_if(layouts.begin(), layouts.end(),
                                            [&layoutName](const PowerPoint::PresentationSlideLayout::Ptr& candidate)
                                            { return candidate != nullptr && candidate->Name() == layoutName; });
            if (match == layouts.end())
            {
                return MakeError(ErrorCode::LayoutNotFound,
                                 "The presentation has no layout named '" + layoutName + "'.", layoutName,
                                 "Call list_layouts to see the available layouts.");
            }

            layout = *match;
        }
        else if (!layouts.empty())
        {
            layout = layouts.front();
        }

        auto builder = session.Editor().CreateSlideBuilder();
        if (layout != nullptr)
        {
            builder.SetLayout(layout);
        }

        auto slide = session.Editor().AddSlide(builder);
        if (slide == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide could not be added.");
        }

        const auto title = arguments.value("title", std::string());
        if (!title.empty())
        {
            PowerPoint::PresentationTextFrame frame;
            PowerPoint::PresentationTextParagraph paragraph;
            PowerPoint::PresentationTextRun run;
            run.Text = title;
            paragraph.Runs.push_back(std::move(run));
            frame.Paragraphs.push_back(std::move(paragraph));
            if (!WritePlaceholderText(*slide, P::PlaceholderValues::Title, frame))
            {
                return MakeError(ErrorCode::OperationFailed, "The title placeholder could not be written.");
            }
        }

        const auto bullets = arguments.find("bullets");
        if (bullets != arguments.end() && bullets->is_array() && !bullets->empty())
        {
            PowerPoint::PresentationTextFrame frame;
            if (!PptAddressing::ReadTextFrame(arguments, frame, failure))
            {
                return failure;
            }

            if (!WritePlaceholderText(*slide, P::PlaceholderValues::Body, frame))
            {
                return MakeError(ErrorCode::OperationFailed, "The body placeholder could not be written.");
            }
        }

        const auto notes = arguments.value("notes", std::string());
        if (!notes.empty())
        {
            slide->SetNotesText(notes);
        }

        Size index = session.Editor().SlideCount();
        const Size requested = arguments.value("index", static_cast<Size>(0));
        if (requested > 0 && requested < index)
        {
            session.Editor().MoveSlide(index - 1, requested - 1);
            index = requested;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = static_cast<UInt64>(index);
        data["layout"] = layout != nullptr ? layout->Name() : std::string();

        return ResultBuilder("Added slide " + std::to_string(index) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDeleteSlide(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();

        auto definition = MakeDefinition("delete_slide", "Delete slide",
                                         "Remove one slide. Later slides shift down by one.", "slides");
        definition.InputSchema =
            Schema::Object("Arguments of delete_slide.", {"documentId", "slide"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Deletion result.", {"slideCount"},
                                            nlohmann::json{{"slideCount", Schema::Integer("Remaining slides.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 2}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteSlide(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteSlide(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        if (PptAddressing::FindSlide(session.Editor(), arguments, failure) == nullptr)
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        const Size index = arguments.value("slide", static_cast<Size>(0));
        if (!session.Editor().RemoveSlide(index - 1))
        {
            return MakeError(ErrorCode::OperationFailed, "The slide could not be removed.", std::to_string(index));
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slideCount"] = static_cast<UInt64>(session.Editor().SlideCount());

        return ResultBuilder("Deleted slide " + std::to_string(index) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterMoveSlide(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["from"] = Schema::Integer("1-based index of the slide to move.", 1);
        properties["to"] = Schema::Integer("1-based destination index.", 1);

        auto definition = MakeDefinition("move_slide", "Move slide", "Move one slide to another position.", "slides");
        definition.InputSchema =
            Schema::Object("Arguments of move_slide.", {"documentId", "from", "to"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Move result.", {"slide"},
                                            nlohmann::json{{"slide", Schema::Integer("New index of the slide.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"from", 3}, {"to", 1}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return MoveSlide(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome MoveSlide(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size from = arguments.value("from", static_cast<Size>(0));
        const Size to = arguments.value("to", static_cast<Size>(0));
        const auto count = session.Editor().SlideCount();
        if (from == 0 || from > count || to == 0 || to > count)
        {
            return MakeError(ErrorCode::SlideNotFound,
                             "The presentation has " + std::to_string(count) + " slide(s).",
                             std::to_string(from) + "->" + std::to_string(to),
                             "Call list_slides to see the slide indices.");
        }

        MutationGuard guard(session.Session());

        if (!session.Editor().MoveSlide(from - 1, to - 1))
        {
            return MakeError(ErrorCode::OperationFailed, "The slide could not be moved.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = static_cast<UInt64>(to);

        return ResultBuilder("Moved slide " + std::to_string(from) + " to position " + std::to_string(to) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDuplicateSlide(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["to_index"] = InsertPositionProperty("the copy");

        auto definition = MakeDefinition("duplicate_slide", "Duplicate slide",
                                         "Copy one slide inside the same presentation, keeping its layout and "
                                         "content.",
                                         "slides");
        definition.InputSchema =
            Schema::Object("Arguments of duplicate_slide.", {"documentId", "slide"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New slide.", {"slide"},
                                            nlohmann::json{{"slide", Schema::Integer("Index of the copy.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DuplicateSlide(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DuplicateSlide(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        if (PptAddressing::FindSlide(session.Editor(), arguments, failure) == nullptr)
        {
            return failure;
        }

        if (!CheckInsertPosition(arguments, "to_index", session.Editor().SlideCount(), failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        const Size source = arguments.value("slide", static_cast<Size>(0));
        if (session.Editor().CopySlide(source - 1) == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide could not be duplicated.",
                             std::to_string(source));
        }

        Size index = session.Editor().SlideCount();
        const Size requested = arguments.value("to_index", static_cast<Size>(0));
        if (requested > 0 && requested < index)
        {
            session.Editor().MoveSlide(index - 1, requested - 1);
            index = requested;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = static_cast<UInt64>(index);

        return ResultBuilder("Duplicated slide " + std::to_string(source) + " as slide " + std::to_string(index) +
                             ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterCopySlideFrom(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["source_path"] = Schema::String("Workspace-relative presentation to copy from.");
        properties["source_slide"] = Schema::Integer("1-based slide index in the source presentation.", 1);
        properties["to_index"] = InsertPositionProperty("the imported slide");

        auto definition = MakeDefinition(
            "copy_slide_from", "Copy slide from another presentation",
            "Import one slide from another presentation together with its layout, master, theme, media, and "
            "charts. The source file is never modified.",
            "slides");
        definition.InputSchema = Schema::Object("Arguments of copy_slide_from.",
                                                {"documentId", "source_path", "source_slide"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Imported slide.", {"slide"},
                                            nlohmann::json{{"slide", Schema::Integer("Index of the imported slide.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"source_path", "template.pptx"}, {"source_slide", 2}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return CopySlideFrom(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome CopySlideFrom(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sourcePath =
            ToolSupport::ResolveExistingFile(context, arguments.value("source_path", std::string()), failure);
        if (!sourcePath.has_value())
        {
            return failure;
        }

        auto source = PowerPoint::PowerPointDocumentEditor::Open(
            *sourcePath, SettingsWithLimits(context.Adapter().PackageLimits()));
        if (source == nullptr)
        {
            return MakeError(ErrorCode::PackageLoadFailed, "The source presentation could not be opened.",
                             arguments.value("source_path", std::string()));
        }

        const Size sourceSlide = arguments.value("source_slide", static_cast<Size>(0));
        if (sourceSlide == 0 || sourceSlide > source->SlideCount())
        {
            return MakeError(ErrorCode::SlideNotFound,
                             "The source presentation has " + std::to_string(source->SlideCount()) + " slide(s).",
                             std::to_string(sourceSlide));
        }

        if (!CheckInsertPosition(arguments, "to_index", session.Editor().SlideCount(), failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        if (session.Editor().CopySlideFrom(*source, sourceSlide - 1) == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide could not be imported.",
                             std::to_string(sourceSlide));
        }

        Size index = session.Editor().SlideCount();
        const Size requested = arguments.value("to_index", static_cast<Size>(0));
        if (requested > 0 && requested < index)
        {
            session.Editor().MoveSlide(index - 1, requested - 1);
            index = requested;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = static_cast<UInt64>(index);

        return ResultBuilder("Imported the slide as slide " + std::to_string(index) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetSlideHidden(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["hidden"] = Schema::Boolean("True to skip the slide during a show.");

        auto definition = MakeDefinition("set_slide_hidden", "Show or hide slide",
                                         "Mark a slide as hidden or visible in a slide show.", "slides");
        definition.InputSchema = Schema::Object("Arguments of set_slide_hidden.", {"documentId", "slide", "hidden"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Visibility.", {"hidden"},
                                            nlohmann::json{{"hidden", Schema::Boolean("Resulting state.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 3}, {"hidden", true}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetSlideHidden(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetSlideHidden(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        const bool hidden = arguments.value("hidden", false);
        if (!slide->SetHidden(hidden))
        {
            return MakeError(ErrorCode::OperationFailed, "The slide visibility could not be changed.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["hidden"] = hidden;

        return ResultBuilder(hidden ? "Hid the slide." : "Made the slide visible.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- content ------------------------------------------------------------

    static void RegisterSetPlaceholderText(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["placeholder"] =
            Schema::String("Placeholder type such as \"title\" or \"body\", or a 1-based placeholder index.");
        properties["paragraphs"] = PptAddressing::ParagraphsSchema();
        properties["text"] = Schema::String("Plain text shorthand for a single paragraph.");
        properties["bullets"] = Schema::Array("One paragraph per entry.", Schema::String("Bullet line."));

        auto definition = MakeDefinition("set_placeholder_text", "Set placeholder text",
                                         "Write text into a slide placeholder, keeping the layout's formatting. "
                                         "The placeholder is created on the slide when it only exists in the "
                                         "layout.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of set_placeholder_text.",
                                                {"documentId", "slide", "placeholder"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Written placeholder.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Shape path of the "
                                                                                    "placeholder.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"placeholder", "title"}, {"text", "Overview"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetPlaceholderText(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetPlaceholderText(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        PowerPoint::PresentationTextFrame frame;
        if (!PptAddressing::ReadTextFrame(arguments, frame, failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        const auto token = arguments.value("placeholder", std::string());
        const auto type = PptAddressing::ParsePlaceholderType(token);

        // The reported path is the shape the write actually landed on. Looking
        // the placeholder up again by type afterwards reports nothing for an
        // index-addressed placeholder, and the wrong shape when the slide
        // carries two placeholders of the same type.
        std::string path;
        if (type.has_value())
        {
            if (!WritePlaceholderText(*slide, *type, frame, path))
            {
                return MakeError(ErrorCode::ShapeNotFound, "The '" + token + "' placeholder could not be written.",
                                 token, "Call get_slide to see which placeholders the slide has.");
            }
        }
        else
        {
            auto placeholder = PptAddressing::FindPlaceholder(*slide, arguments, failure);
            if (placeholder == nullptr)
            {
                return failure;
            }

            bool written = false;
            for (const auto& [shape, candidate] : ShapesOf(*slide))
            {
                if (SameElement(shape->GetElement(), placeholder->GetElement()))
                {
                    written = shape->SetTextFrame(frame);
                    if (written)
                    {
                        path = candidate;
                    }

                    break;
                }
            }

            if (!written)
            {
                return MakeError(ErrorCode::OperationFailed, "The placeholder text could not be written.", token);
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["shape"] = path;

        return ResultBuilder("Wrote the '" + token + "' placeholder.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddTextBox(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["x"] = Schema::Length("Distance from the left edge of the slide.");
        properties["y"] = Schema::Length("Distance from the top edge of the slide.");
        properties["width"] = Schema::Length("Box width.");
        properties["height"] = Schema::Length("Box height.");
        properties["paragraphs"] = PptAddressing::ParagraphsSchema();
        properties["text"] = Schema::String("Plain text shorthand for a single paragraph.");
        properties["bullets"] = Schema::Array("One paragraph per entry.", Schema::String("Bullet line."));

        auto definition = MakeDefinition("add_text_box", "Add text box",
                                         "Add a free-floating text box at an explicit position. Use "
                                         "set_placeholder_text when the layout already provides a placeholder.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of add_text_box.",
                                                {"documentId", "slide", "x", "y", "width", "height"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New text box.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Shape path of the box.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"x", "2cm"}, {"y", "2cm"}, {"width", "10cm"}, {"height", "3cm"}, {"text", "Hello"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddTextBox(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddTextBox(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        PowerPoint::PresentationShapeTransform transform;
        if (!PptAddressing::ReadTransform(arguments, transform, true, failure))
        {
            return failure;
        }

        PowerPoint::PresentationTextFrame frame;
        if (!PptAddressing::ReadTextFrame(arguments, frame, failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        auto tree = slide->ShapeTree();
        if (tree == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide has no shape tree.");
        }

        auto shape = tree->AddShape();
        if (shape == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The text box could not be added.");
        }

        shape->SetTransform(transform);
        if (!shape->SetTextFrame(frame))
        {
            return MakeError(ErrorCode::OperationFailed, "The text could not be written into the new box.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["shape"] = std::to_string(tree->Count());

        return ResultBuilder("Added a text box to slide " + std::to_string(arguments.value("slide", 0)) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterEditTextFrame(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["shape"] = Schema::String("Shape path from get_slide, for example \"2\" or \"2/1\".");
        properties["paragraphs"] = PptAddressing::ParagraphsSchema();
        properties["text"] = Schema::String("Plain text shorthand for a single paragraph.");
        properties["bullets"] = Schema::Array("One paragraph per entry.", Schema::String("Bullet line."));

        auto definition = MakeDefinition("edit_text_frame", "Edit shape text",
                                         "Replace the text of a shape's text frame.", "content");
        definition.InputSchema =
            Schema::Object("Arguments of edit_text_frame.", {"documentId", "slide", "shape"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Edited shape.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Shape path.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"shape", "2"}, {"text", "Updated"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return EditTextFrame(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome EditTextFrame(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        const auto path = arguments.value("shape", std::string());
        auto shape = PptAddressing::FindShape(*slide, path, failure);
        if (shape == nullptr)
        {
            return failure;
        }

        PowerPoint::PresentationTextFrame frame;
        if (!PptAddressing::ReadTextFrame(arguments, frame, failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        if (!shape->SetTextFrame(frame))
        {
            return MakeError(ErrorCode::OperationFailed, "The shape has no text frame to write into.", path,
                             "Call get_slide to see which shapes carry text.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["shape"] = path;

        return ResultBuilder("Rewrote the text of shape " + path + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDeleteShape(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["shape"] = Schema::String("Shape path from get_slide.");

        auto definition = MakeDefinition("delete_shape", "Delete shape",
                                         "Remove one shape from a slide. Shape paths shift afterwards, so read "
                                         "them back before addressing shapes again.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of delete_shape.", {"documentId", "slide", "shape"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Deletion result.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Path of the removed shape.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"shape", "3"}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteShape(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteShape(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        const auto path = arguments.value("shape", std::string());
        auto shape = PptAddressing::FindShape(*slide, path, failure);
        if (shape == nullptr)
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        if (!shape->Remove())
        {
            return MakeError(ErrorCode::OperationFailed, "The shape could not be removed.", path);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["shape"] = path;

        return ResultBuilder("Deleted shape " + path + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetShapeTransform(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["shape"] = Schema::String("Shape path from get_slide.");
        properties["x"] = Schema::Length("New distance from the left edge.");
        properties["y"] = Schema::Length("New distance from the top edge.");
        properties["width"] = Schema::Length("New width.");
        properties["height"] = Schema::Length("New height.");
        properties["rotation"] = PptAddressing::RotationSchema();

        auto definition = MakeDefinition("set_shape_transform", "Move or resize shape",
                                         "Change the position, size, or rotation of a shape. Rotation is in "
                                         "degrees. Omitted members keep their current value.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of set_shape_transform.", {"documentId", "slide", "shape"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New transform.", {"transform"},
                                            nlohmann::json{{"transform", Schema::FreeObject("Position and size in "
                                                                                            "points and EMU, and "
                                                                                            "'rotation' in "
                                                                                            "degrees.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"}, {"slide", 1}, {"shape", "2"}, {"x", "3cm"}, {"y", "4cm"}, {"rotation", 45}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetShapeTransform(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetShapeTransform(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        const auto path = arguments.value("shape", std::string());
        auto shape = PptAddressing::FindShape(*slide, path, failure);
        if (shape == nullptr)
        {
            return failure;
        }

        auto transform = shape->GetTransform().value_or(PowerPoint::PresentationShapeTransform{});
        if (!PptAddressing::ReadTransform(arguments, transform, false, failure))
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        if (!shape->SetTransform(transform))
        {
            return MakeError(ErrorCode::OperationFailed, "The shape transform could not be written.", path);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["transform"] = PptAddressing::TransformToJson(transform);

        return ResultBuilder("Updated the transform of shape " + path + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- media --------------------------------------------------------------

    static void RegisterAddImage(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["path"] = Schema::String("Workspace-relative image file; mutually exclusive with dataBase64.");
        properties["dataBase64"] = Schema::String("Base64 image payload; mutually exclusive with path.");
        properties["contentType"] = Schema::String("Media type; detected from the payload when omitted.");
        properties["x"] = Schema::Length("Distance from the left edge of the slide.");
        properties["y"] = Schema::Length("Distance from the top edge of the slide.");
        properties["width"] = Schema::Length("Rendered width; the aspect ratio is kept when only one is given.");
        properties["height"] = Schema::Length("Rendered height; the aspect ratio is kept when only one is given.");
        properties["alt"] = Schema::String("Alternative text.");

        auto definition = MakeDefinition("add_image", "Add image",
                                         "Place a picture on a slide. Supplying only one of width or height keeps "
                                         "the image's natural aspect ratio.",
                                         "media");
        definition.InputSchema =
            Schema::Object("Arguments of add_image.", {"documentId", "slide", "x", "y"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New picture.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Shape path of the picture.")},
                                                           {"contentType", Schema::String("Media type stored.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"}, {"slide", 1}, {"path", "logo.png"}, {"x", "2cm"}, {"y", "2cm"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddImage(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddImage(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        std::vector<Byte> bytes;
        std::string contentType;
        if (!ToolSupport::LoadImagePayload(context, arguments, bytes, contentType, failure))
        {
            return failure;
        }

        PowerPoint::PresentationShapeTransform transform;
        if (!PptAddressing::ReadTransform(arguments, transform, false, failure))
        {
            return failure;
        }

        // A picture must have an extent; the natural pixel size at its own
        // resolution fills in whichever dimension the caller left out.
        const auto detected = DetectImageFormat(bytes);
        Real naturalWidthPt = 0.0;
        Real naturalHeightPt = 0.0;
        if (detected.has_value() && detected->HorizontalDpi > 0.0 && detected->VerticalDpi > 0.0)
        {
            naturalWidthPt = static_cast<Real>(detected->PixelWidth) / detected->HorizontalDpi * 72.0;
            naturalHeightPt = static_cast<Real>(detected->PixelHeight) / detected->VerticalDpi * 72.0;
        }

        const bool hasWidth = arguments.contains("width");
        const bool hasHeight = arguments.contains("height");
        if (!hasWidth && !hasHeight)
        {
            transform.Size.Width = MeasuringUnits(naturalWidthPt > 0.0 ? naturalWidthPt : 200.0,
                                                  MeasurementUnit::Point);
            transform.Size.Height = MeasuringUnits(naturalHeightPt > 0.0 ? naturalHeightPt : 150.0,
                                                   MeasurementUnit::Point);
        }
        else if (hasWidth && !hasHeight)
        {
            const Real ratio = naturalWidthPt > 0.0 ? naturalHeightPt / naturalWidthPt : 0.75;
            transform.Size.Height = MeasuringUnits(ToPointValue(transform.Size.Width) * ratio,
                                                   MeasurementUnit::Point);
        }
        else if (!hasWidth && hasHeight)
        {
            const Real ratio = naturalHeightPt > 0.0 ? naturalWidthPt / naturalHeightPt : 4.0 / 3.0;
            transform.Size.Width = MeasuringUnits(ToPointValue(transform.Size.Height) * ratio,
                                                  MeasurementUnit::Point);
        }

        MutationGuard guard(session.Session());

        auto tree = slide->ShapeTree();
        if (tree == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide has no shape tree.");
        }

        PowerPoint::PresentationPictureData picture;
        PowerPoint::PresentationEmbeddedPicture embedded;
        embedded.Data = std::move(bytes);
        embedded.ContentType = contentType;
        picture.Embedded = std::move(embedded);
        picture.AltText = arguments.value("alt", std::string());
        picture.Transform = transform;

        auto shape = tree->AddPicture(picture);
        if (shape == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The picture could not be added.", {},
                             "Check that the payload is a supported image format.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["shape"] = std::to_string(tree->Count());
        data["contentType"] = contentType;

        return ResultBuilder("Added a picture to slide " + std::to_string(arguments.value("slide", 0)) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddTable(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["rows"] = Schema::Integer("Number of rows.", 1, 200);
        properties["cols"] = Schema::Integer("Number of columns.", 1, 50);
        properties["data"] = Schema::Array("Cell texts, row by row.",
                                           Schema::Array("One row of cell texts.", Schema::String("Cell text.")));
        properties["x"] = Schema::Length("Distance from the left edge of the slide.");
        properties["y"] = Schema::Length("Distance from the top edge of the slide.");
        properties["width"] = Schema::Length("Table width.");
        properties["height"] = Schema::Length("Table height.");

        auto definition = MakeDefinition("add_table", "Add table",
                                         "Add a table to a slide, optionally filled from a two-dimensional array "
                                         "of cell texts.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of add_table.",
                           {"documentId", "slide", "rows", "cols", "x", "y", "width", "height"},
                           std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New table.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Shape path of the table.")},
                                                           {"rows", Schema::Integer("Row count.")},
                                                           {"columns", Schema::Integer("Column count.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"rows", 2}, {"cols", 2}, {"x", "2cm"}, {"y", "2cm"}, {"width", "16cm"}, {"height", "4cm"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddTable(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        PowerPoint::PresentationShapeTransform transform;
        if (!PptAddressing::ReadTransform(arguments, transform, true, failure))
        {
            return failure;
        }

        const Size rows = arguments.value("rows", static_cast<Size>(0));
        const Size columns = arguments.value("cols", static_cast<Size>(0));

        PowerPoint::PresentationTableData table;
        table.Transform = transform;

        const Real columnWidthPt = ToPointValue(transform.Size.Width) / static_cast<Real>(columns);
        const Real rowHeightPt = ToPointValue(transform.Size.Height) / static_cast<Real>(rows);
        for (Size column = 0; column < columns; ++column)
        {
            table.ColumnWidths.emplace_back(columnWidthPt, MeasurementUnit::Point);
        }

        const auto data = arguments.find("data");
        for (Size row = 0; row < rows; ++row)
        {
            PowerPoint::PresentationTableRow tableRow;
            tableRow.Height = MeasuringUnits(rowHeightPt, MeasurementUnit::Point);
            for (Size column = 0; column < columns; ++column)
            {
                PowerPoint::PresentationTableCell cell;
                if (data != arguments.end() && data->is_array() && row < data->size() && (*data)[row].is_array() &&
                    column < (*data)[row].size())
                {
                    cell.Text = (*data)[row][column].get<std::string>();
                }

                tableRow.Cells.push_back(std::move(cell));
            }

            table.Rows.push_back(std::move(tableRow));
        }

        MutationGuard guard(session.Session());

        auto tree = slide->ShapeTree();
        if (tree == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide has no shape tree.");
        }

        auto shape = tree->AddTable(table);
        if (shape == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The table could not be added.");
        }

        guard.Commit();

        nlohmann::json result = nlohmann::json::object();
        result["shape"] = std::to_string(tree->Count());
        result["rows"] = static_cast<UInt64>(rows);
        result["columns"] = static_cast<UInt64>(columns);

        return ResultBuilder("Added a " + std::to_string(rows) + "x" + std::to_string(columns) + " table.")
            .WithSession(session.Session())
            .WithData(std::move(result))
            .Build();
    }

    static void RegisterEditTableCell(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["shape"] = Schema::String("Shape path of the table.");
        properties["row"] = Schema::Integer("1-based row index.", 1);
        properties["col"] = Schema::Integer("1-based column index.", 1);
        properties["text"] = Schema::String("Cell text.");

        auto definition = MakeDefinition("edit_table_cell", "Edit table cell",
                                         "Rewrite one cell of a slide table. A cell covered by a merge cannot be "
                                         "addressed.",
                                         "content");
        definition.InputSchema = Schema::Object("Arguments of edit_table_cell.",
                                                {"documentId", "slide", "shape", "row", "col"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Edited cell.", {"row", "col"},
                                            nlohmann::json{{"row", Schema::Integer("Row index.")},
                                                           {"col", Schema::Integer("Column index.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"}, {"slide", 1}, {"shape", "2"}, {"row", 1}, {"col", 1}, {"text", "Region"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return EditTableCell(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome EditTableCell(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        const auto path = arguments.value("shape", std::string());
        auto shape = PptAddressing::FindShape(*slide, path, failure);
        if (shape == nullptr)
        {
            return failure;
        }

        auto table = shape->GetTable();
        if (!table.has_value())
        {
            return MakeError(ErrorCode::ShapeNotFound, "Shape " + path + " is not a table.", path,
                             "Call get_slide to see which shapes are tables.");
        }

        const Size row = arguments.value("row", static_cast<Size>(0));
        const Size column = arguments.value("col", static_cast<Size>(0));
        if (row == 0 || row > table->Rows.size())
        {
            return MakeError(ErrorCode::AnchorInvalid,
                             "The table has " + std::to_string(table->Rows.size()) + " row(s).",
                             std::to_string(row) + "," + std::to_string(column),
                             "Call get_slide to see the table shapes of the slide.");
        }

        // A ragged table has rows of different widths, so the column has to be
        // checked against the row that is about to be indexed.
        const auto& cells = table->Rows[row - 1].Cells;
        if (column == 0 || column > cells.size())
        {
            return MakeError(ErrorCode::AnchorInvalid,
                             "Row " + std::to_string(row) + " has " + std::to_string(cells.size()) + " cell(s).",
                             std::to_string(row) + "," + std::to_string(column),
                             "Call get_slide to see the table shapes of the slide.");
        }

        const auto covered = std::any_of(table->Merges.begin(), table->Merges.end(),
                                         [row, column](const PowerPoint::PresentationTableMerge& merge)
                                         {
                                             const bool insideRows =
                                                 row - 1 >= merge.Row && row - 1 < merge.Row + merge.RowSpan;
                                             const bool insideColumns = column - 1 >= merge.Column &&
                                                                        column - 1 < merge.Column + merge.ColumnSpan;
                                             const bool isAnchor = row - 1 == merge.Row && column - 1 == merge.Column;
                                             return insideRows && insideColumns && !isAnchor;
                                         });
        if (covered)
        {
            return MakeError(ErrorCode::AnchorInvalid,
                             "The addressed cell is covered by a merge and holds no content of its own.",
                             std::to_string(row) + "," + std::to_string(column),
                             "Address the anchor cell of the merged region instead.");
        }

        MutationGuard guard(session.Session());

        table->Rows[row - 1].Cells[column - 1].Text = arguments.value("text", std::string());
        if (!shape->SetTable(*table))
        {
            return MakeError(ErrorCode::OperationFailed, "The table could not be written back.", path);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["row"] = static_cast<UInt64>(row);
        data["col"] = static_cast<UInt64>(column);

        return ResultBuilder("Rewrote cell " + std::to_string(row) + "," + std::to_string(column) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddChart(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["type"] = Schema::Enumeration("Chart type.", {"bar", "column", "line", "pie", "scatter", "area"});
        properties["categories"] = Schema::Array("Category labels shared by every series.",
                                                 Schema::String("One category label."));
        properties["series"] = Schema::Array(
            "Data series.",
            Schema::Object("One series.", {"name", "values"},
                           nlohmann::json{{"name", Schema::String("Series name shown in the legend.")},
                                          {"values", Schema::Array("Numeric values in category order.",
                                                                   Schema::Number("One value."))}}));
        properties["x"] = Schema::Length("Distance from the left edge of the slide.");
        properties["y"] = Schema::Length("Distance from the top edge of the slide.");
        properties["width"] = Schema::Length("Chart width.");
        properties["height"] = Schema::Length("Chart height.");
        properties["title"] = Schema::String("Chart title.");

        auto definition = MakeDefinition("add_chart", "Add chart",
                                         "Add a chart built from categories and series values. This version offers "
                                         "the basic chart types only.",
                                         "content");
        definition.InputSchema = Schema::Object(
            "Arguments of add_chart.", {"documentId", "slide", "type", "series", "x", "y", "width", "height"},
            std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New chart.", {"shape"},
                                            nlohmann::json{{"shape", Schema::String("Shape path of the chart.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"slide", 1},
            {"type", "column"},
            {"categories", nlohmann::json::array({"Q1", "Q2"})},
            {"series", nlohmann::json::array({nlohmann::json{{"name", "Revenue"},
                                                             {"values", nlohmann::json::array({10.0, 12.0})}}})},
            {"x", "2cm"},
            {"y", "3cm"},
            {"width", "18cm"},
            {"height", "10cm"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddChart(context, arguments); };
        registry.Add(std::move(definition));
    }

    static PowerPoint::PresentationChartType ParseChartType(const std::string& token)
    {
        if (token == "bar")
        {
            return PowerPoint::PresentationChartType::Bar;
        }

        if (token == "line")
        {
            return PowerPoint::PresentationChartType::Line;
        }

        if (token == "pie")
        {
            return PowerPoint::PresentationChartType::Pie;
        }

        if (token == "scatter")
        {
            return PowerPoint::PresentationChartType::XyScatter;
        }

        if (token == "area")
        {
            return PowerPoint::PresentationChartType::Area;
        }

        return PowerPoint::PresentationChartType::Column;
    }

    static ToolOutcome AddChart(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        PowerPoint::PresentationShapeTransform transform;
        if (!PptAddressing::ReadTransform(arguments, transform, true, failure))
        {
            return failure;
        }

        std::vector<std::string> categories;
        const auto categoryValues = arguments.find("categories");
        if (categoryValues != arguments.end() && categoryValues->is_array())
        {
            for (const auto& value : *categoryValues)
            {
                categories.push_back(value.get<std::string>());
            }
        }

        PowerPoint::PresentationChartDefinition chart;
        chart.Type = ParseChartType(arguments.value("type", std::string("column")));
        chart.Title = arguments.value("title", std::string());
        chart.Transform = transform;

        for (const auto& value : arguments.at("series"))
        {
            PowerPoint::PresentationChartSeries series;
            series.Name = value.value("name", std::string());
            for (const auto& number : value.at("values"))
            {
                series.Values.push_back(number.get<Real>());
            }

            if (!categories.empty())
            {
                series.Categories = categories;
            }

            chart.Series.push_back(std::move(series));
        }

        if (chart.Series.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "'series' must hold at least one series.");
        }

        MutationGuard guard(session.Session());

        auto tree = slide->ShapeTree();
        if (tree == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The slide has no shape tree.");
        }

        auto shape = tree->AddChart(chart);
        if (shape == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The chart could not be added.", {},
                             "Check that every series has the same number of values as there are categories.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["shape"] = std::to_string(tree->Count());

        return ResultBuilder("Added a chart to slide " + std::to_string(arguments.value("slide", 0)) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetNotes(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["text"] = Schema::String("Speaker notes; newline characters start new paragraphs.");

        auto definition = MakeDefinition("set_notes", "Set speaker notes",
                                         "Replace the speaker notes of a slide.", "content");
        definition.InputSchema =
            Schema::Object("Arguments of set_notes.", {"documentId", "slide", "text"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Notes.", {"slide"},
                                            nlohmann::json{{"slide", Schema::Integer("Slide index.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"text", "Mention the outlook."}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetNotes(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetNotes(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        if (!slide->SetNotesText(arguments.value("text", std::string())))
        {
            return MakeError(ErrorCode::OperationFailed, "The speaker notes could not be written.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slide"] = arguments.value("slide", static_cast<UInt64>(0));

        return ResultBuilder("Wrote the speaker notes.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterListComments(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["slide"] = Schema::Integer("1-based slide index; omit to list every slide.", 1);

        nlohmann::json comment =
            Schema::Object("One comment.", {"slide", "text"},
                           nlohmann::json{{"slide", Schema::Integer("1-based slide index.")},
                                          {"id", Schema::String("Comment identifier.")},
                                          {"author", Schema::String("Author display name.")},
                                          {"text", Schema::String("Comment text.")},
                                          {"status", Schema::String("active, resolved, or closed.")}});

        auto definition = MakeDefinition("list_comments", "List comments",
                                         "List the comments of the presentation, optionally narrowed to one slide.",
                                         "content");
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

    static std::string CommentStatusToken(PowerPoint::PresentationCommentStatus status)
    {
        switch (status)
        {
            case PowerPoint::PresentationCommentStatus::Active:
                return "active";
            case PowerPoint::PresentationCommentStatus::Resolved:
                return "resolved";
            case PowerPoint::PresentationCommentStatus::Closed:
                return "closed";
        }

        return "active";
    }

    static ToolOutcome ListComments(ToolContext& context, const nlohmann::json& arguments)
    {
        PptReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        std::vector<PowerPoint::PresentationCommentAuthor> authors = reader.Editor().CommentAuthors();
        const auto authorName = [&authors](const std::string& id)
        {
            const auto match = std::find_if(authors.begin(), authors.end(),
                                            [&id](const PowerPoint::PresentationCommentAuthor& author)
                                            { return author.Id == id; });
            return match == authors.end() ? std::string() : match->Name;
        };

        const Size wanted = arguments.value("slide", static_cast<Size>(0));
        nlohmann::json comments = nlohmann::json::array();
        const auto slides = reader.Editor().Slides();
        for (Size index = 0; index < slides.size(); ++index)
        {
            if (wanted > 0 && index + 1 != wanted)
            {
                continue;
            }

            if (slides[index] == nullptr)
            {
                continue;
            }

            for (const auto& comment : slides[index]->Comments())
            {
                nlohmann::json entry = nlohmann::json::object();
                entry["slide"] = static_cast<UInt64>(index + 1);
                entry["id"] = comment.Id;
                entry["author"] = authorName(comment.AuthorId);
                entry["text"] = comment.Text;
                entry["status"] = CommentStatusToken(comment.Status);
                comments.push_back(std::move(entry));
            }
        }

        const bool truncated = TruncateArrayToBudget(comments);

        nlohmann::json data = nlohmann::json::object();
        const auto count = comments.size();
        data["comments"] = std::move(comments);

        return ResultBuilder("The presentation holds " + std::to_string(count) + " comment(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterAddComment(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = SlideProperty();
        properties["text"] = Schema::String("Comment text.");
        properties["author"] = Schema::StringWithDefault("Author display name.", "ExyokiOffice");

        auto definition = MakeDefinition("add_comment", "Add comment",
                                         "Attach a comment to a slide. The comment author is registered with the "
                                         "presentation when it does not exist yet.",
                                         "content");
        definition.InputSchema =
            Schema::Object("Arguments of add_comment.", {"documentId", "slide", "text"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New comment.", {"commentId"},
                                            nlohmann::json{{"commentId", Schema::String("Comment identifier.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"slide", 1}, {"text", "Check this number."}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddComment(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddComment(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
        if (slide == nullptr)
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        const auto authorName = arguments.value("author", std::string("ExyokiOffice"));
        auto authors = session.Editor().CommentAuthors();
        const auto existing = std::find_if(authors.begin(), authors.end(),
                                           [&authorName](const PowerPoint::PresentationCommentAuthor& author)
                                           { return author.Name == authorName; });

        std::string authorId;
        if (existing != authors.end())
        {
            authorId = existing->Id;
        }
        else
        {
            // PowerPoint reads the author id as a GUID, like the comment id below.
            PowerPoint::PresentationCommentAuthor author;
            author.Id = Guid::New();
            author.Name = authorName;
            author.Initials = authorName.empty() ? std::string("EO") : authorName.substr(0, 2);
            if (!session.Editor().AddCommentAuthor(author))
            {
                return MakeError(ErrorCode::OperationFailed, "The comment author could not be registered.",
                                 authorName);
            }

            authorId = author.Id;
        }

        // A comment identifier has to be unique across the whole presentation,
        // not only within the slide, and PowerPoint writes a GUID there.
        PowerPoint::PresentationComment comment;
        comment.Id = Guid::New();
        comment.AuthorId = authorId;
        comment.Text = arguments.value("text", std::string());

        if (!slide->AddComment(comment))
        {
            return MakeError(ErrorCode::OperationFailed, "The comment could not be added.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["commentId"] = comment.Id;

        return ResultBuilder("Added a comment to slide " + std::to_string(arguments.value("slide", 0)) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- design -------------------------------------------------------------

    static void RegisterSetTransition(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slide"] = Schema::Integer("1-based slide index; omit together with 'all'.", 1);
        properties["all"] = Schema::BooleanWithDefault("Apply the transition to every slide.", false);
        properties["type"] = Schema::Enumeration(
            "Transition effect.",
            {"none", "fade", "cut", "wipe", "push", "cover", "pull", "split", "dissolve", "zoom", "wheel", "random"});
        properties["duration_ms"] = Schema::Integer("Effect duration in milliseconds.", 0);

        auto definition = MakeDefinition("set_transition", "Set slide transition",
                                         "Set or remove the transition effect of one slide or of every slide.",
                                         "design");
        definition.InputSchema =
            Schema::Object("Arguments of set_transition.", {"documentId", "type"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Transition result.", {"slides"},
                                            nlohmann::json{{"slides", Schema::Integer("Slides changed.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"all", true}, {"type", "fade"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetTransition(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::optional<PowerPoint::PresentationTransitionKind> ParseTransition(const std::string& token)
    {
        if (token == "fade")
        {
            return PowerPoint::PresentationTransitionKind::Fade;
        }

        if (token == "cut")
        {
            return PowerPoint::PresentationTransitionKind::Cut;
        }

        if (token == "wipe")
        {
            return PowerPoint::PresentationTransitionKind::Wipe;
        }

        if (token == "push")
        {
            return PowerPoint::PresentationTransitionKind::Push;
        }

        if (token == "cover")
        {
            return PowerPoint::PresentationTransitionKind::Cover;
        }

        if (token == "pull")
        {
            return PowerPoint::PresentationTransitionKind::Pull;
        }

        if (token == "split")
        {
            return PowerPoint::PresentationTransitionKind::Split;
        }

        if (token == "dissolve")
        {
            return PowerPoint::PresentationTransitionKind::Dissolve;
        }

        if (token == "zoom")
        {
            return PowerPoint::PresentationTransitionKind::Zoom;
        }

        if (token == "wheel")
        {
            return PowerPoint::PresentationTransitionKind::Wheel;
        }

        if (token == "random")
        {
            return PowerPoint::PresentationTransitionKind::Random;
        }

        return std::nullopt;
    }

    static ToolOutcome SetTransition(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto token = arguments.value("type", std::string());
        const bool remove = token == "none";
        const auto kind = ParseTransition(token);
        if (!remove && !kind.has_value())
        {
            return MakeError(ErrorCode::InputInvalid, "Unknown transition '" + token + "'.", token);
        }

        std::vector<PowerPoint::PresentationSlide::Ptr> targets;
        if (arguments.value("all", false))
        {
            targets = session.Editor().Slides();
        }
        else
        {
            ToolOutcome failure;
            auto slide = PptAddressing::FindSlide(session.Editor(), arguments, failure);
            if (slide == nullptr)
            {
                return failure;
            }

            targets.push_back(slide);
        }

        MutationGuard guard(session.Session());

        Size changed = 0;
        for (const auto& slide : targets)
        {
            if (slide == nullptr)
            {
                continue;
            }

            if (remove)
            {
                if (slide->RemoveTransition())
                {
                    ++changed;
                }

                continue;
            }

            PowerPoint::PresentationTransitionData transition;
            transition.Kind = *kind;
            const auto duration = arguments.value("duration_ms", 0);
            if (duration > 0)
            {
                transition.Duration = static_cast<UInt32>(duration);
            }

            if (slide->SetTransition(transition))
            {
                ++changed;
            }
        }

        if (changed == 0)
        {
            return MakeError(ErrorCode::OperationFailed, "No slide transition could be changed.", token);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["slides"] = static_cast<UInt64>(changed);

        return ResultBuilder("Applied the '" + token + "' transition to " + std::to_string(changed) + " slide(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddSection(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["name"] = Schema::String("Section name.");
        properties["before_slide"] = Schema::Integer("1-based slide the section starts at.", 1);

        auto definition = MakeDefinition("add_section", "Add section",
                                         "Group slides into a named section starting at a slide.", "design");
        definition.InputSchema = Schema::Object("Arguments of add_section.", {"documentId", "name", "before_slide"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New section.", {"name", "sectionId"},
                                            nlohmann::json{{"name", Schema::String("Section name.")},
                                                           {"sectionId", Schema::String("Section identifier, a GUID "
                                                                                        "in braces.")},
                                                           {"slideCount", Schema::Integer("Slides in the section.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"name", "Results"}, {"before_slide", 3}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddSection(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddSection(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const Size first = arguments.value("before_slide", static_cast<Size>(0));
        const auto slides = session.Editor().Slides();
        if (first == 0 || first > slides.size())
        {
            return MakeError(ErrorCode::SlideNotFound,
                             "The presentation has " + std::to_string(slides.size()) + " slide(s).",
                             std::to_string(first), "Call list_slides to see the slide indices.");
        }

        // PowerPoint identifies a section by a GUID in braces, which is what
        // Guid::New mints; a name-derived identifier is neither unique nor the
        // shape the format expects.
        PowerPoint::PresentationSection section;
        section.Name = arguments.value("name", std::string());
        section.Id = Guid::New();
        for (Size index = first - 1; index < slides.size(); ++index)
        {
            if (slides[index] != nullptr)
            {
                section.SlideIds.push_back(slides[index]->Id());
            }
        }

        MutationGuard guard(session.Session());

        if (!session.Editor().AddSection(section))
        {
            return MakeError(ErrorCode::OperationFailed, "The section could not be added.", section.Name,
                             "Every slide belongs to at most one section; the slides from 'before_slide' on may "
                             "already belong to another one.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = section.Name;
        data["sectionId"] = section.Id;
        data["slideCount"] = static_cast<UInt64>(section.SlideIds.size());

        return ResultBuilder("Added section '" + section.Name + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetSlideSize(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["preset"] = Schema::Enumeration("Named slide size.", {"16:9", "16:10", "4:3", "A4"});
        properties["width"] = Schema::Length("Slide width; use together with height instead of a preset.");
        properties["height"] = Schema::Length("Slide height; use together with width instead of a preset.");

        auto definition = MakeDefinition("set_slide_size", "Set slide size",
                                         "Set the slide size from a preset or from explicit dimensions.", "design");
        definition.InputSchema = Schema::Object("Arguments of set_slide_size.", {"documentId"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Slide size.", {"widthPt", "heightPt"},
                                            nlohmann::json{{"widthPt", Schema::Number("Slide width in points.")},
                                                           {"heightPt", Schema::Number("Slide height in points.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"preset", "16:9"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetSlideSize(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetSlideSize(ToolContext& context, const nlohmann::json& arguments)
    {
        PptSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        PowerPoint::PresentationSlideSize size;
        const auto preset = arguments.value("preset", std::string());
        if (!preset.empty())
        {
            if (preset == "16:9")
            {
                size = PowerPoint::PresentationSlideSize::Widescreen16x9();
            }
            else if (preset == "16:10")
            {
                size = PowerPoint::PresentationSlideSize::Widescreen16x10();
            }
            else if (preset == "4:3")
            {
                size = PowerPoint::PresentationSlideSize::Standard4x3();
            }
            else if (preset == "A4")
            {
                size = PowerPoint::PresentationSlideSize::A4Landscape();
            }
            else
            {
                return MakeError(ErrorCode::InputInvalid, "Unknown slide size preset '" + preset + "'.", preset);
            }
        }
        else
        {
            const auto width = arguments.find("width");
            const auto height = arguments.find("height");
            if (width == arguments.end() || height == arguments.end())
            {
                return MakeError(ErrorCode::InputInvalid, "Pass either 'preset' or both 'width' and 'height'.");
            }

            const auto parsedWidth = ParseLength(*width);
            const auto parsedHeight = ParseLength(*height);
            if (!parsedWidth.has_value() || !parsedHeight.has_value())
            {
                return MakeError(ErrorCode::InputInvalid, "The slide width or height is not a valid length.");
            }

            size.Size = PowerPoint::PresentationSize(*parsedWidth, *parsedHeight);
        }

        MutationGuard guard(session.Session());

        if (!session.Editor().SetSlideSize(size))
        {
            return MakeError(ErrorCode::OperationFailed, "The slide size could not be written.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["widthPt"] = ToPointValue(size.Size.Width);
        data["heightPt"] = ToPointValue(size.Size.Height);

        return ResultBuilder("Set the slide size.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }
};

void RegisterPowerPointToolset(ToolRegistry& registry)
{
    PowerPointTools::Register(registry);
}

} // namespace ExyokiOffice::Mcp
