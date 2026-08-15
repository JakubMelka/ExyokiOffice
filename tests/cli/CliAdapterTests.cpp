// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "Adapters.hpp"
#include "CommandCatalog.hpp"
#include "Completions.hpp"

#include "ExyokiOffice/Tools/Report.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace ExyokiOffice::Tools;

class ReportTestHelper
{
public:
    static const ReportNode& Member(const ReportNode& object, std::string_view name)
    {
        const auto& members = object.AsObject();
        const auto found = std::find_if(members.begin(), members.end(), [name](const auto& member)
                                        { return member.first == name; });
        REQUIRE(found != members.end());
        return found->second;
    }
};

TEST_CASE("Inspection adapters preserve sorting, dangling edges and issue details [cli] [cli-report]")
{
    PartRecord small;
    small.Uri = "/z-small.xml";
    small.ContentType = "z/type";
    small.Size = 2;
    PartRecord large;
    large.Uri = "/a-large.bin";
    large.ContentType = "a/type";
    large.Kind = PartPayloadKind::Binary;
    large.Size = 20;

    const auto bySize = RenderJson(exyoki::AdaptParts({small, large}, "size"));
    CHECK(bySize.find("/a-large.bin") < bySize.find("/z-small.xml"));
    const auto byType = RenderJson(exyoki::AdaptParts({small, large}, "type"));
    CHECK(byType.find("/a-large.bin") < byType.find("/z-small.xml"));

    RelationshipRecord live;
    live.SourceUri = "/word/document.xml";
    live.Relationship.Id = "rId1";
    live.Relationship.Target = "styles.xml";
    live.ResolvedTargetUri = "/word/styles.xml";
    live.TargetExists = true;
    RelationshipRecord dangling = live;
    dangling.Relationship.Id = "rId2";
    dangling.Relationship.Target = "missing.xml";
    dangling.TargetExists = false;
    RelationshipRecord externalRelationship = dangling;
    externalRelationship.Relationship.Id = "rId3";
    externalRelationship.Relationship.IsExternal = true;

    const auto relationshipReport = exyoki::AdaptRelationships({live, dangling, externalRelationship}, true);
    const auto relationships = RenderJson(relationshipReport);
    CHECK(ReportTestHelper::Member(relationshipReport.Data, "danglingCount").AsUInt() == 1);
    CHECK(relationships.find("rId2") != std::string::npos);
    CHECK(relationships.find("rId1") == std::string::npos);
    CHECK(relationships.find("rId3") == std::string::npos);

    ExyokiOffice::ValidationIssue warning;
    warning.Severity = ExyokiOffice::ValidationSeverity::Warning;
    warning.Message = "warning";
    warning.PartUri = "/word/document.xml";
    warning.Location = {"/w:document/w:body", "w:body", "w:val"};
    warning.ConstraintId = "rule-1";
    warning.RelationshipId = "rId9";
    warning.RelationshipSourceUri = "/word/document.xml";
    warning.RelationshipType = "type";
    warning.TargetUri = "/word/missing.xml";
    auto error = warning;
    error.Severity = ExyokiOffice::ValidationSeverity::Error;
    error.Message = "error";

    ValidationReport validation;
    validation.Loaded = true;
    validation.LoadIssues = {warning};
    validation.ValidationIssues = {error};

    const auto promotedReport = exyoki::AdaptValidate(validation, false, true, 1);
    const auto promoted = RenderJson(promotedReport);
    CHECK(ReportTestHelper::Member(promotedReport.Data, "errorCount").AsUInt() == 2);
    CHECK(ReportTestHelper::Member(promotedReport.Data, "warningCount").AsUInt() == 0);
    CHECK(ReportTestHelper::Member(promotedReport.Data, "issues").AsArray().size() == 1);
    CHECK(promoted.find("\"xmlPath\": \"/w:document/w:body\"") != std::string::npos);
    CHECK(promoted.find("\"attributeName\": \"w:val\"") != std::string::npos);
    CHECK(promoted.find("\"constraintId\": \"rule-1\"") != std::string::npos);
    CHECK(promoted.find("\"relationshipId\": \"rId9\"") != std::string::npos);
    CHECK(promoted.find("\"message\": \"error\"") == std::string::npos);

    const auto errorsOnlyReport = exyoki::AdaptValidate(validation, true, false, std::nullopt);
    const auto errorsOnly = RenderJson(errorsOnlyReport);
    CHECK(ReportTestHelper::Member(errorsOnlyReport.Data, "errorCount").AsUInt() == 1);
    CHECK(ReportTestHelper::Member(errorsOnlyReport.Data, "warningCount").AsUInt() == 1);
    CHECK(errorsOnly.find("\"message\": \"warning\"") == std::string::npos);
    CHECK(errorsOnly.find("\"message\": \"error\"") != std::string::npos);

    PackageInfo strict;
    strict.IsStrictConformance = true;
    const auto strictReport = exyoki::AdaptInfo(strict, false);
    CHECK(strictReport.Diagnostics.size() == 1);

    ExternalResourceReport external;
    external.Loaded = true;
    external.References.push_back(
        {"/word/document.xml", "rId8", "hyperlink", "https://example.invalid", "hyperlink"});
    const auto externalJson = RenderJson(exyoki::AdaptExternal(external));
    CHECK(externalJson.find("https://example.invalid") != std::string::npos);

    ValidationReport unloaded;
    const auto batch = exyoki::AdaptValidateBatch({{"missing.docx", unloaded}}, false, false, std::nullopt);
    CHECK(batch.Status == "error");
    CHECK(ReportTestHelper::Member(batch.Data, "filesFailedToLoad").AsUInt() == 1);

    PackResult packed;
    packed.Validation.ValidationIssues.push_back(error);
    CHECK(RenderJson(exyoki::AdaptPack(packed)).find("validationErrorCount") != std::string::npos);
}

