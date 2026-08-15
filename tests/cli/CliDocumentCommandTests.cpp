// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "CliTestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Tools/Report.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using exyoki::ExitCode;
using ExyokiOffice::Tools::RenderJson;
using ExyokiOfficeCliTests::Fixture;
using ExyokiOfficeCliTests::ParserFixture;

// The commands that write something. Each is checked for the artefact it
// promises as well as for its exit code: a command that reports success without
// having written the file is the failure worth catching here.

/// Counts the entries a command produced under a directory.
class ProducedFiles
{
public:
    static ExyokiOffice::Size Count(const std::filesystem::path& directory)
    {
        ExyokiOffice::Size count = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (entry.is_regular_file())
            {
                ++count;
            }
        }
        return count;
    }
};

TEST_CASE("unpack extracts a package and refuses a non-empty directory [cli] [cli-document]")
{
    const auto document = Fixture::WordDocument();
    const auto directory = Fixture::EmptyDirectory();

    ParserFixture first;
    first.Commands().Unpack.Package = document.string();
    first.Commands().Unpack.OutDir = directory.string();
    const auto unpacked = first.Commands().Unpack.Run(first.Context());

    REQUIRE(unpacked.Code == ExitCode::Ok);
    CHECK(unpacked.Document.Command == "unpack");
    CHECK(ProducedFiles::Count(directory) > 0);
    CHECK(std::filesystem::exists(directory / "[Content_Types].xml"));

    // Writing into a directory that already holds something needs saying so.
    ParserFixture again;
    again.Commands().Unpack.Package = document.string();
    again.Commands().Unpack.OutDir = directory.string();
    CHECK(again.Commands().Unpack.Run(again.Context()).Code == ExitCode::OperationFailed);

    ParserFixture overwrite;
    overwrite.Commands().Unpack.Package = document.string();
    overwrite.Commands().Unpack.OutDir = directory.string();
    overwrite.Commands().Unpack.Overwrite = true;
    CHECK(overwrite.Commands().Unpack.Run(overwrite.Context()).Code == ExitCode::Ok);
}

TEST_CASE("pack rebuilds a package an unpack produced [cli] [cli-document]")
{
    const auto document = Fixture::WordDocument();
    const auto directory = Fixture::EmptyDirectory();

    ParserFixture unpack;
    unpack.Commands().Unpack.Package = document.string();
    unpack.Commands().Unpack.OutDir = directory.string();
    REQUIRE(unpack.Commands().Unpack.Run(unpack.Context()).Code == ExitCode::Ok);

    const auto repacked = Fixture::UnusedPath(".docx");
    ParserFixture pack;
    pack.Commands().Pack.InDir = directory.string();
    pack.Commands().Pack.OutPackage = repacked.string();
    pack.Commands().Pack.Validate = true;
    const auto packed = pack.Commands().Pack.Run(pack.Context());

    REQUIRE(packed.Code == ExitCode::Ok);
    CHECK(packed.Document.Command == "pack");
    REQUIRE(std::filesystem::exists(repacked));

    // The rebuilt package has to still be the same document.
    ParserFixture diff;
    diff.Commands().Diff.Left = document.string();
    diff.Commands().Diff.Right = repacked.string();
    CHECK(diff.Commands().Diff.Run(diff.Context()).Code == ExitCode::Ok);
}

TEST_CASE("pack refuses to overwrite unless told to [cli] [cli-document]")
{
    const auto directory = Fixture::EmptyDirectory();
    ParserFixture unpack;
    unpack.Commands().Unpack.Package = Fixture::WordDocument().string();
    unpack.Commands().Unpack.OutDir = directory.string();
    REQUIRE(unpack.Commands().Unpack.Run(unpack.Context()).Code == ExitCode::Ok);

    const auto output = Fixture::UnusedPath(".docx");

    ParserFixture first;
    first.Commands().Pack.InDir = directory.string();
    first.Commands().Pack.OutPackage = output.string();
    REQUIRE(first.Commands().Pack.Run(first.Context()).Code == ExitCode::Ok);

    ParserFixture again;
    again.Commands().Pack.InDir = directory.string();
    again.Commands().Pack.OutPackage = output.string();
    CHECK(again.Commands().Pack.Run(again.Context()).Code == ExitCode::OperationFailed);

    ParserFixture overwrite;
    overwrite.Commands().Pack.InDir = directory.string();
    overwrite.Commands().Pack.OutPackage = output.string();
    overwrite.Commands().Pack.Overwrite = true;
    CHECK(overwrite.Commands().Pack.Run(overwrite.Context()).Code == ExitCode::Ok);
}

