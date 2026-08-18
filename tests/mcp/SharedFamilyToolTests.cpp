// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "ExcelToolset.hpp"
#include "PowerPointToolset.hpp"
#include "WordToolset.hpp"

#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

class SharedFamilyFixture final
{
public:
    enum class TestFamily
    {
        Word,
        Excel,
        PowerPoint
    };

    static std::unique_ptr<McpTestServer> MakeFamilyServer(TestFamily family, ServerOptions options = {})
    {
        switch (family)
        {
            case TestFamily::Word:
                return std::make_unique<McpTestServer>(
                    std::make_shared<WordFamilyAdapter>(),
                    std::vector<ToolsetRegistrar>{{[](ToolRegistry& registry)
                                                   { RegisterWordToolset(registry); }}},
                    std::move(options));
            case TestFamily::Excel:
                return std::make_unique<McpTestServer>(
                    std::make_shared<ExcelFamilyAdapter>(),
                    std::vector<ToolsetRegistrar>{{[](ToolRegistry& registry)
                                                   { RegisterExcelToolset(registry); }}},
                    std::move(options));
            case TestFamily::PowerPoint:
                return std::make_unique<McpTestServer>(
                    std::make_shared<PowerPointFamilyAdapter>(),
                    std::vector<ToolsetRegistrar>{{[](ToolRegistry& registry)
                                                   { RegisterPowerPointToolset(registry); }}},
                    std::move(options));
        }

        return nullptr;
    }

    static std::string Extension(TestFamily family)
    {
        switch (family)
        {
            case TestFamily::Word:
                return ".docx";
            case TestFamily::Excel:
                return ".xlsx";
            case TestFamily::PowerPoint:
                return ".pptx";
        }

        return {};
    }

    static std::string FamilyName(TestFamily family)
    {
        switch (family)
        {
            case TestFamily::Word:
                return "word";
            case TestFamily::Excel:
                return "excel";
            case TestFamily::PowerPoint:
                return "powerpoint";
        }

        return {};
    }

