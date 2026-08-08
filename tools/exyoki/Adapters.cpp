// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Adapters.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>

namespace exyoki
{

namespace
{

std::string FormatReadingTime(ExyokiOffice::Real minutes)
{
    if (minutes < 1.0)
    {
        auto seconds = static_cast<ExyokiOffice::Int64>(std::llround(minutes * 60.0));
        if (seconds < 0)
        {
            seconds = 0;
        }
        return std::to_string(seconds) + " sec";
    }
    return std::to_string(static_cast<ExyokiOffice::Int64>(std::llround(minutes))) + " min";
}

void AppendDiagnostics(ReportDocument& document, const std::vector<ToolDiagnostic>& diagnostics)
{
    document.Diagnostics.insert(document.Diagnostics.end(), diagnostics.begin(), diagnostics.end());
}

ReportNode PartToNode(const PartRecord& part)
{
    auto node = ReportNode::MakeObject();
    node.Set("uri", part.Uri);
    node.Set("contentType", part.ContentType);
    node.Set("kind", part.Kind == PartPayloadKind::Xml ? std::string("xml") : std::string("binary"));
    node.Set("size", static_cast<ExyokiOffice::UInt64>(part.Size));
    node.Set("descriptor", part.DescriptorName);
    node.Set("incomingRelationshipCount", static_cast<ExyokiOffice::UInt64>(part.Incoming.size()));
    return node;
}

ReportNode RelationshipToNode(const RelationshipRecord& relationship)
{
    auto node = ReportNode::MakeObject();
    node.Set("sourceUri", relationship.SourceUri);
    node.Set("id", relationship.Relationship.Id);
    node.Set("type", relationship.Relationship.Type);
    node.Set("target", relationship.Relationship.Target);
    node.Set("targetMode", relationship.Relationship.TargetMode);
    node.Set("isExternal", relationship.Relationship.IsExternal);
    node.Set("resolvedTargetUri", relationship.ResolvedTargetUri);
    node.Set("targetExists", relationship.TargetExists);
    return node;
}

ReportNode ValidationIssueToNode(const ExyokiOffice::ValidationIssue& issue)
{
    auto node = ReportNode::MakeObject();
    node.Set("severity", std::string(ExyokiOffice::Tools::ToString(issue.Severity)));
    node.Set("domain", std::string(ExyokiOffice::Tools::ToString(issue.Domain)));
    node.Set("errorId", std::string(ExyokiOffice::Tools::ToString(issue.Id)));
    node.Set("message", issue.Message);
    if (!issue.PartUri.empty())
    {
        node.Set("partUri", issue.PartUri);
    }
    if (issue.Location.IsValid())
    {
        node.Set("xmlPath", issue.Location.Path);
        if (!issue.Location.AttributeName.empty())
        {
            node.Set("attributeName", issue.Location.AttributeName);
        }
    }
    if (!issue.ConstraintId.empty())
    {
        node.Set("constraintId", issue.ConstraintId);
    }
    if (!issue.RelationshipId.empty())
    {
        node.Set("relationshipId", issue.RelationshipId);
        node.Set("relationshipSourceUri", issue.RelationshipSourceUri);
        node.Set("relationshipType", issue.RelationshipType);
        node.Set("targetUri", issue.TargetUri);
    }
    return node;
}

ReportNode CorePropertiesToNode(const CoreProperties& properties)
{
    auto node = ReportNode::MakeObject();
    node.Set("title", properties.Title);
    node.Set("subject", properties.Subject);
    node.Set("creator", properties.Creator);
    node.Set("keywords", properties.Keywords);
    node.Set("description", properties.Description);
    node.Set("lastModifiedBy", properties.LastModifiedBy);
    node.Set("category", properties.Category);
    node.Set("contentStatus", properties.ContentStatus);
    node.Set("created", properties.Created);
    node.Set("modified", properties.Modified);
    node.Set("application", properties.Application);
    node.Set("appVersion", properties.AppVersion);
    node.Set("company", properties.Company);
    return node;
}

} // namespace

ReportDocument AdaptParts(const std::vector<PartRecord>& parts, const std::string& sortBy)
{
    ReportDocument document;
    document.Command = "parts";

    auto sorted = parts;
    if (sortBy == "size")
    {
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                         { return a.Size > b.Size; });
    }
    else if (sortBy == "type")
    {
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto& a, const auto& b)
                         { return a.ContentType < b.ContentType; });
    }
    else
    {
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                         { return a.Uri < b.Uri; });
    }

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"uri", "contentType", "kind", "size", "descriptor"});
    for (const auto& part : sorted)
    {
        array.Push(PartToNode(part));
    }
    document.Data.Set("partCount", static_cast<ExyokiOffice::UInt64>(sorted.size()));
    document.Data.Set("parts", std::move(array));
    return document;
}

