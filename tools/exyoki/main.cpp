// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Adapters.hpp"
#include "CommandCatalog.hpp"
#include "Completions.hpp"
#include "ExitCodes.hpp"

#include "ExyokiOffice/Version.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <CLI11.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>

using namespace ExyokiOffice::Tools;
using exyoki::ExitCode;
using ExyokiOffice::OpenXmlPackage;

namespace
{

struct GlobalOptions
{
    std::string Format = "plain";
    std::string Output = "-";
    std::string PackageLimits = "recommended";
    bool Quiet = false;
};

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
bool ApplyPackageLimits(const std::string& value, bool quiet)
{
    if (value == "recommended")
    {
        OpenXmlPackage::SetDefaultPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Recommended());
        return true;
    }

    if (value == "unlimited")
    {
        OpenXmlPackage::SetDefaultPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
        if (!quiet)
        {
            std::cerr << "[warning] Package safety limits are switched off; a decompression bomb or deeply "
                         "nested XML can exhaust memory or the stack.\n";
        }

        return true;
    }

    return false;
}

std::string RenderReport(const ReportDocument& document, const std::string& format)
{
    if (format == "json")
    {
        return RenderJson(document);
    }
    if (format == "markdown")
    {
        return RenderMarkdown(document);
    }
    if (format == "xml")
    {
        return RenderXml(document);
    }
    return RenderPlain(document);
}

