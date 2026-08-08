// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

/// Helpers shared by the PowerPoint tool cases.
class PowerPointToolTestSupport
{
public:
    /// The transform of one top-level shape of the first slide of a saved presentation.
    static ExyokiOffice::PowerPoint::PresentationShapeTransform ShapeTransformAt(const std::filesystem::path& path,
                                                                                 const std::string& shapePath)
    {
        auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(path);
        REQUIRE(editor != nullptr);
        REQUIRE(editor->SlideCount() >= 1);

        auto tree = editor->GetSlide(0)->ShapeTree();
        REQUIRE(tree != nullptr);

        const auto index = static_cast<std::size_t>(std::stoul(shapePath));
        const auto shapes = tree->Shapes();
        REQUIRE(shapes.size() >= index);

        const auto transform = shapes[index - 1]->GetTransform();
        REQUIRE(transform.has_value());
        return *transform;
    }

    /**
     * @brief Drops the last cell of the second table row of a saved presentation.
     *
     * SetTable refuses a grid whose rows do not all match the column widths, so
     * a ragged table cannot be written through the presentation API — it only
     * ever arrives from a foreign file. Editing the DrawingML directly is the
     * only way to reproduce one, and it is exactly what edit_table_cell has to
     * survive.
     */
    static void MakeSecondRowShort(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

        auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(source);
        REQUIRE(editor != nullptr);

        auto slide = editor->GetSlide(0);
        REQUIRE(slide != nullptr);

        auto tree = slide->ShapeTree();
        REQUIRE(tree != nullptr);

        bool trimmed = false;
        for (const auto& shape : tree->Shapes())
        {
            auto element = shape->GetElement();
            if (element == nullptr)
            {
                continue;
            }

            const auto tables = element->Descendants<Drawing::Table>();
            if (tables.empty())
            {
                continue;
            }

            const auto rows = tables.front()->Elements<Drawing::TableRow>();
            REQUIRE(rows.size() >= 2);

            const auto cells = rows[1]->Elements<Drawing::TableCell>();
            REQUIRE(cells.size() >= 2);
            REQUIRE(rows[1]->RemoveChild(cells.back()));
            trimmed = true;
            break;
        }

        REQUIRE(trimmed);
        REQUIRE(editor->SaveToFile(destination));
    }

    /// Appends a second body placeholder to the first slide of a saved presentation.
    static void AddSecondBodyPlaceholder(const std::filesystem::path& source,
                                         const std::filesystem::path& destination)
    {
        namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

        auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(source);
        REQUIRE(editor != nullptr);

        auto slide = editor->GetSlide(0);
        REQUIRE(slide != nullptr);
        REQUIRE(slide->AddPlaceholder(P::PlaceholderValues::Body, ExyokiOffice::UInt32{3}) != nullptr);
        REQUIRE(editor->SaveToFile(destination));
    }
};