TEST_CASE("a package survives the Flat OPC round trip [cli] [cli-document]")
{
    const auto document = Fixture::WordDocument();
    const auto flat = Fixture::UnusedPath(".xml");

    ParserFixture toFlat;
    toFlat.Commands().ToFlatOpc.Package = document.string();
    toFlat.Commands().ToFlatOpc.OutFile = flat.string();
    const auto converted = toFlat.Commands().ToFlatOpc.Run(toFlat.Context());

    REQUIRE(converted.Code == ExitCode::Ok);
    CHECK(converted.Document.Command == "to-flat-opc");
    REQUIRE(std::filesystem::exists(flat));

    const auto rebuilt = Fixture::UnusedPath(".docx");
    ParserFixture fromFlat;
    fromFlat.Commands().FromFlatOpc.FlatOpc = flat.string();
    fromFlat.Commands().FromFlatOpc.OutPackage = rebuilt.string();
    const auto restored = fromFlat.Commands().FromFlatOpc.Run(fromFlat.Context());

    REQUIRE(restored.Code == ExitCode::Ok);
    CHECK(restored.Document.Command == "from-flat-opc");

    ParserFixture diff;
    diff.Commands().Diff.Left = document.string();
    diff.Commands().Diff.Right = rebuilt.string();
    CHECK(diff.Commands().Diff.Run(diff.Context()).Code == ExitCode::Ok);
}

