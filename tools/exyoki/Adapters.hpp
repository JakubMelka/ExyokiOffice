// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Tools/PackageArchiver.hpp"
#include "ExyokiOffice/Tools/ExternalResourceInspector.hpp"
#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"
#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/MediaExporter.hpp"
#include "ExyokiOffice/Tools/Report.hpp"
#include "ExyokiOffice/Tools/SignatureInspector.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Tools/WordTextTools.hpp"
#include "ExyokiOffice/Tools/WordDocumentTools.hpp"
#include "ExyokiOffice/Tools/DocumentTools.hpp"
#include "ExyokiOffice/Tools/DocumentStats.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentModelSchema.hpp"
#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/ResourceDeduplicator.hpp"
#include "ExyokiOffice/Tools/SpreadsheetTools.hpp"
#include "ExyokiOffice/Tools/WordAutomationTools.hpp"
#include "ExyokiOffice/Tools/XmlQueryTool.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exyoki
{

using namespace ExyokiOffice::Tools;

ReportDocument AdaptParts(const std::vector<PartRecord>& parts, const std::string& sortBy);
ReportDocument AdaptRelationships(const std::vector<RelationshipRecord>& relationships, bool danglingOnly);
ReportDocument AdaptInfo(const PackageInfo& info, bool propsOnly);
ReportDocument AdaptValidate(const ValidationReport& report, bool errorsOnly, bool warningsAsErrors,
                             std::optional<ExyokiOffice::Size> maxIssues);
ReportDocument AdaptSignatures(const SignatureInspectionReport& report);
ReportDocument AdaptExternal(const ExternalResourceReport& report);
ReportDocument AdaptUnpack(const UnpackResult& result);
ReportDocument AdaptPack(const PackResult& result);
ReportDocument AdaptToFlatOpc(const ToFlatOpcResult& result);
ReportDocument AdaptFromFlatOpc(const FromFlatOpcResult& result);
ReportDocument AdaptExportMedia(const MediaExportResult& result);
ReportDocument AdaptSearch(const DocumentSearchResult& result);
ReportDocument AdaptExtractText(const ExtractedDocumentText& result);
ReportDocument AdaptReplace(const DocumentReplaceResult& result);
ReportDocument AdaptSplit(const WordSplitResult& result);
ReportDocument AdaptMerge(const WordMergeResult& result);
ReportDocument AdaptSplit(const DocumentSplitResult& result);
ReportDocument AdaptMerge(const DocumentMergeResult& result);
ReportDocument AdaptDiff(const DiffResult& result, bool partsOnly);
ReportDocument AdaptPropsGet(const CoreProperties& properties,
                             const std::vector<ExyokiOffice::Packaging::DocumentCustomProperty>& custom);
ReportDocument AdaptPropsSet(bool ok, const std::string& name, const std::string& value);
ReportDocument AdaptStat(const DocumentStats& result);
ReportDocument AdaptQuery(const QueryResult& result);
ReportDocument AdaptConvert(const ConvertResult& result, const std::string& input, const std::string& output);
ReportDocument AdaptDedup(const ResourceDeduplicationResult& result, bool dryRun);
ReportDocument AdaptRedact(const RedactResult& result);
ReportDocument AdaptFill(const TemplateFillResult& result);
ReportDocument AdaptRecalc(const WorkbookRecalcResult& result);
ReportDocument AdaptCompare(const WordCompareResult& result);
/// `schema --check`: the outcome of validating one envelope against the model schema.
ReportDocument AdaptSchemaCheck(const std::string& input, bool valid,
                                const std::vector<ToolDiagnostic>& violations);
/// Batch form of AdaptValidate: one entry per input file, plus totals.
ReportDocument AdaptValidateBatch(const std::vector<std::pair<std::string, ValidationReport>>& reports,
                                  bool errorsOnly, bool warningsAsErrors,
                                  std::optional<ExyokiOffice::Size> maxIssues);

} // namespace exyoki