TEST_CASE("slides are added with real placeholders [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "deck.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto added = server->Call(
        "add_slide", nlohmann::json{{"documentId", documentId},
                                    {"title", "Quarterly results"},
                                    {"bullets", nlohmann::json::array({"Revenue up 12%", "Costs flat"})},
                                    {"notes", "Mention the outlook."}});
    REQUIRE(added["ok"] == true);
    CHECK(added["data"]["slide"] == 1);

    // add_slide creates the master and layout a fresh presentation lacks, so
    // the layout catalog is populated afterwards.
    const auto layouts = server->Call("list_layouts", nlohmann::json{{"documentId", documentId}});
    REQUIRE(layouts["ok"] == true);
    CHECK_FALSE(layouts["data"]["layouts"].empty());

    const auto slide = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    REQUIRE(slide["ok"] == true);
    CHECK(slide["data"]["notes"] == "Mention the outlook.");

    bool sawTitlePlaceholder = false;
    for (const auto& shape : slide["data"]["shapes"])
    {
        if (shape["placeholderType"] == "title")
        {
            sawTitlePlaceholder = true;
            CHECK(shape["text"] == "Quarterly results");
        }
    }

    // A plain text box would render but stay out of the outline, so the title
    // has to be a real p:ph placeholder.
    CHECK(sawTitlePlaceholder);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(server->Path("deck.pptx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->SlideCount() == 1);
    CHECK_FALSE(editor->GetSlide(0)->Placeholders(false).empty());

    const auto report = ExyokiOffice::Tools::Run(server->Path("deck.pptx"));
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("an unknown layout is reported as layout_not_found [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto refused =
        server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"layout", "No Such Layout"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "layout_not_found");
}

TEST_CASE("slides are listed, moved, duplicated, hidden, and deleted [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "First"}}));
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "Second"}}));

    const auto listed = server->Call("list_slides", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["data"]["slides"].size() == 2);
    CHECK(listed["data"]["slides"][0]["title"] == "First");

    const auto moved =
        server->Call("move_slide", nlohmann::json{{"documentId", documentId}, {"from", 2}, {"to", 1}});
    REQUIRE(moved["ok"] == true);

    const auto reordered = server->Call("list_slides", nlohmann::json{{"documentId", documentId}});
    CHECK(reordered["data"]["slides"][0]["title"] == "Second");

    const auto duplicated =
        server->Call("duplicate_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    REQUIRE(duplicated["ok"] == true);
    CHECK(duplicated["data"]["slide"] == 3);

    const auto hidden = server->Call(
        "set_slide_hidden", nlohmann::json{{"documentId", documentId}, {"slide", 3}, {"hidden", true}});
    REQUIRE(hidden["ok"] == true);

    const auto deleted = server->Call("delete_slide", nlohmann::json{{"documentId", documentId}, {"slide", 3}});
    REQUIRE(deleted["ok"] == true);
    CHECK(deleted["data"]["slideCount"] == 2);

    const auto missing = server->Call("delete_slide", nlohmann::json{{"documentId", documentId}, {"slide", 9}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "slide_not_found");
}

TEST_CASE("text boxes and shape transforms are written [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "shapes.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}}));

    const auto box = server->Call("add_text_box", nlohmann::json{{"documentId", documentId},
                                                                 {"slide", 1},
                                                                 {"x", "2cm"},
                                                                 {"y", "3cm"},
                                                                 {"width", "10cm"},
                                                                 {"height", "3cm"},
                                                                 {"text", "Free floating"}});
    REQUIRE(box["ok"] == true);
    const auto path = box["data"]["shape"].get<std::string>();

    const auto edited = server->Call(
        "edit_text_frame",
        nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"shape", path}, {"text", "Rewritten"}});
    REQUIRE(edited["ok"] == true);

    const auto moved = server->Call("set_shape_transform", nlohmann::json{{"documentId", documentId},
                                                                          {"slide", 1},
                                                                          {"shape", path},
                                                                          {"x", "5cm"}});
    REQUIRE(moved["ok"] == true);
    CHECK(moved["data"]["transform"]["x"]["pt"].get<double>() > 100.0);

    const auto slide = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    bool sawText = false;
    for (const auto& shape : slide["data"]["shapes"])
    {
        if (shape.contains("text") && shape["text"] == "Rewritten")
        {
            sawText = true;
        }
    }

    CHECK(sawText);

    const auto unknownShape = server->Call(
        "edit_text_frame",
        nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"shape", "99"}, {"text", "x"}});
    CHECK(unknownShape["ok"] == false);
    CHECK(unknownShape["error"]["code"] == "shape_not_found");

    const auto deleted =
        server->Call("delete_shape", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"shape", path}});
    CHECK(deleted["ok"] == true);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
    const auto report = ExyokiOffice::Tools::Run(server->Path("shapes.pptx"));
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("placeholder text is written by type [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}}));

    const auto written = server->Call("set_placeholder_text", nlohmann::json{{"documentId", documentId},
                                                                             {"slide", 1},
                                                                             {"placeholder", "title"},
                                                                             {"text", "Agenda"}});
    REQUIRE(written["ok"] == true);

    const auto slide = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    bool sawTitle = false;
    for (const auto& shape : slide["data"]["shapes"])
    {
        if (shape["placeholderType"] == "title" && shape["text"] == "Agenda")
        {
            sawTitle = true;
        }
    }

    CHECK(sawTitle);

    const auto unknown = server->Call("set_placeholder_text", nlohmann::json{{"documentId", documentId},
                                                                             {"slide", 1},
                                                                             {"placeholder", "nonsense"},
                                                                             {"text", "x"}});
    CHECK(unknown["ok"] == false);
}