    static void AddSearchableContent(McpTestServer& server, TestFamily family, const std::string& documentId,
                                     const std::string& text)
    {
        nlohmann::json result;
        switch (family)
        {
            case TestFamily::Word:
                result = server.Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                        {"anchor", {{"position", "end"}}},
                                                                        {"text", text}});
                break;
            case TestFamily::Excel:
                result = server.Call("write_range", nlohmann::json{{"documentId", documentId},
                                                                   {"origin", "A1"},
                                                                   {"values", nlohmann::json::array(
                                                                                  {nlohmann::json::array({text})})}});
                break;
            case TestFamily::PowerPoint:
                result = server.Call("add_slide", nlohmann::json{{"documentId", documentId}, {"title", text}});
                break;
        }

        REQUIRE(result["ok"] == true);
    }

    static void CheckSharedRoundTrip(TestFamily family)
    {
        auto server = MakeFamilyServer(family);
        server->Initialize();

        const auto path = "roundtrip" + Extension(family);
        const auto created = server->Call("create_document", nlohmann::json{{"path", path}});
        REQUIRE(created["ok"] == true);
        const auto originalId = created["data"]["documentId"].get<std::string>();
        AddSearchableContent(*server, family, originalId, "Needle 2025");

        const auto saved = server->Call("save_document", nlohmann::json{{"documentId", originalId}});
        REQUIRE(saved["ok"] == true);
        REQUIRE(server->Call("close_document", nlohmann::json{{"documentId", originalId}})["ok"] == true);

        const auto opened = server->Call("open_document", nlohmann::json{{"path", path}});
        REQUIRE(opened["ok"] == true);
        CHECK(opened["data"]["family"] == FamilyName(family));
        CHECK(opened["dirty"] == false);
        const auto documentId = opened["data"]["documentId"].get<std::string>();

        const auto info = server->Call("get_document_info", nlohmann::json{{"documentId", documentId}});
        REQUIRE(info["ok"] == true);
        CHECK(info["data"]["family"] == FamilyName(family));

        const auto text = server->Call("get_document_text", nlohmann::json{{"documentId", documentId}});
        REQUIRE(text["ok"] == true);
        CHECK(text["data"]["text"].get<std::string>().find("Needle 2025") != std::string::npos);

        const auto search = server->Call(
            "search_text", nlohmann::json{{"documentId", documentId}, {"needle", "Needle 2025"}});
        REQUIRE(search["ok"] == true);
        CHECK(search["data"]["matchCount"].get<int>() >= 1);

        const auto replaced = server->Call("replace_text", nlohmann::json{{"documentId", documentId},
                                                                          {"needle", "2025"},
                                                                          {"replacement", "2026"}});
        REQUIRE(replaced["ok"] == true);
        CHECK(replaced["data"]["replacementCount"].get<int>() >= 1);

        const auto properties = server->Call("set_properties", nlohmann::json{{"documentId", documentId},
                                                                              {"title", "Family round trip"},
                                                                              {"creator", "MCP tests"}});
        REQUIRE(properties["ok"] == true);
        const auto readProperties = server->Call("get_properties", nlohmann::json{{"documentId", documentId}});
        CHECK(readProperties["data"]["core"]["title"] == "Family round trip");

        const auto model = server->Call("get_document_model", nlohmann::json{{"documentId", documentId}});
        REQUIRE(model["ok"] == true);
        CHECK(model["data"]["document"]["format"] == "exyokioffice-document");

        const auto markdown = server->Call("get_document_markdown", nlohmann::json{{"documentId", documentId}});
        REQUIRE(markdown["ok"] == true);
        CHECK(markdown["data"]["markdown"].get<std::string>().find("2026") != std::string::npos);

        const auto validation = server->Call("validate_document", nlohmann::json{{"documentId", documentId}});
        REQUIRE(validation["ok"] == true);
        CHECK(validation["data"]["errorCount"] == 0);

        REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
        const auto report = ExyokiOffice::Tools::Run(server->Path(path));
        CHECK(report.Loaded);
        CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
    }

    static void CheckFileUtilities(TestFamily family)
    {
        auto server = MakeFamilyServer(family);
        server->Initialize();

        const auto extension = Extension(family);
        for (const auto& stem : {std::string("first"), std::string("second")})
        {
            const auto path = stem + extension;
            const auto created = server->Call("create_document", nlohmann::json{{"path", path}});
            REQUIRE(created["ok"] == true);
            const auto documentId = created["data"]["documentId"].get<std::string>();
            AddSearchableContent(*server, family, documentId, stem + " content");
            REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
            REQUIRE(server->Call("close_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
        }

        const auto mergedPath = "merged" + extension;
        const auto merged = server->Call("merge_documents", nlohmann::json{{"input_paths", {"first" + extension, "second" + extension}},
                                                                           {"output_path", mergedPath}});
        REQUIRE(merged["ok"] == true);
        REQUIRE(std::filesystem::exists(server->Path(mergedPath)));
        auto report = ExyokiOffice::Tools::Run(server->Path(mergedPath));
        std::string validationDetails;
        for (const auto& issue : report.ValidationIssues)
        {
            validationDetails += "merged id=" + std::to_string(static_cast<int>(issue.Id)) + " part=" + issue.PartUri +
                                 " message=" + issue.Message + "\n";
        }
        INFO(validationDetails);
        CHECK(report.Loaded);
        CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));

        const auto split = server->Call("split_document", nlohmann::json{{"input_path", mergedPath},
                                                                         {"output_dir", "parts"},
                                                                         {"count", 1}});
        REQUIRE(split["ok"] == true);
        REQUIRE_FALSE(split["data"]["outputFiles"].empty());
        for (const auto& file : split["data"]["outputFiles"])
        {
            const auto splitPath = file.get<std::string>();
            REQUIRE(std::filesystem::exists(server->Path(splitPath)));
            report = ExyokiOffice::Tools::Run(server->Path(splitPath));
            validationDetails.clear();
            for (const auto& issue : report.ValidationIssues)
            {
                validationDetails += "split id=" + std::to_string(static_cast<int>(issue.Id)) + " part=" + issue.PartUri +
                                     " message=" + issue.Message + "\n";
            }
            INFO(validationDetails);
            CHECK(report.Loaded);
            CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
        }

        const auto redactedPath = "redacted" + extension;
        const auto redacted = server->Call("redact_document", nlohmann::json{{"input_path", mergedPath},
                                                                             {"output_path", redactedPath}});
        REQUIRE(redacted["ok"] == true);
        REQUIRE(std::filesystem::exists(server->Path(redactedPath)));
        report = ExyokiOffice::Tools::Run(server->Path(redactedPath));
        validationDetails.clear();
        for (const auto& issue : report.ValidationIssues)
        {
            validationDetails += "redacted id=" + std::to_string(static_cast<int>(issue.Id)) + " part=" + issue.PartUri +
                                 " message=" + issue.Message + "\n";
        }
        INFO(validationDetails);
        CHECK(report.Loaded);
        CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
    }
};

