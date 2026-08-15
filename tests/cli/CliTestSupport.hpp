// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "CommandLine.hpp"
#include "Commands.hpp"

#include <CLI11.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOfficeCliTests
{

/**
 * @file
 * @brief What the CLI layer needs beyond the shared test support.
 *
 * The cases run the tool's own code in process, so the two things they need are
 * sample packages on disk and a way to read what a command wrote to the
 * standard streams.
 */

/**
 * @brief Redirects std::cout and std::cerr into buffers for its lifetime.
 *
 * Three commands write their payload to the standard output directly - the
 * schema, a completion script, and a conversion sent to stdout - and every
 * command may write diagnostics to the standard error. Capturing both is what
 * lets a case assert on them instead of letting them escape into the test log.
 */
class CapturedOutput
{
public:
    CapturedOutput();
    ~CapturedOutput();

    CapturedOutput(const CapturedOutput&) = delete;
    CapturedOutput& operator=(const CapturedOutput&) = delete;
    CapturedOutput(CapturedOutput&&) = delete;
    CapturedOutput& operator=(CapturedOutput&&) = delete;

    [[nodiscard]] std::string Out() const { return m_out.str(); }
    [[nodiscard]] std::string Err() const { return m_err.str(); }

private:
    std::ostringstream m_out;
    std::ostringstream m_err;
    std::streambuf* m_previousOut;
    std::streambuf* m_previousErr;
};

/**
 * @brief A parser wired exactly as the tool wires it, ready to run a command.
 *
 * The commands are reached through this rather than constructed loose, because
 * two of them describe the interface itself and need the live parser to do it.
 * Options() is the global option block; a case that wants a different output
 * format or --quiet writes to it before calling Run().
 */
class ParserFixture
{
public:
    ParserFixture();

    ParserFixture(const ParserFixture&) = delete;
    ParserFixture& operator=(const ParserFixture&) = delete;
    ParserFixture(ParserFixture&&) = delete;
    ParserFixture& operator=(ParserFixture&&) = delete;

    [[nodiscard]] const exyoki::CommandContext& Context() const noexcept { return m_context; }
    [[nodiscard]] exyoki::GlobalOptions& Options() noexcept { return m_options; }
    [[nodiscard]] exyoki::CommandSet& Commands() noexcept { return m_commands; }
    [[nodiscard]] CLI::App& App() noexcept { return m_app; }

private:
    CLI::App m_app{"exyoki - ExyokiOffice command-line utility for OPC packages"};
    exyoki::GlobalOptions m_options;
    exyoki::CommandSet m_commands;
    std::vector<exyoki::DispatchEntry> m_dispatch;
    exyoki::CommandContext m_context;
};

/// The sample packages and directories the cases run against.
class Fixture
{
public:
    /**
     * @brief A .docx with several paragraphs, one of which holds @p marker.
     *
     * The text is what the search, replace, extract-text and split cases match
     * on, so it is spelled out here rather than left to a default document.
     */
    [[nodiscard]] static std::filesystem::path WordDocument(std::string_view marker = "Alpha");

    /// A .xlsx with a named worksheet, a few text cells and a formula.
    [[nodiscard]] static std::filesystem::path Workbook();

    /// A .pptx with two titled slides.
    [[nodiscard]] static std::filesystem::path Presentation();

    /// An existing, empty directory.
    [[nodiscard]] static std::filesystem::path EmptyDirectory();

    /// A path inside the temporary root that nothing has created.
    [[nodiscard]] static std::filesystem::path UnusedPath(std::string_view extension);

    /// A path to a file holding @p contents.
    [[nodiscard]] static std::filesystem::path TextFile(std::string_view contents,
                                                        std::string_view extension);

    /// Reads a whole file back, for asserting on what a command wrote.
    [[nodiscard]] static std::string ReadText(const std::filesystem::path& path);
};

/// What running a whole command line produced.
struct CommandLineResult
{
    int Code = 0;
    std::string Out;
    std::string Err;
};

/**
 * @brief Runs @p arguments through the real parser, as the binary would.
 *
 * The program name is prepended, so a case passes only what a user would type.
 * This is the entry point that covers argument parsing and the exit codes a
 * rejected command line produces.
 */
[[nodiscard]] CommandLineResult RunCommandLine(std::vector<std::string> arguments);

} // namespace ExyokiOfficeCliTests