TEST_CASE("tables and charts are placed on a slide [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "content.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}}));

    const auto table = server->Call(
        "add_table", nlohmann::json{{"documentId", documentId},
                                    {"slide", 1},
                                    {"rows", 2},
                                    {"cols", 2},
                                    {"x", "2cm"},
                                    {"y", "2cm"},
                                    {"width", "16cm"},
                                    {"height", "4cm"},
                                    {"data", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                                                    nlohmann::json::array({"North", "1200"})})}});
    REQUIRE(table["ok"] == true);
    const auto tablePath = table["data"]["shape"].get<std::string>();

    const auto cell = server->Call("edit_table_cell", nlohmann::json{{"documentId", documentId},
                                                                     {"slide", 1},
                                                                     {"shape", tablePath},
                                                                     {"row", 2},
                                                                     {"col", 2},
                                                                     {"text", "1500"}});
    REQUIRE(cell["ok"] == true);

    const auto outOfRange = server->Call("edit_table_cell", nlohmann::json{{"documentId", documentId},
                                                                           {"slide", 1},
                                                                           {"shape", tablePath},
                                                                           {"row", 9},
                                                                           {"col", 1},
                                                                           {"text", "x"}});
    CHECK(outOfRange["ok"] == false);
    CHECK(outOfRange["error"]["code"] == "anchor_invalid");

    const auto chart = server->Call(
        "add_chart",
        nlohmann::json{{"documentId", documentId},
                       {"slide", 1},
                       {"type", "column"},
                       {"categories", nlohmann::json::array({"Q1", "Q2"})},
                       {"series", nlohmann::json::array({nlohmann::json{
                                      {"name", "Revenue"}, {"values", nlohmann::json::array({10.0, 12.0})}}})},
                       {"x", "2cm"},
                       {"y", "8cm"},
                       {"width", "18cm"},
                       {"height", "8cm"}});
    REQUIRE(chart["ok"] == true);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
    const auto report = ExyokiOffice::Tools::Run(server->Path("content.pptx"));
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("an image keeps its aspect ratio when only the width is given [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}}));

    // A 1x1 PNG: the natural ratio is square, so the height must match the width.
    const std::string png =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    const auto image = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                {"slide", 1},
                                                                {"dataBase64", png},
                                                                {"x", "1cm"},
                                                                {"y", "1cm"},
                                                                {"width", "4cm"}});
    REQUIRE(image["ok"] == true);
    CHECK(image["data"]["contentType"] == "image/png");

    const auto slide = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    bool sawPicture = false;
    for (const auto& shape : slide["data"]["shapes"])
    {
        if (shape["kind"] == "picture")
        {
            sawPicture = true;
            const auto width = shape["transform"]["width"]["pt"].get<double>();
            const auto height = shape["transform"]["height"]["pt"].get<double>();
            CHECK(height == doctest::Approx(width).epsilon(0.05));
        }
    }

    CHECK(sawPicture);

    const auto broken =
        server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                 {"slide", 1},
                                                 {"dataBase64", "not base64 at all!!"},
                                                 {"x", "1cm"},
                                                 {"y", "1cm"}});
    CHECK(broken["ok"] == false);
}

