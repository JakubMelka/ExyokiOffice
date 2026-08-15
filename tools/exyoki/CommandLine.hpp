// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Commands.hpp"

#include <CLI11.hpp>

#include <functional>
#include <vector>

namespace exyoki
{

/**
 * @file
 * @brief The command line itself: which commands exist and how one is chosen.
 *
 * This is deliberately not in main.cpp. `main()` there is the entry point and
 * nothing else, so that everything the tool does - building the parser, mapping
 * a parse failure onto an exit code, choosing and running a command - can be
 * exercised in process by tests/cli rather than only by running the binary.
 */

/// Every command the tool has, holding the options the parser writes into.
struct CommandSet
{
    CommandsCommand Commands;
    PartsCommand Parts;
    RelationshipsCommand Relationships;
    InfoCommand Info;
    PropsGetCommand PropsGet;
    PropsSetCommand PropsSet;
    ValidateCommand Validate;
    SignaturesCommand Signatures;
    ExternalCommand External;
    UnpackCommand Unpack;
    PackCommand Pack;
    ToFlatOpcCommand ToFlatOpc;
    FromFlatOpcCommand FromFlatOpc;
    ConvertCommand Convert;
    ExportMediaCommand ExportMedia;
    DedupCommand Dedup;
    QueryCommand Query;
    SearchCommand Search;
    ExtractTextCommand ExtractText;
    StatCommand Stat;
    ReplaceCommand Replace;
    SplitCommand Split;
    MergeCommand Merge;
    DiffCommand Diff;
    CompareCommand Compare;
    RedactCommand Redact;
    FillCommand Fill;
    RecalcCommand Recalc;
    SchemaCommand Schema;
    CompletionsCommand Completions;
};

/// One row of the dispatch table: the subcommand that selects a command, and the call that runs it.
struct DispatchEntry
{
    const CLI::App* Command;
    std::function<CommandOutcome(const CommandContext&)> Run;
};

/**
 * @brief Describes the whole interface on @p app and returns the dispatch table.
 *
 * The parser writes straight into the fields of @p options and @p commands,
 * both of which must outlive the returned table. The order of the table is the
 * order the commands are declared in, which is also the order `--help` lists
 * them and the order the catalog reports.
 */
std::vector<DispatchEntry> BuildCommandLine(CLI::App& app, GlobalOptions& options, CommandSet& commands);

/**
 * @brief Runs whichever command @p app selected and returns its exit code.
 *
 * Emits the command's report unless the command wrote its own output. Returns
 * ExitCode::UsageError when no command was selected, and
 * ExitCode::UnhandledException when one throws - a command reaches files chosen
 * by whoever ran the tool, so an I/O error or a corrupt package has to end in a
 * message and an exit code rather than in std::terminate().
 */
int RunSelectedCommand(const std::vector<DispatchEntry>& dispatch, const CommandContext& context);

/**
 * @brief The whole tool: build the parser, parse @p argc / @p argv, run the command.
 *
 * This is what main() calls, and what tests/cli calls to cover argument parsing
 * and the exit codes a failed parse produces.
 */
int RunCommandLine(int argc, char** argv);

} // namespace exyoki
