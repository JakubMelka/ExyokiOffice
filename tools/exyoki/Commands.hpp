// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Adapters.hpp"
#include "ExitCodes.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <CLI11.hpp>

#include <string>
#include <vector>

namespace exyoki
{

/**
 * @file
 * @brief One structure per command: the options it takes and what it does.
 *
 * Each command owns its options as named fields, and the parser binds straight
 * to them, so `main()` holds the description of the interface and nothing
 * else. Run() carries the work, and every command answers in the same shape,
 * which is what keeps the dispatch in `main()` a flat list.
 */

/// The options that precede the subcommand and apply to every command.
struct GlobalOptions
{
    std::string Format = "plain";
    std::string Output = "-";
    std::string PackageLimits = "recommended";
    bool Quiet = false;
};

/// What a command may need beyond its own options.
struct CommandContext
{
    /// The live parser, for the two commands that describe the tool itself.
    const CLI::App& Parser;
    const GlobalOptions& Options;
};

/**
 * @brief What running a command produced.
 *
 * Nearly every command answers with a report document and an exit code; the
 * caller emits the one and returns the other. The few whose output *is* the
 * payload - a JSON schema, a completion script, a conversion written to
 * stdout - write it themselves and set WroteOwnOutput, leaving Document empty.
 */
struct CommandOutcome
{
    ReportDocument Document;
    ExitCode Code = ExitCode::Ok;
    bool WroteOwnOutput = false;
};

/// Report rendering, payload writing, and the settings that precede any command.
class ReportEmitter
{
public:
    /**
     * @brief Applies --package-limits to every package this process will load.
     *
     * A command line utility is pointed at files chosen by whoever runs it, which
     * routinely means files that arrived by mail or download. The commands reach
     * the loader through a dozen `ExyokiOffice::Tools` entry points that construct
     * their own packages and take no settings, so the limits are set once here
     * rather than threaded through each of them.
     *
     * `unlimited` is a deliberate escape hatch: the limits are the one thing
     * standing between the tool and a package that is legitimately enormous, or
     * damaged in a way that only forensic inspection will explain.
     */
    static bool ApplyPackageLimits(const std::string& value, bool quiet);

    static std::string Render(const ReportDocument& document, const std::string& format);

    /// Writes the rendered report to --output (or stdout) and diagnostics to stderr.
    static void Emit(const ReportDocument& document, const GlobalOptions& options);

    /// Writes a command's own payload to --output (or stdout), with no envelope.
    static void WritePayload(const std::string& payload, const GlobalOptions& options);

    /// Writes @p diagnostics to stderr, unless --quiet.
    static void WriteDiagnostics(const std::vector<ToolDiagnostic>& diagnostics, bool quiet);

    /// Emits whatever @p outcome carries and returns the process exit code.
    static int Finish(const CommandOutcome& outcome, const GlobalOptions& options);

    /// The exit code the majority of commands return: success, or a failed operation.
    static ExitCode OkOrFailed(bool ok) noexcept
    {
        return ok ? ExitCode::Ok : ExitCode::OperationFailed;
    }
};

/// `commands` - describes every command, option and exit code machine-readably.
struct CommandsCommand
{
    CommandOutcome Run(const CommandContext& context) const;
};

/// `parts` - lists every part in a package.
struct PartsCommand
{
    std::string Package;
    std::string Sort = "uri";