TEST_CASE("notes, comments, transitions, sections, and the slide size are set [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "design.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "One"}}));
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "Two"}}));

    const auto notes = server->Call(
        "set_notes", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"text", "Speaker notes"}});
    REQUIRE(notes["ok"] == true);

    const auto comment = server->Call(
        "add_comment",
        nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"text", "Check this"}, {"author", "Reviewer"}});
    REQUIRE(comment["ok"] == true);

    const auto comments = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    REQUIRE(comments["data"]["comments"].size() == 1);
    CHECK(comments["data"]["comments"][0]["author"] == "Reviewer");

    const auto transition =
        server->Call("set_transition", nlohmann::json{{"documentId", documentId}, {"all", true}, {"type", "fade"}});
    REQUIRE(transition["ok"] == true);
    CHECK(transition["data"]["slides"] == 2);

    const auto section = server->Call(
        "add_section", nlohmann::json{{"documentId", documentId}, {"name", "Results"}, {"before_slide", 2}});
    REQUIRE(section["ok"] == true);
    CHECK(section["data"]["slideCount"] == 1);

    const auto size = server->Call("set_slide_size", nlohmann::json{{"documentId", documentId}, {"preset", "4:3"}});
    REQUIRE(size["ok"] == true);
    CHECK(size["data"]["widthPt"].get<double>() == doctest::Approx(720.0).epsilon(0.01));

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
    const auto report = ExyokiOffice::Tools::Run(server->Path("design.pptx"));
    CAPTURE(report.LoadIssues.size());
    CAPTURE(report.ValidationIssues.size());
    std::string validationDetails;
    for (const auto& issue : report.ValidationIssues)
    {
        validationDetails += "id=" + std::to_string(static_cast<int>(issue.Id)) + " part=" + issue.PartUri +
                             " message=" + issue.Message + "\n";
    }
    INFO(validationDetails);
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a slide is imported from another presentation [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto sourceCreated = server->Call("create_document", nlohmann::json{{"path", "source.pptx"}});
    const auto sourceId = sourceCreated["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", sourceId}, {"title", "Imported"}}));
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", sourceId}}));
    static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", sourceId}}));

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto imported = server->Call("copy_slide_from", nlohmann::json{{"documentId", documentId},
                                                                         {"source_path", "source.pptx"},
                                                                         {"source_slide", 1}});
    REQUIRE(imported["ok"] == true);

    const auto slides = server->Call("list_slides", nlohmann::json{{"documentId", documentId}});
    CHECK(slides["data"]["slides"].size() == 1);

    const auto outOfRange = server->Call("copy_slide_from", nlohmann::json{{"documentId", documentId},
                                                                           {"source_path", "source.pptx"},
                                                                           {"source_slide", 9}});
    CHECK(outOfRange["ok"] == false);
    CHECK(outOfRange["error"]["code"] == "slide_not_found");
}

