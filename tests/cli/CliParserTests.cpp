// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "CliTestSupport.hpp"

#include "ExitCodes.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using exyoki::ExitCode;
using ExyokiOfficeCliTests::Fixture;
using ExyokiOfficeCliTests::ParserFixture;
using ExyokiOfficeCliTests::RunCommandLine;

/// The command names the tool promises, in the order the interface declares them.
class CliInterface
{
public:
    static const std::vector<std::string>& CommandNames()
    {
        static const std::vector<std::string> names{
            "commands", "parts", "relationships", "info", "props", "validate",
            "signatures", "external", "unpack", "pack", "to-flat-opc", "from-flat-opc",
            "convert", "export-media", "dedup", "search", "query", "extract-text",
            "stat", "replace", "split", "merge", "diff", "compare",
            "redact", "fill", "recalc", "schema", "completions"};
        return names;
    }
};

TEST_CASE("Every promised command is wired into the parser [cli] [cli-parser]")
{
    ParserFixture fixture;

    for (const auto& name : CliInterface::CommandNames())
    {
        CAPTURE(name);
        CHECK_NOTHROW((void)fixture.App().get_subcommand(name));
    }

    // props is the only command with subcommands of its own.
    auto* props = fixture.App().get_subcommand("props");
    REQUIRE(props != nullptr);
    CHECK_NOTHROW((void)props->get_subcommand("get"));
    CHECK_NOTHROW((void)props->get_subcommand("set"));

    auto* sort = fixture.App().get_subcommand("parts")->get_option("--sort");
    REQUIRE(sort != nullptr);
    CHECK(sort->get_default_str() == "uri");
}