ReportDocument AdaptRelationships(const std::vector<RelationshipRecord>& relationships, bool danglingOnly)
{
    ReportDocument document;
    document.Command = "relationships";

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"sourceUri", "id", "type", "target", "targetExists"});
    ExyokiOffice::Size danglingCount = 0;
    for (const auto& relationship : relationships)
    {
        const bool isDangling = !relationship.Relationship.IsExternal && !relationship.TargetExists;
        if (isDangling)
        {
            ++danglingCount;
        }
        if (danglingOnly && !isDangling)
        {
            continue;
        }
        array.Push(RelationshipToNode(relationship));
    }

    document.Data.Set("relationshipCount", static_cast<ExyokiOffice::UInt64>(relationships.size()));
    document.Data.Set("danglingCount", static_cast<ExyokiOffice::UInt64>(danglingCount));
    document.Data.Set("relationships", std::move(array));
    return document;
}

ReportDocument AdaptInfo(const PackageInfo& info, bool propsOnly)
{
    ReportDocument document;
    document.Command = "info";

    if (!propsOnly)
    {
        document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(info.Family)));
        document.Data.Set("documentType", info.DocumentTypeName);
        // Reported as a field, not only as prose, so a caller can tell an
        // unsupported conformance class from a genuinely unrecognized package.
        document.Data.Set("strictConformance", info.IsStrictConformance);
        document.Data.Set("mainPartUri", info.MainPartUri);
        document.Data.Set("mainPartContentType", info.MainPartContentType);
        document.Data.Set("partCount", static_cast<ExyokiOffice::UInt64>(info.PartCount));
        document.Data.Set("relationshipCount", static_cast<ExyokiOffice::UInt64>(info.RelationshipCount));
        document.Data.Set("totalPartSize", static_cast<ExyokiOffice::UInt64>(info.TotalPartSize));
    }
    document.Data.Set("properties", CorePropertiesToNode(info.Properties));
    if (info.IsStrictConformance)
    {
        document.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Warning, DescribeUnknownFamily(info)});
    }
    return document;
}

ReportDocument AdaptValidate(const ValidationReport& report, bool errorsOnly, bool warningsAsErrors,
                             std::optional<ExyokiOffice::Size> maxIssues)
{
    ReportDocument document;
    document.Command = "validate";

    auto allIssues = report.LoadIssues;
    allIssues.insert(allIssues.end(), report.ValidationIssues.begin(), report.ValidationIssues.end());

    ExyokiOffice::Size errorCount = 0;
    ExyokiOffice::Size warningCount = 0;
    auto array = ReportNode::MakeArray();
    array.SetTableHint({"severity", "domain", "errorId", "message", "partUri", "xmlPath"});
    for (auto issue : allIssues)
    {
        const bool isError = issue.Severity == ExyokiOffice::ValidationSeverity::Error ||
                             (warningsAsErrors && issue.Severity == ExyokiOffice::ValidationSeverity::Warning);
        if (isError)
        {
            ++errorCount;
            issue.Severity = ExyokiOffice::ValidationSeverity::Error;
        }
        else
        {
            ++warningCount;
        }

        if (errorsOnly && !isError)
        {
            continue;
        }
        if (maxIssues && array.AsArray().size() >= *maxIssues)
        {
            continue;
        }
        array.Push(ValidationIssueToNode(issue));
    }

    document.Data.Set("loaded", report.Loaded);
    document.Data.Set("errorCount", static_cast<ExyokiOffice::UInt64>(errorCount));
    document.Data.Set("warningCount", static_cast<ExyokiOffice::UInt64>(warningCount));
    document.Data.Set("issues", std::move(array));
    document.Status = report.Loaded ? "ok" : "error";
    return document;
}