TEST_CASE("Property and statistics adapters retain every scalar type [cli] [cli-report]")
{
    using ExyokiOffice::Packaging::DocumentCustomProperty;

    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::seconds{123}};
    const std::vector<DocumentCustomProperty> properties = {
        {"Flag", true},
        {"Count", ExyokiOffice::Int32{7}},
        {"Ratio", ExyokiOffice::Real{2.5}},
        {"Label", std::string("value")},
        {"When", timestamp},
    };
    const auto propertyJson = RenderJson(exyoki::AdaptPropsGet({}, properties));
    for (const auto* type : {"bool", "int", "double", "string", "dateTime"})
    {
        CHECK(propertyJson.find(std::string("\"type\": \"") + type + "\"") != std::string::npos);
    }

    DocumentStats seconds;
    seconds.Ok = true;
    seconds.WordCount = 3;
    seconds.ReadingTimeMinutes = -0.5;
    const auto secondsJson = RenderJson(exyoki::AdaptStat(seconds));
    CHECK(secondsJson.find("\"readingTimeText\": \"0 sec\"") != std::string::npos);

    DocumentStats minutes;
    minutes.Ok = true;
    minutes.ReadingTimeMinutes = 1.6;
    CHECK(RenderJson(exyoki::AdaptStat(minutes)).find("\"readingTimeText\": \"2 min\"") !=
          std::string::npos);
}

