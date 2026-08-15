// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Commands.hpp"

#include "CommandCatalog.hpp"
#include "Completions.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <utility>

namespace exyoki
{

using ExyokiOffice::OpenXmlPackage;

bool ReportEmitter::ApplyPackageLimits(const std::string& value, bool quiet)
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

std::string ReportEmitter::Render(const ReportDocument& document, const std::string& format)
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

void ReportEmitter::WritePayload(const std::string& payload, const GlobalOptions& options)
{
    if (options.Output == "-")
    {
        std::cout << payload;
        return;
    }

    std::ofstream file(options.Output, std::ios::binary | std::ios::trunc);
    file << payload;
}

void ReportEmitter::WriteDiagnostics(const std::vector<ToolDiagnostic>& diagnostics, bool quiet)
{
    if (quiet)
    {
        return;
    }

    for (const auto& diagnostic : diagnostics)
    {
        std::cerr << "[" << ToString(diagnostic.Severity) << "] " << diagnostic.Message;
        if (!diagnostic.Context.empty())
        {
            std::cerr << " (" << diagnostic.Context << ")";
        }
        std::cerr << "\n";
    }
}

void ReportEmitter::Emit(const ReportDocument& document, const GlobalOptions& options)
{
    WritePayload(Render(document, options.Format), options);

    // The other formats are consumed by a program, which reads the diagnostics
    // out of the document itself rather than off the error stream.
    if (options.Format == "plain" || options.Format == "markdown")
    {
        WriteDiagnostics(document.Diagnostics, options.Quiet);
    }
}

int ReportEmitter::Finish(const CommandOutcome& outcome, const GlobalOptions& options)
{
    if (!outcome.WroteOwnOutput)
    {
        Emit(outcome.Document, options);
    }

    return static_cast<int>(outcome.Code);
}

CommandOutcome CommandsCommand::Run(const CommandContext& context) const
{
    auto document = AdaptCommands(context.Parser);
    const auto code = document.Status == "ok" ? ExitCode::Ok : ExitCode::OperationFailed;
    return {std::move(document), code};
}

CommandOutcome PartsCommand::Run(const CommandContext&) const
{
    OpenXmlPackage package;
    if (!package.LoadFromFile(Package))
    {
        return {AdaptParts({}, Sort), ExitCode::OperationFailed};
    }

    return {AdaptParts(ListParts(package), Sort)};
}

CommandOutcome RelationshipsCommand::Run(const CommandContext&) const
{
    OpenXmlPackage package;
    if (!package.LoadFromFile(Package))
    {
        return {AdaptRelationships({}, DanglingOnly), ExitCode::OperationFailed};
    }

    auto relationships = ListRelationships(package);
    if (!Parts.empty())
    {
        std::erase_if(relationships, [this](const RelationshipRecord& relationship)
                      { return std::find(Parts.begin(), Parts.end(), relationship.SourceUri) == Parts.end(); });
    }

    return {AdaptRelationships(relationships, DanglingOnly)};
}

CommandOutcome InfoCommand::Run(const CommandContext&) const
{
    OpenXmlPackage package;
    if (!package.LoadFromFile(Package))
    {
        return {AdaptInfo({}, PropsOnly), ExitCode::OperationFailed};
    }

    return {AdaptInfo(GetInfo(package), PropsOnly)};
}

CommandOutcome PropsGetCommand::Run(const CommandContext&) const
{
    OpenXmlPackage package;
    if (!package.LoadFromFile(Package))
    {
        return {AdaptPropsGet({}, {}), ExitCode::OperationFailed};
    }

    return {AdaptPropsGet(ReadCoreProperties(package), ReadCustomProperties(package))};
}

CommandOutcome PropsSetCommand::Run(const CommandContext&) const
{
    OpenXmlPackage package;
    if (!package.LoadFromFile(Package))
    {
        return {AdaptPropsSet(false, "", ""), ExitCode::OperationFailed};
    }

    // The four named options map onto core property names; everything else
    // arrives through --set already spelled the way it is stored.
    const std::array<std::pair<const char*, const std::string*>, 4> named{
        {{"Title", &Title}, {"Creator", &Creator}, {"Subject", &Subject}, {"Keywords", &Keywords}}};

    std::vector<std::pair<std::string, std::string>> updates;
    for (const auto& [name, value] : named)
    {
        if (!value->empty())
        {
            updates.emplace_back(name, *value);
        }
    }

    for (const auto& entry : Custom)
    {
        const auto equals = entry.find('=');
        if (equals != std::string::npos)
        {
            updates.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        }
    }

    // The report names the last property written, so an empty --set list is
    // reported as a successful no-op rather than as a write of nothing.
    bool allOk = true;
    std::string lastName;
    std::string lastValue;
    for (const auto& [name, value] : updates)
    {
        allOk = WriteCoreProperty(package, name, value) && allOk;
        lastName = name;
        lastValue = value;
    }

    if (allOk && !updates.empty())
    {
        allOk = package.SaveToFile(OutPackage.empty() ? Package : OutPackage);
    }

    return {AdaptPropsSet(allOk, lastName, lastValue), ReportEmitter::OkOrFailed(allOk)};
}

CommandOutcome ValidateCommand::Run(const CommandContext&) const
{
    ValidationRunOptions runOptions;
    runOptions.TargetVersion = *ParseFileFormatVersion(OfficeVersion);
    runOptions.RunDomValidation = !NoDom;
    runOptions.CrossCheckContentModel = CrossCheckContentModel;

    std::optional<ExyokiOffice::Size> maxIssues;
    if (MaxIssues >= 0)
    {
        maxIssues = static_cast<ExyokiOffice::Size>(MaxIssues);
    }

    std::vector<ToolDiagnostic> expansionDiagnostics;
    const auto files = ExpandInputPaths(Packages, expansionDiagnostics);
    if (files.empty())
    {
        ReportDocument document;
        document.Command = "validate";
        document.Status = "error";
        document.Diagnostics = expansionDiagnostics;
        document.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "No input files to validate"});
        return {std::move(document), ExitCode::OperationFailed};
    }

    std::vector<std::pair<std::string, ValidationReport>> reports;
    reports.reserve(files.size());
    for (const auto& file : files)
    {
        reports.emplace_back(file.string(), ExyokiOffice::Tools::Run(file, runOptions));
    }

    bool anyLoadFailure = false;
    bool anyValidationFailure = false;
    for (const auto& [file, report] : reports)
    {
        anyLoadFailure = anyLoadFailure || !report.Loaded;
        anyValidationFailure = anyValidationFailure || report.ErrorCount > 0 ||
                               (WarningsAsErrors && report.WarningCount > 0);
    }

    // A single file keeps the established single-file report shape; several
    // files produce the batch report with per-file entries.
    auto document = reports.size() == 1
                        ? AdaptValidate(reports.front().second, ErrorsOnly, WarningsAsErrors, maxIssues)
                        : AdaptValidateBatch(reports, ErrorsOnly, WarningsAsErrors, maxIssues);
    document.Diagnostics.insert(document.Diagnostics.begin(), expansionDiagnostics.begin(),
                                expansionDiagnostics.end());

    if (anyLoadFailure)
    {
        return {std::move(document), ExitCode::OperationFailed};
    }

    return {std::move(document), anyValidationFailure ? ExitCode::ValidationErrors : ExitCode::Ok};
}