TEST_CASE("A command line naming no command is a usage error [cli] [cli-parser]")
{
    const auto result = RunCommandLine({});
    CHECK(result.Code == static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("An unknown command is a usage error [cli] [cli-parser]")
{
    const auto result = RunCommandLine({"nosuchcommand"});
    CHECK(result.Code == static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("A missing required argument is a usage error [cli] [cli-parser]")
{
    CHECK(RunCommandLine({"parts"}).Code == static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"diff"}).Code == static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"unpack", "only-one-argument.docx"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    // props requires one of its two subcommands.
    CHECK(RunCommandLine({"props"}).Code == static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("A value outside a constrained set is a usage error [cli] [cli-parser]")
{
    const auto document = Fixture::WordDocument();
    const auto path = document.string();

    CHECK(RunCommandLine({"--format", "bogus", "parts", path}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"parts", path, "--sort", "bogus"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"validate", path, "--office-version", "1999"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"completions", "fish"}).Code == static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"split", path, path, "--by", "bogus"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"--package-limits", "bogus", "parts", path}).Code ==
          static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("A compression level outside 0-9 is a usage error [cli] [cli-parser]")
{
    const auto directory = Fixture::EmptyDirectory();
    const auto output = Fixture::UnusedPath(".docx");

    CHECK(RunCommandLine({"pack", directory.string(), output.string(), "--compression", "99"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"pack", directory.string(), output.string(), "--compression", "-1"}).Code ==
          static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("Counts and limits reject negative values [cli] [cli-parser]")
{
    const auto document = Fixture::WordDocument().string();
    CHECK(RunCommandLine({"search", document, "Alpha", "--context", "-1"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"query", document, "//w:p", "--max", "-1"}).Code ==
          static_cast<int>(ExitCode::UsageError));
    CHECK(RunCommandLine({"validate", document, "--max-issues", "-1"}).Code ==
          static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("Inspection command options bind to their command fields [cli] [cli-parser]")
{
    {
        ParserFixture fixture;
        fixture.Parse({"parts", "a.docx", "--sort", "size"});
        CHECK(fixture.Commands().Parts.Package == "a.docx");
        CHECK(fixture.Commands().Parts.Sort == "size");
    }
    {
        ParserFixture fixture;
        fixture.Parse({"relationships", "a.docx", "--part", "/one.xml", "--part", "/two.xml",
                       "--dangling-only"});
        CHECK((fixture.Commands().Relationships.Parts == std::vector<std::string>{"/one.xml", "/two.xml"}));
        CHECK(fixture.Commands().Relationships.DanglingOnly);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"info", "a.docx", "--props-only"});
        CHECK(fixture.Commands().Info.PropsOnly);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"props", "set", "a.docx", "--title", "T", "--creator", "C", "--subject", "S",
                       "--keywords", "K", "--set", "Department=R", "--out-package", "b.docx"});
        CHECK(fixture.Commands().PropsSet.Title == "T");
        CHECK(fixture.Commands().PropsSet.Creator == "C");
        CHECK(fixture.Commands().PropsSet.Subject == "S");
        CHECK(fixture.Commands().PropsSet.Keywords == "K");
        CHECK((fixture.Commands().PropsSet.Custom == std::vector<std::string>{"Department=R"}));
        CHECK(fixture.Commands().PropsSet.OutPackage == "b.docx");
    }
    {
        ParserFixture fixture;
        fixture.Parse({"validate", "a.docx", "b.xlsx", "--office-version", "2013", "--no-dom",
                       "--cross-check-content-model", "--max-issues", "7", "--errors-only",
                       "--warnings-as-errors"});
        CHECK((fixture.Commands().Validate.Packages == std::vector<std::string>{"a.docx", "b.xlsx"}));
        CHECK(fixture.Commands().Validate.OfficeVersion == "2013");
        CHECK(fixture.Commands().Validate.MaxIssues == 7);
        CHECK(fixture.Commands().Validate.NoDom);
        CHECK(fixture.Commands().Validate.CrossCheckContentModel);
        CHECK(fixture.Commands().Validate.ErrorsOnly);
        CHECK(fixture.Commands().Validate.WarningsAsErrors);
    }
}

TEST_CASE("Archive and conversion options bind to their command fields [cli] [cli-parser]")
{
    {
        ParserFixture fixture;
        fixture.Parse({"unpack", "a.docx", "tree", "--pretty", "--overwrite"});
        CHECK(fixture.Commands().Unpack.Pretty);
        CHECK(fixture.Commands().Unpack.Overwrite);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"pack", "tree", "a.docx", "--regenerate-content-types", "--validate",
                       "--compression", "4", "--overwrite"});
        CHECK(fixture.Commands().Pack.RegenerateContentTypes);
        CHECK(fixture.Commands().Pack.Validate);
        CHECK(fixture.Commands().Pack.Compression == 4);
        CHECK(fixture.Commands().Pack.Overwrite);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"to-flat-opc", "a.docx", "a.xml", "--no-pretty"});
        CHECK(fixture.Commands().ToFlatOpc.NoPretty);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"from-flat-opc", "a.xml", "a.docx", "--compression", "2"});
        CHECK(fixture.Commands().FromFlatOpc.Compression == 2);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"convert", "a.docx", "a.json", "--from", "docx", "--to", "json", "--media-dir",
                       "media", "--embed-media", "--no-media", "--overwrite", "--sheet", "Data",
                       "--csv-separator", ";"});
        CHECK(fixture.Commands().Convert.From == "docx");
        CHECK(fixture.Commands().Convert.To == "json");
        CHECK(fixture.Commands().Convert.MediaDir == "media");
        CHECK(fixture.Commands().Convert.EmbedMedia);
        CHECK(fixture.Commands().Convert.NoMedia);
        CHECK(fixture.Commands().Convert.Overwrite);
        CHECK(fixture.Commands().Convert.Sheet == "Data");
        CHECK(fixture.Commands().Convert.CsvSeparator == ";");
    }
    {
        ParserFixture fixture;
        fixture.Parse({"export-media", "a.docx", "media", "--overwrite"});
        CHECK(fixture.Commands().ExportMedia.Overwrite);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"dedup", "a.docx", "b.docx", "--dry-run", "--overwrite", "--fonts", "--all-binary"});
        CHECK(fixture.Commands().Dedup.OutPackage == "b.docx");
        CHECK(fixture.Commands().Dedup.DryRun);
        CHECK(fixture.Commands().Dedup.Overwrite);
        CHECK(fixture.Commands().Dedup.Fonts);
        CHECK(fixture.Commands().Dedup.AllBinary);
    }
}