TEST_CASE("to-flat-opc reports a package it cannot load [cli] [cli-document]")
{
    ParserFixture fixture;
    fixture.Commands().ToFlatOpc.Package = "no-such-package-b21.docx";
    fixture.Commands().ToFlatOpc.OutFile = Fixture::UnusedPath(".xml").string();

    CHECK(fixture.Commands().ToFlatOpc.Run(fixture.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("convert writes the format the extension names [cli] [cli-document]")
{
    const auto document = Fixture::WordDocument("Alpha");

    const auto markdown = Fixture::UnusedPath(".md");
    ParserFixture toMarkdown;
    toMarkdown.Commands().Convert.Input = document.string();
    toMarkdown.Commands().Convert.Output = markdown.string();
    const auto outcome = toMarkdown.Commands().Convert.Run(toMarkdown.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "convert");
    REQUIRE(std::filesystem::exists(markdown));
    CHECK(Fixture::ReadText(markdown).find("Alpha") != std::string::npos);
}

TEST_CASE("convert to '-' writes the payload rather than a report [cli] [cli-document]")
{
    ParserFixture fixture;
    fixture.Commands().Convert.Input = Fixture::WordDocument("Alpha").string();
    fixture.Commands().Convert.Output = "-";
    fixture.Commands().Convert.To = "md";

    const ExyokiOfficeCliTests::CapturedOutput captured;
    const auto outcome = fixture.Commands().Convert.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.WroteOwnOutput);
    CHECK(captured.Out().find("Alpha") != std::string::npos);
}

TEST_CASE("convert to '-' still reports a failed conversion [cli] [cli-document]")
{
    ParserFixture fixture;
    fixture.Commands().Convert.Input = "no-such-input.docx";
    fixture.Commands().Convert.Output = "-";
    fixture.Commands().Convert.To = "md";

    const ExyokiOfficeCliTests::CapturedOutput captured;
    const auto outcome = fixture.Commands().Convert.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::OperationFailed);
    CHECK_FALSE(outcome.WroteOwnOutput);
    CHECK(captured.Out().empty());
}

TEST_CASE("convert rejects a destination format it does not know [cli] [cli-document]")
{
    // Nothing can be inferred from an unknown extension, and no --to was given,
    // so this is the caller's mistake rather than a failed conversion.
    ParserFixture fixture;
    fixture.Commands().Convert.Input = Fixture::WordDocument().string();
    fixture.Commands().Convert.Output = Fixture::UnusedPath(".zzz").string();

    CHECK(fixture.Commands().Convert.Run(fixture.Context()).Code == ExitCode::UsageError);
}

TEST_CASE("convert exports a workbook as CSV [cli] [cli-document]")
{
    const auto csv = Fixture::UnusedPath(".csv");

    ParserFixture fixture;
    fixture.Commands().Convert.Input = Fixture::Workbook().string();
    fixture.Commands().Convert.Output = csv.string();
    fixture.Commands().Convert.CsvSeparator = ";";

    REQUIRE(fixture.Commands().Convert.Run(fixture.Context()).Code == ExitCode::Ok);
    REQUIRE(std::filesystem::exists(csv));
    CHECK(Fixture::ReadText(csv).find(';') != std::string::npos);
}

TEST_CASE("convert honours an explicit media directory [cli] [cli-document]")
{
    const auto markdown = Fixture::UnusedPath(".md");
    const auto media = Fixture::EmptyDirectory();

    ParserFixture fixture;
    fixture.Commands().Convert.Input = Fixture::WordDocumentWithImage().string();
    fixture.Commands().Convert.Output = markdown.string();
    fixture.Commands().Convert.To = "md";
    fixture.Commands().Convert.MediaDir = media.string();

    REQUIRE(fixture.Commands().Convert.Run(fixture.Context()).Code == ExitCode::Ok);
    CHECK(std::filesystem::exists(markdown));
    CHECK(ProducedFiles::Count(media) == 1);
}

TEST_CASE("export-media preserves existing files unless --overwrite is set [cli] [cli-document]")
{
    const auto directory = Fixture::EmptyDirectory();

    ParserFixture fixture;
    fixture.Commands().ExportMedia.Package = Fixture::WordDocumentWithImage().string();
    fixture.Commands().ExportMedia.OutDir = directory.string();
    const auto outcome = fixture.Commands().ExportMedia.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "export-media");
    CHECK(ProducedFiles::Count(directory) == 1);
    const auto exported = *std::filesystem::directory_iterator(directory);
    const auto original = Fixture::ReadText(exported.path());
    {
        std::ofstream changed(exported.path(), std::ios::binary | std::ios::trunc);
        changed << "changed";
    }

    ParserFixture again;
    again.Commands().ExportMedia.Package = fixture.Commands().ExportMedia.Package;
    again.Commands().ExportMedia.OutDir = directory.string();
    CHECK(again.Commands().ExportMedia.Run(again.Context()).Code == ExitCode::Ok);
    CHECK(Fixture::ReadText(exported.path()) == "changed");

    ParserFixture overwrite;
    overwrite.Commands().ExportMedia.Package = fixture.Commands().ExportMedia.Package;
    overwrite.Commands().ExportMedia.OutDir = directory.string();
    overwrite.Commands().ExportMedia.Overwrite = true;
    CHECK(overwrite.Commands().ExportMedia.Run(overwrite.Context()).Code == ExitCode::Ok);
    CHECK(Fixture::ReadText(exported.path()) == original);

    ParserFixture missing;
    missing.Commands().ExportMedia.Package = "no-such-presentation-c73.pptx";
    missing.Commands().ExportMedia.OutDir = directory.string();
    CHECK(missing.Commands().ExportMedia.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("dedup leaves the package alone on a dry run [cli] [cli-document]")
{
    const auto presentation = Fixture::Presentation();
    const auto before = Fixture::ReadText(presentation);

    ParserFixture fixture;
    fixture.Commands().Dedup.Package = presentation.string();
    fixture.Commands().Dedup.DryRun = true;
    const auto outcome = fixture.Commands().Dedup.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "dedup");
    CHECK(Fixture::ReadText(presentation) == before);
}

TEST_CASE("replace counts matches without touching the document on a dry run [cli] [cli-document]")
{
    const auto document = Fixture::WordDocument("Alpha");
    const auto before = Fixture::ReadText(document);

    ParserFixture dry;
    dry.Commands().Replace.Package = document.string();
    dry.Commands().Replace.Needle = "Alpha";
    dry.Commands().Replace.Replacement = "Omega";
    dry.Commands().Replace.DryRun = true;
    const auto outcome = dry.Commands().Replace.Run(dry.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "replace");
    CHECK(Fixture::ReadText(document) == before);

    // The same replacement into a copy must actually change the text.
    const auto output = Fixture::UnusedPath(".docx");
    ParserFixture write;
    write.Commands().Replace.Package = document.string();
    write.Commands().Replace.Needle = "Alpha";
    write.Commands().Replace.Replacement = "Omega";
    write.Commands().Replace.OutPackage = output.string();
    REQUIRE(write.Commands().Replace.Run(write.Context()).Code == ExitCode::Ok);
    REQUIRE(std::filesystem::exists(output));

    ParserFixture search;
    search.Commands().Search.Package = output.string();
    search.Commands().Search.Needle = "Omega";
    CHECK(search.Commands().Search.Run(search.Context()).Code == ExitCode::Ok);

    ParserFixture gone;
    gone.Commands().Search.Package = output.string();
    gone.Commands().Search.Needle = "Alpha";
    CHECK(gone.Commands().Search.Run(gone.Context()).Code == ExitCode::SearchNoMatch);
}

TEST_CASE("split produces one document per unit [cli] [cli-document]")
{
    const auto directory = Fixture::EmptyDirectory();

    ParserFixture fixture;
    fixture.Commands().Split.Package = Fixture::WordDocument().string();
    fixture.Commands().Split.OutDir = directory.string();
    fixture.Commands().Split.By = "paragraphs";
    fixture.Commands().Split.Count = 2;
    const auto outcome = fixture.Commands().Split.Run(fixture.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "split");
    CHECK(ProducedFiles::Count(directory) > 1);
}

TEST_CASE("split by slides splits a presentation [cli] [cli-document]")
{
    const auto directory = Fixture::EmptyDirectory();

    ParserFixture fixture;
    fixture.Commands().Split.Package = Fixture::Presentation().string();
    fixture.Commands().Split.OutDir = directory.string();
    fixture.Commands().Split.By = "slides";
    fixture.Commands().Split.Count = 1;
    fixture.Commands().Split.Prefix = "slide";

    REQUIRE(fixture.Commands().Split.Run(fixture.Context()).Code == ExitCode::Ok);
    CHECK(ProducedFiles::Count(directory) > 0);
}

TEST_CASE("split maps every documented family-specific strategy [cli] [cli-document]")
{
    for (const auto* strategy : {"section", "page"})
    {
        CAPTURE(strategy);
        ParserFixture fixture;
        fixture.Commands().Split.Package = Fixture::WordDocument().string();
        fixture.Commands().Split.OutDir = Fixture::EmptyDirectory().string();
        fixture.Commands().Split.By = strategy;
        CHECK(fixture.Commands().Split.Run(fixture.Context()).Code == ExitCode::Ok);
    }

    ParserFixture marker;
    marker.Commands().Split.Package = Fixture::WordDocument("Chapter").string();
    marker.Commands().Split.OutDir = Fixture::EmptyDirectory().string();
    marker.Commands().Split.By = "marker";
    marker.Commands().Split.Marker = "Chapter";
    CHECK(marker.Commands().Split.Run(marker.Context()).Code == ExitCode::Ok);

    ParserFixture worksheets;
    worksheets.Commands().Split.Package = Fixture::Workbook().string();
    worksheets.Commands().Split.OutDir = Fixture::EmptyDirectory().string();
    worksheets.Commands().Split.By = "worksheets";
    CHECK(worksheets.Commands().Split.Run(worksheets.Context()).Code == ExitCode::Ok);
}

TEST_CASE("merge writes one document from several [cli] [cli-document]")
{
    const auto output = Fixture::UnusedPath(".docx");

    ParserFixture fixture;
    fixture.Commands().Merge.Inputs = {Fixture::WordDocument("Alpha").string(),
                                       Fixture::WordDocument("Omega").string()};
    fixture.Commands().Merge.OutPackage = output.string();
    const auto outcome = fixture.Commands().Merge.Run(fixture.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "merge");
    REQUIRE(std::filesystem::exists(output));

    // Both sources have to be in the result.
    for (const auto* needle : {"Alpha", "Omega"})
    {
        CAPTURE(needle);
        ParserFixture search;
        search.Commands().Search.Package = output.string();
        search.Commands().Search.Needle = needle;
        CHECK(search.Commands().Search.Run(search.Context()).Code == ExitCode::Ok);
    }
}

TEST_CASE("merge maps both non-default style conflict policies [cli] [cli-document]")
{
    const auto inputs = std::vector<std::string>{Fixture::WordDocument("Alpha").string(),
                                                 Fixture::WordDocument("Omega").string()};
    for (const auto* policy : {"keep", "replace"})
    {
        CAPTURE(policy);
        ParserFixture fixture;
        fixture.Commands().Merge.Inputs = inputs;
        fixture.Commands().Merge.OutPackage = Fixture::UnusedPath(".docx").string();
        fixture.Commands().Merge.StyleConflict = policy;
        CHECK(fixture.Commands().Merge.Run(fixture.Context()).Code == ExitCode::Ok);
    }
}

TEST_CASE("merge reports an input it cannot load [cli] [cli-document]")
{
    ParserFixture fixture;
    fixture.Commands().Merge.Inputs = {"no-such-document-e55.docx",
                                       Fixture::WordDocument().string()};
    fixture.Commands().Merge.OutPackage = Fixture::UnusedPath(".docx").string();

    CHECK(fixture.Commands().Merge.Run(fixture.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("compare separates identical from different [cli] [cli-document]")
{
    const auto original = Fixture::WordDocument("Alpha");
    const auto revised = Fixture::WordDocument("Omega");

    ParserFixture different;
    different.Commands().Compare.Original = original.string();
    different.Commands().Compare.Revised = revised.string();
    different.Commands().Compare.OutPackage = Fixture::UnusedPath(".docx").string();
    different.Commands().Compare.Author = "CLI Reviewer";
    const auto outcome = different.Commands().Compare.Run(different.Context());

    CHECK(outcome.Code == ExitCode::DiffDifferent);
    CHECK(outcome.Document.Command == "compare");
    CHECK(std::filesystem::exists(different.Commands().Compare.OutPackage));
    auto compared = ExyokiOffice::Word::WordDocumentEditor::Open(different.Commands().Compare.OutPackage);
    REQUIRE(compared);
    const auto revisions = compared->Revisions();
    REQUIRE_FALSE(revisions.empty());
    CHECK(revisions.front()->GetAuthor() == "CLI Reviewer");

    ParserFixture same;
    same.Commands().Compare.Original = original.string();
    same.Commands().Compare.Revised = original.string();
    same.Commands().Compare.OutPackage = Fixture::UnusedPath(".docx").string();
    CHECK(same.Commands().Compare.Run(same.Context()).Code == ExitCode::Ok);
}

TEST_CASE("redact writes a cleaned copy [cli] [cli-document]")
{
    const auto output = Fixture::UnusedPath(".docx");
    const auto input = Fixture::WordDocument();
    auto source = ExyokiOffice::Word::WordDocumentEditor::Open(input);
    REQUIRE(source);
    REQUIRE(source->Properties().SetCreator("Private Author"));
    REQUIRE(source->SaveToFile(input));

    ParserFixture fixture;
    fixture.Commands().Redact.Package = input.string();
    fixture.Commands().Redact.OutPackage = output.string();
    const auto outcome = fixture.Commands().Redact.Run(fixture.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "redact");
    CHECK(std::filesystem::exists(output));
    auto redacted = ExyokiOffice::Word::WordDocumentEditor::Open(output);
    REQUIRE(redacted);
    CHECK(redacted->Properties().GetCreator().empty());

    ParserFixture missing;
    missing.Commands().Redact.Package = "no-such-document-f19.docx";
    missing.Commands().Redact.OutPackage = Fixture::UnusedPath(".docx").string();
    CHECK(missing.Commands().Redact.Run(missing.Context()).Code == ExitCode::OperationFailed);
}

TEST_CASE("fill reports data it cannot read [cli] [cli-document]")
{
    ParserFixture fixture;
    fixture.Commands().Fill.Package = Fixture::WordDocument().string();
    fixture.Commands().Fill.Data = "no-such-data-3c8.json";
    fixture.Commands().Fill.OutPackage = Fixture::UnusedPath(".docx").string();

    const auto outcome = fixture.Commands().Fill.Run(fixture.Context());

    CHECK(outcome.Code == ExitCode::OperationFailed);
    CHECK(outcome.Document.Command == "fill");
}

TEST_CASE("fill writes merged field values through the CLI adapter [cli] [cli-document]")
{
    const auto output = Fixture::UnusedPath(".docx");
    ParserFixture fixture;
    fixture.Commands().Fill.Package = Fixture::WordTemplate().string();
    fixture.Commands().Fill.Data = Fixture::TextFile(R"({"FirstName":"Ada"})", ".json").string();
    fixture.Commands().Fill.OutPackage = output.string();

    const auto outcome = fixture.Commands().Fill.Run(fixture.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    REQUIRE(std::filesystem::exists(output));
    ParserFixture search;
    search.Commands().Search.Package = output.string();
    search.Commands().Search.Needle = "Ada";
    CHECK(search.Commands().Search.Run(search.Context()).Code == ExitCode::Ok);
}

TEST_CASE("recalc leaves the workbook alone on a dry run [cli] [cli-document]")
{
    const auto workbook = Fixture::Workbook();
    const auto before = Fixture::ReadText(workbook);

    ParserFixture fixture;
    fixture.Commands().Recalc.Package = workbook.string();
    fixture.Commands().Recalc.DryRun = true;
    const auto outcome = fixture.Commands().Recalc.Run(fixture.Context());

    REQUIRE(outcome.Code == ExitCode::Ok);
    CHECK(outcome.Document.Command == "recalc");
    CHECK(Fixture::ReadText(workbook) == before);
    CHECK(RenderJson(outcome.Document).find("\"recalculatedCellCount\": 1") != std::string::npos);

    const auto output = Fixture::UnusedPath(".xlsx");
    ParserFixture write;
    write.Commands().Recalc.Package = workbook.string();
    write.Commands().Recalc.Sheet = "Data";
    write.Commands().Recalc.OutPackage = output.string();
    REQUIRE(write.Commands().Recalc.Run(write.Context()).Code == ExitCode::Ok);
    auto reopened = ExyokiOffice::Excel::ExcelDocumentEditor::Open(output);
    REQUIRE(reopened);
    const auto address = ExyokiOffice::Excel::CellAddress::ParseA1("B4");
    REQUIRE(address);
    const auto formula = reopened->FirstWorksheet()->GetCellFormula(*address);
    REQUIRE(formula);
    CHECK(formula->CachedText == "42");

    ParserFixture missing;
    missing.Commands().Recalc.Package = "no-such-workbook-8b2.xlsx";
    missing.Commands().Recalc.DryRun = true;
    CHECK(missing.Commands().Recalc.Run(missing.Context()).Code == ExitCode::OperationFailed);
}
