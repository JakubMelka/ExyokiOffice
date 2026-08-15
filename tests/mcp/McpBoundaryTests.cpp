// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "ExcelToolset.hpp"
#include "WordToolset.hpp"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

class McpBoundaryFixture
{
public:
    static std::unique_ptr<McpTestServer> Word(ServerOptions options)
    {
        return std::make_unique<McpTestServer>(
            std::make_shared<WordFamilyAdapter>(),
            std::vector<ToolsetRegistrar>{[](ToolRegistry& registry)
                                          { RegisterWordToolset(registry); }},
            std::move(options));
    }

    static std::unique_ptr<McpTestServer> Excel(ServerOptions options = {})
    {
        return std::make_unique<McpTestServer>(
            std::make_shared<ExcelFamilyAdapter>(),
            std::vector<ToolsetRegistrar>{[](ToolRegistry& registry)
                                          { RegisterExcelToolset(registry); }},
            std::move(options));
    }
};

TEST_CASE("snapshot depth bounds undo history and zero disables snapshots [mcp-lifecycle]")
{
    ServerOptions options;
    options.SnapshotDepth = 1;
    auto server = McpBoundaryFixture::Word(options);
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    for (const auto& text : {"First", "Second"})
    {
        REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                {"anchor", {{"position", "end"}}},
                                                                {"text", text}})["ok"] == true);
    }

    REQUIRE(server->Call("undo", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    const auto exhausted = server->Call("undo", nlohmann::json{{"documentId", documentId}});
    CHECK(exhausted["ok"] == false);
    CHECK(exhausted["error"]["code"] == "snapshot_unavailable");

    options.SnapshotDepth = 0;
    auto disabled = McpBoundaryFixture::Word(options);
    disabled->Initialize();
    const auto disabledCreated = disabled->Call("create_document", nlohmann::json::object());
    const auto disabledId = disabledCreated["data"]["documentId"].get<std::string>();
    REQUIRE(disabled->Call("insert_paragraph", nlohmann::json{{"documentId", disabledId},
                                                              {"anchor", {{"position", "end"}}},
                                                              {"text", "Not captured"}})["ok"] == true);
    const auto unavailable = disabled->Call("undo", nlohmann::json{{"documentId", disabledId}});
    CHECK(unavailable["ok"] == false);
    CHECK(unavailable["error"]["code"] == "snapshot_unavailable");
}

TEST_CASE("without snapshots a failed edit leaves the earlier ones alone [mcp-lifecycle]")
{
    ServerOptions options;
    options.SnapshotDepth = 0;

    auto server = McpBoundaryFixture::Word(options);
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                            {"anchor", {{"position", "end"}}},
                                                            {"text", "Kept"}})["ok"] == true);

    const auto failure = server->Call("apply_style", nlohmann::json{{"documentId", documentId},
                                                                    {"blocks", nlohmann::json::array({1})},
                                                                    {"style_id", "NoSuchStyle"}});
    REQUIRE(failure["ok"] == false);
    CHECK(failure["error"]["code"] == "style_not_found");

    // The failed edit took no snapshot of its own, so it must not restore one:
    // rolling back to "the newest snapshot" would discard the paragraph that
    // the earlier, successful call added.
    const auto blocks = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    REQUIRE(blocks["ok"] == true);
    CHECK(blocks["data"]["blockCount"] == 1);
}

