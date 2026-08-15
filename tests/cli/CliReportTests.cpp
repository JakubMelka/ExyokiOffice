// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "CliTestSupport.hpp"

#include "ExyokiOffice/Tools/Report.hpp"

#include <filesystem>
#include <string>

using exyoki::ExitCode;
using exyoki::GlobalOptions;
using exyoki::ReportEmitter;
using ExyokiOffice::Tools::ReportDocument;
using ExyokiOffice::Tools::ToolDiagnostic;
using ExyokiOffice::Tools::ToolSeverity;
using ExyokiOfficeCliTests::CapturedOutput;
using ExyokiOfficeCliTests::Fixture;

// What every command shares: how its report is rendered, where the result goes,
// and which of the two streams a diagnostic ends up on.

/// A report with one diagnostic of each severity that matters here.
class SampleReport
{
public:
    static ReportDocument Build()
    {
        ReportDocument document;
        document.Command = "sample";
        document.Status = "ok";
        document.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Warning, "A warning worth printing", "the context"});
        return document;
    }
};

TEST_CASE("Every documented output format renders [cli] [cli-report]")
{
    const auto document = SampleReport::Build();

    const auto plain = ReportEmitter::Render(document, "plain");
    const auto markdown = ReportEmitter::Render(document, "markdown");
    const auto json = ReportEmitter::Render(document, "json");
    const auto xml = ReportEmitter::Render(document, "xml");

    CHECK(!plain.empty());
    CHECK(!markdown.empty());
    CHECK(json.find('{') != std::string::npos);
    CHECK(xml.find("<exyoki") != std::string::npos);

    // An unknown name is not reachable through the parser, which restricts the
    // value; the renderer still has to answer with something rather than throw.
    CHECK(ReportEmitter::Render(document, "no-such-format") == plain);
}

TEST_CASE("--output sends the report to a file instead of stdout [cli] [cli-report]")
{
    const auto destination = Fixture::UnusedPath(".txt");

    GlobalOptions options;
    options.Output = destination.string();

    const CapturedOutput captured;
    REQUIRE(ReportEmitter::Emit(SampleReport::Build(), options));

    CHECK(captured.Out().empty());
    REQUIRE(std::filesystem::exists(destination));
    CHECK(!Fixture::ReadText(destination).empty());
}

TEST_CASE("Diagnostics reach stderr only for the human formats [cli] [cli-report]")
{
    // json and xml are read by a program, which takes the diagnostics out of the
    // document itself; repeating them on stderr would be noise in a pipeline.
    const auto document = SampleReport::Build();

    for (const auto* format : {"plain", "markdown"})
    {
        CAPTURE(format);
        GlobalOptions options;
        options.Format = format;

        const CapturedOutput captured;
        REQUIRE(ReportEmitter::Emit(document, options));
        CHECK(captured.Err().find("A warning worth printing") != std::string::npos);
        CHECK(captured.Err().find("the context") != std::string::npos);
    }

    for (const auto* format : {"json", "xml"})
    {
        CAPTURE(format);
        GlobalOptions options;
        options.Format = format;

        const CapturedOutput captured;
        REQUIRE(ReportEmitter::Emit(document, options));
        CHECK(captured.Err().empty());
    }
}

TEST_CASE("--quiet silences the diagnostics but not the report [cli] [cli-report]")
{
    GlobalOptions options;
    options.Quiet = true;

    const CapturedOutput captured;
    REQUIRE(ReportEmitter::Emit(SampleReport::Build(), options));

    CHECK(captured.Err().empty());
    CHECK(!captured.Out().empty());
}

TEST_CASE("A payload goes to stdout or to --output, with no envelope [cli] [cli-report]")
{
    {
        GlobalOptions options;
        const CapturedOutput captured;
        REQUIRE(ReportEmitter::WritePayload("the payload", options));
        CHECK(captured.Out() == "the payload");
    }

    const auto destination = Fixture::UnusedPath(".json");
    GlobalOptions options;
    options.Output = destination.string();

    const CapturedOutput captured;
    REQUIRE(ReportEmitter::WritePayload("the payload", options));

    CHECK(captured.Out().empty());
    CHECK(Fixture::ReadText(destination) == "the payload");
}

TEST_CASE("An unwritable --output is an operation failure [cli] [cli-report]")
{
    const auto missingDirectory = Fixture::UnusedPath("");
    GlobalOptions options;
    options.Output = (missingDirectory / "report.json").string();

    {
        const CapturedOutput captured;
        CHECK_FALSE(ReportEmitter::WritePayload("the payload", options));
        CHECK(captured.Err().find("cannot write") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(options.Output));

    const CapturedOutput captured;
    const exyoki::CommandOutcome outcome{SampleReport::Build(), ExitCode::Ok, false};
    CHECK(ReportEmitter::Finish(outcome, options) == static_cast<int>(ExitCode::OperationFailed));
    CHECK(captured.Err().find("cannot write") != std::string::npos);
}

TEST_CASE("Finish emits a report but never a payload command's [cli] [cli-report]")
{
    GlobalOptions options;

    {
        const CapturedOutput captured;
        const exyoki::CommandOutcome outcome{SampleReport::Build(), ExitCode::ValidationErrors, false};
        CHECK(ReportEmitter::Finish(outcome, options) == static_cast<int>(ExitCode::ValidationErrors));
        CHECK(!captured.Out().empty());
    }

    // The command already wrote what it had to say; Document is a placeholder.
    const CapturedOutput captured;
    const exyoki::CommandOutcome wrote{{}, ExitCode::Ok, true};
    CHECK(ReportEmitter::Finish(wrote, options) == static_cast<int>(ExitCode::Ok));
    CHECK(captured.Out().empty());
}

TEST_CASE("OkOrFailed maps the common two-way outcome [cli] [cli-report]")
{
    CHECK(ReportEmitter::OkOrFailed(true) == ExitCode::Ok);
    CHECK(ReportEmitter::OkOrFailed(false) == ExitCode::OperationFailed);
}

TEST_CASE("The package limit names the parser allows are the ones applied [cli] [cli-report]")
{
    // The parser restricts --package-limits to these two, and this function is
    // what turns them into settings. A value accepted by one and rejected by the
    // other would leave the tool refusing a documented command line.
    CHECK(ReportEmitter::ApplyPackageLimits("recommended", true));
    CHECK(ReportEmitter::ApplyPackageLimits("unlimited", true));
    CHECK(!ReportEmitter::ApplyPackageLimits("bogus", true));
    CHECK(!ReportEmitter::ApplyPackageLimits("", true));

    // The setting is process-wide, so the default has to be put back: every
    // other case in this executable expects a package to be read with limits on.
    REQUIRE(ReportEmitter::ApplyPackageLimits("recommended", true));
}

TEST_CASE("Switching the limits off warns unless silenced [cli] [cli-report]")
{
    {
        const CapturedOutput captured;
        REQUIRE(ReportEmitter::ApplyPackageLimits("unlimited", false));
        CHECK(captured.Err().find("limits are switched off") != std::string::npos);
    }

    {
        const CapturedOutput captured;
        REQUIRE(ReportEmitter::ApplyPackageLimits("unlimited", true));
        CHECK(captured.Err().empty());
    }

    REQUIRE(ReportEmitter::ApplyPackageLimits("recommended", true));
}