/// Renders a check outcome as a word rather than a number.
std::string SignatureCheckToString(ExyokiOffice::Security::SignatureCheck check)
{
    switch (check)
    {
        case ExyokiOffice::Security::SignatureCheck::Valid:
            return "valid";
        case ExyokiOffice::Security::SignatureCheck::Invalid:
            return "invalid";
        case ExyokiOffice::Security::SignatureCheck::NotChecked:
            break;
    }
    return "notChecked";
}

ReportDocument AdaptSignatures(const SignatureInspectionReport& report)
{
    ReportDocument document;
    document.Command = "signatures";
    document.Status = report.Loaded ? "ok" : "error";

    auto signatures = ReportNode::MakeArray();
    signatures.SetTableHint({"partUri", "signatureId", "contentIntegrity", "signatureValue", "signingTime"});
    for (const auto& signature : report.Result.Signatures)
    {
        auto node = ReportNode::MakeObject();
        node.Set("partUri", signature.PartUri);
        node.Set("signatureId", signature.SignatureId);
        node.Set("contentIntegrity", SignatureCheckToString(signature.ContentIntegrity));
        node.Set("signatureValue", SignatureCheckToString(signature.SignatureValue));
        node.Set("signingTime", signature.SigningTimeText);
        node.Set("certificateCount", static_cast<ExyokiOffice::UInt64>(signature.Certificates.size()));
        if (signature.Algorithm)
        {
            node.Set("signatureAlgorithm",
                     std::string(ExyokiOffice::Security::GetSignatureAlgorithmUri(*signature.Algorithm)));
        }
        if (signature.Digest)
        {
            node.Set("digestAlgorithm",
                     std::string(ExyokiOffice::Security::GetDigestAlgorithmUri(*signature.Digest)));
        }

        auto references = ReportNode::MakeArray();
        references.SetTableHint({"uri", "partUri", "digest", "message"});
        for (const auto& reference : signature.References)
        {
            auto referenceNode = ReportNode::MakeObject();
            referenceNode.Set("uri", reference.Uri);
            referenceNode.Set("partUri", reference.PartUri);
            referenceNode.Set("digest", SignatureCheckToString(reference.Digest));
            referenceNode.Set("message", reference.Message);
            references.Push(std::move(referenceNode));
        }
        node.Set("references", std::move(references));
        signatures.Push(std::move(node));
    }

    document.Data.Set("signatureCount", static_cast<ExyokiOffice::UInt64>(report.Result.Signatures.size()));
    if (report.Result.HasSignatures())
    {
        // Only meaningful when there is something to be intact.
        document.Data.Set("contentIntact", report.Result.AllContentIntact());
    }
    document.Data.Set("signatures", std::move(signatures));

    auto issues = ReportNode::MakeArray();
    issues.SetTableHint({"severity", "domain", "errorId", "message", "partUri", "xmlPath"});
    for (const auto& issue : report.Result.Diagnostics.Issues())
    {
        issues.Push(ValidationIssueToNode(issue));
    }
    document.Data.Set("issues", std::move(issues));
    return document;
}

ReportDocument AdaptExternal(const ExternalResourceReport& report)
{
    ReportDocument document;
    document.Command = "external";
    document.Status = report.Loaded ? "ok" : "error";

    auto references = ReportNode::MakeArray();
    references.SetTableHint({"sourcePartUri", "relationshipId", "kind", "target"});
    for (const auto& reference : report.References)
    {
        auto node = ReportNode::MakeObject();
        node.Set("sourcePartUri", reference.SourcePartUri);
        node.Set("relationshipId", reference.RelationshipId);
        node.Set("kind", reference.Kind);
        node.Set("target", reference.Target);
        node.Set("relationshipType", reference.RelationshipType);
        references.Push(std::move(node));
    }

    document.Data.Set("referenceCount", static_cast<ExyokiOffice::UInt64>(report.Count()));
    document.Data.Set("references", std::move(references));
    AppendDiagnostics(document, report.Diagnostics);
    return document;
}