CommandOutcome SignaturesCommand::Run(const CommandContext&) const
{
    const auto report = InspectSignatures(std::filesystem::path(Package));
    auto document = AdaptSignatures(report);

    if (!report.Loaded)
    {
        return {std::move(document), ExitCode::OperationFailed};
    }

    // No signature at all is not a failure; a broken one is.
    if (report.Result.HasSignatures() && !report.Result.AllContentIntact())
    {
        return {std::move(document), ExitCode::SignatureInvalid};
    }

    return {std::move(document)};
}

CommandOutcome ExternalCommand::Run(const CommandContext&) const
{
    // The command line has no resolver, so this only lists what the package
    // claims to reference. Nothing outside it is touched.
    const auto report = InspectExternalResources(std::filesystem::path(Package));
    return {AdaptExternal(report), ReportEmitter::OkOrFailed(report.Loaded)};
}

CommandOutcome UnpackCommand::Run(const CommandContext&) const
{
    UnpackOptions unpackOptions;
    unpackOptions.PrettyPrintXml = Pretty;
    unpackOptions.Overwrite = Overwrite;

    const auto result = Unpack(Package, OutDir, unpackOptions);
    return {AdaptUnpack(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome PackCommand::Run(const CommandContext&) const
{
    PackOptions packOptions;
    packOptions.RegenerateContentTypes = RegenerateContentTypes;
    packOptions.ValidateAfterPack = Validate;
    packOptions.CompressionLevel = Compression;
    packOptions.Overwrite = Overwrite;

    const auto result = Pack(InDir, OutPackage, packOptions);
    return {AdaptPack(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome ToFlatOpcCommand::Run(const CommandContext&) const
{
    ToFlatOpcOptions conversionOptions;
    conversionOptions.PrettyPrint = !NoPretty;

    const auto result = ConvertToFlatOpc(Package, OutFile, conversionOptions);
    return {AdaptToFlatOpc(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome FromFlatOpcCommand::Run(const CommandContext&) const
{
    FromFlatOpcOptions conversionOptions;
    conversionOptions.CompressionLevel = Compression;

    const auto result = ConvertFromFlatOpc(FlatOpc, OutPackage, conversionOptions);
    return {AdaptFromFlatOpc(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome ConvertCommand::Run(const CommandContext& context) const
{
    ConvertOptions convertOptions;
    convertOptions.From = ParseConvertFormat(From);
    convertOptions.To = ParseConvertFormat(To);
    if (!MediaDir.empty())
    {
        convertOptions.MediaDir = MediaDir;
    }
    convertOptions.EmbedMedia = EmbedMedia;
    convertOptions.NoMedia = NoMedia;
    convertOptions.Overwrite = Overwrite;
    convertOptions.CsvSeparator = CsvSeparator;
    convertOptions.SheetName = Sheet;

    const auto result = ConvertDocument(Input, Output, convertOptions);

    if (Output == "-" && result.Ok)
    {
        // The converted payload itself goes to stdout; diagnostics stay on stderr.
        std::cout << result.OutputText;
        ReportEmitter::WriteDiagnostics(result.Diagnostics, context.Options.Quiet);
        return {{}, ExitCode::Ok, true};
    }

    auto document = AdaptConvert(result, Input, Output);
    if (result.Ok)
    {
        return {std::move(document)};
    }

    // A usage mistake is the caller's, not the document's.
    return {std::move(document), result.UsageOk ? ExitCode::OperationFailed : ExitCode::UsageError};
}

CommandOutcome ExportMediaCommand::Run(const CommandContext&) const
{
    OpenXmlPackage package;
    if (!package.LoadFromFile(Package))
    {
        return {AdaptExportMedia({}), ExitCode::OperationFailed};
    }

    const auto result = ExportMedia(package, OutDir, Overwrite);
    return {AdaptExportMedia(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome DedupCommand::Run(const CommandContext&) const
{
    ResourceDeduplicationOptions dedupOptions;
    dedupOptions.DryRun = DryRun;
    dedupOptions.Overwrite = Overwrite;
    dedupOptions.IncludeFonts = Fonts;
    dedupOptions.IncludeAllBinaryParts = AllBinary;

    const auto result =
        DeduplicateSharedResources(Package, OutPackage.empty() ? Package : OutPackage, dedupOptions);
    return {AdaptDedup(result, dedupOptions.DryRun), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome QueryCommand::Run(const CommandContext&) const
{
    QueryOptions queryOptions;
    queryOptions.PartName = Part;
    queryOptions.MaxMatches = static_cast<ExyokiOffice::Size>(std::max(0, Max));
    for (const auto& binding : Namespaces)
    {
        const auto separator = binding.find('=');
        if (separator == std::string::npos)
        {
            QueryResult invalid;
            invalid.Diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Namespace binding must be in prefix=uri form", binding});
            return {AdaptQuery(invalid), ExitCode::UsageError};
        }

        queryOptions.NamespaceBindings.emplace_back(binding.substr(0, separator),
                                                    binding.substr(separator + 1));
    }

    const auto result = Query(Package, Xpath, queryOptions);
    auto document = AdaptQuery(result);
    if (!result.Ok)
    {
        return {std::move(document), ExitCode::OperationFailed};
    }

    // Matching nothing is not an error, but it is worth its own exit code.
    return {std::move(document), result.Matches.empty() ? ExitCode::QueryNoMatch : ExitCode::Ok};
}

CommandOutcome SearchCommand::Run(const CommandContext&) const
{
    const auto result = SearchDocumentText(Package, Needle, static_cast<ExyokiOffice::Size>(Context), Regex,
                                           IgnoreCase);
    auto document = AdaptSearch(result);
    if (!result.Ok)
    {
        return {std::move(document), ExitCode::OperationFailed};
    }

    return {std::move(document), result.Matches.empty() ? ExitCode::SearchNoMatch : ExitCode::Ok};
}

CommandOutcome ExtractTextCommand::Run(const CommandContext&) const
{
    const auto result = Extract(Package);
    return {AdaptExtractText(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome StatCommand::Run(const CommandContext&) const
{
    const auto result = Stat(Package);
    return {AdaptStat(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome ReplaceCommand::Run(const CommandContext&) const
{
    const auto result =
        ReplaceDocumentText(Package, Needle, Replacement, DryRun, OutPackage, Regex, IgnoreCase);
    return {AdaptReplace(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome SplitCommand::Run(const CommandContext&) const
{
    DocumentSplitOptions splitOptions;
    splitOptions.ItemCount = Count;
    splitOptions.Marker = Marker;
    splitOptions.OutputPrefix = Prefix;
    splitOptions.Overwrite = Overwrite;

    // "auto" leaves the strategy at its default, which is what the option means.
    if (By == "section")
    {
        splitOptions.Strategy = DocumentSplitStrategy::SectionBreaks;
    }
    else if (By == "page")
    {
        splitOptions.Strategy = DocumentSplitStrategy::PageBreaks;
    }
    else if (By == "paragraphs")
    {
        splitOptions.Strategy = DocumentSplitStrategy::ParagraphCount;
    }
    else if (By == "marker")
    {
        splitOptions.Strategy = DocumentSplitStrategy::Marker;
    }
    else if (By == "worksheets")
    {
        splitOptions.Strategy = DocumentSplitStrategy::Worksheets;
    }
    else if (By == "slides")
    {
        splitOptions.Strategy = DocumentSplitStrategy::Slides;
    }

    const auto result = SplitDocument(Package, OutDir, splitOptions);
    return {AdaptSplit(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome MergeCommand::Run(const CommandContext&) const
{
    std::vector<std::filesystem::path> inputs;
    inputs.reserve(Inputs.size());
    for (const auto& input : Inputs)
    {
        inputs.emplace_back(input);
    }

    DocumentMergeOptions mergeOptions;
    mergeOptions.InsertWordPageBreaks = !NoPageBreaks;
    mergeOptions.Overwrite = Overwrite;
    if (StyleConflict == "keep")
    {
        mergeOptions.WordStyleConflictPolicy = ExyokiOffice::Word::StyleCopyConflictPolicy::KeepExisting;
    }
    else if (StyleConflict == "replace")
    {
        mergeOptions.WordStyleConflictPolicy = ExyokiOffice::Word::StyleCopyConflictPolicy::Replace;
    }

    const auto result = MergeDocuments(inputs, OutPackage, mergeOptions);
    return {AdaptMerge(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome DiffCommand::Run(const CommandContext&) const
{
    const auto result = Compare(Left, Right, !NoNormalize);
    auto document = AdaptDiff(result, PartsOnly);
    if (!result.Ok)
    {
        return {std::move(document), ExitCode::OperationFailed};
    }

    return {std::move(document), result.Identical ? ExitCode::Ok : ExitCode::DiffDifferent};
}

CommandOutcome CompareCommand::Run(const CommandContext&) const
{
    const auto result = CompareWordDocuments(Original, Revised, OutPackage, Author);
    auto document = AdaptCompare(result);
    if (!result.Ok)
    {
        return {std::move(document), ExitCode::OperationFailed};
    }

    return {std::move(document), result.Identical ? ExitCode::Ok : ExitCode::DiffDifferent};
}

CommandOutcome RedactCommand::Run(const CommandContext&) const
{
    RedactOptions redactOptions;
    redactOptions.RemoveComments = !KeepComments;
    redactOptions.ResolveRevisions = !KeepRevisions;
    redactOptions.RemoveHiddenText = !KeepHiddenText;
    redactOptions.RemovePersonalMetadata = !KeepMetadata;

    const auto result = RedactDocument(Package, OutPackage, redactOptions);
    return {AdaptRedact(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome FillCommand::Run(const CommandContext&) const
{
    const auto result = FillWordTemplate(Package, Data, OutPackage);
    return {AdaptFill(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome RecalcCommand::Run(const CommandContext&) const
{
    const auto result = RecalculateWorkbook(Package, Sheet, OutPackage, DryRun);
    return {AdaptRecalc(result), ReportEmitter::OkOrFailed(result.Ok)};
}

CommandOutcome SchemaCommand::Run(const CommandContext& context) const
{
    if (!Check.empty())
    {
        std::ifstream input(Check, std::ios::binary);
        if (!input)
        {
            std::cerr << "exyoki: cannot read '" << Check << "'\n";
            return {{}, ExitCode::OperationFailed, true};
        }

        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

        std::vector<ToolDiagnostic> violations;
        const bool valid = ValidateModelJson(content, violations);
        return {AdaptSchemaCheck(Check, valid, violations),
                valid ? ExitCode::Ok : ExitCode::ValidationErrors};
    }

    // The schema is the payload itself, not a report envelope; --format does
    // not apply, while --output still selects the destination.
    ReportEmitter::WritePayload(GetDocumentModelJsonSchema(), context.Options);
    return {{}, ExitCode::Ok, true};
}

CommandOutcome CompletionsCommand::Run(const CommandContext& context) const
{
    // The script is the payload itself, not a report envelope; --format does
    // not apply, while --output still selects the destination.
    const auto script = GenerateCompletionScript(context.Parser, Shell);
    if (script.empty())
    {
        std::cerr << "exyoki: unsupported shell '" << Shell << "'\n";
        return {{}, ExitCode::UsageError, true};
    }

    ReportEmitter::WritePayload(script, context.Options);
    return {{}, ExitCode::Ok, true};
}

} // namespace exyoki
