// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "CliTestSupport.hpp"

#include "ExyokiOffice/Tools/Report.hpp"

#include <string>

using exyoki::ExitCode;
using ExyokiOffice::Tools::RenderJson;
using ExyokiOfficeCliTests::Fixture;
using ExyokiOfficeCliTests::ParserFixture;

// The commands that only read: what each has to get right is the report it
// produces and the exit code it maps its outcome onto. A package that cannot be
// loaded is deliberately not a crash and not an empty success - every one of
// them answers with a well-formed report and ExitCode::OperationFailed, which
// is what the "operationFailed" row of the exit code table promises.

/// The path a command that cannot succeed is pointed at.
class MissingPackage
{
public:
    static std::string Path() { return "no-such-package-a4f19c.docx"; }
};

TEST_CASE("parts lists the parts of a package [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Parts.Package = Fixture::WordDocument().string();

    const auto outcome = fixture.Commands().Parts.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "parts");
    CHECK(!outcome.WroteOwnOutput);
    CHECK(RenderJson(outcome.Document).find("document.xml") != std::string::npos);
}

TEST_CASE("parts reports a package it cannot load [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Parts.Package = MissingPackage::Path();

    const auto outcome = fixture.Commands().Parts.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::OperationFailed);
    CHECK(outcome.Document.Command == "parts");
}

TEST_CASE("relationships restricts its listing to the named sources [cli] [cli-inspect]")
{
    const auto document = Fixture::WordDocument();

    ParserFixture unfiltered;
    unfiltered.Commands().Relationships.Package = document.string();
    const auto all = unfiltered.Commands().Relationships.Run(unfiltered.Context());
    REQUIRE(all.Code == ExitCode::Ok);

    ParserFixture filtered;
    filtered.Commands().Relationships.Package = document.string();
    filtered.Commands().Relationships.Parts = {"no-such-part.xml"};
    const auto none = filtered.Commands().Relationships.Run(filtered.Context());

    CHECK(none.Code == ExitCode::Ok);
    CHECK(RenderJson(none.Document).size() < RenderJson(all.Document).size());
}

