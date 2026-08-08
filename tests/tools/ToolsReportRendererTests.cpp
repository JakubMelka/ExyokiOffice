// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/Report.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

using namespace ExyokiOffice::Tools;

namespace
{

ReportDocument MakeSampleDocument()
{
    ReportDocument document;
    document.Command = "parts";
    document.Status = "ok";
    document.Data.Set("partCount", static_cast<ExyokiOffice::UInt64>(2));

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"uri", "size"});
    auto row1 = ReportNode::MakeObject();
    row1.Set("uri", std::string("/word/document.xml"));
    row1.Set("size", static_cast<ExyokiOffice::UInt64>(123));
    array.Push(std::move(row1));
    auto row2 = ReportNode::MakeObject();
    row2.Set("uri", std::string("a & b <tag>"));
    row2.Set("size", static_cast<ExyokiOffice::UInt64>(456));
    array.Push(std::move(row2));
    document.Data.Set("parts", std::move(array));

    document.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, "Something to note", "context"});
    return document;
}

} // namespace

TEST_CASE("RenderPlain includes command, status, data, and diagnostics [unit] [tools]")
{
    const auto document = MakeSampleDocument();
    const auto rendered = RenderPlain(document);
    CHECK(rendered.find("command: parts") != std::string::npos);
    CHECK(rendered.find("status: ok") != std::string::npos);
    CHECK(rendered.find("uri=/word/document.xml") != std::string::npos);
    CHECK(rendered.find("[warning] Something to note") != std::string::npos);
}

TEST_CASE("RenderMarkdown renders a table for array-of-object nodes with a table hint [unit] [tools]")
{
    const auto document = MakeSampleDocument();
    const auto rendered = RenderMarkdown(document);
    CHECK(rendered.find("| uri | size |") != std::string::npos);
    CHECK(rendered.find("| --- | --- |") != std::string::npos);
    CHECK(rendered.find("/word/document.xml") != std::string::npos);
}

TEST_CASE("RenderXml escapes reserved characters and produces well-formed nesting [unit] [tools]")
{
    const auto document = MakeSampleDocument();
    const auto rendered = RenderXml(document);
    CHECK(rendered.find("<exyoki>") != std::string::npos);
    CHECK(rendered.find("<command>parts</command>") != std::string::npos);
    CHECK(rendered.find("a &amp; b &lt;tag&gt;") != std::string::npos);
    CHECK(rendered.find("</exyoki>") != std::string::npos);
}

TEST_CASE("RenderJson preserves report value types and diagnostics [unit] [tools]")
{
    const auto document = MakeSampleDocument();
    const auto rendered = RenderJson(document);
    const auto json = nlohmann::json::parse(rendered);
    CHECK(json["tool"] == "exyoki");
    CHECK(json["command"] == "parts");
    CHECK(json["data"]["partCount"] == 2);
    CHECK(json["data"]["parts"][0]["size"] == 123);
    CHECK(json["diagnostics"][0]["severity"] == "warning");
}

TEST_CASE("ReportNode Push/Set build arrays and objects incrementally [unit] [tools]")
{
    auto node = ReportNode::MakeObject();
    node.Set("a", 1);
    node.Set("b", std::string("text"));
    node.Set("a", 2); // overwrite
    CHECK(node.AsObject().size() == 2);
    CHECK(node.AsObject()[0].second.AsInt() == 2);

    auto array = ReportNode::MakeArray();
    array.Push(ReportNode(1)).Push(ReportNode(2));
    CHECK(array.AsArray().size() == 2);
}