TEST_CASE("a PowerPoint batch is atomic and is one undo step [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();
    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto applied = server->Call(
        "batch", nlohmann::json{{"documentId", documentId},
                                {"operations", nlohmann::json::array(
                                                   {nlohmann::json{{"tool", "add_slide"},
                                                                   {"arguments", {{"title", "One"}}}},
                                                    nlohmann::json{{"tool", "add_slide"},
                                                                   {"arguments", {{"title", "Two"}}}}})}});
    REQUIRE(applied["ok"] == true);
    CHECK(applied["revision"] == 1);
    CHECK(server->Call("list_slides", nlohmann::json{{"documentId", documentId}})["data"]["slides"].size() == 2);

    const auto failed = server->Call(
        "batch", nlohmann::json{{"documentId", documentId},
                                {"operations", nlohmann::json::array(
                                                   {nlohmann::json{{"tool", "add_slide"},
                                                                   {"arguments", {{"title", "Three"}}}},
                                                    nlohmann::json{{"tool", "delete_slide"},
                                                                   {"arguments", {{"slide", 99}}}}})}});
    CHECK(failed["ok"] == false);
    CHECK(failed["error"]["code"] == "batch_aborted");
    CHECK(server->Call("list_slides", nlohmann::json{{"documentId", documentId}})["data"]["slides"].size() == 2);

    REQUIRE(server->Call("undo", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    CHECK(server->Call("list_slides", nlohmann::json{{"documentId", documentId}})["data"]["slides"].empty());
}

TEST_CASE("get_slide renders one slide as shapes, a model, Markdown, or text [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId},
                                                               {"title", "Quarterly results"},
                                                               {"notes", "Mention the outlook."}}));
    static_cast<void>(
        server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "Appendix only"}}));

    // The default keeps the shape list every editing tool addresses.
    const auto shapes = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    REQUIRE(shapes["ok"] == true);
    CHECK(shapes["data"]["format"] == "shapes");
    CHECK_FALSE(shapes["data"]["shapes"].empty());
    CHECK(shapes["data"]["notes"] == "Mention the outlook.");

    const auto model = server->Call(
        "get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"format", "model"}});
    REQUIRE(model["ok"] == true);
    CHECK(model["data"]["format"] == "model");
    REQUIRE(model["data"]["document"].is_object());

    // The model is narrowed to the addressed slide, exactly as the `scope`
    // argument of get_document_model narrows the deck.
    const auto document = model["data"]["document"].dump();
    CHECK(document.find("Quarterly results") != std::string::npos);
    CHECK(document.find("Appendix only") == std::string::npos);

    const auto markdown = server->Call(
        "get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"format", "markdown"}});
    REQUIRE(markdown["ok"] == true);
    CHECK(markdown["data"]["format"] == "markdown");
    const auto rendered = markdown["data"]["markdown"].get<std::string>();
    CHECK(rendered.find("Quarterly results") != std::string::npos);
    CHECK(rendered.find("Appendix only") == std::string::npos);
    CHECK(rendered.find("Mention the outlook.") != std::string::npos);

    const auto text = server->Call(
        "get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"format", "text"}});
    REQUIRE(text["ok"] == true);
    CHECK(text["data"]["format"] == "text");
    const auto extracted = text["data"]["text"].get<std::string>();
    CHECK(extracted.find("Quarterly results") != std::string::npos);
    CHECK(extracted.find("Appendix only") == std::string::npos);

    const auto withoutNotes = server->Call("get_slide", nlohmann::json{{"documentId", documentId},
                                                                       {"slide", 1},
                                                                       {"format", "text"},
                                                                       {"include_notes", false}});
    REQUIRE(withoutNotes["ok"] == true);
    CHECK(withoutNotes["data"]["text"].get<std::string>().find("Mention the outlook.") == std::string::npos);

    const auto missing = server->Call(
        "get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 9}, {"format", "markdown"}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "slide_not_found");

    const auto unknownFormat = server->Call(
        "get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"format", "pdf"}});
    CHECK(unknownFormat["ok"] == false);
    CHECK(unknownFormat["error"]["code"] == "input_invalid");
}

TEST_CASE("shape rotation is expressed in degrees [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "rotation.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}}));

    const auto box = server->Call("add_text_box", nlohmann::json{{"documentId", documentId},
                                                                 {"slide", 1},
                                                                 {"x", "2cm"},
                                                                 {"y", "2cm"},
                                                                 {"width", "8cm"},
                                                                 {"height", "3cm"},
                                                                 {"text", "Tilted"}});
    REQUIRE(box["ok"] == true);
    const auto path = box["data"]["shape"].get<std::string>();

    const auto negative = server->Call("set_shape_transform", nlohmann::json{{"documentId", documentId},
                                                                             {"slide", 1},
                                                                             {"shape", path},
                                                                             {"rotation", -12.5}});
    REQUIRE(negative["ok"] == true);
    CHECK(negative["data"]["transform"]["rotation"].get<double>() == doctest::Approx(-12.5));

    const auto radians = server->Call("set_shape_transform", nlohmann::json{{"documentId", documentId},
                                                                            {"slide", 1},
                                                                            {"shape", path},
                                                                            {"rotation", "0.5rad"}});
    REQUIRE(radians["ok"] == true);
    CHECK(radians["data"]["transform"]["rotation"].get<double>() == doctest::Approx(28.6478897).epsilon(0.0001));

    const auto nonsense = server->Call("set_shape_transform", nlohmann::json{{"documentId", documentId},
                                                                             {"slide", 1},
                                                                             {"shape", path},
                                                                             {"rotation", "sideways"}});
    CHECK(nonsense["ok"] == false);
    CHECK(nonsense["error"]["code"] == "input_invalid");

    const auto quarter = server->Call("set_shape_transform", nlohmann::json{{"documentId", documentId},
                                                                            {"slide", 1},
                                                                            {"shape", path},
                                                                            {"rotation", "45deg"}});
    REQUIRE(quarter["ok"] == true);
    CHECK(quarter["data"]["transform"]["rotation"].get<double>() == doctest::Approx(45.0));

    const auto slide = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    REQUIRE(slide["ok"] == true);

    bool sawRotation = false;
    for (const auto& shape : slide["data"]["shapes"])
    {
        if (shape["path"] == path)
        {
            sawRotation = true;
            CHECK(shape["transform"]["rotation"].get<double>() == doctest::Approx(45.0));
        }
    }

    CHECK(sawRotation);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    // 45 degrees is 2 700 000 of the 1/60000-degree units PresentationML stores.
    const auto transform = PowerPointToolTestSupport::ShapeTransformAt(server->Path("rotation.pptx"), path);
    CHECK(transform.Rotation == 2700000);
}