/// Writes the rendered report to --output (or stdout) and human diagnostics to stderr.
void EmitReport(const ReportDocument& document, const GlobalOptions& options)
{
    const auto rendered = RenderReport(document, options.Format);

    if (options.Output == "-")
    {
        std::cout << rendered;
    }
    else
    {
        std::ofstream file(options.Output, std::ios::binary | std::ios::trunc);
        file << rendered;
    }

    if (!options.Quiet && (options.Format == "plain" || options.Format == "markdown"))
    {
        for (const auto& diagnostic : document.Diagnostics)
        {
            std::cerr << "[" << ToString(diagnostic.Severity) << "] " << diagnostic.Message;
            if (!diagnostic.Context.empty())
            {
                std::cerr << " (" << diagnostic.Context << ")";
            }
            std::cerr << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"exyoki - ExyokiOffice command-line utility for OPC packages"};
    app.set_version_flag("--version", "exyoki " + std::string(ExyokiOffice::GetVersion()));
    app.require_subcommand(1);
    // XPath arguments (query command) routinely start with '/' or '//'; disable Windows-style
    // '/option' parsing so those values are captured as positionals rather than treated as flags.
    app.allow_windows_style_options(false);

    auto options = std::make_shared<GlobalOptions>();
    app.add_option("--format", options->Format, "Output format")
        ->check(CLI::IsMember({"plain", "markdown", "json", "xml"}))
        ->default_val("plain");
    // Every subcommand inherits this one through fallthrough(), and a subcommand
    // option of the same name would shadow it after the subcommand name while the
    // global still won before it - the same spelling meaning two things depending
    // on where it sits. Commands that write a document name that destination
    // "--out-package" (or a positional "outpackage") for that reason.
    app.add_option("--output", options->Output, "Report file path ('-' for stdout)")->default_val("-");
    app.add_option("--package-limits", options->PackageLimits,
                   "ZIP/XML safety limits applied to every package read: 'recommended' bounds entry counts, "
                   "sizes, compression ratio and XML nesting; 'unlimited' switches the guard off")
        ->check(CLI::IsMember({"recommended", "unlimited"}))
        ->default_val("recommended");
    app.add_flag("--quiet", options->Quiet, "Suppress human-readable diagnostics on stderr");

    // commands
    auto* commandsCmd =
        app.add_subcommand("commands",
                           "Describe every command, option and exit code in a machine-readable form")
            ->fallthrough();

    // parts
    auto partsPackage = std::make_shared<std::string>();
    auto partsSort = std::make_shared<std::string>("uri");
    auto* partsCmd = app.add_subcommand("parts", "List every part in a package")->fallthrough();
    partsCmd->add_option("package", *partsPackage, "Path to the .docx/.xlsx/.pptx package")->required();
    partsCmd->add_option("--sort", *partsSort, "Sort order")->check(CLI::IsMember({"uri", "size", "type"}));

    // relationships
    auto relPackage = std::make_shared<std::string>();
    auto relParts = std::make_shared<std::vector<std::string>>();
    auto relDanglingOnly = std::make_shared<bool>(false);
    auto* relCmd = app.add_subcommand("relationships", "List relationships in a package")->fallthrough();
    relCmd->add_option("package", *relPackage, "Path to the package")->required();
    relCmd->add_option("--part", *relParts, "Restrict to relationships whose source is this part URI (repeatable)");
    relCmd->add_flag("--dangling-only", *relDanglingOnly, "Only show relationships whose target is missing");

    // info
    auto infoPackage = std::make_shared<std::string>();
    auto infoPropsOnly = std::make_shared<bool>(false);
    auto* infoCmd = app.add_subcommand("info", "Show package summary and core properties")->fallthrough();
    infoCmd->add_option("package", *infoPackage, "Path to the package")->required();
    infoCmd->add_flag("--props-only", *infoPropsOnly, "Only show core/extended properties");

    // props get/set
    auto* propsCmd = app.add_subcommand("props", "Read or write core/extended document properties")->fallthrough();
    propsCmd->require_subcommand(1);

    auto propsGetPackage = std::make_shared<std::string>();
    auto* propsGetCmd = propsCmd->add_subcommand("get", "Read core/extended properties")->fallthrough();
    propsGetCmd->add_option("package", *propsGetPackage, "Path to the package")->required();

    auto propsSetPackage = std::make_shared<std::string>();
    auto propsSetTitle = std::make_shared<std::string>();
    auto propsSetCreator = std::make_shared<std::string>();
    auto propsSetSubject = std::make_shared<std::string>();
    auto propsSetKeywords = std::make_shared<std::string>();
    auto propsSetCustom = std::make_shared<std::vector<std::string>>();
    auto propsSetOutput = std::make_shared<std::string>();
    auto* propsSetCmd = propsCmd->add_subcommand("set", "Write one or more core/extended properties")->fallthrough();
    propsSetCmd->add_option("package", *propsSetPackage, "Path to the package")->required();
    propsSetCmd->add_option("--title", *propsSetTitle, "Set the Title property");
    propsSetCmd->add_option("--creator", *propsSetCreator, "Set the Creator property");
    propsSetCmd->add_option("--subject", *propsSetSubject, "Set the Subject property");
    propsSetCmd->add_option("--keywords", *propsSetKeywords, "Set the Keywords property");
    propsSetCmd->add_option("--set", *propsSetCustom, "Set an arbitrary property as name=value (repeatable)");
    propsSetCmd->add_option("--out-package", *propsSetOutput, "Save to a different package instead of in-place");

    // validate
    auto validatePackages = std::make_shared<std::vector<std::string>>();
    auto validateVersion = std::make_shared<std::string>("365");
    auto validateNoDom = std::make_shared<bool>(false);
    auto validateCrossCheck = std::make_shared<bool>(false);
    auto validateMaxIssues = std::make_shared<int>(-1);
    auto validateErrorsOnly = std::make_shared<bool>(false);
    auto validateWarningsAsErrors = std::make_shared<bool>(false);
    auto* validateCmd = app.add_subcommand("validate", "Validate OPC structure and (optionally) DOM/schema rules")->fallthrough();
    validateCmd
        ->add_option("package", *validatePackages,
                     "Package path(s); '*' and '?' wildcards in the filename are expanded")
        ->required()
        ->expected(1, -1);
    validateCmd->add_option("--office-version", *validateVersion, "Target Office generation")
        ->default_val("365")
        ->check(CLI::IsMember({"2007", "2010", "2013", "2016", "2019", "2021", "365"}));
    validateCmd->add_flag("--no-dom", *validateNoDom, "Skip per-part DOM/schema validation");
    validateCmd->add_flag("--cross-check-content-model", *validateCrossCheck,
                          "Check every content-model verdict against the reference matcher (slow)");
    validateCmd->add_option("--max-issues", *validateMaxIssues, "Cap the number of reported issues");
    validateCmd->add_flag("--errors-only", *validateErrorsOnly, "Only report errors");
    validateCmd->add_flag("--warnings-as-errors", *validateWarningsAsErrors, "Treat warnings as errors");

    // signatures
    auto signaturesPackage = std::make_shared<std::string>();
    auto* signaturesCmd =
        app.add_subcommand("signatures", "List digital signatures and check whether the signed content changed")
            ->fallthrough();
    signaturesCmd->add_option("package", *signaturesPackage, "Path to the package")->required();

    // external
    auto externalPackage = std::make_shared<std::string>();
    auto* externalCmd =
        app.add_subcommand("external", "List resources the package references from outside itself")->fallthrough();
    externalCmd->add_option("package", *externalPackage, "Path to the package")->required();

    // unpack
    auto unpackPackage = std::make_shared<std::string>();
    auto unpackOutDir = std::make_shared<std::string>();
    auto unpackPretty = std::make_shared<bool>(false);
    auto unpackOverwrite = std::make_shared<bool>(false);
    auto* unpackCmd = app.add_subcommand("unpack", "Extract a package's ZIP entries to a directory")->fallthrough();
    unpackCmd->add_option("package", *unpackPackage, "Path to the package")->required();
    unpackCmd->add_option("outdir", *unpackOutDir, "Output directory")->required();
    unpackCmd->add_flag("--pretty", *unpackPretty, "Re-indent XML/rels entries for readability");
    unpackCmd->add_flag("--overwrite", *unpackOverwrite, "Allow writing into a non-empty output directory");

    // pack
    auto packInDir = std::make_shared<std::string>();
    auto packOutPackage = std::make_shared<std::string>();
    auto packRegenerateContentTypes = std::make_shared<bool>(false);
    auto packValidate = std::make_shared<bool>(false);
    auto packCompression = std::make_shared<int>(6);
    auto packOverwrite = std::make_shared<bool>(false);
    auto* packCmd = app.add_subcommand("pack", "Rebuild a package from a directory tree (see unpack)")->fallthrough();
    packCmd->add_option("indir", *packInDir, "Input directory")->required();
    packCmd->add_option("outpackage", *packOutPackage, "Destination package path")->required();
    packCmd->add_flag("--regenerate-content-types", *packRegenerateContentTypes,
                      "Rebuild [Content_Types].xml instead of copying it verbatim");
    packCmd->add_flag("--validate", *packValidate, "Validate the freshly written package");
    packCmd->add_option("--compression", *packCompression, "Deflate compression level (0-9)")
        ->default_val(6)
        ->check(CLI::Range(0, 9));
    packCmd->add_flag("--overwrite", *packOverwrite, "Overwrite an existing destination package");

    // to-flat-opc / from-flat-opc
    auto toFlatOpcPackage = std::make_shared<std::string>();
    auto toFlatOpcOutFile = std::make_shared<std::string>();
    auto toFlatOpcNoPretty = std::make_shared<bool>(false);
    auto* toFlatOpcCmd = app.add_subcommand("to-flat-opc", "Convert a package to a single Flat OPC XML file")->fallthrough();
    toFlatOpcCmd->add_option("package", *toFlatOpcPackage, "Path to the package")->required();
    toFlatOpcCmd->add_option("outfile", *toFlatOpcOutFile, "Destination Flat OPC .xml file")->required();
    toFlatOpcCmd->add_flag("--no-pretty", *toFlatOpcNoPretty, "Write compact XML instead of indented");

    auto fromFlatOpcInFile = std::make_shared<std::string>();
    auto fromFlatOpcPackage = std::make_shared<std::string>();
    auto fromFlatOpcCompression = std::make_shared<int>(6);
    auto* fromFlatOpcCmd = app.add_subcommand("from-flat-opc", "Rebuild a package from a Flat OPC XML file")->fallthrough();
    fromFlatOpcCmd->add_option("flatopc", *fromFlatOpcInFile, "Path to the Flat OPC .xml file")->required();
    fromFlatOpcCmd->add_option("outpackage", *fromFlatOpcPackage, "Destination package path")->required();
    fromFlatOpcCmd->add_option("--compression", *fromFlatOpcCompression, "Deflate compression level (0-9)")
        ->default_val(6)
        ->check(CLI::Range(0, 9));

    // convert
    auto convertInput = std::make_shared<std::string>();
    auto convertOutput = std::make_shared<std::string>();
    auto convertFrom = std::make_shared<std::string>();
    auto convertTo = std::make_shared<std::string>();
    auto convertMediaDir = std::make_shared<std::string>();
    auto convertEmbedMedia = std::make_shared<bool>(false);
    auto convertNoMedia = std::make_shared<bool>(false);
    auto convertOverwrite = std::make_shared<bool>(false);
    auto* convertCmd =
        app.add_subcommand("convert",
                           "Convert between Office packages (docx/xlsx/pptx) and "
                           "Markdown/JSON/text/XML (plus CSV for workbooks)")
            ->fallthrough();
    convertCmd->add_option("input", *convertInput, "Source file (Office package or md/json/txt/xml)")
        ->required();
    convertCmd->add_option("outputfile", *convertOutput,
                           "Destination file; '-' writes a text-format result to stdout (requires --to)")
        ->required();
    convertCmd->add_option("--from", *convertFrom, "Input format (default: inferred from the extension)")
        ->check(CLI::IsMember({"docx", "xlsx", "pptx", "md", "json", "txt", "xml", "csv"}));
    convertCmd->add_option("--to", *convertTo, "Output format (default: inferred from the extension)")
        ->check(CLI::IsMember({"docx", "xlsx", "pptx", "md", "json", "txt", "xml", "csv"}));
    convertCmd->add_option("--media-dir", *convertMediaDir,
                           "Media directory (default: '<output stem>_media' when exporting, the input "
                           "directory when importing)");
    convertCmd->add_flag("--embed-media", *convertEmbedMedia,
                         "Embed media as base64 into json/xml instead of writing external files");
    convertCmd->add_flag("--no-media", *convertNoMedia, "Drop images and other media entirely");
    convertCmd->add_flag("--overwrite", *convertOverwrite, "Overwrite existing media files");
    auto convertSheet = std::make_shared<std::string>();
    auto convertCsvSeparator = std::make_shared<std::string>(",");
    convertCmd->add_option("--sheet", *convertSheet,
                           "CSV: worksheet to export (default: the first one) or the name of the "
                           "worksheet a CSV import creates");
    convertCmd->add_option("--csv-separator", *convertCsvSeparator, "CSV field separator")
        ->default_val(",");

    // export-media
    auto mediaPackage = std::make_shared<std::string>();
    auto mediaOutDir = std::make_shared<std::string>();
    auto mediaOverwrite = std::make_shared<bool>(false);
    auto* mediaCmd = app.add_subcommand("export-media", "Export images/audio/video/embedded objects")->fallthrough();
    mediaCmd->add_option("package", *mediaPackage, "Path to the package")->required();
    mediaCmd->add_option("outdir", *mediaOutDir, "Output directory")->required();
    mediaCmd->add_flag("--overwrite", *mediaOverwrite, "Overwrite existing files");

    // dedup
    auto dedupInput = std::make_shared<std::string>();
    auto dedupOutput = std::make_shared<std::string>();
    auto dedupDryRun = std::make_shared<bool>(false);
    auto dedupOverwrite = std::make_shared<bool>(false);
    auto dedupFonts = std::make_shared<bool>(false);
    auto dedupAllBinary = std::make_shared<bool>(false);
    auto* dedupCmd =
        app.add_subcommand("dedup", "Merge byte-identical shared resources (images/audio/video)")
            ->fallthrough();
    dedupCmd->add_option("package", *dedupInput, "Path to the package")->required();
    // Named "outpackage" rather than "output" so it cannot be confused with the global --output
    // option; CLI11 rejects a positional whose name matches a configurable option on the parent.
    dedupCmd->add_option("outpackage", *dedupOutput,
                         "Output package path (defaults to rewriting the input in place)");
    dedupCmd->add_flag("--dry-run", *dedupDryRun, "Only report duplicate groups; write nothing");
    dedupCmd->add_flag("--overwrite", *dedupOverwrite, "Overwrite an existing output file");
    dedupCmd->add_flag("--fonts", *dedupFonts, "Also merge embedded font parts");
    dedupCmd->add_flag("--all-binary", *dedupAllBinary,
                       "Merge every leaf binary part regardless of content type");

    // search
    auto searchPackage = std::make_shared<std::string>();
    auto searchNeedle = std::make_shared<std::string>();
    auto searchContext = std::make_shared<int>(40);
    auto searchIsRegex = std::make_shared<bool>(false);
    auto searchIgnoreCase = std::make_shared<bool>(false);
    auto* searchCmd =
        app.add_subcommand("search",
                           "Search text in a Word, Excel, or PowerPoint document (paragraphs, cells, "
                           "slide shapes, notes)")
            ->fallthrough();
    searchCmd->add_option("package", *searchPackage, "Path to the .docx/.xlsx/.pptx package")->required();
    searchCmd->add_option("needle", *searchNeedle, "Text to search for")->required();
    searchCmd->add_option("--context", *searchContext, "Characters of context on each side of a match")
        ->default_val(40);
    searchCmd->add_flag("--regex", *searchIsRegex, "Treat <needle> as an ECMAScript regular expression");
    searchCmd->add_flag("--ignore-case", *searchIgnoreCase, "Case-insensitive match (plain text or regex)");

    // query
    auto queryPackage = std::make_shared<std::string>();
    auto queryXpath = std::make_shared<std::string>();
    auto queryPart = std::make_shared<std::string>();
    auto queryNamespaces = std::make_shared<std::vector<std::string>>();
    auto queryMax = std::make_shared<int>(0);
    auto* queryCmd = app.add_subcommand("query", "Run a dynamic XPath query over an XML part of any OPC package")->fallthrough();
    queryCmd->add_option("package", *queryPackage, "Path to the .docx/.xlsx/.pptx package")->required();
    queryCmd->add_option("xpath", *queryXpath, "XPath 1.0 expression (prefixed names resolve by namespace)")->required();
    queryCmd->add_option("--part", *queryPart, "Part URI to query (default: the main document part)");
    queryCmd->add_option("--ns", *queryNamespaces, "Bind a namespace prefix as prefix=uri (repeatable)");
    queryCmd->add_option("--max", *queryMax, "Maximum matches to return (0 = unlimited)")->default_val(0);

    // extract-text
    auto extractPackage = std::make_shared<std::string>();
    auto* extractCmd = app.add_subcommand("extract-text", "Extract all readable text from a Word/Excel/PowerPoint package")->fallthrough();
    extractCmd->add_option("package", *extractPackage, "Path to the package")->required();

    // stat
    auto statPackage = std::make_shared<std::string>();
    auto* statCmd = app.add_subcommand(
                           "stat", "Compute content statistics (words, paragraphs, tables, ...) for a Word/Excel/PowerPoint package")
                        ->fallthrough();
    statCmd->add_option("package", *statPackage, "Path to the .docx/.xlsx/.pptx package")->required();

    // replace
    auto replacePackage = std::make_shared<std::string>();
    auto replaceNeedle = std::make_shared<std::string>();
    auto replaceReplacement = std::make_shared<std::string>();
    auto replaceDryRun = std::make_shared<bool>(false);
    auto replaceOutput = std::make_shared<std::string>();
    auto replaceIsRegex = std::make_shared<bool>(false);
    auto replaceIgnoreCase = std::make_shared<bool>(false);
    auto* replaceCmd =
        app.add_subcommand("replace", "Replace text across a Word, Excel, or PowerPoint document")
            ->fallthrough();
    replaceCmd->add_option("package", *replacePackage, "Path to the .docx/.xlsx/.pptx package")->required();
    replaceCmd->add_option("needle", *replaceNeedle, "Text to find")->required();
    replaceCmd->add_option("replacement", *replaceReplacement,
                           "Replacement text; may reference capture groups as $1, $2, ... when --regex is set")
        ->required();
    replaceCmd->add_flag("--dry-run", *replaceDryRun, "Only count matches; never modify or save the document");
    replaceCmd->add_option("--out-package", *replaceOutput, "Save to a different package instead of in-place");
    replaceCmd->add_flag("--regex", *replaceIsRegex, "Treat <needle> as an ECMAScript regular expression");
    replaceCmd->add_flag("--ignore-case", *replaceIgnoreCase, "Case-insensitive match (plain text or regex)");

    // split
    auto splitPackage = std::make_shared<std::string>();
    auto splitOutDir = std::make_shared<std::string>();
    auto splitBy = std::make_shared<std::string>("auto");
    auto splitCount = std::make_shared<ExyokiOffice::Size>(0);
    auto splitMarker = std::make_shared<std::string>();
    auto splitPrefix = std::make_shared<std::string>("part");
    auto splitOverwrite = std::make_shared<bool>(false);
    auto* splitCmd = app.add_subcommand("split", "Split a Word, Excel, or PowerPoint document")->fallthrough();
    splitCmd->add_option("package", *splitPackage, "Path to the input Office package")->required();
    splitCmd->add_option("outdir", *splitOutDir, "Output directory")->required();
    splitCmd->add_option("--by", *splitBy, "Split unit: auto, section, page, paragraphs, marker, worksheets, or slides")
        ->default_val("auto")
        ->check(CLI::IsMember({"auto", "section", "page", "paragraphs", "marker", "worksheets", "slides"}));
    splitCmd->add_option("--count", *splitCount, "Paragraphs, worksheets, or slides per output document");
    splitCmd->add_option("--marker", *splitMarker, "Start a new document at paragraphs containing this text");
    splitCmd->add_option("--prefix", *splitPrefix, "Output filename prefix")->default_val("part");
    splitCmd->add_flag("--overwrite", *splitOverwrite, "Overwrite existing output files");

    // merge
    auto mergeInputs = std::make_shared<std::vector<std::string>>();
    auto mergeOutput = std::make_shared<std::string>();
    auto mergeNoPageBreaks = std::make_shared<bool>(false);
    auto mergeOverwrite = std::make_shared<bool>(false);
    auto mergeStyleConflict = std::make_shared<std::string>("rename");
    auto* mergeCmd = app.add_subcommand("merge", "Merge same-family Word, Excel, or PowerPoint documents")->fallthrough();
    mergeCmd->add_option("inputs", *mergeInputs, "Input Office packages")->required()->expected(1, -1);
    mergeCmd->add_option("--out-package", *mergeOutput, "Destination Office package")->required();
    mergeCmd->add_flag("--no-page-breaks", *mergeNoPageBreaks, "Word only: do not insert page breaks between documents");
    mergeCmd->add_flag("--overwrite", *mergeOverwrite, "Overwrite an existing destination");
    mergeCmd->add_option("--style-conflict", *mergeStyleConflict, "Style conflict policy")
        ->default_val("rename")
        ->check(CLI::IsMember({"rename", "keep", "replace"}));

    // diff
    auto diffLeft = std::make_shared<std::string>();
    auto diffRight = std::make_shared<std::string>();
    auto diffNoNormalize = std::make_shared<bool>(false);
    auto diffPartsOnly = std::make_shared<bool>(false);
    auto* diffCmd = app.add_subcommand("diff", "Compare two packages part-by-part and relationship-by-relationship")->fallthrough();
    diffCmd->add_option("left", *diffLeft, "First package")->required();
    diffCmd->add_option("right", *diffRight, "Second package")->required();
    diffCmd->add_flag("--no-normalize", *diffNoNormalize, "Compare XML parts as raw bytes instead of normalized trees");
    diffCmd->add_flag("--parts-only", *diffPartsOnly, "Omit relationship changes from the report");

    // compare
    auto compareOriginal = std::make_shared<std::string>();
    auto compareRevised = std::make_shared<std::string>();
    auto compareOutput = std::make_shared<std::string>();
    auto compareAuthor = std::make_shared<std::string>("exyoki");
    auto* compareCmd =
        app.add_subcommand("compare",
                           "Compare two Word documents and write the differences as tracked revisions")
            ->fallthrough();
    compareCmd->add_option("original", *compareOriginal, "Original .docx document")->required();
    compareCmd->add_option("revised", *compareRevised, "Revised .docx document")->required();
    compareCmd->add_option("--out-package", *compareOutput, "Destination .docx with tracked revisions")
        ->required();
    compareCmd->add_option("--author", *compareAuthor, "Author recorded on the generated revisions")
        ->default_val("exyoki");

    // redact
    auto redactPackage = std::make_shared<std::string>();
    auto redactOutput = std::make_shared<std::string>();
    auto redactKeepComments = std::make_shared<bool>(false);
    auto redactKeepRevisions = std::make_shared<bool>(false);
    auto redactKeepHidden = std::make_shared<bool>(false);
    auto redactKeepMetadata = std::make_shared<bool>(false);
    auto* redactCmd =
        app.add_subcommand("redact",
                           "Remove comments, tracked revisions, hidden text, and personal metadata "
                           "before publication")
            ->fallthrough();
    redactCmd->add_option("package", *redactPackage, "Path to the .docx/.xlsx/.pptx package")->required();
    redactCmd->add_option("--out-package", *redactOutput, "Save to a different package instead of in-place");
    redactCmd->add_flag("--keep-comments", *redactKeepComments, "Do not remove comments");
    redactCmd->add_flag("--keep-revisions", *redactKeepRevisions,
                        "Word only: do not accept tracked revisions");
    redactCmd->add_flag("--keep-hidden-text", *redactKeepHidden, "Word only: do not delete hidden text");
    redactCmd->add_flag("--keep-metadata", *redactKeepMetadata,
                        "Do not clear Creator/LastModifiedBy/Company or remove custom properties");

    // fill
    auto fillPackage = std::make_shared<std::string>();
    auto fillData = std::make_shared<std::string>();
    auto fillOutput = std::make_shared<std::string>();
    auto* fillCmd =
        app.add_subcommand("fill", "Fill a Word mail-merge template (MERGEFIELD/bookmarks) from JSON data")
            ->fallthrough();
    fillCmd->add_option("package", *fillPackage, "Path to the .docx template")->required();
    fillCmd->add_option("data", *fillData, "JSON data file (object; arrays of objects drive regions)")
        ->required();
    fillCmd->add_option("--out-package", *fillOutput,
                        "Save to a different package instead of overwriting the template");

    // recalc
    auto recalcPackage = std::make_shared<std::string>();
    auto recalcSheet = std::make_shared<std::string>();
    auto recalcOutput = std::make_shared<std::string>();
    auto recalcDryRun = std::make_shared<bool>(false);
    auto* recalcCmd =
        app.add_subcommand("recalc", "Recalculate workbook formulas and rewrite the cached results")
            ->fallthrough();
    recalcCmd->add_option("package", *recalcPackage, "Path to the .xlsx package")->required();
    recalcCmd->add_option("--sheet", *recalcSheet, "Recalculate only this worksheet");
    recalcCmd->add_option("--out-package", *recalcOutput, "Save to a different package instead of in-place");
    recalcCmd->add_flag("--dry-run", *recalcDryRun, "Evaluate and report without saving");

    // schema
    auto schemaCheck = std::make_shared<std::string>();
    auto* schemaCmd =
        app.add_subcommand("schema",
                           "Print the JSON Schema of the exyokioffice-document model, or check a "
                           "document against it")
            ->fallthrough();
    schemaCmd->add_option("--check", *schemaCheck,
                          "Validate this JSON envelope against the schema instead of printing it");

    // completions
    auto completionsShell = std::make_shared<std::string>();
    auto* completionsCmd =
        app.add_subcommand("completions", "Print a shell completion script for bash, zsh, or PowerShell")
            ->fallthrough();
    completionsCmd->add_option("shell", *completionsShell, "Target shell")
        ->required()
        ->check(CLI::IsMember({"bash", "zsh", "powershell", "pwsh"}));

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
    if (!ApplyPackageLimits(options->PackageLimits, options->Quiet))
    {
        std::cerr << "[error] Unknown --package-limits value '" << options->PackageLimits << "'.\n";
        return static_cast<int>(ExitCode::UsageError);
    }

    // Command execution below can throw (I/O errors, corrupt packages, allocation failures, ...);
    // catch here so failures produce a clean message and exit code instead of std::terminate().
    try
    {
        if (*commandsCmd)
        {
            const auto document = exyoki::AdaptCommands(app);
            EmitReport(document, *options);
            return document.Status == "ok" ? static_cast<int>(ExitCode::Ok)
                                           : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*partsCmd)
        {
            OpenXmlPackage package;
            if (!package.LoadFromFile(*partsPackage))
            {
                EmitReport(exyoki::AdaptParts({}, *partsSort), *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }
            EmitReport(exyoki::AdaptParts(ListParts(package), *partsSort), *options);
            return static_cast<int>(ExitCode::Ok);
        }

        if (*relCmd)
        {
            OpenXmlPackage package;
            if (!package.LoadFromFile(*relPackage))
            {
                EmitReport(exyoki::AdaptRelationships({}, *relDanglingOnly), *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }
            auto relationships = ListRelationships(package);
            if (!relParts->empty())
            {
                std::vector<RelationshipRecord> filtered;
                for (auto& relationship : relationships)
                {
                    if (std::find(relParts->begin(), relParts->end(), relationship.SourceUri) != relParts->end())
                    {
                        filtered.push_back(relationship);
                    }
                }
                relationships = std::move(filtered);
            }
            EmitReport(exyoki::AdaptRelationships(relationships, *relDanglingOnly), *options);
            return static_cast<int>(ExitCode::Ok);
        }

        if (*infoCmd)
        {
            OpenXmlPackage package;
            if (!package.LoadFromFile(*infoPackage))
            {
                EmitReport(exyoki::AdaptInfo({}, *infoPropsOnly), *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }
            EmitReport(exyoki::AdaptInfo(GetInfo(package), *infoPropsOnly), *options);
            return static_cast<int>(ExitCode::Ok);
        }

        if (*propsGetCmd)
        {
            OpenXmlPackage package;
            if (!package.LoadFromFile(*propsGetPackage))
            {
                EmitReport(exyoki::AdaptPropsGet({}), *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }
            EmitReport(exyoki::AdaptPropsGet(ReadCoreProperties(package)), *options);
            return static_cast<int>(ExitCode::Ok);
        }

        if (*propsSetCmd)
        {
            OpenXmlPackage package;
            if (!package.LoadFromFile(*propsSetPackage))
            {
                EmitReport(exyoki::AdaptPropsSet(false, "", ""), *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }

            std::vector<std::pair<std::string, std::string>> updates;
            if (!propsSetTitle->empty())
            {
                updates.emplace_back("Title", *propsSetTitle);
            }
            if (!propsSetCreator->empty())
            {
                updates.emplace_back("Creator", *propsSetCreator);
            }
            if (!propsSetSubject->empty())
            {
                updates.emplace_back("Subject", *propsSetSubject);
            }
            if (!propsSetKeywords->empty())
            {
                updates.emplace_back("Keywords", *propsSetKeywords);
            }
            for (const auto& entry : *propsSetCustom)
            {
                const auto equals = entry.find('=');
                if (equals != std::string::npos)
                {
                    updates.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
                }
            }

            bool allOk = true;
            std::string lastName;
            std::string lastValue;
            for (const auto& [name, value] : updates)
            {
                const bool ok = WriteCoreProperty(package, name, value);
                allOk = allOk && ok;
                lastName = name;
                lastValue = value;
            }

            if (allOk && !updates.empty())
            {
                const auto destination = propsSetOutput->empty() ? *propsSetPackage : *propsSetOutput;
                allOk = package.SaveToFile(destination);
            }

            EmitReport(exyoki::AdaptPropsSet(allOk, lastName, lastValue), *options);
            return allOk ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*signaturesCmd)
        {
            const auto report = InspectSignatures(std::filesystem::path(*signaturesPackage));
            EmitReport(exyoki::AdaptSignatures(report), *options);

            if (!report.Loaded)
            {
                return static_cast<int>(ExitCode::OperationFailed);
            }
            // No signature at all is not a failure; a broken one is.
            if (report.Result.HasSignatures() && !report.Result.AllContentIntact())
            {
                return static_cast<int>(ExitCode::SignatureInvalid);
            }
            return static_cast<int>(ExitCode::Ok);
        }

        if (*externalCmd)
        {
            // The command line has no resolver, so this only lists what the
            // package claims to reference. Nothing outside it is touched.
            const auto report = InspectExternalResources(std::filesystem::path(*externalPackage));
            EmitReport(exyoki::AdaptExternal(report), *options);
            return report.Loaded ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*validateCmd)
        {
            ValidationRunOptions runOptions;
            runOptions.TargetVersion = *ParseFileFormatVersion(*validateVersion);
            runOptions.RunDomValidation = !*validateNoDom;
            runOptions.CrossCheckContentModel = *validateCrossCheck;

            std::optional<ExyokiOffice::Size> maxIssues;
            if (*validateMaxIssues >= 0)
            {
                maxIssues = static_cast<ExyokiOffice::Size>(*validateMaxIssues);
            }

            std::vector<ToolDiagnostic> expansionDiagnostics;
            const auto files = ExpandInputPaths(*validatePackages, expansionDiagnostics);
            if (files.empty())
            {
                ReportDocument document;
                document.Command = "validate";
                document.Status = "error";
                document.Diagnostics = expansionDiagnostics;
                document.Diagnostics.push_back(
                    ToolDiagnostic{ToolSeverity::Error, "No input files to validate"});
                EmitReport(document, *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }

            std::vector<std::pair<std::string, ValidationReport>> reports;
            reports.reserve(files.size());
            for (const auto& file : files)
            {
                reports.emplace_back(file.string(), Run(file, runOptions));
            }

            bool anyLoadFailure = false;
            bool anyValidationFailure = false;
            for (const auto& [file, report] : reports)
            {
                anyLoadFailure = anyLoadFailure || !report.Loaded;
                anyValidationFailure =
                    anyValidationFailure || report.ErrorCount > 0 ||
                    (*validateWarningsAsErrors && report.WarningCount > 0);
            }

            // A single file keeps the established single-file report shape;
            // several files produce the batch report with per-file entries.
            auto document =
                reports.size() == 1
                    ? exyoki::AdaptValidate(reports.front().second, *validateErrorsOnly,
                                            *validateWarningsAsErrors, maxIssues)
                    : exyoki::AdaptValidateBatch(reports, *validateErrorsOnly, *validateWarningsAsErrors,
                                                 maxIssues);
            document.Diagnostics.insert(document.Diagnostics.begin(), expansionDiagnostics.begin(),
                                        expansionDiagnostics.end());
            EmitReport(document, *options);

            if (anyLoadFailure)
            {
                return static_cast<int>(ExitCode::OperationFailed);
            }
            return anyValidationFailure ? static_cast<int>(ExitCode::ValidationErrors)
                                        : static_cast<int>(ExitCode::Ok);
        }

        if (*unpackCmd)
        {
            UnpackOptions unpackOptions;
            unpackOptions.PrettyPrintXml = *unpackPretty;
            unpackOptions.Overwrite = *unpackOverwrite;
            const auto result = Unpack(*unpackPackage, *unpackOutDir, unpackOptions);
            EmitReport(exyoki::AdaptUnpack(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*packCmd)
        {
            PackOptions packOptions;
            packOptions.RegenerateContentTypes = *packRegenerateContentTypes;
            packOptions.ValidateAfterPack = *packValidate;
            packOptions.CompressionLevel = *packCompression;
            packOptions.Overwrite = *packOverwrite;
            const auto result = Pack(*packInDir, *packOutPackage, packOptions);
            EmitReport(exyoki::AdaptPack(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*toFlatOpcCmd)
        {
            ToFlatOpcOptions conversionOptions;
            conversionOptions.PrettyPrint = !*toFlatOpcNoPretty;
            const auto result = ConvertToFlatOpc(*toFlatOpcPackage, *toFlatOpcOutFile, conversionOptions);
            EmitReport(exyoki::AdaptToFlatOpc(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*fromFlatOpcCmd)
        {
            FromFlatOpcOptions conversionOptions;
            conversionOptions.CompressionLevel = *fromFlatOpcCompression;
            const auto result = ConvertFromFlatOpc(*fromFlatOpcInFile, *fromFlatOpcPackage, conversionOptions);
            EmitReport(exyoki::AdaptFromFlatOpc(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*convertCmd)
        {
            ConvertOptions convertOptions;
            convertOptions.From = ParseConvertFormat(*convertFrom);
            convertOptions.To = ParseConvertFormat(*convertTo);
            if (!convertMediaDir->empty())
            {
                convertOptions.MediaDir = *convertMediaDir;
            }
            convertOptions.EmbedMedia = *convertEmbedMedia;
            convertOptions.NoMedia = *convertNoMedia;
            convertOptions.Overwrite = *convertOverwrite;
            convertOptions.CsvSeparator = *convertCsvSeparator;
            convertOptions.SheetName = *convertSheet;

            const auto result = ConvertDocument(*convertInput, *convertOutput, convertOptions);

            if (*convertOutput == "-" && result.Ok)
            {
                // The converted payload itself goes to stdout; diagnostics stay on stderr.
                std::cout << result.OutputText;
                if (!options->Quiet)
                {
                    for (const auto& diagnostic : result.Diagnostics)
                    {
                        std::cerr << "[" << ToString(diagnostic.Severity) << "] " << diagnostic.Message;
                        if (!diagnostic.Context.empty())
                        {
                            std::cerr << " (" << diagnostic.Context << ")";
                        }
                        std::cerr << "\n";
                    }
                }
                return static_cast<int>(ExitCode::Ok);
            }

            EmitReport(exyoki::AdaptConvert(result, *convertInput, *convertOutput), *options);
            if (result.Ok)
            {
                return static_cast<int>(ExitCode::Ok);
            }
            return result.UsageOk ? static_cast<int>(ExitCode::OperationFailed)
                                  : static_cast<int>(ExitCode::UsageError);
        }

        if (*mediaCmd)
        {
            OpenXmlPackage package;
            if (!package.LoadFromFile(*mediaPackage))
            {
                EmitReport(exyoki::AdaptExportMedia({}), *options);
                return static_cast<int>(ExitCode::OperationFailed);
            }
            const auto result = ExportMedia(package, *mediaOutDir, *mediaOverwrite);
            EmitReport(exyoki::AdaptExportMedia(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*dedupCmd)
        {
            ResourceDeduplicationOptions dedupOptions;
            dedupOptions.DryRun = *dedupDryRun;
            dedupOptions.Overwrite = *dedupOverwrite;
            dedupOptions.IncludeFonts = *dedupFonts;
            dedupOptions.IncludeAllBinaryParts = *dedupAllBinary;
            const auto destination = dedupOutput->empty() ? *dedupInput : *dedupOutput;
            const auto result = DeduplicateSharedResources(*dedupInput, destination, dedupOptions);
            EmitReport(exyoki::AdaptDedup(result, dedupOptions.DryRun), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*queryCmd)
        {
            QueryOptions queryOptions;
            queryOptions.PartName = *queryPart;
            queryOptions.MaxMatches = static_cast<ExyokiOffice::Size>(std::max(0, *queryMax));
            for (const auto& binding : *queryNamespaces)
            {
                const auto separator = binding.find('=');
                if (separator == std::string::npos)
                {
                    EmitReport(exyoki::AdaptQuery([&]
                                                  {
                               QueryResult invalid;
                               invalid.Diagnostics.push_back(ExyokiOffice::Tools::ToolDiagnostic{
                                   ExyokiOffice::Tools::ToolSeverity::Error,
                                   "Namespace binding must be in prefix=uri form", binding});
                               return invalid; }()),
                               *options);
                    return static_cast<int>(ExitCode::UsageError);
                }
                queryOptions.NamespaceBindings.emplace_back(binding.substr(0, separator),
                                                            binding.substr(separator + 1));
            }

            const auto result = Query(*queryPackage, *queryXpath, queryOptions);
            EmitReport(exyoki::AdaptQuery(result), *options);
            if (!result.Ok)
            {
                return static_cast<int>(ExitCode::OperationFailed);
            }
            return result.Matches.empty() ? static_cast<int>(ExitCode::QueryNoMatch)
                                          : static_cast<int>(ExitCode::Ok);
        }

        if (*searchCmd)
        {
            const auto result =
                SearchDocumentText(*searchPackage, *searchNeedle,
                                   static_cast<ExyokiOffice::Size>(*searchContext), *searchIsRegex,
                                   *searchIgnoreCase);
            EmitReport(exyoki::AdaptSearch(result), *options);
            if (!result.Ok)
            {
                return static_cast<int>(ExitCode::OperationFailed);
            }
            return result.Matches.empty() ? static_cast<int>(ExitCode::SearchNoMatch) : static_cast<int>(ExitCode::Ok);
        }

        if (*extractCmd)
        {
            const auto result = Extract(*extractPackage);
            EmitReport(exyoki::AdaptExtractText(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*statCmd)
        {
            const auto result = Stat(*statPackage);
            EmitReport(exyoki::AdaptStat(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*replaceCmd)
        {
            const auto result =
                ReplaceDocumentText(*replacePackage, *replaceNeedle, *replaceReplacement, *replaceDryRun,
                                    *replaceOutput, *replaceIsRegex, *replaceIgnoreCase);
            EmitReport(exyoki::AdaptReplace(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*splitCmd)
        {
            DocumentSplitOptions splitOptions;
            splitOptions.ItemCount = *splitCount;
            splitOptions.Marker = *splitMarker;
            splitOptions.OutputPrefix = *splitPrefix;
            splitOptions.Overwrite = *splitOverwrite;
            if (*splitBy == "section")
            {
                splitOptions.Strategy = DocumentSplitStrategy::SectionBreaks;
            }
            else if (*splitBy == "page")
            {
                splitOptions.Strategy = DocumentSplitStrategy::PageBreaks;
            }
            else if (*splitBy == "paragraphs")
            {
                splitOptions.Strategy = DocumentSplitStrategy::ParagraphCount;
            }
            else if (*splitBy == "marker")
            {
                splitOptions.Strategy = DocumentSplitStrategy::Marker;
            }
            else if (*splitBy == "worksheets")
            {
                splitOptions.Strategy = DocumentSplitStrategy::Worksheets;
            }
            else if (*splitBy == "slides")
            {
                splitOptions.Strategy = DocumentSplitStrategy::Slides;
            }
            const auto result = SplitDocument(*splitPackage, *splitOutDir, splitOptions);
            EmitReport(exyoki::AdaptSplit(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*mergeCmd)
        {
            std::vector<std::filesystem::path> inputs;
            inputs.reserve(mergeInputs->size());
            for (const auto& input : *mergeInputs)
            {
                inputs.emplace_back(input);
            }
            DocumentMergeOptions mergeOptions;
            mergeOptions.InsertWordPageBreaks = !*mergeNoPageBreaks;
            mergeOptions.Overwrite = *mergeOverwrite;
            if (*mergeStyleConflict == "keep")
            {
                mergeOptions.WordStyleConflictPolicy = ExyokiOffice::Word::StyleCopyConflictPolicy::KeepExisting;
            }
            else if (*mergeStyleConflict == "replace")
            {
                mergeOptions.WordStyleConflictPolicy = ExyokiOffice::Word::StyleCopyConflictPolicy::Replace;
            }
            const auto result = MergeDocuments(inputs, *mergeOutput, mergeOptions);
            EmitReport(exyoki::AdaptMerge(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*diffCmd)
        {
            const auto result = Compare(*diffLeft, *diffRight, !*diffNoNormalize);
            EmitReport(exyoki::AdaptDiff(result, *diffPartsOnly), *options);
            if (!result.Ok)
            {
                return static_cast<int>(ExitCode::OperationFailed);
            }
            return result.Identical ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::DiffDifferent);
        }

        if (*compareCmd)
        {
            const auto result =
                CompareWordDocuments(*compareOriginal, *compareRevised, *compareOutput, *compareAuthor);
            EmitReport(exyoki::AdaptCompare(result), *options);
            if (!result.Ok)
            {
                return static_cast<int>(ExitCode::OperationFailed);
            }
            return result.Identical ? static_cast<int>(ExitCode::Ok)
                                    : static_cast<int>(ExitCode::DiffDifferent);
        }

        if (*redactCmd)
        {
            RedactOptions redactOptions;
            redactOptions.RemoveComments = !*redactKeepComments;
            redactOptions.ResolveRevisions = !*redactKeepRevisions;
            redactOptions.RemoveHiddenText = !*redactKeepHidden;
            redactOptions.RemovePersonalMetadata = !*redactKeepMetadata;
            const auto result = RedactDocument(*redactPackage, *redactOutput, redactOptions);
            EmitReport(exyoki::AdaptRedact(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*fillCmd)
        {
            const auto result = FillWordTemplate(*fillPackage, *fillData, *fillOutput);
            EmitReport(exyoki::AdaptFill(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*recalcCmd)
        {
            const auto result =
                RecalculateWorkbook(*recalcPackage, *recalcSheet, *recalcOutput, *recalcDryRun);
            EmitReport(exyoki::AdaptRecalc(result), *options);
            return result.Ok ? static_cast<int>(ExitCode::Ok) : static_cast<int>(ExitCode::OperationFailed);
        }

        if (*schemaCmd)
        {
            if (!schemaCheck->empty())
            {
                std::ifstream input(*schemaCheck, std::ios::binary);
                if (!input)
                {
                    std::cerr << "exyoki: cannot read '" << *schemaCheck << "'\n";
                    return static_cast<int>(ExitCode::OperationFailed);
                }
                const std::string content((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());

                std::vector<ToolDiagnostic> violations;
                const bool valid = ValidateModelJson(content, violations);
                EmitReport(exyoki::AdaptSchemaCheck(*schemaCheck, valid, violations), *options);
                return valid ? static_cast<int>(ExitCode::Ok)
                             : static_cast<int>(ExitCode::ValidationErrors);
            }

            // The schema is the payload itself, not a report envelope; --format
            // does not apply, while --output still selects the destination.
            const auto schema = GetDocumentModelJsonSchema();
            if (options->Output == "-")
            {
                std::cout << schema;
            }
            else
            {
                std::ofstream file(options->Output, std::ios::binary | std::ios::trunc);
                file << schema;
            }
            return static_cast<int>(ExitCode::Ok);
        }

        if (*completionsCmd)
        {
            // The script is the payload itself, not a report envelope; --format
            // does not apply, while --output still selects the destination.
            const auto script = exyoki::GenerateCompletionScript(app, *completionsShell);
            if (script.empty())
            {
                std::cerr << "exyoki: unsupported shell '" << *completionsShell << "'\n";
                return static_cast<int>(ExitCode::UsageError);
            }
            if (options->Output == "-")
            {
                std::cout << script;
            }
            else
            {
                std::ofstream file(options->Output, std::ios::binary | std::ios::trunc);
                file << script;
            }
            return static_cast<int>(ExitCode::Ok);
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
