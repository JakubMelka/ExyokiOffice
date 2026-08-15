// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/WordAutomationTools.hpp"

#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace ExyokiOffice::Tools
{

/// File-local helpers behind the Word automation tools.
class WordAutomationToolsHelper
{
public:
    static void AddError(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, std::move(message), std::move(context)});
    }

    static void AddWarning(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
    }

    /// Converts one scalar JSON value to the literal merge text.
    static std::string ScalarText(const nlohmann::json& value)
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        if (value.is_boolean())
        {
            return value.get<bool>() ? "true" : "false";
        }
        if (value.is_null())
        {
            return {};
        }
        return value.dump();
    }

    [[nodiscard]] static bool IsScalar(const nlohmann::json& value)
    {
        return value.is_string() || value.is_number() || value.is_boolean() || value.is_null();
    }

    /// Maps the JSON root object onto TemplateMergeData; non-mappable members are
    /// reported and skipped.
    static Word::TemplateMergeData BuildMergeData(const nlohmann::json& root,
                                                  std::vector<ToolDiagnostic>& diagnostics)
    {
        Word::TemplateMergeData data;
        for (const auto& [key, value] : root.items())
        {
            if (IsScalar(value))
            {
                data.Values.emplace(key, ScalarText(value));
                continue;
            }
            if (value.is_array())
            {
                std::vector<std::unordered_map<std::string, std::string>> rows;
                bool usable = true;
                for (const auto& element : value)
                {
                    if (!element.is_object())
                    {
                        usable = false;
                        break;
                    }
                    std::unordered_map<std::string, std::string> row;
                    for (const auto& [rowKey, rowValue] : element.items())
                    {
                        if (IsScalar(rowValue))
                        {
                            row.emplace(rowKey, ScalarText(rowValue));
                        }
                        else
                        {
                            AddWarning(diagnostics, "Region row member is not a scalar; skipped",
                                       key + "." + rowKey);
                        }
                    }
                    rows.push_back(std::move(row));
                }
                if (usable)
                {
                    data.Regions.emplace(key, std::move(rows));
                }
                else
                {
                    AddWarning(diagnostics,
                               "Array member must contain only objects to drive a repeating region; skipped",
                               key);
                }
                continue;
            }
            AddWarning(diagnostics, "Member is neither a scalar nor an array of objects; skipped", key);
        }
        return data;
    }
};

TemplateFillResult FillWordTemplate(const std::filesystem::path& docxPath,
                                    const std::filesystem::path& dataJsonPath,
                                    const std::filesystem::path& outputPath)
{
    TemplateFillResult result;

    std::ifstream file(dataJsonPath, std::ios::binary);
    if (!file)
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Cannot read data file", dataJsonPath.string());
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    nlohmann::json root = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (root.is_discarded())
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Data file is not valid JSON", dataJsonPath.string());
        return result;
    }
    if (!root.is_object())
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Data JSON root must be an object", dataJsonPath.string());
        return result;
    }

    auto editor = Word::WordDocumentEditor::Open(docxPath, UntrustedOpenSettings());
    if (!editor)
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Failed to open Word document", docxPath.string());
        return result;
    }

    const auto data = WordAutomationToolsHelper::BuildMergeData(root, result.Diagnostics);
    const auto merged = editor->MergeTemplate(data);
    result.FieldsMerged = merged.FieldsMerged;
    result.BookmarksMerged = merged.BookmarksMerged;
    result.RegionsMerged = merged.RegionsMerged;
    result.RegionRowsInserted = merged.RegionRowsInserted;

    const auto destination = outputPath.empty() ? docxPath : outputPath;
    if (!editor->SaveToFile(destination))
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Failed to save document", destination.string());
        return result;
    }
    result.Saved = true;
    result.Ok = true;
    return result;
}

WordCompareResult CompareWordDocuments(const std::filesystem::path& originalPath,
                                       const std::filesystem::path& revisedPath,
                                       const std::filesystem::path& outputPath,
                                       const std::string& author)
{
    WordCompareResult result;

    auto original = Word::WordDocumentEditor::Open(originalPath, UntrustedOpenSettings());
    if (!original)
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Failed to open original document", originalPath.string());
        return result;
    }
    auto revised = Word::WordDocumentEditor::Open(revisedPath, UntrustedOpenSettings());
    if (!revised)
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Failed to open revised document", revisedPath.string());
        return result;
    }

    Word::RevisionAuthor revisionAuthor;
    revisionAuthor.Name = author;
    result.RevisionsCreated = original->CompareWith(*revised, revisionAuthor);
    result.Identical = result.RevisionsCreated == 0;

    if (!original->SaveToFile(outputPath))
    {
        WordAutomationToolsHelper::AddError(result.Diagnostics, "Failed to save comparison result", outputPath.string());
        return result;
    }
    result.OutputFile = outputPath;
    result.Ok = true;
    return result;
}

} // namespace ExyokiOffice::Tools