TEST_CASE("shared lifecycle and readers round-trip every document family [mcp-lifecycle]")
{
    SUBCASE("Word")
    {
        SharedFamilyFixture::CheckSharedRoundTrip(SharedFamilyFixture::TestFamily::Word);
    }
    SUBCASE("Excel")
    {
        SharedFamilyFixture::CheckSharedRoundTrip(SharedFamilyFixture::TestFamily::Excel);
    }
    SUBCASE("PowerPoint")
    {
        SharedFamilyFixture::CheckSharedRoundTrip(SharedFamilyFixture::TestFamily::PowerPoint);
    }
}

TEST_CASE("merge split and redact produce valid packages for every family [mcp-lifecycle]")
{
    SUBCASE("Word")
    {
        SharedFamilyFixture::CheckFileUtilities(SharedFamilyFixture::TestFamily::Word);
    }
    SUBCASE("Excel")
    {
        SharedFamilyFixture::CheckFileUtilities(SharedFamilyFixture::TestFamily::Excel);
    }
    SUBCASE("PowerPoint")
    {
        SharedFamilyFixture::CheckFileUtilities(SharedFamilyFixture::TestFamily::PowerPoint);
    }
}

TEST_CASE("a destination naming another family is rejected before anything is written [mcp-lifecycle]")
{
    using TestFamily = SharedFamilyFixture::TestFamily;

    for (const auto family : {TestFamily::Word, TestFamily::Excel, TestFamily::PowerPoint})
    {
        auto server = SharedFamilyFixture::MakeFamilyServer(family);
        server->Initialize();

        const auto own = SharedFamilyFixture::Extension(family);
        for (const auto& foreign : {std::string(".docx"), std::string(".xlsx"), std::string(".pptx")})
        {
            if (foreign == own)
            {
                continue;
            }

            // Whatever the destination is called, this server writes its own
            // family into it; the client would only find out when something
            // else refused to open the result.
            const auto created = server->Call("create_document", nlohmann::json{{"path", "wrong" + foreign}});
            CHECK(created["ok"] == false);
            CHECK(created["error"]["code"] == "family_mismatch");
            CHECK_FALSE(created["error"]["hint"].get<std::string>().empty());
            CHECK_FALSE(std::filesystem::exists(server->Path("wrong" + foreign)));
        }

        // The rule is about the family, not about the extension being the
        // canonical one: a macro-enabled name of the same family is accepted,
        // and so is a name this server does not recognize as an Office file.
        const auto sameFamily = std::string(own == ".docx" ? ".docm" : (own == ".xlsx" ? ".xlsm" : ".pptm"));
        CHECK(server->Call("create_document", nlohmann::json{{"path", "macro" + sameFamily}})["ok"] == true);
        CHECK(server->Call("create_document", nlohmann::json{{"path", "draft.tmp"}})["ok"] == true);

        // save_document is checked too, so the mismatch cannot be introduced
        // after the session already exists.
        const auto created = server->Call("create_document", nlohmann::json::object());
        const auto documentId = created["data"]["documentId"].get<std::string>();
        const auto foreign = std::string(own == ".docx" ? ".xlsx" : ".docx");
        const auto saved =
            server->Call("save_document", nlohmann::json{{"documentId", documentId}, {"path", "saved" + foreign}});
        CHECK(saved["ok"] == false);
        CHECK(saved["error"]["code"] == "family_mismatch");
        CHECK_FALSE(std::filesystem::exists(server->Path("saved" + foreign)));
    }
}

TEST_CASE("opening a package through the wrong family is rejected [mcp-lifecycle]")
{
    auto excel = SharedFamilyFixture::MakeFamilyServer(SharedFamilyFixture::TestFamily::Excel);
    excel->Initialize();
    const auto created = excel->Call("create_document", nlohmann::json{{"path", "book.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(excel->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto word = SharedFamilyFixture::MakeFamilyServer(SharedFamilyFixture::TestFamily::Word);
    word->Initialize();
    std::filesystem::copy_file(excel->Path("book.xlsx"), word->Path("book.docx"));
    const auto refused = word->Call("open_document", nlohmann::json{{"path", "book.docx"}});
    CHECK(refused["ok"] == false);
    CHECK((refused["error"]["code"] == "family_mismatch" ||
           refused["error"]["code"] == "package_load_failed"));
}