TEST_CASE("set_placeholder_text reports the placeholder it wrote [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "placeholders.pptx"}});
    const auto firstId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", firstId},
                                                               {"title", "Agenda"},
                                                               {"bullets", nlohmann::json::array({"First"})}}));
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", firstId}}));
    static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", firstId}}));

    // Two body placeholders on one slide is what a two-content layout produces;
    // the presentation API reaches it through the placeholder index.
    PowerPointToolTestSupport::AddSecondBodyPlaceholder(server->Path("placeholders.pptx"),
                                                        server->Path("two-bodies.pptx"));

    const auto opened = server->Call("open_document", nlohmann::json{{"path", "two-bodies.pptx"}});
    REQUIRE(opened["ok"] == true);
    const auto documentId = opened["data"]["documentId"].get<std::string>();

    // Addressing by type writes the first placeholder of that type and has to
    // report that one's path, not an empty string.
    const auto byType = server->Call("set_placeholder_text", nlohmann::json{{"documentId", documentId},
                                                                            {"slide", 1},
                                                                            {"placeholder", "body"},
                                                                            {"text", "First body"}});
    REQUIRE(byType["ok"] == true);
    const auto firstBodyPath = byType["data"]["shape"].get<std::string>();
    CHECK_FALSE(firstBodyPath.empty());

    // Addressing by index used to report an empty path, because the lookup
    // compared the placeholder type against the literal argument.
    const auto byIndex = server->Call("set_placeholder_text", nlohmann::json{{"documentId", documentId},
                                                                             {"slide", 1},
                                                                             {"placeholder", "3"},
                                                                             {"text", "Second body"}});
    REQUIRE(byIndex["ok"] == true);
    const auto secondBodyPath = byIndex["data"]["shape"].get<std::string>();
    CHECK_FALSE(secondBodyPath.empty());
    CHECK(secondBodyPath != firstBodyPath);

    const auto slide = server->Call("get_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}});
    REQUIRE(slide["ok"] == true);

    std::string firstBodyText;
    std::string secondBodyText;
    for (const auto& shape : slide["data"]["shapes"])
    {
        if (shape["path"] == firstBodyPath)
        {
            firstBodyText = shape.value("text", std::string());
        }
        else if (shape["path"] == secondBodyPath)
        {
            secondBodyText = shape.value("text", std::string());
        }
    }

    CHECK(firstBodyText == "First body");
    CHECK(secondBodyText == "Second body");
}