TEST_CASE("relationships reports a package it cannot load [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Relationships.Package = MissingPackage::Path();

    CHECK(fixture.Commands().Relationships.Run(fixture.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("info recognizes the document family [cli] [cli-inspect]")
{
    ParserFixture word;
    word.Commands().Info.Package = Fixture::WordDocument().string();
    const auto wordOutcome = word.Commands().Info.Run(word.Context());
    CHECK(wordOutcome.Code == ExitCode::Ok);
    CHECK(wordOutcome.Document.Command == "info");

    ParserFixture workbook;
    workbook.Commands().Info.Package = Fixture::Workbook().string();
    CHECK(workbook.Commands().Info.Run(workbook.Context()).Code == ExitCode::Ok);

    ParserFixture presentation;
    presentation.Commands().Info.Package = Fixture::Presentation().string();
    CHECK(presentation.Commands().Info.Run(presentation.Context()).Code == ExitCode::Ok);

    ParserFixture missing;
    missing.Commands().Info.Package = MissingPackage::Path();
    CHECK(missing.Commands().Info.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("props set writes what props get reads back [cli] [cli-inspect]")
{
    const auto document = Fixture::WordDocument();
    const auto output = Fixture::UnusedPath(".docx");

    ParserFixture writer;
    writer.Commands().PropsSet.Package = document.string();
    writer.Commands().PropsSet.Title = "Quarterly report";
    writer.Commands().PropsSet.Creator = "A tester";
    writer.Commands().PropsSet.Custom = {"Description=Written by a test"};
    writer.Commands().PropsSet.OutPackage = output.string();

    const auto written = writer.Commands().PropsSet.Run(writer.Context());
    REQUIRE(written.Code == ExitCode::Ok);
    CHECK(written.Document.Command == "props set");

    ParserFixture reader;
    reader.Commands().PropsGet.Package = output.string();
    const auto read = reader.Commands().PropsGet.Run(reader.Context());

    REQUIRE(read.Code == ExitCode::Ok);
    CHECK(read.Document.Command == "props get");
    const auto rendered = RenderJson(read.Document);
    CHECK(rendered.find("Quarterly report") != std::string::npos);
    CHECK(rendered.find("A tester") != std::string::npos);
    CHECK(rendered.find("Written by a test") != std::string::npos);
}

TEST_CASE("props set stores an arbitrary name as a custom property [cli] [cli-inspect]")
{
    // What the option's help text promises: a name the document has no property
    // of its own for is written to docProps/custom.xml, and props get reports it
    // back with the type it was stored as.
    const auto output = Fixture::UnusedPath(".docx");

    ParserFixture writer;
    writer.Commands().PropsSet.Package = Fixture::WordDocument().string();
    writer.Commands().PropsSet.Custom = {"Department=Research", "Reviewer=A tester"};
    writer.Commands().PropsSet.OutPackage = output.string();

    REQUIRE(writer.Commands().PropsSet.Run(writer.Context()).Code == ExitCode::Ok);
    REQUIRE(std::filesystem::exists(output));

    ParserFixture reader;
    reader.Commands().PropsGet.Package = output.string();
    const auto read = reader.Commands().PropsGet.Run(reader.Context());

    REQUIRE(read.Code == ExitCode::Ok);
    const auto rendered = RenderJson(read.Document);
    CHECK(rendered.find("Department") != std::string::npos);
    CHECK(rendered.find("Research") != std::string::npos);
    CHECK(rendered.find("Reviewer") != std::string::npos);
    CHECK(rendered.find("\"string\"") != std::string::npos);
}

TEST_CASE("props set reaches the names beyond the CoreProperties fields [cli] [cli-inspect]")
{
    // The properties editor underneath covers the whole of docProps/core.xml and
    // docProps/app.xml, so a name it has a slot for must go there rather than
    // become a custom property that happens to share the name.
    const auto output = Fixture::UnusedPath(".docx");

    ParserFixture fixture;
    fixture.Commands().PropsSet.Package = Fixture::WordDocument().string();
    fixture.Commands().PropsSet.Custom = {"Manager=A manager", "hyperlinkbase=https://example.invalid/"};
    fixture.Commands().PropsSet.OutPackage = output.string();

    REQUIRE(fixture.Commands().PropsSet.Run(fixture.Context()).Code == ExitCode::Ok);

    ParserFixture reader;
    reader.Commands().PropsGet.Package = output.string();
    const auto read = reader.Commands().PropsGet.Run(reader.Context());

    REQUIRE(read.Code == ExitCode::Ok);
    // Neither is a field of CoreProperties, so neither shows up in the report -
    // but neither may have been written as a custom property either.
    CHECK(RenderJson(read.Document).find("\"Manager\"") == std::string::npos);
}

TEST_CASE("props set refuses a timestamp it cannot type [cli] [cli-inspect]")
{
    // Created and Modified are points in time, not text. Writing whatever
    // arrived would produce a date no reader can parse, so the command refuses.
    ParserFixture fixture;
    fixture.Commands().PropsSet.Package = Fixture::WordDocument().string();
    fixture.Commands().PropsSet.Custom = {"Created=yesterday"};
    fixture.Commands().PropsSet.OutPackage = Fixture::UnusedPath(".docx").string();

    const auto outcome = fixture.Commands().PropsSet.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::OperationFailed);
    CHECK(!std::filesystem::exists(fixture.Commands().PropsSet.OutPackage));
}

TEST_CASE("props set with nothing to write succeeds without saving [cli] [cli-inspect]")
{
    // An empty update list is a no-op rather than an error: there is nothing to
    // fail at, and the command has not been asked to change anything.
    ParserFixture fixture;
    fixture.Commands().PropsSet.Package = Fixture::WordDocument().string();
    fixture.Commands().PropsSet.OutPackage = Fixture::UnusedPath(".docx").string();

    const auto outcome = fixture.Commands().PropsSet.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(!std::filesystem::exists(fixture.Commands().PropsSet.OutPackage));
}

TEST_CASE("props set ignores a --set argument with no equals sign [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().PropsSet.Package = Fixture::WordDocument().string();
    fixture.Commands().PropsSet.Custom = {"NoEqualsSign"};
    fixture.Commands().PropsSet.OutPackage = Fixture::UnusedPath(".docx").string();

    const auto outcome = fixture.Commands().PropsSet.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(!std::filesystem::exists(fixture.Commands().PropsSet.OutPackage));
}

TEST_CASE("signatures treats an unsigned package as intact [cli] [cli-inspect]")
{
    // No signature at all is not a failure; only a signature whose content
    // changed is, and that is the SignatureInvalid row of the exit code table.
    ParserFixture fixture;
    fixture.Commands().Signatures.Package = Fixture::WordDocument().string();

    const auto outcome = fixture.Commands().Signatures.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "signatures");

    ParserFixture missing;
    missing.Commands().Signatures.Package = MissingPackage::Path();
    CHECK(missing.Commands().Signatures.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("external lists what the package references from outside [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().External.Package = Fixture::WordDocument().string();

    const auto outcome = fixture.Commands().External.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "external");

    ParserFixture missing;
    missing.Commands().External.Package = MissingPackage::Path();
    CHECK(missing.Commands().External.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("validate accepts a document this library wrote [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Validate.Packages = {Fixture::WordDocument().string()};

    const auto outcome = fixture.Commands().Validate.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "validate");
}

TEST_CASE("validate reports a batch when given several packages [cli] [cli-inspect]")
{
    // One file keeps the single-file report shape; several produce the batch
    // report, and the two are different enough that the choice needs covering.
    ParserFixture fixture;
    fixture.Commands().Validate.Packages = {Fixture::WordDocument().string(),
                                            Fixture::Workbook().string(),
                                            Fixture::Presentation().string()};

    const auto outcome = fixture.Commands().Validate.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "validate");
}

TEST_CASE("validate reports no input rather than succeeding at nothing [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Validate.Packages = {"no-such-directory-9f/*.docx"};

    const auto outcome = fixture.Commands().Validate.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::OperationFailed);
    CHECK(outcome.Document.Status == "error");
    CHECK(RenderJson(outcome.Document).find("No input files") != std::string::npos);
}

TEST_CASE("validate honours --max-issues and the target Office version [cli] [cli-inspect]")
{
    const auto document = Fixture::WordDocument();

    ParserFixture capped;
    capped.Commands().Validate.Packages = {document.string()};
    capped.Commands().Validate.MaxIssues = 1;
    CHECK(capped.Commands().Validate.Run(capped.Context()).Code == ExitCode::Ok);

    ParserFixture legacy;
    legacy.Commands().Validate.Packages = {document.string()};
    legacy.Commands().Validate.OfficeVersion = "2007";
    const auto outcome = legacy.Commands().Validate.Run(legacy.Context());
    CHECK((outcome.Code == ExitCode::Ok || outcome.Code == ExitCode::ValidationErrors));
}

TEST_CASE("extract-text returns the text of each family [cli] [cli-inspect]")
{
    ParserFixture word;
    word.Commands().ExtractText.Package = Fixture::WordDocument("Alpha").string();
    const auto outcome = word.Commands().ExtractText.Run(word.Context());
    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "extract-text");
    CHECK(RenderJson(outcome.Document).find("Alpha") != std::string::npos);

    ParserFixture workbook;
    workbook.Commands().ExtractText.Package = Fixture::Workbook().string();
    CHECK(workbook.Commands().ExtractText.Run(workbook.Context()).Code == ExitCode::Ok);

    ParserFixture presentation;
    presentation.Commands().ExtractText.Package = Fixture::Presentation().string();
    CHECK(presentation.Commands().ExtractText.Run(presentation.Context()).Code == ExitCode::Ok);

    ParserFixture missing;
    missing.Commands().ExtractText.Package = MissingPackage::Path();
    CHECK(missing.Commands().ExtractText.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("stat counts the content of a document [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Stat.Package = Fixture::WordDocument().string();

    const auto outcome = fixture.Commands().Stat.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "stat");

    ParserFixture missing;
    missing.Commands().Stat.Package = MissingPackage::Path();
    CHECK(missing.Commands().Stat.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("search separates no match from failure [cli] [cli-inspect]")
{
    // Matching nothing is the grep convention: a distinct exit code, not an
    // error, so a shell can tell "searched and found nothing" from "could not
    // search". Anything else would make `exyoki search` unusable in a script.
    const auto document = Fixture::WordDocument("Alpha");

    ParserFixture hit;
    hit.Commands().Search.Package = document.string();
    hit.Commands().Search.Needle = "Alpha";
    const auto found = hit.Commands().Search.Run(hit.Context());
    CHECK(found.Code == ExitCode::Ok);
    CHECK(found.Document.Command == "search");

    ParserFixture miss;
    miss.Commands().Search.Package = document.string();
    miss.Commands().Search.Needle = "Zzzzznotpresent";
    CHECK(miss.Commands().Search.Run(miss.Context()).Code == ExitCode::SearchNoMatch);

    ParserFixture missing;
    missing.Commands().Search.Package = MissingPackage::Path();
    missing.Commands().Search.Needle = "Alpha";
    CHECK(missing.Commands().Search.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("search honours --regex and --ignore-case [cli] [cli-inspect]")
{
    const auto document = Fixture::WordDocument("Alpha");

    ParserFixture plain;
    plain.Commands().Search.Package = document.string();
    plain.Commands().Search.Needle = "ALPHA";
    CHECK(plain.Commands().Search.Run(plain.Context()).Code == ExitCode::SearchNoMatch);

    ParserFixture insensitive;
    insensitive.Commands().Search.Package = document.string();
    insensitive.Commands().Search.Needle = "ALPHA";
    insensitive.Commands().Search.IgnoreCase = true;
    CHECK(insensitive.Commands().Search.Run(insensitive.Context()).Code == ExitCode::Ok);

    ParserFixture regex;
    regex.Commands().Search.Package = document.string();
    regex.Commands().Search.Needle = "Al[a-z]+a";
    regex.Commands().Search.Regex = true;
    CHECK(regex.Commands().Search.Run(regex.Context()).Code == ExitCode::Ok);
}

TEST_CASE("query separates no match from failure [cli] [cli-inspect]")
{
    const auto document = Fixture::WordDocument();

    ParserFixture hit;
    hit.Commands().Query.Package = document.string();
    hit.Commands().Query.Xpath = "//w:p";
    const auto found = hit.Commands().Query.Run(hit.Context());
    CHECK(found.Code == ExitCode::Ok);
    CHECK(found.Document.Command == "query");

    ParserFixture miss;
    miss.Commands().Query.Package = document.string();
    miss.Commands().Query.Xpath = "//w:noSuchElement";
    CHECK(miss.Commands().Query.Run(miss.Context()).Code == ExitCode::QueryNoMatch);

    ParserFixture missing;
    missing.Commands().Query.Package = MissingPackage::Path();
    missing.Commands().Query.Xpath = "//w:p";
    CHECK(missing.Commands().Query.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("query rejects a namespace binding that is not prefix=uri [cli] [cli-inspect]")
{
    // A malformed binding is the caller's mistake, so it is a usage error even
    // though it is only noticed once the command runs.
    ParserFixture fixture;
    fixture.Commands().Query.Package = Fixture::WordDocument().string();
    fixture.Commands().Query.Xpath = "//w:p";
    fixture.Commands().Query.Namespaces = {"missing-the-equals-sign"};

    const auto outcome = fixture.Commands().Query.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::UsageError);
    CHECK(RenderJson(outcome.Document).find("prefix=uri") != std::string::npos);
}

TEST_CASE("query resolves a prefix bound with --ns [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Query.Package = Fixture::WordDocument().string();
    fixture.Commands().Query.Xpath = "//x:p";
    fixture.Commands().Query.Namespaces = {
        "x=http://schemas.openxmlformats.org/wordprocessingml/2006/main"};

    CHECK(fixture.Commands().Query.Run(fixture.Context()).Code == ExitCode::Ok);
}

TEST_CASE("diff separates identical from different [cli] [cli-inspect]")
{
    const auto left = Fixture::WordDocument("Alpha");
    const auto right = Fixture::WordDocument("Omega");

    ParserFixture same;
    same.Commands().Diff.Left = left.string();
    same.Commands().Diff.Right = left.string();
    const auto identical = same.Commands().Diff.Run(same.Context());
    CHECK(identical.Code == ExitCode::Ok);
    CHECK(identical.Document.Command == "diff");

    ParserFixture different;
    different.Commands().Diff.Left = left.string();
    different.Commands().Diff.Right = right.string();
    CHECK(different.Commands().Diff.Run(different.Context()).Code == ExitCode::DiffDifferent);

    ParserFixture missing;
    missing.Commands().Diff.Left = MissingPackage::Path();
    missing.Commands().Diff.Right = left.string();
    CHECK(missing.Commands().Diff.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("schema prints the model schema as the payload [cli] [cli-inspect]")
{
    // The schema is not a report envelope, so --format does not apply to it and
    // the command writes it itself; the caller must not then emit a document.
    ParserFixture fixture;
    const ExyokiOfficeCliTests::CapturedOutput captured;

    const auto outcome = fixture.Commands().Schema.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.WroteOwnOutput);
    CHECK(captured.Out().find("$schema") != std::string::npos);
}

TEST_CASE("schema --check reports a document that is not the model [cli] [cli-inspect]")
{
    const auto notAnEnvelope = Fixture::TextFile("{\"unrelated\": true}", ".json");

    ParserFixture fixture;
    fixture.Commands().Schema.Check = notAnEnvelope.string();
    const auto outcome = fixture.Commands().Schema.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::ValidationErrors);
    CHECK(outcome.Document.Command == "schema");
    CHECK(!outcome.WroteOwnOutput);
}

TEST_CASE("schema --check reports a file it cannot read [cli] [cli-inspect]")
{
    ParserFixture fixture;
    fixture.Commands().Schema.Check = "no-such-envelope-7d1.json";

    const ExyokiOfficeCliTests::CapturedOutput captured;
    const auto outcome = fixture.Commands().Schema.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::OperationFailed);
    CHECK(outcome.WroteOwnOutput);
    CHECK(captured.Err().find("cannot read") != std::string::npos);
}

TEST_CASE("completions writes a script for every supported shell [cli] [cli-inspect]")
{
    for (const auto* shell : {"bash", "zsh", "powershell", "pwsh"})
    {
        CAPTURE(shell);
        ParserFixture fixture;
        fixture.Commands().Completions.Shell = shell;

        const ExyokiOfficeCliTests::CapturedOutput captured;
        const auto outcome = fixture.Commands().Completions.Run(fixture.Context());

        CHECK(outcome.Code == ExitCode::Ok);
        CHECK(outcome.WroteOwnOutput);
        CHECK(captured.Out().find("exyoki") != std::string::npos);
    }
}

TEST_CASE("completions rejects a shell it cannot generate for [cli] [cli-inspect]")
{
    // The parser already restricts the value; this is the command's own guard,
    // which is what keeps the two from drifting apart unnoticed.
    ParserFixture fixture;
    fixture.Commands().Completions.Shell = "fish";

    const ExyokiOfficeCliTests::CapturedOutput captured;
    const auto outcome = fixture.Commands().Completions.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::UsageError);
    CHECK(captured.Err().find("unsupported shell") != std::string::npos);
}

TEST_CASE("commands describes the interface it is part of [cli] [cli-inspect]")
{
    ParserFixture fixture;

    const auto outcome = fixture.Commands().Commands.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Status == "ok");
    const auto rendered = RenderJson(outcome.Document);
    CHECK(rendered.find("extract-text") != std::string::npos);
    CHECK(rendered.find("signatureInvalid") != std::string::npos);
}
