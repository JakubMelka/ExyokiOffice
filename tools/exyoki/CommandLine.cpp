// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "CommandLine.hpp"

#include "ExyokiOffice/Version.hpp"

#include <iostream>
#include <string>

namespace exyoki
{

std::vector<DispatchEntry> BuildCommandLine(CLI::App& app, GlobalOptions& options, CommandSet& commands)
{
    app.set_version_flag("--version", "exyoki " + std::string(ExyokiOffice::GetVersion()));
    app.require_subcommand(1);
    // XPath arguments (query command) routinely start with '/' or '//'; disable Windows-style
    // '/option' parsing so those values are captured as positionals rather than treated as flags.
    app.allow_windows_style_options(false);

    app.add_option("--format", options.Format, "Output format")
        ->check(CLI::IsMember({"plain", "markdown", "json", "xml"}))
        ->default_val("plain");
    // Every subcommand inherits this one through fallthrough(), and a subcommand
    // option of the same name would shadow it after the subcommand name while the
    // global still won before it - the same spelling meaning two things depending
    // on where it sits. Commands that write a document name that destination
    // "--out-package" (or a positional "outpackage") for that reason.
    app.add_option("--output", options.Output, "Report file path ('-' for stdout)")->default_val("-");
    app.add_option("--package-limits", options.PackageLimits,
                   "ZIP/XML safety limits applied to every package read: 'recommended' bounds entry counts, "
                   "sizes, compression ratio and XML nesting; 'unlimited' switches the guard off")
        ->check(CLI::IsMember({"recommended", "unlimited"}))
        ->default_val("recommended");
    app.add_flag("--quiet", options.Quiet, "Suppress human-readable diagnostics on stderr");

    // commands
    auto* commandsCmd =
        app.add_subcommand("commands",
                           "Describe every command, option and exit code in a machine-readable form")
            ->fallthrough();

    // parts
    auto* partsCmd = app.add_subcommand("parts", "List every part in a package")->fallthrough();
    partsCmd->add_option("package", commands.Parts.Package, "Path to the .docx/.xlsx/.pptx package")->required();
    partsCmd->add_option("--sort", commands.Parts.Sort, "Sort order")
        ->default_val("uri")
        ->check(CLI::IsMember({"uri", "size", "type"}));

    // relationships
    auto* relCmd = app.add_subcommand("relationships", "List relationships in a package")->fallthrough();
    relCmd->add_option("package", commands.Relationships.Package, "Path to the package")->required();
    relCmd->add_option("--part", commands.Relationships.Parts,
                       "Restrict to relationships whose source is this part URI (repeatable)");
    relCmd->add_flag("--dangling-only", commands.Relationships.DanglingOnly,
                     "Only show relationships whose target is missing");

    // info
    auto* infoCmd = app.add_subcommand("info", "Show package summary and core properties")->fallthrough();
    infoCmd->add_option("package", commands.Info.Package, "Path to the package")->required();
    infoCmd->add_flag("--props-only", commands.Info.PropsOnly, "Only show core/extended properties");

    // props get/set
    auto* propsCmd = app.add_subcommand("props", "Read or write core/extended document properties")->fallthrough();
    propsCmd->require_subcommand(1);

    auto* propsGetCmd = propsCmd->add_subcommand("get", "Read core/extended properties")->fallthrough();
    propsGetCmd->add_option("package", commands.PropsGet.Package, "Path to the package")->required();

    auto* propsSetCmd = propsCmd->add_subcommand("set", "Write one or more core/extended properties")->fallthrough();
    propsSetCmd->add_option("package", commands.PropsSet.Package, "Path to the package")->required();
    propsSetCmd->add_option("--title", commands.PropsSet.Title, "Set the Title property");
    propsSetCmd->add_option("--creator", commands.PropsSet.Creator, "Set the Creator property");
    propsSetCmd->add_option("--subject", commands.PropsSet.Subject, "Set the Subject property");
    propsSetCmd->add_option("--keywords", commands.PropsSet.Keywords, "Set the Keywords property");
    propsSetCmd->add_option("--set", commands.PropsSet.Custom, "Set an arbitrary property as name=value (repeatable)");
    propsSetCmd->add_option("--out-package", commands.PropsSet.OutPackage, "Save to a different package instead of in-place");

    // validate
    auto* validateCmd = app.add_subcommand("validate", "Validate OPC structure and (optionally) DOM/schema rules")->fallthrough();
    validateCmd
        ->add_option("package", commands.Validate.Packages,
                     "Package path(s); '*' and '?' wildcards in the filename are expanded")
        ->required()
        ->expected(1, -1);
    validateCmd->add_option("--office-version", commands.Validate.OfficeVersion, "Target Office generation")
        ->default_val("365")
        ->check(CLI::IsMember({"2007", "2010", "2013", "2016", "2019", "2021", "365"}));
    validateCmd->add_flag("--no-dom", commands.Validate.NoDom, "Skip per-part DOM/schema validation");
    validateCmd->add_flag("--cross-check-content-model", commands.Validate.CrossCheckContentModel,
                          "Check every content-model verdict against the reference matcher (slow)");
    validateCmd->add_option("--max-issues", commands.Validate.MaxIssues, "Cap the number of reported issues")
        ->check(CLI::NonNegativeNumber);
    validateCmd->add_flag("--errors-only", commands.Validate.ErrorsOnly, "Only report errors");
    validateCmd->add_flag("--warnings-as-errors", commands.Validate.WarningsAsErrors, "Treat warnings as errors");

    // signatures
    auto* signaturesCmd =
        app.add_subcommand("signatures", "List digital signatures and check whether the signed content changed")
            ->fallthrough();
    signaturesCmd->add_option("package", commands.Signatures.Package, "Path to the package")->required();

    // external
    auto* externalCmd =
        app.add_subcommand("external", "List resources the package references from outside itself")->fallthrough();
    externalCmd->add_option("package", commands.External.Package, "Path to the package")->required();

    // unpack
    auto* unpackCmd = app.add_subcommand("unpack", "Extract a package's ZIP entries to a directory")->fallthrough();
    unpackCmd->add_option("package", commands.Unpack.Package, "Path to the package")->required();
    unpackCmd->add_option("outdir", commands.Unpack.OutDir, "Output directory")->required();
    unpackCmd->add_flag("--pretty", commands.Unpack.Pretty, "Re-indent XML/rels entries for readability");
    unpackCmd->add_flag("--overwrite", commands.Unpack.Overwrite, "Allow writing into a non-empty output directory");

    // pack
    auto* packCmd = app.add_subcommand("pack", "Rebuild a package from a directory tree (see unpack)")->fallthrough();
    packCmd->add_option("indir", commands.Pack.InDir, "Input directory")->required();
    packCmd->add_option("outpackage", commands.Pack.OutPackage, "Destination package path")->required();
    packCmd->add_flag("--regenerate-content-types", commands.Pack.RegenerateContentTypes,
                      "Rebuild [Content_Types].xml instead of copying it verbatim");
    packCmd->add_flag("--validate", commands.Pack.Validate, "Validate the freshly written package");
    packCmd->add_option("--compression", commands.Pack.Compression, "Deflate compression level (0-9)")
        ->default_val(6)
        ->check(CLI::Range(0, 9));
    packCmd->add_flag("--overwrite", commands.Pack.Overwrite, "Overwrite an existing destination package");

    // to-flat-opc / from-flat-opc
    auto* toFlatOpcCmd = app.add_subcommand("to-flat-opc", "Convert a package to a single Flat OPC XML file")->fallthrough();
    toFlatOpcCmd->add_option("package", commands.ToFlatOpc.Package, "Path to the package")->required();
    toFlatOpcCmd->add_option("outfile", commands.ToFlatOpc.OutFile, "Destination Flat OPC .xml file")->required();
    toFlatOpcCmd->add_flag("--no-pretty", commands.ToFlatOpc.NoPretty, "Write compact XML instead of indented");

    auto* fromFlatOpcCmd = app.add_subcommand("from-flat-opc", "Rebuild a package from a Flat OPC XML file")->fallthrough();
    fromFlatOpcCmd->add_option("flatopc", commands.FromFlatOpc.FlatOpc, "Path to the Flat OPC .xml file")->required();
    fromFlatOpcCmd->add_option("outpackage", commands.FromFlatOpc.OutPackage, "Destination package path")->required();
    fromFlatOpcCmd->add_option("--compression", commands.FromFlatOpc.Compression, "Deflate compression level (0-9)")
        ->default_val(6)
        ->check(CLI::Range(0, 9));

    // convert
    auto* convertCmd =
        app.add_subcommand("convert",
                           "Convert between Office packages (docx/xlsx/pptx) and "
                           "Markdown/JSON/text/XML (plus CSV for workbooks)")
            ->fallthrough();
    convertCmd->add_option("input", commands.Convert.Input, "Source file (Office package or md/json/txt/xml)")
        ->required();
    convertCmd->add_option("outputfile", commands.Convert.Output,
                           "Destination file; '-' writes a text-format result to stdout (requires --to)")
        ->required();
    convertCmd->add_option("--from", commands.Convert.From, "Input format (default: inferred from the extension)")
        ->check(CLI::IsMember({"docx", "xlsx", "pptx", "md", "json", "txt", "xml", "csv"}));
    convertCmd->add_option("--to", commands.Convert.To, "Output format (default: inferred from the extension)")
        ->check(CLI::IsMember({"docx", "xlsx", "pptx", "md", "json", "txt", "xml", "csv"}));
    convertCmd->add_option("--media-dir", commands.Convert.MediaDir,
                           "Media directory (default: '<output stem>_media' when exporting, the input "
                           "directory when importing)");
    convertCmd->add_flag("--embed-media", commands.Convert.EmbedMedia,
                         "Embed media as base64 into json/xml instead of writing external files");
    convertCmd->add_flag("--no-media", commands.Convert.NoMedia, "Drop images and other media entirely");
    convertCmd->add_flag("--overwrite", commands.Convert.Overwrite, "Overwrite existing media files");
    convertCmd->add_option("--sheet", commands.Convert.Sheet,
                           "CSV: worksheet to export (default: the first one) or the name of the "
                           "worksheet a CSV import creates");
    convertCmd->add_option("--csv-separator", commands.Convert.CsvSeparator, "CSV field separator")
        ->default_val(",");

    // export-media
    auto* mediaCmd = app.add_subcommand("export-media", "Export images/audio/video/embedded objects")->fallthrough();
    mediaCmd->add_option("package", commands.ExportMedia.Package, "Path to the package")->required();
    mediaCmd->add_option("outdir", commands.ExportMedia.OutDir, "Output directory")->required();
    mediaCmd->add_flag("--overwrite", commands.ExportMedia.Overwrite, "Overwrite existing files");

    // dedup
    auto* dedupCmd =
        app.add_subcommand("dedup", "Merge byte-identical shared resources (images/audio/video)")
            ->fallthrough();
    dedupCmd->add_option("package", commands.Dedup.Package, "Path to the package")->required();
    // Named "outpackage" rather than "output" so it cannot be confused with the global --output
    // option; CLI11 rejects a positional whose name matches a configurable option on the parent.
    dedupCmd->add_option("outpackage", commands.Dedup.OutPackage,
                         "Output package path (defaults to rewriting the input in place)");
    dedupCmd->add_flag("--dry-run", commands.Dedup.DryRun, "Only report duplicate groups; write nothing");
    dedupCmd->add_flag("--overwrite", commands.Dedup.Overwrite, "Overwrite an existing output file");
    dedupCmd->add_flag("--fonts", commands.Dedup.Fonts, "Also merge embedded font parts");
    dedupCmd->add_flag("--all-binary", commands.Dedup.AllBinary,
                       "Merge every leaf binary part regardless of content type");

    // search
    auto* searchCmd =
        app.add_subcommand("search",
                           "Search text in a Word, Excel, or PowerPoint document (paragraphs, cells, "
                           "slide shapes, notes)")
            ->fallthrough();
    searchCmd->add_option("package", commands.Search.Package, "Path to the .docx/.xlsx/.pptx package")->required();
    searchCmd->add_option("needle", commands.Search.Needle, "Text to search for")->required();
    searchCmd->add_option("--context", commands.Search.Context, "Characters of context on each side of a match")
        ->default_val(40)
        ->check(CLI::NonNegativeNumber);
    searchCmd->add_flag("--regex", commands.Search.Regex, "Treat <needle> as an ECMAScript regular expression");
    searchCmd->add_flag("--ignore-case", commands.Search.IgnoreCase, "Case-insensitive match (plain text or regex)");

    // query
    auto* queryCmd = app.add_subcommand("query", "Run a dynamic XPath query over an XML part of any OPC package")->fallthrough();
    queryCmd->add_option("package", commands.Query.Package, "Path to the .docx/.xlsx/.pptx package")->required();
    queryCmd->add_option("xpath", commands.Query.Xpath, "XPath 1.0 expression (prefixed names resolve by namespace)")->required();
    queryCmd->add_option("--part", commands.Query.Part, "Part URI to query (default: the main document part)");
    queryCmd->add_option("--ns", commands.Query.Namespaces, "Bind a namespace prefix as prefix=uri (repeatable)");
    queryCmd->add_option("--max", commands.Query.Max, "Maximum matches to return (0 = unlimited)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);

    // extract-text
    auto* extractCmd = app.add_subcommand("extract-text", "Extract all readable text from a Word/Excel/PowerPoint package")->fallthrough();
    extractCmd->add_option("package", commands.ExtractText.Package, "Path to the package")->required();

    // stat
    auto* statCmd = app.add_subcommand(
                           "stat", "Compute content statistics (words, paragraphs, tables, ...) for a Word/Excel/PowerPoint package")
                        ->fallthrough();
    statCmd->add_option("package", commands.Stat.Package, "Path to the .docx/.xlsx/.pptx package")->required();

    // replace
    auto* replaceCmd =
        app.add_subcommand("replace", "Replace text across a Word, Excel, or PowerPoint document")
            ->fallthrough();
    replaceCmd->add_option("package", commands.Replace.Package, "Path to the .docx/.xlsx/.pptx package")->required();
    replaceCmd->add_option("needle", commands.Replace.Needle, "Text to find")->required();
    replaceCmd->add_option("replacement", commands.Replace.Replacement,
                           "Replacement text; may reference capture groups as $1, $2, ... when --regex is set")
        ->required();
    replaceCmd->add_flag("--dry-run", commands.Replace.DryRun, "Only count matches; never modify or save the document");
    replaceCmd->add_option("--out-package", commands.Replace.OutPackage, "Save to a different package instead of in-place");
    replaceCmd->add_flag("--regex", commands.Replace.Regex, "Treat <needle> as an ECMAScript regular expression");
    replaceCmd->add_flag("--ignore-case", commands.Replace.IgnoreCase, "Case-insensitive match (plain text or regex)");

    // split
    auto* splitCmd = app.add_subcommand("split", "Split a Word, Excel, or PowerPoint document")->fallthrough();
    splitCmd->add_option("package", commands.Split.Package, "Path to the input Office package")->required();
    splitCmd->add_option("outdir", commands.Split.OutDir, "Output directory")->required();
    splitCmd->add_option("--by", commands.Split.By, "Split unit: auto, section, page, paragraphs, marker, worksheets, or slides")
        ->default_val("auto")
        ->check(CLI::IsMember({"auto", "section", "page", "paragraphs", "marker", "worksheets", "slides"}));
    splitCmd->add_option("--count", commands.Split.Count, "Paragraphs, worksheets, or slides per output document");
    splitCmd->add_option("--marker", commands.Split.Marker, "Start a new document at paragraphs containing this text");
    splitCmd->add_option("--prefix", commands.Split.Prefix, "Output filename prefix")->default_val("part");
    splitCmd->add_flag("--overwrite", commands.Split.Overwrite, "Overwrite existing output files");

    // merge
    auto* mergeCmd = app.add_subcommand("merge", "Merge same-family Word, Excel, or PowerPoint documents")->fallthrough();
    mergeCmd->add_option("inputs", commands.Merge.Inputs, "Input Office packages")->required()->expected(1, -1);
    mergeCmd->add_option("--out-package", commands.Merge.OutPackage, "Destination Office package")->required();
    mergeCmd->add_flag("--no-page-breaks", commands.Merge.NoPageBreaks, "Word only: do not insert page breaks between documents");
    mergeCmd->add_flag("--overwrite", commands.Merge.Overwrite, "Overwrite an existing destination");
    mergeCmd->add_option("--style-conflict", commands.Merge.StyleConflict, "Style conflict policy")
        ->default_val("rename")
        ->check(CLI::IsMember({"rename", "keep", "replace"}));

    // diff
    auto* diffCmd = app.add_subcommand("diff", "Compare two packages part-by-part and relationship-by-relationship")->fallthrough();
    diffCmd->add_option("left", commands.Diff.Left, "First package")->required();
    diffCmd->add_option("right", commands.Diff.Right, "Second package")->required();
    diffCmd->add_flag("--no-normalize", commands.Diff.NoNormalize, "Compare XML parts as raw bytes instead of normalized trees");
    diffCmd->add_flag("--parts-only", commands.Diff.PartsOnly, "Omit relationship changes from the report");

    // compare
    auto* compareCmd =
        app.add_subcommand("compare",
                           "Compare two Word documents and write the differences as tracked revisions")
            ->fallthrough();
    compareCmd->add_option("original", commands.Compare.Original, "Original .docx document")->required();
    compareCmd->add_option("revised", commands.Compare.Revised, "Revised .docx document")->required();
    compareCmd->add_option("--out-package", commands.Compare.OutPackage, "Destination .docx with tracked revisions")
        ->required();
    compareCmd->add_option("--author", commands.Compare.Author, "Author recorded on the generated revisions")
        ->default_val("exyoki");

    // redact
    auto* redactCmd =
        app.add_subcommand("redact",
                           "Remove comments, tracked revisions, hidden text, and personal metadata "
                           "before publication")
            ->fallthrough();
    redactCmd->add_option("package", commands.Redact.Package, "Path to the .docx/.xlsx/.pptx package")->required();
    redactCmd->add_option("--out-package", commands.Redact.OutPackage, "Save to a different package instead of in-place");
    redactCmd->add_flag("--keep-comments", commands.Redact.KeepComments, "Do not remove comments");
    redactCmd->add_flag("--keep-revisions", commands.Redact.KeepRevisions,
                        "Word only: do not accept tracked revisions");
    redactCmd->add_flag("--keep-hidden-text", commands.Redact.KeepHiddenText, "Word only: do not delete hidden text");
    redactCmd->add_flag("--keep-metadata", commands.Redact.KeepMetadata,
                        "Do not clear Creator/LastModifiedBy/Company or remove custom properties");

    // fill
    auto* fillCmd =
        app.add_subcommand("fill", "Fill a Word mail-merge template (MERGEFIELD/bookmarks) from JSON data")
            ->fallthrough();
    fillCmd->add_option("package", commands.Fill.Package, "Path to the .docx template")->required();
    fillCmd->add_option("data", commands.Fill.Data, "JSON data file (object; arrays of objects drive regions)")
        ->required();
    fillCmd->add_option("--out-package", commands.Fill.OutPackage,
                        "Save to a different package instead of overwriting the template");

    // recalc
    auto* recalcCmd =
        app.add_subcommand("recalc", "Recalculate workbook formulas and rewrite the cached results")
            ->fallthrough();
    recalcCmd->add_option("package", commands.Recalc.Package, "Path to the .xlsx package")->required();
    recalcCmd->add_option("--sheet", commands.Recalc.Sheet, "Recalculate only this worksheet");
    recalcCmd->add_option("--out-package", commands.Recalc.OutPackage, "Save to a different package instead of in-place");
    recalcCmd->add_flag("--dry-run", commands.Recalc.DryRun, "Evaluate and report without saving");

    // schema
    auto* schemaCmd =
        app.add_subcommand("schema",
                           "Print the JSON Schema of the exyokioffice-document model, or check a "
                           "document against it")
            ->fallthrough();
    schemaCmd->add_option("--check", commands.Schema.Check,
                          "Validate this JSON envelope against the schema instead of printing it");

    // completions
    auto* completionsCmd =
        app.add_subcommand("completions", "Print a shell completion script for bash, zsh, or PowerShell")
            ->fallthrough();
    completionsCmd->add_option("shell", commands.Completions.Shell, "Target shell")
        ->required()
        ->check(CLI::IsMember({"bash", "zsh", "powershell", "pwsh"}));

    return {
        {commandsCmd, [&](const CommandContext& context)
         { return commands.Commands.Run(context); }},
        {partsCmd, [&](const CommandContext& context)
         { return commands.Parts.Run(context); }},
        {relCmd, [&](const CommandContext& context)
         { return commands.Relationships.Run(context); }},
        {infoCmd, [&](const CommandContext& context)
         { return commands.Info.Run(context); }},
        {propsGetCmd, [&](const CommandContext& context)
         { return commands.PropsGet.Run(context); }},
        {propsSetCmd, [&](const CommandContext& context)
         { return commands.PropsSet.Run(context); }},
        {signaturesCmd, [&](const CommandContext& context)
         { return commands.Signatures.Run(context); }},
        {externalCmd, [&](const CommandContext& context)
         { return commands.External.Run(context); }},
        {validateCmd, [&](const CommandContext& context)
         { return commands.Validate.Run(context); }},
        {unpackCmd, [&](const CommandContext& context)
         { return commands.Unpack.Run(context); }},
        {packCmd, [&](const CommandContext& context)
         { return commands.Pack.Run(context); }},
        {toFlatOpcCmd, [&](const CommandContext& context)
         { return commands.ToFlatOpc.Run(context); }},
        {fromFlatOpcCmd, [&](const CommandContext& context)
         { return commands.FromFlatOpc.Run(context); }},
        {convertCmd, [&](const CommandContext& context)
         { return commands.Convert.Run(context); }},
        {mediaCmd, [&](const CommandContext& context)
         { return commands.ExportMedia.Run(context); }},
        {dedupCmd, [&](const CommandContext& context)
         { return commands.Dedup.Run(context); }},
        {queryCmd, [&](const CommandContext& context)
         { return commands.Query.Run(context); }},
        {searchCmd, [&](const CommandContext& context)
         { return commands.Search.Run(context); }},
        {extractCmd, [&](const CommandContext& context)
         { return commands.ExtractText.Run(context); }},
        {statCmd, [&](const CommandContext& context)
         { return commands.Stat.Run(context); }},
        {replaceCmd, [&](const CommandContext& context)
         { return commands.Replace.Run(context); }},
        {splitCmd, [&](const CommandContext& context)
         { return commands.Split.Run(context); }},
        {mergeCmd, [&](const CommandContext& context)
         { return commands.Merge.Run(context); }},
        {diffCmd, [&](const CommandContext& context)
         { return commands.Diff.Run(context); }},
        {compareCmd, [&](const CommandContext& context)
         { return commands.Compare.Run(context); }},
        {redactCmd, [&](const CommandContext& context)
         { return commands.Redact.Run(context); }},
        {fillCmd, [&](const CommandContext& context)
         { return commands.Fill.Run(context); }},
        {recalcCmd, [&](const CommandContext& context)
         { return commands.Recalc.Run(context); }},
        {schemaCmd, [&](const CommandContext& context)
         { return commands.Schema.Run(context); }},
        {completionsCmd, [&](const CommandContext& context)
         { return commands.Completions.Run(context); }}};
}

int RunSelectedCommand(const std::vector<DispatchEntry>& dispatch, const CommandContext& context)
{
    // Command execution below can throw (I/O errors, corrupt packages, allocation failures, ...);
    // catch here so failures produce a clean message and exit code instead of std::terminate().
    try
    {
        // require_subcommand(1) guarantees one of these parsed, and CLI11 has
        // already rejected anything that named two. The order is presentational.
        for (const auto& entry : dispatch)
        {
            if (*entry.Command)
            {
                return ReportEmitter::Finish(entry.Run(context), context.Options);
            }
        }

        return static_cast<int>(ExitCode::UsageError);
    }
    catch (const std::exception& error)
    {
        std::cerr << "exyoki: " << error.what() << "\n";
        return static_cast<int>(ExitCode::UnhandledException);
    }
    catch (...)
    {
        std::cerr << "exyoki: unknown error\n";
        return static_cast<int>(ExitCode::UnhandledException);
    }
}

int RunCommandLine(int argc, char** argv)
{
    CLI::App app{"exyoki - ExyokiOffice command-line utility for OPC packages"};

    GlobalOptions options;
    CommandSet commands;
    const auto dispatch = BuildCommandLine(app, options, commands);

    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::CallForHelp& error)
    {
        return app.exit(error);
    }
    catch (const CLI::CallForVersion& error)
    {
        return app.exit(error);
    }
    catch (const CLI::ParseError& error)
    {
        app.exit(error);
        return static_cast<int>(ExitCode::UsageError);
    }

    // Before any command runs, so that no package is ever loaded unbounded.
    if (!ReportEmitter::ApplyPackageLimits(options.PackageLimits, options.Quiet))
    {
        std::cerr << "[error] Unknown --package-limits value '" << options.PackageLimits << "'.\n";
        return static_cast<int>(ExitCode::UsageError);
    }

    const CommandContext context{app, options};
    return RunSelectedCommand(dispatch, context);
}

} // namespace exyoki