    CommandOutcome Run(const CommandContext& context) const;
};

/// `relationships` - lists relationships, optionally restricted to given sources.
struct RelationshipsCommand
{
    std::string Package;
    std::vector<std::string> Parts;
    bool DanglingOnly = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `info` - package summary and core properties.
struct InfoCommand
{
    std::string Package;
    bool PropsOnly = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `props get` - reads core and extended properties.
struct PropsGetCommand
{
    std::string Package;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `props set` - writes one or more properties and saves the package.
struct PropsSetCommand
{
    std::string Package;
    std::string Title;
    std::string Creator;
    std::string Subject;
    std::string Keywords;
    std::vector<std::string> Custom;
    std::string OutPackage;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `validate` - OPC structure and, unless switched off, DOM and schema rules.
struct ValidateCommand
{
    std::vector<std::string> Packages;
    std::string OfficeVersion = "365";
    /// Negative means "no cap", which is what the option defaults to.
    int MaxIssues = -1;
    bool NoDom = false;
    bool CrossCheckContentModel = false;
    bool ErrorsOnly = false;
    bool WarningsAsErrors = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `signatures` - lists digital signatures and checks the signed content.
struct SignaturesCommand
{
    std::string Package;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `external` - lists resources the package references from outside itself.
struct ExternalCommand
{
    std::string Package;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `unpack` - extracts a package's ZIP entries to a directory.
struct UnpackCommand
{
    std::string Package;
    std::string OutDir;
    bool Pretty = false;
    bool Overwrite = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `pack` - rebuilds a package from a directory tree.
struct PackCommand
{
    std::string InDir;
    std::string OutPackage;
    int Compression = 6;
    bool RegenerateContentTypes = false;
    bool Validate = false;
    bool Overwrite = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `to-flat-opc` - converts a package to a single Flat OPC XML file.
struct ToFlatOpcCommand
{
    std::string Package;
    std::string OutFile;
    bool NoPretty = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `from-flat-opc` - rebuilds a package from a Flat OPC XML file.
struct FromFlatOpcCommand
{
    std::string FlatOpc;
    std::string OutPackage;
    int Compression = 6;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `convert` - converts between Office packages and text formats.
struct ConvertCommand
{
    std::string Input;
    std::string Output;
    std::string From;
    std::string To;
    std::string MediaDir;
    std::string Sheet;
    std::string CsvSeparator = ",";
    bool EmbedMedia = false;
    bool NoMedia = false;
    bool Overwrite = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `export-media` - exports images, audio, video and embedded objects.
struct ExportMediaCommand
{
    std::string Package;
    std::string OutDir;
    bool Overwrite = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `dedup` - merges byte-identical shared resources.
struct DedupCommand
{
    std::string Package;
    std::string OutPackage;
    bool DryRun = false;
    bool Overwrite = false;
    bool Fonts = false;
    bool AllBinary = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `query` - runs an XPath expression over an XML part.
struct QueryCommand
{
    std::string Package;
    std::string Xpath;
    std::string Part;
    std::vector<std::string> Namespaces;
    int Max = 0;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `search` - searches text across a Word, Excel or PowerPoint document.
struct SearchCommand
{
    std::string Package;
    std::string Needle;
    int Context = 40;
    bool Regex = false;
    bool IgnoreCase = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `extract-text` - extracts all readable text.
struct ExtractTextCommand
{
    std::string Package;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `stat` - content statistics for a document.
struct StatCommand
{
    std::string Package;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `replace` - replaces text across a document.
struct ReplaceCommand
{
    std::string Package;
    std::string Needle;
    std::string Replacement;
    std::string OutPackage;
    bool DryRun = false;
    bool Regex = false;
    bool IgnoreCase = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `split` - splits a document into several output documents.
struct SplitCommand
{
    std::string Package;
    std::string OutDir;
    std::string By = "auto";
    std::string Marker;
    std::string Prefix = "part";
    ExyokiOffice::Size Count = 0;
    bool Overwrite = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `merge` - merges same-family documents into one.
struct MergeCommand
{
    std::vector<std::string> Inputs;
    std::string OutPackage;
    std::string StyleConflict = "rename";
    bool NoPageBreaks = false;
    bool Overwrite = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `diff` - compares two packages part by part.
struct DiffCommand
{
    std::string Left;
    std::string Right;
    bool NoNormalize = false;
    bool PartsOnly = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `compare` - writes the differences between two Word documents as revisions.
struct CompareCommand
{
    std::string Original;
    std::string Revised;
    std::string OutPackage;
    std::string Author = "exyoki";

    CommandOutcome Run(const CommandContext& context) const;
};

/// `redact` - removes comments, revisions, hidden text and personal metadata.
struct RedactCommand
{
    std::string Package;
    std::string OutPackage;
    bool KeepComments = false;
    bool KeepRevisions = false;
    bool KeepHiddenText = false;
    bool KeepMetadata = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `fill` - fills a Word mail-merge template from JSON data.
struct FillCommand
{
    std::string Package;
    std::string Data;
    std::string OutPackage;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `recalc` - recalculates workbook formulas and rewrites the cached results.
struct RecalcCommand
{
    std::string Package;
    std::string Sheet;
    std::string OutPackage;
    bool DryRun = false;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `schema` - prints the document model schema, or checks a document against it.
struct SchemaCommand
{
    std::string Check;

    CommandOutcome Run(const CommandContext& context) const;
};

/// `completions` - prints a shell completion script.
struct CompletionsCommand
{
    std::string Shell;

    CommandOutcome Run(const CommandContext& context) const;
};

} // namespace exyoki