TEST_CASE("configured media limit rejects uploads and downloads [mcp-lifecycle]")
{
    static const std::string png =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    ServerOptions zeroLimit;
    zeroLimit.MaxMediaSizeMiB = 0;
    auto uploadServer = McpBoundaryFixture::Word(zeroLimit);
    uploadServer->Initialize();
    const auto created = uploadServer->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    const auto refused = uploadServer->Call("insert_image", nlohmann::json{{"documentId", documentId},
                                                                           {"anchor", {{"position", "end"}}},
                                                                           {"dataBase64", png}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "input_invalid");

    auto sourceServer = McpBoundaryFixture::Word(ServerOptions{});
    sourceServer->Initialize();
    const auto sourceCreated = sourceServer->Call("create_document", nlohmann::json{{"path", "image.docx"}});
    const auto sourceId = sourceCreated["data"]["documentId"].get<std::string>();
    REQUIRE(sourceServer->Call("insert_image", nlohmann::json{{"documentId", sourceId},
                                                              {"anchor", {{"position", "end"}}},
                                                              {"dataBase64", png}})["ok"] == true);
    REQUIRE(sourceServer->Call("save_document", nlohmann::json{{"documentId", sourceId}})["ok"] == true);

    std::filesystem::copy_file(sourceServer->Path("image.docx"), uploadServer->Path("image.docx"));
    const auto media = uploadServer->Call("list_media", nlohmann::json{{"path", "image.docx"}});
    REQUIRE(media["ok"] == true);
    REQUIRE(media["data"]["media"].size() == 1);
    const auto mediaId = media["data"]["media"][0]["id"].get<std::string>();
    const auto download = uploadServer->Call("get_media", nlohmann::json{{"path", "image.docx"},
                                                                         {"media_id", mediaId}});
    CHECK(download["ok"] == false);
    // The arguments are valid; it is the payload that does not fit, so the
    // failure is about the operation rather than about the input.
    CHECK(download["error"]["code"] == "operation_failed");
    CHECK(download["error"]["hint"].get<std::string>().find("export_media") != std::string::npos);
}

TEST_CASE("Word and Excel readers report deterministic pagination [mcp-lifecycle]")
{
    SUBCASE("Word blocks")
    {
        auto server = McpBoundaryFixture::Word(ServerOptions{});
        server->Initialize();
        const auto created = server->Call("create_document", nlohmann::json::object());
        const auto documentId = created["data"]["documentId"].get<std::string>();
        for (int index = 1; index <= 5; ++index)
        {
            REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                    {"anchor", {{"position", "end"}}},
                                                                    {"text", "Paragraph " + std::to_string(index)}})["ok"] == true);
        }

        const auto first = server->Call("read_blocks", nlohmann::json{{"documentId", documentId},
                                                                      {"from", 1},
                                                                      {"count", 2}});
        CHECK(first["data"]["blocks"].size() == 2);
        CHECK(first["data"]["nextOffset"] == 3);
        const auto second = server->Call("read_blocks", nlohmann::json{{"documentId", documentId},
                                                                       {"from", first["data"]["nextOffset"]},
                                                                       {"count", 3}});
        CHECK(second["data"]["blocks"].size() == 3);
        CHECK(second["data"]["nextOffset"] == 0);
    }

    SUBCASE("Excel 10000-cell cap")
    {
        auto server = McpBoundaryFixture::Excel();
        server->Initialize();
        const auto created = server->Call("create_document", nlohmann::json::object());
        const auto documentId = created["data"]["documentId"].get<std::string>();
        REQUIRE(server->Call("write_cells", nlohmann::json{{"documentId", documentId},
                                                           {"cells", nlohmann::json::array(
                                                                         {nlohmann::json{{"address", "CV101"},
                                                                                         {"value", "end"}}})}})["ok"] == true);
        const auto page = server->Call("read_range", nlohmann::json{{"documentId", documentId},
                                                                    {"range", "A1:CV101"}});
        REQUIRE(page["ok"] == true);
        CHECK(page["truncated"] == true);
        CHECK(page["data"]["values"].size() == 100);
        CHECK(page["data"]["nextOffset"] == 100);
    }
}

