// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "CliTestSupport.hpp"

#include "ExitCodes.hpp"

#include <algorithm>
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