TEST_CASE("Text query and edit options bind to their command fields [cli] [cli-parser]")
{
    {
        ParserFixture fixture;
        fixture.Parse({"search", "a.docx", "needle", "--context", "12", "--regex", "--ignore-case"});
        CHECK(fixture.Commands().Search.Context == 12);
        CHECK(fixture.Commands().Search.Regex);
        CHECK(fixture.Commands().Search.IgnoreCase);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"query", "a.docx", "//x:p", "--part", "/word/document.xml", "--ns", "x=urn:x",
                       "--max", "3"});
        CHECK(fixture.Commands().Query.Part == "/word/document.xml");
        CHECK((fixture.Commands().Query.Namespaces == std::vector<std::string>{"x=urn:x"}));
        CHECK(fixture.Commands().Query.Max == 3);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"replace", "a.docx", "old", "new", "--dry-run", "--out-package", "b.docx",
                       "--regex", "--ignore-case"});
        CHECK(fixture.Commands().Replace.DryRun);
        CHECK(fixture.Commands().Replace.OutPackage == "b.docx");
        CHECK(fixture.Commands().Replace.Regex);
        CHECK(fixture.Commands().Replace.IgnoreCase);
    }
}

TEST_CASE("Document workflow options bind to their command fields [cli] [cli-parser]")
{
    {
        ParserFixture fixture;
        fixture.Parse({"split", "a.docx", "parts", "--by", "marker", "--count", "2", "--marker", "Chapter",
                       "--prefix", "chapter", "--overwrite"});
        CHECK(fixture.Commands().Split.By == "marker");
        CHECK(fixture.Commands().Split.Count == 2);
        CHECK(fixture.Commands().Split.Marker == "Chapter");
        CHECK(fixture.Commands().Split.Prefix == "chapter");
        CHECK(fixture.Commands().Split.Overwrite);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"merge", "a.docx", "b.docx", "--out-package", "out.docx", "--no-page-breaks",
                       "--style-conflict", "replace", "--overwrite"});
        CHECK((fixture.Commands().Merge.Inputs == std::vector<std::string>{"a.docx", "b.docx"}));
        CHECK(fixture.Commands().Merge.NoPageBreaks);
        CHECK(fixture.Commands().Merge.StyleConflict == "replace");
        CHECK(fixture.Commands().Merge.Overwrite);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"diff", "a.docx", "b.docx", "--no-normalize", "--parts-only"});
        CHECK(fixture.Commands().Diff.NoNormalize);
        CHECK(fixture.Commands().Diff.PartsOnly);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"compare", "a.docx", "b.docx", "--out-package", "out.docx", "--author", "Reviewer"});
        CHECK(fixture.Commands().Compare.Author == "Reviewer");
        CHECK(fixture.Commands().Compare.OutPackage == "out.docx");
    }
    {
        ParserFixture fixture;
        fixture.Parse({"redact", "a.docx", "--out-package", "out.docx", "--keep-comments", "--keep-revisions",
                       "--keep-hidden-text", "--keep-metadata"});
        CHECK(fixture.Commands().Redact.KeepComments);
        CHECK(fixture.Commands().Redact.KeepRevisions);
        CHECK(fixture.Commands().Redact.KeepHiddenText);
        CHECK(fixture.Commands().Redact.KeepMetadata);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"fill", "a.docx", "data.json", "--out-package", "out.docx"});
        CHECK(fixture.Commands().Fill.OutPackage == "out.docx");
    }
    {
        ParserFixture fixture;
        fixture.Parse({"recalc", "a.xlsx", "--sheet", "Data", "--out-package", "out.xlsx", "--dry-run"});
        CHECK(fixture.Commands().Recalc.Sheet == "Data");
        CHECK(fixture.Commands().Recalc.OutPackage == "out.xlsx");
        CHECK(fixture.Commands().Recalc.DryRun);
    }
    {
        ParserFixture fixture;
        fixture.Parse({"schema", "--check", "model.json", "--output", "report.json", "--quiet"});
        CHECK(fixture.Commands().Schema.Check == "model.json");
        CHECK(fixture.Options().Output == "report.json");
        CHECK(fixture.Options().Quiet);
    }
}