TEST_CASE("batch rejects more than fifty operations before mutating [mcp-lifecycle]")
{
    auto server = McpBoundaryFixture::Word(ServerOptions{});
    server->Initialize();
    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    nlohmann::json operations = nlohmann::json::array();
    for (int index = 0; index < 51; ++index)
    {
        operations.push_back(nlohmann::json{{"tool", "insert_paragraph"},
                                            {"arguments", {{"anchor", {{"position", "end"}}}, {"text", "x"}}}});
    }

    const auto refused = server->Call("batch", nlohmann::json{{"documentId", documentId},
                                                              {"operations", operations}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "input_invalid");
    CHECK(server->Call("read_blocks", nlohmann::json{{"documentId", documentId}})["data"]["blockCount"] == 0);
}

TEST_CASE("the workspace spans several roots and refuses traversal [mcp-lifecycle]")
{
    const auto firstRoot = MakeTemporaryPath("mcp-root-one", "");
    const auto secondRoot = MakeTemporaryPath("mcp-root-two", "");
    std::filesystem::create_directories(firstRoot);
    std::filesystem::create_directories(secondRoot);

    ServerOptions options;
    options.WorkspaceRoots = {firstRoot, secondRoot};
    auto server = McpBoundaryFixture::Word(options);
    server->Initialize();

    std::ofstream(firstRoot / "one.docx").put('1');
    std::ofstream(secondRoot / "two.docx").put('2');
    const auto listed = server->Call("list_workspace", nlohmann::json{{"glob", "*.docx"}, {"limit", 10}});
    REQUIRE(listed["ok"] == true);
    CHECK(listed["data"]["files"].size() == 2);
    CHECK(listed["data"]["files"][0]["path"] == "one.docx");
    CHECK(listed["data"]["files"][1]["path"] == "two.docx");

    const auto traversal = server->Call("create_document", nlohmann::json{{"path", "../escape.docx"}});
    CHECK(traversal["ok"] == false);
    CHECK(traversal["error"]["code"] == "path_outside_workspace");

    server.reset();
    std::error_code cleanupError;
    std::filesystem::remove_all(firstRoot, cleanupError);
    cleanupError.clear();
    std::filesystem::remove_all(secondRoot, cleanupError);
}

TEST_CASE("workspace containment is decided on the canonical path [mcp-lifecycle]")
{
    const auto root = MakeTemporaryPath("mcp-resolve-root", "");
    std::filesystem::create_directories(root);
    const Workspace workspace(std::vector<std::filesystem::path>{root});

    // Ordinary shapes keep resolving; a name that does not exist yet is the
    // common case for create_document and must not be refused.
    CHECK(workspace.Resolve("report.docx").has_value());
    CHECK(workspace.Resolve("nested/deeper/report.docx").has_value());
    CHECK(workspace.Resolve("zpráva – finální verze.docx").has_value());
    CHECK(workspace.Resolve((root / "report.docx").string()).has_value());

    CHECK_FALSE(workspace.Resolve("").has_value());
    CHECK_FALSE(workspace.Resolve("../escape.docx").has_value());
    CHECK_FALSE(workspace.Resolve((root.parent_path() / "escape.docx").string()).has_value());

    // A path the file system cannot canonicalize is refused rather than decided
    // on its text: a component this long exceeds what any volume accepts, while
    // being lexically inside the root.
    const std::string longName(40000, 'a');
    std::error_code probe;
    static_cast<void>(std::filesystem::weakly_canonical(root / longName, probe));
    REQUIRE_MESSAGE(static_cast<bool>(probe),
                    "This platform canonicalizes a 40 000-character component; the case needs another trigger.");
    CHECK_FALSE(workspace.Resolve(longName).has_value());

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

TEST_CASE("the workspace refuses pathological path shapes [mcp-lifecycle]")
{
    const auto root = MakeTemporaryPath("mcp-forms-root", "");
    std::filesystem::create_directories(root);
    const Workspace workspace(std::vector<std::filesystem::path>{root});

    const auto refused = [&workspace](std::string_view path)
    {
        CAPTURE(path);
        CHECK(Workspace::IsRefusedForm(path));
        CHECK_FALSE(workspace.Resolve(path).has_value());
    };

    // An embedded NUL ends the path for the operating system but not for the
    // containment check, so the two would disagree about which file is meant.
    refused(std::string_view("report\0.docx", 12));
    refused(R"(\\?\C:\windows\system32\drivers\etc\hosts)");
    refused(R"(\\.\PhysicalDrive0)");

#if defined(_WIN32)
    // Alternate data streams: the second spelling even reports ".docx" from
    // extension(), so a family check on the extension reads the wrong value.
    refused("report.docx:hidden");
    refused("report:evil.docx");
    refused("NUL");
    refused("nul.txt");
    refused("COM1.docx");
    refused("sub/CON/report.docx");
    refused(R"(\\server\share\report.docx)");
    refused("C:report.docx");
    refused("report.docx.");
    refused("report.docx ");
#else
    // The same spellings are ordinary file names on a POSIX file system.
    CHECK(workspace.Resolve("nul.txt").has_value());
    CHECK(workspace.Resolve("2026-08-05T10:00:00.docx").has_value());
#endif

    // Navigation still reads as leaving the workspace, not as a refused shape.
    CHECK_FALSE(Workspace::IsRefusedForm("../escape.docx"));

    CHECK(Workspace::IsAcceptableName("part"));
    CHECK_FALSE(Workspace::IsAcceptableName(""));
    CHECK_FALSE(Workspace::IsAcceptableName(".."));
    CHECK_FALSE(Workspace::IsAcceptableName("../../evil"));
    CHECK_FALSE(Workspace::IsAcceptableName("sub/part"));
    CHECK_FALSE(Workspace::IsAcceptableName(R"(sub\part)"));

    // Unlike IsRefusedForm, this one does not follow the platform: it asks
    // whether a name may become a file, which is what the library asks about
    // the names it derives itself, so both sides answer the same everywhere.
    // Without that, `prefix: "NUL"` would be input_invalid on Windows and
    // operation_failed on Linux for the same call.
    CHECK_FALSE(Workspace::IsAcceptableName("NUL"));
    CHECK_FALSE(Workspace::IsAcceptableName("com1.docx"));
    CHECK_FALSE(Workspace::IsAcceptableName("part:hidden"));
    CHECK_FALSE(Workspace::IsAcceptableName("part "));
    CHECK(Workspace::IsAcceptableName("nul-report"));

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

TEST_CASE("a refused path shape is reported as path_invalid [mcp-lifecycle]")
{
    auto server = McpBoundaryFixture::Word(ServerOptions{});
    server->Initialize();

#if defined(_WIN32)
    const std::string refusedPath = "NUL.docx";
#else
    const std::string refusedPath = R"(\\?\C:\escape.docx)";
#endif

    const auto created = server->Call("create_document", nlohmann::json{{"path", refusedPath}});
    CHECK(created["ok"] == false);
    CHECK(created["error"]["code"] == "path_invalid");
    CHECK_FALSE(created["error"]["hint"].get<std::string>().empty());

    // A path that merely leaves the workspace keeps its own code, because the
    // repair an agent has to make is a different one.
    const auto outside = server->Call("open_document", nlohmann::json{{"path", "../escape.docx"}});
    CHECK(outside["ok"] == false);
    CHECK(outside["error"]["code"] == "path_outside_workspace");
}

TEST_CASE("split_document refuses a prefix that leaves the output directory [mcp-lifecycle]")
{
    auto server = McpBoundaryFixture::Word(ServerOptions{});
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "source.docx"}});
    REQUIRE(created["ok"] == true);
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                            {"anchor", nlohmann::json{{"position", "end"}}},
                                                            {"text", "Body"}})["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    // The prefix ends up inside the output file names, which the workspace never
    // resolves; without the guard these numbered files land outside output_dir.
    const auto split = server->Call("split_document", nlohmann::json{{"input_path", "source.docx"},
                                                                     {"output_dir", "parts"},
                                                                     {"prefix", "../../evil"}});
    CHECK(split["ok"] == false);
    CHECK(split["error"]["code"] == "input_invalid");

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(server->Root().parent_path(), error))
    {
        CHECK(entry.path().filename().string().rfind("evil", 0) != 0);
    }
}