ReportDocument AdaptUnpack(const UnpackResult& result)
{
    ReportDocument document;
    document.Command = "unpack";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("entryCount", static_cast<ExyokiOffice::UInt64>(result.EntryCount));
    document.Data.Set("manifestWritten", result.ManifestWritten);
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptPack(const PackResult& result)
{
    ReportDocument document;
    document.Command = "pack";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("entryCount", static_cast<ExyokiOffice::UInt64>(result.EntryCount));
    document.Data.Set("contentTypesRegenerated", result.ContentTypesRegenerated);
    if (result.Validation.Loaded || !result.Validation.ValidationIssues.empty())
    {
        document.Data.Set("validationErrorCount", static_cast<ExyokiOffice::UInt64>(result.Validation.ErrorCount));
        document.Data.Set("validationWarningCount", static_cast<ExyokiOffice::UInt64>(result.Validation.WarningCount));
    }
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptToFlatOpc(const ToFlatOpcResult& result)
{
    ReportDocument document;
    document.Command = "to-flat-opc";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("partCount", static_cast<ExyokiOffice::UInt64>(result.PartCount));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptFromFlatOpc(const FromFlatOpcResult& result)
{
    ReportDocument document;
    document.Command = "from-flat-opc";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("partCount", static_cast<ExyokiOffice::UInt64>(result.PartCount));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptExportMedia(const MediaExportResult& result)
{
    ReportDocument document;
    document.Command = "export-media";
    document.Status = result.Ok ? "ok" : "error";

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"partUri", "outputPath", "contentType", "size"});
    for (const auto& item : result.Items)
    {
        auto node = ReportNode::MakeObject();
        node.Set("partUri", item.PartUri);
        node.Set("outputPath", item.OutputPath.string());
        node.Set("contentType", item.ContentType);
        node.Set("size", static_cast<ExyokiOffice::UInt64>(item.Size));
        array.Push(std::move(node));
    }
    document.Data.Set("itemCount", static_cast<ExyokiOffice::UInt64>(result.Items.size()));
    document.Data.Set("items", std::move(array));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptSearch(const DocumentSearchResult& result)
{
    ReportDocument document;
    document.Command = "search";
    document.Status = result.Ok ? "ok" : "error";

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"label", "offset", "matchText", "context"});
    for (const auto& match : result.Matches)
    {
        auto node = ReportNode::MakeObject();
        node.Set("label", match.Label);
        node.Set("offset", static_cast<ExyokiOffice::UInt64>(match.Offset));
        node.Set("length", static_cast<ExyokiOffice::UInt64>(match.Length));
        node.Set("matchText", match.MatchText);
        node.Set("context", match.Context);
        array.Push(std::move(node));
    }
    document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(result.Family)));
    document.Data.Set("matchCount", static_cast<ExyokiOffice::UInt64>(result.Matches.size()));
    document.Data.Set("matches", std::move(array));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptExtractText(const ExtractedDocumentText& result)
{
    ReportDocument document;
    document.Command = "extract-text";
    document.Status = result.Ok ? "ok" : "error";

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"label", "text"});
    for (const auto& block : result.Blocks)
    {
        auto node = ReportNode::MakeObject();
        node.Set("label", block.Label);
        node.Set("text", block.Text);
        array.Push(std::move(node));
    }
    document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(result.Family)));
    document.Data.Set("blockCount", static_cast<ExyokiOffice::UInt64>(result.Blocks.size()));
    document.Data.Set("blocks", std::move(array));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptReplace(const DocumentReplaceResult& result)
{
    ReportDocument document;
    document.Command = "replace";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(result.Family)));
    document.Data.Set("replacementCount", static_cast<ExyokiOffice::UInt64>(result.ReplacementCount));
    document.Data.Set("skippedNonTextMatches", static_cast<ExyokiOffice::UInt64>(result.SkippedMatches));
    document.Data.Set("saved", result.Saved);
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptSplit(const WordSplitResult& result)
{
    ReportDocument document;
    document.Command = "split";
    document.Status = result.Ok ? "ok" : "error";
    auto files = ReportNode::MakeArray();
    files.SetTableHint({"path"});
    for (const auto& path : result.OutputFiles)
    {
        auto node = ReportNode::MakeObject();
        node.Set("path", path.string());
        files.Push(std::move(node));
    }
    document.Data.Set("splitCount", static_cast<ExyokiOffice::UInt64>(result.OutputFiles.size()));
    document.Data.Set("files", std::move(files));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptMerge(const WordMergeResult& result)
{
    ReportDocument document;
    document.Command = "merge";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("documentsMerged", static_cast<ExyokiOffice::UInt64>(result.DocumentsMerged));
    document.Data.Set("outputFile", result.OutputFile.string());
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptSplit(const DocumentSplitResult& result)
{
    ReportDocument document;
    document.Command = "split";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("family", std::string(ToString(result.Family)));
    auto files = ReportNode::MakeArray();
    files.SetTableHint({"path"});
    for (const auto& path : result.OutputFiles)
    {
        auto node = ReportNode::MakeObject();
        node.Set("path", path.string());
        files.Push(std::move(node));
    }
    document.Data.Set("splitCount", static_cast<ExyokiOffice::UInt64>(result.OutputFiles.size()));
    document.Data.Set("files", std::move(files));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptMerge(const DocumentMergeResult& result)
{
    ReportDocument document;
    document.Command = "merge";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("family", std::string(ToString(result.Family)));
    document.Data.Set("documentsMerged", static_cast<ExyokiOffice::UInt64>(result.DocumentsMerged));
    document.Data.Set("itemsMerged", static_cast<ExyokiOffice::UInt64>(result.ItemsMerged));
    document.Data.Set("outputFile", result.OutputFile.string());
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptDiff(const DiffResult& result, bool partsOnly)
{
    ReportDocument document;
    document.Command = "diff";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("identical", result.Identical);

    auto partArray = ReportNode::MakeArray();
    partArray.SetTableHint({"uri", "kind", "leftContentType", "rightContentType", "firstDifferencePath"});
    for (const auto& change : result.PartChanges)
    {
        auto node = ReportNode::MakeObject();
        node.Set("uri", change.Uri);
        std::string kind;
        switch (change.Kind)
        {
            case PartChangeKind::Added:
                kind = "added";
                break;
            case PartChangeKind::Removed:
                kind = "removed";
                break;
            case PartChangeKind::ChangedXml:
                kind = "changedXml";
                break;
            case PartChangeKind::ChangedBinary:
                kind = "changedBinary";
                break;
            case PartChangeKind::ContentTypeChanged:
                kind = "contentTypeChanged";
                break;
        }
        node.Set("kind", kind);
        node.Set("leftContentType", change.LeftContentType);
        node.Set("rightContentType", change.RightContentType);
        node.Set("firstDifferencePath", change.FirstDifferencePath);
        partArray.Push(std::move(node));
    }
    document.Data.Set("partChangeCount", static_cast<ExyokiOffice::UInt64>(result.PartChanges.size()));
    document.Data.Set("partChanges", std::move(partArray));

    if (!partsOnly)
    {
        auto relationshipArray = ReportNode::MakeArray();
        relationshipArray.SetTableHint({"sourceUri", "id", "kind"});
        for (const auto& change : result.RelationshipChanges)
        {
            auto node = ReportNode::MakeObject();
            node.Set("sourceUri", change.SourceUri);
            node.Set("id", change.RelationshipId);
            std::string kind;
            switch (change.Kind)
            {
                case RelationshipChangeKind::Added:
                    kind = "added";
                    break;
                case RelationshipChangeKind::Removed:
                    kind = "removed";
                    break;
                case RelationshipChangeKind::Changed:
                    kind = "changed";
                    break;
            }
            node.Set("kind", kind);
            node.Set("leftTarget", change.Left.Target);
            node.Set("rightTarget", change.Right.Target);
            relationshipArray.Push(std::move(node));
        }
        document.Data.Set("relationshipChangeCount", static_cast<ExyokiOffice::UInt64>(result.RelationshipChanges.size()));
        document.Data.Set("relationshipChanges", std::move(relationshipArray));
    }

    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptPropsGet(const CoreProperties& properties)
{
    ReportDocument document;
    document.Command = "props get";
    document.Data = CorePropertiesToNode(properties);
    return document;
}

ReportDocument AdaptPropsSet(bool ok, const std::string& name, const std::string& value)
{
    ReportDocument document;
    document.Command = "props set";
    document.Status = ok ? "ok" : "error";
    document.Data.Set("name", name);
    document.Data.Set("value", value);
    document.Data.Set("updated", ok);
    if (!ok)
    {
        document.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Unknown property name or package has no properties part to update",
                           name});
    }
    return document;
}

ReportDocument AdaptStat(const DocumentStats& result)
{
    ReportDocument document;
    document.Command = "stat";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(result.Family)));

    const auto setIfPresent = [&](const char* key, const std::optional<ExyokiOffice::UInt64>& value)
    {
        if (value)
        {
            document.Data.Set(key, *value);
        }
    };

    setIfPresent("wordCount", result.WordCount);
    setIfPresent("characterCount", result.CharacterCount);
    setIfPresent("paragraphCount", result.ParagraphCount);
    setIfPresent("headingCount", result.HeadingCount);
    setIfPresent("equationCount", result.EquationCount);
    setIfPresent("footnoteCount", result.FootnoteCount);
    setIfPresent("endnoteCount", result.EndnoteCount);
    setIfPresent("bookmarkCount", result.BookmarkCount);
    setIfPresent("sectionCount", result.SectionCount);

    setIfPresent("worksheetCount", result.WorksheetCount);
    setIfPresent("cellCount", result.CellCount);
    setIfPresent("formulaCount", result.FormulaCount);
    setIfPresent("mergedRangeCount", result.MergedRangeCount);

    setIfPresent("slideCount", result.SlideCount);
    setIfPresent("hiddenSlideCount", result.HiddenSlideCount);
    setIfPresent("slideWithNotesCount", result.SlideWithNotesCount);
    setIfPresent("shapeCount", result.ShapeCount);
    setIfPresent("chartCount", result.ChartCount);

    setIfPresent("tableCount", result.TableCount);
    setIfPresent("imageCount", result.ImageCount);
    setIfPresent("hyperlinkCount", result.HyperlinkCount);
    setIfPresent("commentCount", result.CommentCount);

    if (result.ReadingTimeMinutes)
    {
        document.Data.Set("readingTimeMinutes", *result.ReadingTimeMinutes);
        document.Data.Set("readingTimeText", FormatReadingTime(*result.ReadingTimeMinutes));
    }

    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptQuery(const QueryResult& result)
{
    ReportDocument document;
    document.Command = "query";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("part", result.PartName);

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"path", "name", "attributes", "text"});
    for (const auto& match : result.Matches)
    {
        std::string attributes;
        for (const auto& [name, value] : match.Attributes)
        {
            if (!attributes.empty())
            {
                attributes += "; ";
            }
            attributes += name;
            attributes += "=";
            attributes += value;
        }

        auto node = ReportNode::MakeObject();
        node.Set("path", match.Location);
        node.Set("name", match.Name);
        node.Set("attributes", attributes);
        node.Set("text", match.Text);
        array.Push(std::move(node));
    }
    document.Data.Set("matchCount", static_cast<ExyokiOffice::UInt64>(result.Matches.size()));
    document.Data.Set("matches", std::move(array));
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptConvert(const ConvertResult& result, const std::string& input, const std::string& output)
{
    ReportDocument document;
    document.Command = "convert";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("input", input);
    document.Data.Set("output", output);
    document.Data.Set("from", std::string(ExyokiOffice::Tools::ToString(result.From)));
    document.Data.Set("to", std::string(ExyokiOffice::Tools::ToString(result.To)));
    document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(result.Family)));
    if (result.BlockCount > 0)
    {
        document.Data.Set("blockCount", static_cast<ExyokiOffice::UInt64>(result.BlockCount));
    }
    if (result.SheetCount > 0)
    {
        document.Data.Set("sheetCount", static_cast<ExyokiOffice::UInt64>(result.SheetCount));
        document.Data.Set("cellCount", static_cast<ExyokiOffice::UInt64>(result.CellCount));
    }
    if (result.SlideCount > 0)
    {
        document.Data.Set("slideCount", static_cast<ExyokiOffice::UInt64>(result.SlideCount));
    }

    if (!result.MediaItems.empty())
    {
        auto array = ReportNode::MakeArray();
        array.SetTableHint({"id", "outputPath", "contentType", "size"});
        for (const auto& item : result.MediaItems)
        {
            auto node = ReportNode::MakeObject();
            node.Set("id", item.PartUri);
            node.Set("outputPath", item.OutputPath.string());
            node.Set("contentType", item.ContentType);
            node.Set("size", static_cast<ExyokiOffice::UInt64>(item.Size));
            array.Push(std::move(node));
        }
        document.Data.Set("mediaCount", static_cast<ExyokiOffice::UInt64>(result.MediaItems.size()));
        document.Data.Set("media", std::move(array));
    }
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptDedup(const ResourceDeduplicationResult& result, bool dryRun)
{
    ReportDocument document;
    document.Command = "dedup";
    document.Status = result.Ok ? "ok" : "error";

    auto array = ReportNode::MakeArray();
    array.SetTableHint({"contentType", "keptPartUri", "duplicateCount", "payloadBytes"});
    for (const auto& group : result.Groups)
    {
        auto node = ReportNode::MakeObject();
        node.Set("contentType", group.ContentType);
        node.Set("keptPartUri", group.KeptPartUri);
        node.Set("duplicateCount", static_cast<ExyokiOffice::UInt64>(group.DuplicatePartUris.size()));
        node.Set("payloadBytes", group.PayloadBytes);
        auto duplicates = ReportNode::MakeArray();
        for (const auto& uri : group.DuplicatePartUris)
        {
            auto entry = ReportNode::MakeObject();
            entry.Set("partUri", uri);
            duplicates.Push(std::move(entry));
        }
        node.Set("duplicates", std::move(duplicates));
        array.Push(std::move(node));
    }

    document.Data.Set("dryRun", dryRun);
    document.Data.Set("groupCount", static_cast<ExyokiOffice::UInt64>(result.Groups.size()));
    document.Data.Set("groups", std::move(array));
    document.Data.Set("removedParts", static_cast<ExyokiOffice::UInt64>(result.RemovedParts));
    document.Data.Set("rewrittenRelationships", static_cast<ExyokiOffice::UInt64>(result.RewrittenRelationships));
    document.Data.Set("bytesSaved", result.BytesSaved);
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptRedact(const RedactResult& result)
{
    ReportDocument document;
    document.Command = "redact";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("family", std::string(ExyokiOffice::Tools::ToString(result.Family)));
    document.Data.Set("commentsRemoved", static_cast<ExyokiOffice::UInt64>(result.CommentsRemoved));
    document.Data.Set("revisionsResolved", static_cast<ExyokiOffice::UInt64>(result.RevisionsResolved));
    document.Data.Set("hiddenRunsRemoved", static_cast<ExyokiOffice::UInt64>(result.HiddenRunsRemoved));
    document.Data.Set("metadataFieldsCleared",
                      static_cast<ExyokiOffice::UInt64>(result.MetadataFieldsCleared));
    document.Data.Set("saved", result.Saved);
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptFill(const TemplateFillResult& result)
{
    ReportDocument document;
    document.Command = "fill";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("fieldsMerged", static_cast<ExyokiOffice::UInt64>(result.FieldsMerged));
    document.Data.Set("bookmarksMerged", static_cast<ExyokiOffice::UInt64>(result.BookmarksMerged));
    document.Data.Set("regionsMerged", static_cast<ExyokiOffice::UInt64>(result.RegionsMerged));
    document.Data.Set("regionRowsInserted", static_cast<ExyokiOffice::UInt64>(result.RegionRowsInserted));
    document.Data.Set("saved", result.Saved);
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptRecalc(const WorkbookRecalcResult& result)
{
    ReportDocument document;
    document.Command = "recalc";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("recalculatedCellCount",
                      static_cast<ExyokiOffice::UInt64>(result.RecalculatedCellCount));
    auto cycles = ReportNode::MakeArray();
    cycles.SetTableHint({"cycle"});
    for (const auto& cycle : result.CircularReferenceCycles)
    {
        auto node = ReportNode::MakeObject();
        node.Set("cycle", cycle);
        cycles.Push(std::move(node));
    }
    document.Data.Set("circularReferenceCount",
                      static_cast<ExyokiOffice::UInt64>(result.CircularReferenceCycles.size()));
    document.Data.Set("circularReferences", std::move(cycles));
    document.Data.Set("saved", result.Saved);
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptCompare(const WordCompareResult& result)
{
    ReportDocument document;
    document.Command = "compare";
    document.Status = result.Ok ? "ok" : "error";
    document.Data.Set("revisionsCreated", static_cast<ExyokiOffice::UInt64>(result.RevisionsCreated));
    document.Data.Set("identical", result.Identical);
    document.Data.Set("outputFile", result.OutputFile.string());
    AppendDiagnostics(document, result.Diagnostics);
    return document;
}

ReportDocument AdaptValidateBatch(const std::vector<std::pair<std::string, ValidationReport>>& reports,
                                  bool errorsOnly, bool warningsAsErrors,
                                  std::optional<ExyokiOffice::Size> maxIssues)
{
    ReportDocument document;
    document.Command = "validate";

    ExyokiOffice::Size totalErrors = 0;
    ExyokiOffice::Size totalWarnings = 0;
    ExyokiOffice::Size filesFailedToLoad = 0;

    auto files = ReportNode::MakeArray();
    for (const auto& [file, report] : reports)
    {
        // Reuse the single-file adapter so the per-file payload cannot drift
        // away from the single-file command's shape.
        auto single = AdaptValidate(report, errorsOnly, warningsAsErrors, maxIssues);
        auto node = ReportNode::MakeObject();
        node.Set("file", file);
        for (auto& [key, value] : single.Data.AsObject())
        {
            node.Set(key, value);
            if (key == "errorCount")
            {
                totalErrors += value.AsUInt();
            }
            else if (key == "warningCount")
            {
                totalWarnings += value.AsUInt();
            }
        }
        if (!report.Loaded)
        {
            ++filesFailedToLoad;
        }
        files.Push(std::move(node));
        for (auto& diagnostic : single.Diagnostics)
        {
            diagnostic.Context = diagnostic.Context.empty() ? file : diagnostic.Context;
            document.Diagnostics.push_back(std::move(diagnostic));
        }
    }

    document.Data.Set("fileCount", static_cast<ExyokiOffice::UInt64>(reports.size()));
    document.Data.Set("filesFailedToLoad", static_cast<ExyokiOffice::UInt64>(filesFailedToLoad));
    document.Data.Set("totalErrorCount", static_cast<ExyokiOffice::UInt64>(totalErrors));
    document.Data.Set("totalWarningCount", static_cast<ExyokiOffice::UInt64>(totalWarnings));
    document.Data.Set("files", std::move(files));
    document.Status = filesFailedToLoad == 0 ? "ok" : "error";
    return document;
}

ReportDocument AdaptSchemaCheck(const std::string& input, bool valid,
                                const std::vector<ToolDiagnostic>& violations)
{
    ReportDocument document;
    document.Command = "schema";
    document.Status = valid ? "ok" : "error";
    document.Data.Set("input", input);
    document.Data.Set("schema", GetDocumentModelJsonSchemaFileName());
    document.Data.Set("valid", valid);
    document.Data.Set("violationCount", static_cast<ExyokiOffice::UInt64>(violations.size()));
    AppendDiagnostics(document, violations);
    return document;
}

} // namespace exyoki