TEST_CASE("--version and --help report success without running a command [cli] [cli-parser]")
{
    const auto version = RunCommandLine({"--version"});
    CHECK(version.Code == static_cast<int>(ExitCode::Ok));
    CHECK(version.Out.find("exyoki") != std::string::npos);

    const auto help = RunCommandLine({"--help"});
    CHECK(help.Code == static_cast<int>(ExitCode::Ok));
    for (const auto& name : CliInterface::CommandNames())
    {
        CAPTURE(name);
        CHECK(help.Out.find(name) != std::string::npos);
    }
}

TEST_CASE("An XPath positional is not mistaken for an option [cli] [cli-parser]")
{
    // Windows-style '/option' parsing is switched off precisely so that an
    // expression beginning with '/' or '//' reaches the command as a value.
    const auto document = Fixture::WordDocument();
    const auto result = RunCommandLine({"query", document.string(), "//w:p"});

    CHECK(result.Code != static_cast<int>(ExitCode::UsageError));
}

TEST_CASE("A global option is accepted after the command name [cli] [cli-parser]")
{
    // Every subcommand carries fallthrough(), so --format binds to the global
    // option whichever side of the command name it appears on.
    const auto document = Fixture::WordDocument();

    const auto before = RunCommandLine({"--format", "json", "parts", document.string()});
    const auto after = RunCommandLine({"parts", document.string(), "--format", "json"});

    CHECK(before.Code == static_cast<int>(ExitCode::Ok));
    CHECK(after.Code == static_cast<int>(ExitCode::Ok));
    CHECK(before.Out == after.Out);
    CHECK(before.Out.find("\"command\"") != std::string::npos);
}

TEST_CASE("Every report command is dispatched through the real command line [cli] [cli-parser]")
{
    struct DispatchCase
    {
        std::vector<std::string> Arguments;
        std::string Command;
    };

    const std::string missing = "no-such-dispatch-package-74b.docx";
    const auto output = Fixture::UnusedPath(".docx").string();
    const auto directory = Fixture::UnusedPath("").string();
    const std::vector<DispatchCase> cases{
        {{"commands"}, "commands"},
        {{"parts", missing}, "parts"},
        {{"relationships", missing}, "relationships"},
        {{"info", missing}, "info"},
        {{"props", "get", missing}, "props get"},
        {{"props", "set", missing, "--title", "T"}, "props set"},
        {{"validate", missing}, "validate"},
        {{"signatures", missing}, "signatures"},
        {{"external", missing}, "external"},
        {{"unpack", missing, directory}, "unpack"},
        {{"pack", directory, output}, "pack"},
        {{"to-flat-opc", missing, Fixture::UnusedPath(".xml").string()}, "to-flat-opc"},
        {{"from-flat-opc", Fixture::UnusedPath(".xml").string(), output}, "from-flat-opc"},
        {{"convert", missing, Fixture::UnusedPath(".md").string()}, "convert"},
        {{"export-media", missing, directory}, "export-media"},
        {{"dedup", missing, "--dry-run"}, "dedup"},
        {{"search", missing, "needle"}, "search"},
        {{"query", missing, "//w:p"}, "query"},
        {{"extract-text", missing}, "extract-text"},
        {{"stat", missing}, "stat"},
        {{"replace", missing, "old", "new", "--dry-run"}, "replace"},
        {{"split", missing, directory}, "split"},
        {{"merge", missing, "--out-package", output}, "merge"},
        {{"diff", missing, missing}, "diff"},
        {{"compare", missing, missing, "--out-package", output}, "compare"},
        {{"redact", missing, "--out-package", output}, "redact"},
        {{"fill", missing, "no-such-data-74b.json", "--out-package", output}, "fill"},
        {{"recalc", missing, "--dry-run"}, "recalc"}};

    for (const auto& test : cases)
    {
        CAPTURE(test.Command);
        auto arguments = test.Arguments;
        arguments.insert(arguments.begin(), {"--format", "json"});
        const auto result = RunCommandLine(std::move(arguments));
        CHECK(result.Code != static_cast<int>(ExitCode::UsageError));
        CHECK(result.Out.find("\"command\": \"" + test.Command + "\"") != std::string::npos);
    }

    const auto schema = RunCommandLine({"schema"});
    CHECK(schema.Code == static_cast<int>(ExitCode::Ok));
    CHECK(schema.Out.find("$schema") != std::string::npos);

    const auto completions = RunCommandLine({"completions", "bash"});
    CHECK(completions.Code == static_cast<int>(ExitCode::Ok));
    CHECK(completions.Out.find("_exyoki_completions") != std::string::npos);
}