TEST_CASE("Workflow adapters serialize result collections and every diff kind [cli] [cli-report]")
{
    WordSplitResult split;
    split.Ok = true;
    split.OutputFiles = {"part-1.docx", "part-2.docx"};
    CHECK(RenderJson(exyoki::AdaptSplit(split)).find("\"splitCount\": 2") != std::string::npos);

    WordMergeResult merge;
    merge.Ok = true;
    merge.DocumentsMerged = 2;
    merge.OutputFile = "merged.docx";
    CHECK(RenderJson(exyoki::AdaptMerge(merge)).find("merged.docx") != std::string::npos);

    DiffResult diff;
    diff.Ok = true;
    diff.PartChanges = {
        {"/added", PartChangeKind::Added},
        {"/removed", PartChangeKind::Removed},
        {"/xml", PartChangeKind::ChangedXml},
        {"/binary", PartChangeKind::ChangedBinary},
        {"/type", PartChangeKind::ContentTypeChanged},
    };
    for (const auto kind : {RelationshipChangeKind::Added, RelationshipChangeKind::Removed,
                            RelationshipChangeKind::Changed})
    {
        RelationshipDiffEntry change;
        change.SourceUri = "/";
        change.RelationshipId = std::to_string(diff.RelationshipChanges.size());
        change.Kind = kind;
        change.Left.Target = "left";
        change.Right.Target = "right";
        diff.RelationshipChanges.push_back(std::move(change));
    }
    const auto diffJson = RenderJson(exyoki::AdaptDiff(diff, false));
    for (const auto* kind : {"added", "removed", "changedXml", "changedBinary",
                             "contentTypeChanged", "changed"})
    {
        CHECK(diffJson.find(std::string("\"kind\": \"") + kind + "\"") != std::string::npos);
    }
    CHECK(diffJson.find("\"relationshipChangeCount\": 3") != std::string::npos);

    QueryResult query;
    query.Ok = true;
    query.PartName = "/word/document.xml";
    query.Matches.push_back({"/w:document/w:p", "w:p", {{"w:id", "7"}, {"xml:lang", "en"}}, "Text"});
    const auto queryJson = RenderJson(exyoki::AdaptQuery(query));
    CHECK(queryJson.find("w:id=7; xml:lang=en") != std::string::npos);

    ResourceDeduplicationResult dedup;
    dedup.Ok = true;
    dedup.Groups.push_back({"image/png", "/media/image1.png", {"/media/image2.png"}, 16});
    dedup.RemovedParts = 1;
    dedup.BytesSaved = 16;
    const auto dedupJson = RenderJson(exyoki::AdaptDedup(dedup, true));
    CHECK(dedupJson.find("/media/image2.png") != std::string::npos);
    CHECK(dedupJson.find("\"bytesSaved\": 16") != std::string::npos);

    WorkbookRecalcResult recalc;
    recalc.Ok = true;
    recalc.CircularReferenceCycles = {"Sheet1!A1 -> Sheet1!A1"};
    CHECK(RenderJson(exyoki::AdaptRecalc(recalc)).find("Sheet1!A1 -> Sheet1!A1") != std::string::npos);

    ConvertResult conversion;
    conversion.Ok = true;
    conversion.SlideCount = 2;
    conversion.MediaItems.push_back({"/ppt/media/image1.png", "image1.png", "image/png", 32});
    const auto conversionJson = RenderJson(exyoki::AdaptConvert(conversion, "slides.pptx", "slides.md"));
    CHECK(conversionJson.find("\"slideCount\": 2") != std::string::npos);
    CHECK(conversionJson.find("/ppt/media/image1.png") != std::string::npos);
}

TEST_CASE("Catalog and completion self-description handles adversarial parser metadata [cli] [cli-report]")
{
    CLI::App app{"root"};
    std::string globalValue;
    app.add_option("--global", globalValue);

    auto* command = app.add_subcommand("probe", "quotes: ' \" ` \\ and\na newline");
    command->fallthrough();
    std::string value;
    command->add_option("--global", value);
    command->add_option("--hidden", value)->group("");
    command->add_option("--empty", value)->type_name("text:");
    command->add_option("--front", value)->type_name("text:x}");
    command->add_option("--back", value)->type_name("text:{x");
    command->add_option("--choices", value)->type_name("text:{a,b}");

    const auto catalog = exyoki::AdaptCommands(app);
    CHECK(catalog.Status == "error");
    CHECK(catalog.Diagnostics.size() > 1);

    const auto completion = exyoki::GenerateCompletionScript(app, "zsh");
    CHECK(completion.find("quotes-") != std::string::npos);
    CHECK(completion.find("anda newline") != std::string::npos);
}