TEST_CASE("an unknown placeholder token lists every accepted token [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}}));

    const auto refused = server->Call("set_placeholder_text", nlohmann::json{{"documentId", documentId},
                                                                             {"slide", 1},
                                                                             {"placeholder", "nonsense"},
                                                                             {"text", "x"}});
    REQUIRE(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "input_invalid");

    // A hint that omits accepted tokens sends an agent guessing; the list has
    // to match what ParsePlaceholderType really takes.
    const auto hint = refused["error"]["hint"].get<std::string>();
    for (const auto* token : {"title", "body", "centeredTitle", "subtitle", "dateAndTime", "slideNumber", "footer",
                              "header", "object", "chart", "table", "clipArt", "diagram", "media", "picture"})
    {
        CAPTURE(token);
        CHECK(hint.find(token) != std::string::npos);
    }
}

TEST_CASE("edit_table_cell checks the column against the addressed row [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "ragged.pptx"}});
    const auto firstId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", firstId}}));

    const auto table = server->Call(
        "add_table", nlohmann::json{{"documentId", firstId},
                                    {"slide", 1},
                                    {"rows", 2},
                                    {"cols", 2},
                                    {"x", "2cm"},
                                    {"y", "2cm"},
                                    {"width", "16cm"},
                                    {"height", "4cm"},
                                    {"data", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                                                    nlohmann::json::array({"North", "1200"})})}});
    REQUIRE(table["ok"] == true);
    const auto tablePath = table["data"]["shape"].get<std::string>();

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", firstId}}));
    static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", firstId}}));

    PowerPointToolTestSupport::MakeSecondRowShort(server->Path("ragged.pptx"), server->Path("short-row.pptx"));

    const auto opened = server->Call("open_document", nlohmann::json{{"path", "short-row.pptx"}});
    REQUIRE(opened["ok"] == true);
    const auto documentId = opened["data"]["documentId"].get<std::string>();

    // The second row now holds one cell. Validating the column against the
    // first row would let this call index past the end of that row.
    const auto beyondRow = server->Call("edit_table_cell", nlohmann::json{{"documentId", documentId},
                                                                          {"slide", 1},
                                                                          {"shape", tablePath},
                                                                          {"row", 2},
                                                                          {"col", 2},
                                                                          {"text", "x"}});
    CHECK(beyondRow["ok"] == false);
    CHECK(beyondRow["error"]["code"] == "anchor_invalid");
    CHECK(beyondRow["error"]["message"].get<std::string>().find("Row 2 has 1 cell(s).") != std::string::npos);

    const auto beyondTable = server->Call("edit_table_cell", nlohmann::json{{"documentId", documentId},
                                                                            {"slide", 1},
                                                                            {"shape", tablePath},
                                                                            {"row", 9},
                                                                            {"col", 1},
                                                                            {"text", "x"}});
    CHECK(beyondTable["ok"] == false);
    CHECK(beyondTable["error"]["code"] == "anchor_invalid");
    CHECK(beyondTable["error"]["message"].get<std::string>().find("2 row(s)") != std::string::npos);
}