TEST_CASE("Every outcome-specific exit code survives parsing dispatch and emission [cli] [cli-parser]")
{
    const auto document = Fixture::WordDocument("Alpha");

    const auto invalidModel = Fixture::TextFile(R"({"unrelated":true})", ".json");
    CHECK(RunCommandLine({"schema", "--check", invalidModel.string()}).Code ==
          static_cast<int>(ExitCode::ValidationErrors));

    const auto different = Fixture::WordDocument("Omega");
    CHECK(RunCommandLine({"diff", document.string(), different.string()}).Code ==
          static_cast<int>(ExitCode::DiffDifferent));
    CHECK(RunCommandLine({"search", document.string(), "not-present-74b"}).Code ==
          static_cast<int>(ExitCode::SearchNoMatch));
    CHECK(RunCommandLine({"query", document.string(), "//w:noSuchElement"}).Code ==
          static_cast<int>(ExitCode::QueryNoMatch));
    CHECK(RunCommandLine({"signatures", Fixture::TamperedSignedDocument().string()}).Code ==
          static_cast<int>(ExitCode::SignatureInvalid));
}

TEST_CASE("An exception from a selected command becomes exit code 7 [cli] [cli-parser]")
{
    ParserFixture fixture;
    fixture.Parse({"commands"});
    REQUIRE_FALSE(fixture.Dispatch().empty());
    fixture.Dispatch().front().Run = [](const exyoki::CommandContext&) -> exyoki::CommandOutcome
    { throw std::runtime_error("synthetic command failure"); };

    const ExyokiOfficeCliTests::CapturedOutput captured;
    CHECK(exyoki::RunSelectedCommand(fixture.Dispatch(), fixture.Context()) ==
          static_cast<int>(ExitCode::UnhandledException));
    CHECK(captured.Err().find("synthetic command failure") != std::string::npos);
}

TEST_CASE("The exit code table covers the enumeration exactly once [cli] [cli-parser]")
{
    // The numeric values are published by `exyoki commands`, so a code added to
    // the enumeration without a row would be reported as a code that cannot occur.
    std::vector<int> values;
    for (const auto& description : exyoki::AllExitCodes)
    {
        values.push_back(static_cast<int>(description.Code));
        CHECK(description.Name != nullptr);
        CHECK(description.Meaning != nullptr);
    }

    CHECK(std::is_sorted(values.begin(), values.end()));
    CHECK(std::adjacent_find(values.begin(), values.end()) == values.end());
    CHECK(values.front() == static_cast<int>(ExitCode::Ok));
    CHECK(values.back() == static_cast<int>(ExitCode::SignatureInvalid));
    CHECK(std::string(exyoki::AllExitCodes[3].Meaning).find("schema --check") != std::string::npos);
    CHECK(std::string(exyoki::AllExitCodes[4].Meaning).find("compare") != std::string::npos);
}

TEST_CASE("The command catalog describes every command the parser has [cli] [cli-parser]")
{
    const auto result = RunCommandLine({"--format", "json", "commands"});
    REQUIRE(result.Code == static_cast<int>(ExitCode::Ok));

    // The opening quote alone: "props" is catalogued together with its two
    // subcommands, so the name is not always followed by a closing quote.
    for (const auto& name : CliInterface::CommandNames())
    {
        CAPTURE(name);
        CHECK(result.Out.find("\"" + name) != std::string::npos);
    }

    for (const auto& description : exyoki::AllExitCodes)
    {
        CAPTURE(description.Name);
        CHECK(result.Out.find(std::string("\"") + description.Name + "\"") != std::string::npos);
    }
}