TEST_CASE("corrupt packages are rejected without creating a session [mcp-lifecycle]")
{
    auto server = McpBoundaryFixture::Word(ServerOptions{});
    server->Initialize();
    std::ofstream(server->Path("broken.docx"), std::ios::binary) << "this is not an OPC package";

    const auto refused = server->Call("open_document", nlohmann::json{{"path", "broken.docx"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "package_load_failed");
    CHECK(server->Call("list_documents", nlohmann::json::object())["data"]["documents"].empty());
}

TEST_CASE("the configured package limits govern a document opened by path [mcp-lifecycle]")
{
    // Every read tool that accepts a `path` opens it through the adapter, which
    // is the one place `--package-limits` is applied. Worth pinning down on a
    // tool other than open_document, because these used to reach the package
    // through the path-based Tools API instead, where the adapter's policy did
    // not apply at all.
    auto author = McpBoundaryFixture::Word(ServerOptions{});
    author->Initialize();

    const auto created = author->Call("create_document", nlohmann::json{{"path", "report.docx"}});
    REQUIRE(created["ok"] == true);
    REQUIRE(author->Call("save_document",
                         nlohmann::json{{"documentId", created["data"]["documentId"]}})["ok"] == true);
    REQUIRE(std::filesystem::exists(author->Path("report.docx")));

    // The same file read by a server that permits one ZIP entry. Pointed at the
    // first server's workspace so it is the very package just written.
    ServerOptions tight;
    tight.WorkspaceRoots.push_back(author->Root());
    tight.PackageLimits = ExyokiOffice::OpenXmlPackageLimits::Unlimited();
    tight.PackageLimits.MaxEntries = 1;

    auto constrained = McpBoundaryFixture::Word(tight);
    constrained->Initialize();

    const auto refused = constrained->Call("get_document_model", nlohmann::json{{"path", "report.docx"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "package_load_failed");

    // And the unconstrained server reads the same file, so the refusal above
    // was the policy rather than the document.
    CHECK(author->Call("get_document_model", nlohmann::json{{"path", "report.docx"}})["ok"] == true);
}

TEST_CASE("large read responses are truncated to the protocol budget [mcp-lifecycle]")
{
    auto server = McpBoundaryFixture::Word(ServerOptions{});
    server->Initialize();
    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    const std::string largeText(1200u * 1024u, 'x');
    REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                            {"anchor", {{"position", "end"}}},
                                                            {"text", largeText}})["ok"] == true);

    const auto read = server->Call("get_document_text", nlohmann::json{{"documentId", documentId}});
    REQUIRE(read["ok"] == true);
    CHECK(read["truncated"] == true);
    CHECK(read["data"]["text"].get<std::string>().size() < largeText.size());
    CHECK(read.dump().size() < 1100u * 1024u);
}