TEST_CASE("a section carries a GUID identifier [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "sections.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "One"}}));
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "Two"}}));

    const auto section = server->Call(
        "add_section", nlohmann::json{{"documentId", documentId}, {"name", "Results"}, {"before_slide", 2}});
    REQUIRE(section["ok"] == true);

    // PowerPoint identifies a section by a braced GUID, not by its name.
    const auto sectionId = section["data"]["sectionId"].get<std::string>();
    REQUIRE(sectionId.size() == 38);
    CHECK(sectionId.front() == '{');
    CHECK(sectionId.back() == '}');
    CHECK(sectionId.find("Results") == std::string::npos);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(server->Path("sections.pptx"));
    REQUIRE(editor != nullptr);

    const auto sections = editor->Sections();
    REQUIRE(sections.size() == 1);
    CHECK(sections.front().Id == sectionId);
    CHECK(sections.front().Name == "Results");
    CHECK(sections.front().SlideIds.size() == 1);
}

TEST_CASE("comment identifiers are unique across slides [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "comments.pptx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "One"}}));
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", "Two"}}));

    const auto first = server->Call(
        "add_comment",
        nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"text", "First"}, {"author", "Reviewer"}});
    REQUIRE(first["ok"] == true);

    // A per-slide counter produces the same identifier on the second slide, and
    // the presentation refuses it because comment identifiers are deck-wide.
    const auto second = server->Call(
        "add_comment",
        nlohmann::json{{"documentId", documentId}, {"slide", 2}, {"text", "Second"}, {"author", "Reviewer"}});
    REQUIRE(second["ok"] == true);
    CHECK(second["data"]["commentId"] != first["data"]["commentId"]);

    const auto third = server->Call(
        "add_comment",
        nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"text", "Third"}, {"author", "Reviewer"}});
    REQUIRE(third["ok"] == true);

    const auto comments = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    REQUIRE(comments["data"]["comments"].size() == 3);

    std::set<std::string> identifiers;
    for (const auto& comment : comments["data"]["comments"])
    {
        identifiers.insert(comment["id"].get<std::string>());
    }

    CHECK(identifiers.size() == 3);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(server->Path("comments.pptx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->SlideCount() == 2);
    CHECK(editor->GetSlide(0)->Comments().size() == 2);
    CHECK(editor->GetSlide(1)->Comments().size() == 1);
    CHECK(editor->GetSlide(0)->Comments().front().Id != editor->GetSlide(1)->Comments().front().Id);
}

TEST_CASE("a position past the end of the presentation is refused [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();

    const auto sourceCreated = server->Call("create_document", nlohmann::json{{"path", "template.pptx"}});
    const auto sourceId = sourceCreated["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("add_slide", nlohmann::json{{"documentId", sourceId}, {"title", "Imported"}}));
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", sourceId}}));
    static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", sourceId}}));

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // One past the end appends, which is the documented way to say "at the end".
    const auto appended =
        server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"index", 1}, {"title", "One"}});
    REQUIRE(appended["ok"] == true);
    CHECK(appended["data"]["slide"] == 1);

    // Anything beyond that is an agent mistake and is reported, not clamped.
    const auto refused =
        server->Call("add_slide", nlohmann::json{{"documentId", documentId}, {"index", 9}, {"title", "Nine"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "slide_not_found");
    CHECK(refused["error"]["hint"].get<std::string>().find("list_slides") != std::string::npos);

    const auto duplicated = server->Call(
        "duplicate_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"to_index", 9}});
    CHECK(duplicated["ok"] == false);
    CHECK(duplicated["error"]["code"] == "slide_not_found");

    const auto imported = server->Call("copy_slide_from", nlohmann::json{{"documentId", documentId},
                                                                         {"source_path", "template.pptx"},
                                                                         {"source_slide", 1},
                                                                         {"to_index", 9}});
    CHECK(imported["ok"] == false);
    CHECK(imported["error"]["code"] == "slide_not_found");

    // Every refusal left the deck alone.
    CHECK(server->Call("list_slides", nlohmann::json{{"documentId", documentId}})["data"]["slides"].size() == 1);

    // The boundary itself still works for both copying tools.
    const auto atEnd = server->Call(
        "duplicate_slide", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"to_index", 2}});
    CHECK(atEnd["ok"] == true);
    CHECK(atEnd["data"]["slide"] == 2);
}
