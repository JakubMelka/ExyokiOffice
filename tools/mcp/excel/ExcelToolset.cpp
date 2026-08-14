// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExcelToolset.hpp"

#include "ExcelAddressing.hpp"
#include "SharedToolset.hpp"
#include "Units.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ExyokiOffice::Mcp
{

/// Open settings that carry the configured safety limits and nothing else.
static Packaging::OpenSettings SettingsWithLimits(const OpenXmlPackageLimits& limits)
{
    Packaging::OpenSettings settings;
    settings.PackageLimits = limits;
    return settings;
}

ExcelDocumentHandle::ExcelDocumentHandle(Excel::ExcelDocumentEditor::Ptr editor, OpenXmlPackageLimits limits)
    : m_editor(std::move(editor)), m_packageLimits(limits)
{
}

Tools::DocumentFamily ExcelDocumentHandle::Family() const
{
    return Tools::DocumentFamily::Excel;
}

bool ExcelDocumentHandle::SaveToFile(const std::filesystem::path& path)
{
    return m_editor && m_editor->SaveToFile(path);
}

std::vector<Byte> ExcelDocumentHandle::SaveToMemory()
{
    return m_editor ? m_editor->SaveToMemory() : std::vector<Byte>();
}

bool ExcelDocumentHandle::LoadFromMemory(std::span<const Byte> bytes)
{
    // Snapshot bytes come from this process, but they are a package all the
    // same: a document that was within the limits when it was opened stays
    // within them when it is restored, and a bug that made it grow past them
    // should surface here rather than be waved through.
    auto replacement = Excel::ExcelDocumentEditor::Open(bytes, SettingsWithLimits(m_packageLimits));
    if (replacement == nullptr)
    {
        return false;
    }

    m_editor = std::move(replacement);
    return true;
}

std::shared_ptr<OpenXmlPackage> ExcelDocumentHandle::Package() const
{
    return m_editor ? m_editor->GetDocument() : nullptr;
}

nlohmann::json ExcelDocumentHandle::Summary() const
{
    nlohmann::json summary = nlohmann::json::object();
    if (m_editor == nullptr)
    {
        return summary;
    }

    const auto sheets = m_editor->Worksheets();
    UInt64 cellCount = 0;
    nlohmann::json names = nlohmann::json::array();
    for (const auto& sheet : sheets)
    {
        if (sheet == nullptr)
        {
            continue;
        }

        cellCount += static_cast<UInt64>(sheet->StoredCellCount());
        names.push_back(sheet->Name());
    }

    summary["sheetCount"] = static_cast<UInt64>(sheets.size());
    summary["sheetNames"] = std::move(names);
    summary["storedCellCount"] = cellCount;
    return summary;
}

Tools::DocumentFamily ExcelFamilyAdapter::Family() const
{
    return Tools::DocumentFamily::Excel;
}

std::string ExcelFamilyAdapter::FamilyName() const
{
    return "Excel";
}

std::string ExcelFamilyAdapter::FileExtension() const
{
    return ".xlsx";
}

std::unique_ptr<DocumentHandle> ExcelFamilyAdapter::CreateNew() const
{
    auto editor = Excel::ExcelDocumentEditor::CreateNew();
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<ExcelDocumentHandle>(std::move(editor), PackageLimits());
}

std::unique_ptr<DocumentHandle> ExcelFamilyAdapter::Open(const std::filesystem::path& path) const
{
    auto editor = Excel::ExcelDocumentEditor::Open(path, SettingsWithLimits(PackageLimits()));
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<ExcelDocumentHandle>(std::move(editor), PackageLimits());
}

std::unique_ptr<DocumentHandle> ExcelFamilyAdapter::OpenFromMemory(std::span<const Byte> bytes) const
{
    auto editor = Excel::ExcelDocumentEditor::Open(bytes, SettingsWithLimits(PackageLimits()));
    if (editor == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<ExcelDocumentHandle>(std::move(editor), PackageLimits());
}

/// The editor behind a handle this adapter produced.
static Excel::ExcelDocumentEditor& EditorOf(DocumentHandle& document)
{
    // Every handle reaching a ExcelFamilyAdapter came out of its own CreateNew, Open or
    // OpenFromMemory, so the family is a fact here rather than a guess.
    return static_cast<ExcelDocumentHandle&>(document).Editor();
}

Tools::DocumentModel ExcelFamilyAdapter::ReadModel(DocumentHandle& document, const Tools::ModelReadOptions& options,
                                                   std::vector<Tools::ToolDiagnostic>& diagnostics) const
{
    return Tools::ReadExcelModel(EditorOf(document), options, diagnostics);
}

Tools::DocumentStats ExcelFamilyAdapter::Stat(DocumentHandle& document) const
{
    return Tools::Stat(EditorOf(document));
}

Tools::ExtractedDocumentText ExcelFamilyAdapter::ExtractText(DocumentHandle& document) const
{
    return Tools::Extract(EditorOf(document));
}

Tools::DocumentSearchResult ExcelFamilyAdapter::SearchText(DocumentHandle& document, std::string_view needle, Size contextChars,
                                                           bool useRegex, bool ignoreCase) const
{
    return Tools::SearchDocumentText(EditorOf(document), needle, contextChars, useRegex, ignoreCase);
}

Tools::DocumentReplaceResult ExcelFamilyAdapter::ReplaceText(DocumentHandle& document, std::string_view needle,
                                                             std::string_view replacement, bool dryRun, bool useRegex,
                                                             bool ignoreCase) const
{
    return Tools::ReplaceDocumentText(EditorOf(document), needle, replacement, dryRun, useRegex, ignoreCase);
}

Tools::RedactResult ExcelFamilyAdapter::Redact(DocumentHandle& document, const Tools::RedactOptions& options) const
{
    return Tools::RedactDocument(EditorOf(document), options);
}

/// Implementation of the tools in §10 of the MCP server plan.
class ExcelTools
{
public:
    static void Register(ToolRegistry& registry)
    {
        RegisterListSheets(registry);
        RegisterAddSheet(registry);
        RegisterRenameSheet(registry);
        RegisterDeleteSheet(registry);
        RegisterReadRange(registry);
        RegisterWriteCells(registry);
        RegisterWriteRange(registry);
        RegisterClearRange(registry);
        RegisterModifySheetStructure(registry);
        RegisterSetHyperlink(registry);
        RegisterRecalculate(registry);
        RegisterMergeCells(registry);
        RegisterFormatRange(registry);
        RegisterSetColumnWidth(registry);
        RegisterSetRowHeight(registry);
        RegisterFreezePanes(registry);
        RegisterAddTable(registry);
        RegisterAddNamedRange(registry);
        RegisterAddDataValidation(registry);
        RegisterAddConditionalFormatting(registry);
        RegisterAddChart(registry);
        RegisterAddPivotTable(registry);
    }

private:
    /// Resolves the session and its Excel editor in one step.
    class ExcelSession
    {
    public:
        ExcelSession(ToolContext& context, const nlohmann::json& arguments)
        {
            m_session = ToolSupport::RequireSession(context, arguments, m_failure);
            if (m_session == nullptr)
            {
                return;
            }

            auto* handle = dynamic_cast<ExcelDocumentHandle*>(&m_session->Document());
            if (handle == nullptr)
            {
                m_failure =
                    MakeError(ErrorCode::FamilyMismatch, "The document is not a workbook.", m_session->Id());
                return;
            }

            m_editor = &handle->Editor();
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_editor != nullptr; }
        [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_failure; }
        [[nodiscard]] DocumentSession& Session() const noexcept { return *m_session; }
        [[nodiscard]] Excel::ExcelDocumentEditor& Editor() const noexcept { return *m_editor; }

    private:
        DocumentSession* m_session = nullptr;
        Excel::ExcelDocumentEditor* m_editor = nullptr;
        ToolOutcome m_failure;
    };

    /// Reading tools take a document source; this resolves it to an Excel editor.
    class ExcelReader
    {
    public:
        ExcelReader(ToolContext& context, const nlohmann::json& arguments)
            : m_access(context, arguments)
        {
            if (!m_access.IsValid())
            {
                return;
            }

            auto* handle = dynamic_cast<ExcelDocumentHandle*>(&m_access.Document());
            if (handle != nullptr)
            {
                m_editor = &handle->Editor();
            }
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_editor != nullptr; }
        [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_access.Failure(); }
        [[nodiscard]] Excel::ExcelDocumentEditor& Editor() const noexcept { return *m_editor; }

    private:
        DocumentAccess m_access;
        Excel::ExcelDocumentEditor* m_editor = nullptr;
    };

    static ToolDefinition MakeDefinition(std::string name, std::string title, std::string description,
                                         std::string group)
    {
        ToolDefinition definition;
        definition.Name = std::move(name);
        definition.Title = std::move(title);
        definition.Description = std::move(description);
        definition.Group = std::move(group);
        return definition;
    }

    /**
     * @brief A worksheet-naming property, which always accepts a name or an index.
     *
     * Every parameter that names a worksheet takes the same two JSON types, so
     * an agent never has to remember which one a particular tool wanted.
     */
    static nlohmann::json SheetReferenceProperty(std::string description)
    {
        nlohmann::json schema = nlohmann::json::object();
        schema["description"] = std::move(description);
        schema["type"] = nlohmann::json::array({"string", "integer"});
        return schema;
    }

    /// The optional `sheet` property shared by the sheet-scoped tools.
    static nlohmann::json SheetProperty()
    {
        return SheetReferenceProperty(
            "Worksheet name (case-insensitive) or 1-based index; omit for the first sheet.");
    }

    /**
     * @brief Position of a worksheet in the workbook, or the sheet count on failure.
     *
     * Worksheet wrappers are recreated on every call, so a pointer from an
     * earlier Worksheets() call never compares equal to a fresh one; the name
     * is the stable identity.
     */
    static Size IndexOfSheet(Excel::ExcelDocumentEditor& editor, const Excel::Worksheet& sheet)
    {
        const auto worksheets = editor.Worksheets();
        for (Size index = 0; index < worksheets.size(); ++index)
        {
            if (worksheets[index] != nullptr && worksheets[index]->Name() == sheet.Name())
            {
                return index;
            }
        }

        return worksheets.size();
    }

    /**
     * @brief The workbook sheet list as `(name, hidden)` pairs.
     *
     * The Excel editor exposes no visible state, so the flag is read from the
     * workbook part itself. Pairing it with the name rather than the position
     * keeps the lookup right even when the sheet list holds entries the
     * worksheet enumeration skips, such as chart sheets.
     */
    static std::vector<std::pair<std::string, bool>> WorkbookSheetStates(Excel::ExcelDocumentEditor& editor)
    {
        namespace Spreadsheet = DocumentFormat::OpenXml::Spreadsheet;

        std::vector<std::pair<std::string, bool>> states;
        const auto document = editor.GetDocument();
        const auto part = document != nullptr ? document->GetWorkbookPart() : nullptr;
        const auto workbook = part != nullptr ? part->GetWorkbook() : nullptr;
        const auto sheets = workbook != nullptr ? workbook->GetFirstChildOfType<Spreadsheet::Sheets>() : nullptr;
        if (sheets == nullptr)
        {
            return states;
        }

        for (const auto& sheet : sheets->Elements<Spreadsheet::Sheet>())
        {
            if (sheet == nullptr)
            {
                continue;
            }

            // Excel distinguishes hidden from very hidden, but an agent only
            // needs to know the sheet is not on screen.
            const auto state = sheet->GetState();
            const bool hidden = state.IsDefined() &&
                                (state.Value().GetValue() == Spreadsheet::SheetStateValues::Hidden ||
                                 state.Value().GetValue() == Spreadsheet::SheetStateValues::VeryHidden);
            states.emplace_back(sheet->GetName().ToString(), hidden);
        }

        return states;
    }

    /// Whether the workbook marks the named sheet hidden or very hidden.
    [[nodiscard]] static bool IsSheetHidden(const std::vector<std::pair<std::string, bool>>& states,
                                            const std::string& name)
    {
        for (const auto& state : states)
        {
            if (AsciiText::EqualsIgnoreCase(state.first, name))
            {
                return state.second;
            }
        }

        return false;
    }

    // --- sheets -------------------------------------------------------------

    static void RegisterListSheets(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);

        nlohmann::json sheet =
            Schema::Object("One worksheet.", {"index", "name", "hidden"},
                           nlohmann::json{{"index", Schema::Integer("1-based worksheet index.")},
                                          {"name", Schema::String("Worksheet name.")},
                                          {"hidden", Schema::Boolean("True when the workbook marks the sheet hidden "
                                                                     "or very hidden.")},
                                          {"usedRange", Schema::String("A1 range holding data, empty when blank.")},
                                          {"storedCellCount", Schema::Integer("Number of stored cells.")},
                                          {"tableCount", Schema::Integer("Number of list objects.")}});

        auto definition = MakeDefinition("list_sheets", "List worksheets",
                                         "List the worksheets of the workbook with their used ranges and their "
                                         "hidden state. Call it before reading or writing cells to learn the sheet "
                                         "names.",
                                         "sheets");
        definition.InputSchema = Schema::Object("Arguments of list_sheets.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Worksheets.", {"sheets"},
                           nlohmann::json{{"sheets", Schema::Array("Worksheets in workbook order.", std::move(sheet))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListSheets(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListSheets(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        nlohmann::json sheets = nlohmann::json::array();
        const auto states = WorkbookSheetStates(reader.Editor());
        const auto worksheets = reader.Editor().Worksheets();
        for (Size index = 0; index < worksheets.size(); ++index)
        {
            const auto& sheet = worksheets[index];
            if (sheet == nullptr)
            {
                continue;
            }

            const auto used = ExcelAddressing::UsedRange(*sheet);

            nlohmann::json entry = nlohmann::json::object();
            entry["index"] = static_cast<UInt64>(index + 1);
            entry["name"] = sheet->Name();
            entry["hidden"] = IsSheetHidden(states, sheet->Name());
            entry["usedRange"] = used.has_value() ? used->ToA1() : std::string();
            entry["storedCellCount"] = static_cast<UInt64>(sheet->StoredCellCount());
            entry["tableCount"] = static_cast<UInt64>(sheet->Tables().size());
            sheets.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        const auto count = sheets.size();
        data["sheets"] = std::move(sheets);

        return ResultBuilder("The workbook has " + std::to_string(count) + " worksheet(s).")
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddSheet(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["name"] = Schema::String("Name of the new worksheet, unique within the workbook.");
        properties["position"] = Schema::Integer("1-based position; omit to append at the end.", 1);

        auto definition = MakeDefinition("add_sheet", "Add worksheet", "Add a worksheet to the workbook.", "sheets");
        definition.InputSchema =
            Schema::Object("Arguments of add_sheet.", {"documentId", "name"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New worksheet.", {"index", "name"},
                                            nlohmann::json{{"index", Schema::Integer("1-based worksheet index.")},
                                                           {"name", Schema::String("Worksheet name.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"name", "Summary"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddSheet(context, arguments); };
        registry.Add(std::move(definition));
    }

    /// Excel rejects these characters in a sheet name, and names above 31 characters.
    static bool IsValidSheetName(const std::string& name)
    {
        if (name.empty() || name.size() > 31)
        {
            return false;
        }

        return name.find_first_of("[]:*?/\\") == std::string::npos;
    }

    static ToolOutcome AddSheet(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto name = arguments.value("name", std::string());
        if (!IsValidSheetName(name))
        {
            return MakeError(ErrorCode::InputInvalid,
                             "A worksheet name must be 1 to 31 characters and must not contain [ ] : * ? / \\.",
                             name);
        }

        // Compare by name only: FindSheetByToken would also read "2" as an
        // index and reject a worksheet genuinely called "2".
        if (ExcelAddressing::FindSheetByName(session.Editor(), name) != nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The workbook already has a worksheet named '" + name + "'.",
                             name, "Call list_sheets to see the existing worksheets.");
        }

        MutationGuard guard(session.Session());

        auto sheet = session.Editor().AddWorksheet(name);
        if (sheet == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The worksheet could not be added.", name);
        }

        Size index = session.Editor().Worksheets().size();
        const Size position = arguments.value("position", static_cast<Size>(0));
        if (position > 0 && position < index)
        {
            session.Editor().MoveWorksheet(index - 1, position - 1);
            index = position;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["index"] = static_cast<UInt64>(index);
        data["name"] = name;

        return ResultBuilder("Added worksheet '" + name + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterRenameSheet(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["new_name"] = Schema::String("New worksheet name.");

        auto definition =
            MakeDefinition("rename_sheet", "Rename worksheet", "Rename one worksheet of the workbook.", "sheets");
        definition.InputSchema =
            Schema::Object("Arguments of rename_sheet.", {"documentId", "sheet", "new_name"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Renamed worksheet.", {"name"},
                                            nlohmann::json{{"name", Schema::String("New worksheet name.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"sheet", "Sheet1"}, {"new_name", "Data"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return RenameSheet(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome RenameSheet(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto newName = arguments.value("new_name", std::string());
        if (!IsValidSheetName(newName))
        {
            return MakeError(ErrorCode::InputInvalid,
                             "A worksheet name must be 1 to 31 characters and must not contain [ ] : * ? / \\.",
                             newName);
        }

        const auto index = IndexOfSheet(session.Editor(), *sheet);
        if (index >= session.Editor().Worksheets().size())
        {
            return MakeError(ErrorCode::SheetNotFound, "The worksheet is no longer part of the workbook.");
        }

        MutationGuard guard(session.Session());

        if (!session.Editor().RenameWorksheet(index, newName))
        {
            return MakeError(ErrorCode::OperationFailed, "The worksheet could not be renamed.", newName,
                             "The new name may already be in use.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = newName;

        return ResultBuilder("Renamed the worksheet to '" + newName + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterDeleteSheet(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();

        auto definition = MakeDefinition("delete_sheet", "Delete worksheet",
                                         "Remove one worksheet. A workbook must keep at least one worksheet.",
                                         "sheets");
        definition.InputSchema =
            Schema::Object("Arguments of delete_sheet.", {"documentId", "sheet"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Deleted worksheet.", {"name"},
                                            nlohmann::json{{"name", Schema::String("Removed worksheet name.")},
                                                           {"sheetCount", Schema::Integer("Remaining worksheets.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"sheet", "Sheet2"}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteSheet(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteSheet(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto worksheets = session.Editor().Worksheets();
        if (worksheets.size() <= 1)
        {
            return MakeError(ErrorCode::OperationFailed, "A workbook must keep at least one worksheet.",
                             sheet->Name(), "Add another worksheet before deleting this one.");
        }

        const auto name = sheet->Name();
        const auto index = IndexOfSheet(session.Editor(), *sheet);

        MutationGuard guard(session.Session());

        if (!session.Editor().RemoveWorksheet(index))
        {
            return MakeError(ErrorCode::OperationFailed, "The worksheet could not be removed.", name);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = name;
        data["sheetCount"] = static_cast<UInt64>(session.Editor().Worksheets().size());

        return ResultBuilder("Deleted worksheet '" + name + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- cells --------------------------------------------------------------

    static void RegisterReadRange(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range to read; omit to read the used range.");
        properties["mode"] = Schema::EnumerationWithDefault("Shape of the result.", {"values", "cells", "csv"},
                                                            "values");
        properties["include_formulas"] =
            Schema::BooleanWithDefault("Report formula text next to the cached value in 'cells' mode.", false);
        properties["offset"] = Schema::IntegerWithDefault(
            "Rows to skip from the top of the range; an offset past the last row returns an empty page.", 0, 0);
        properties["limit"] = Schema::IntegerWithDefault("Maximum number of rows to return; 0 means all.", 0, 0);

        nlohmann::json cell =
            Schema::Object("One cell.", {"address", "type"},
                           nlohmann::json{{"address", Schema::String("A1 address.")},
                                          {"type", Schema::String("blank, text, number, boolean, error, datetime, "
                                                                  "or formula.")},
                                          {"value", Schema::Any("Cell value.")},
                                          {"formula", Schema::String("Formula text, when requested.")}});

        auto definition = MakeDefinition("read_range", "Read cell range",
                                         "Read cells as a value matrix, as detailed cell records, or as CSV. Reads "
                                         "are capped per call; page with offset and limit for large ranges.",
                                         "cells");
        definition.InputSchema = Schema::Object("Arguments of read_range.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Range contents.", {"range"},
                           nlohmann::json{{"range", Schema::String("A1 range that was read.")},
                                          {"values", Schema::Array("Row-major value matrix.",
                                                                   Schema::Array("One row.", Schema::Any("Cell value.")))},
                                          {"cells", Schema::Array("Detailed cell records.", std::move(cell))},
                                          {"csv", Schema::String("CSV rendering, when requested.")},
                                          {"nextOffset", Schema::Integer("Offset for the next page; 0 at the end.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"sheet", "Sheet1"}, {"range", "A1:C10"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ReadRange(context, arguments); };
        registry.Add(std::move(definition));
    }

    /// Hard cap on cells per read, independent of the response byte budget.
    static constexpr Size MaximumCellsPerRead = 10000;

    static ToolOutcome ReadRange(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(reader.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto rangeText = arguments.value("range", std::string());
        std::optional<Excel::CellRange> range;
        if (rangeText.empty())
        {
            range = ExcelAddressing::UsedRange(*sheet);
        }
        else
        {
            range = ExcelAddressing::ParseRange(rangeText, failure);
            if (!range.has_value())
            {
                return failure;
            }
        }

        nlohmann::json data = nlohmann::json::object();
        if (!range.has_value())
        {
            data["range"] = std::string();
            data["values"] = nlohmann::json::array();
            data["cells"] = nlohmann::json::array();
            data["csv"] = std::string();
            data["nextOffset"] = 0u;
            return ResultBuilder("The worksheet holds no data.").WithData(std::move(data)).Build();
        }

        const auto sharedStrings = reader.Editor().SharedStrings();
        const UInt32 firstRow = range->First().Row().Value();
        const UInt32 lastRow = range->Last().Row().Value();
        const UInt32 firstColumn = range->First().Column().Value();
        const UInt32 lastColumn = range->Last().Column().Value();
        const UInt32 columnCount = lastColumn - firstColumn + 1;
        const UInt64 rowCount = static_cast<UInt64>(lastRow) - firstRow + 1;

        // Widened before any arithmetic: an offset past the end of the range
        // would otherwise wrap the row span and report a huge, truncated page.
        const auto offset = arguments.value("offset", static_cast<UInt64>(0));
        const auto limit = arguments.value("limit", static_cast<UInt64>(0));

        if (offset >= rowCount)
        {
            // Paging past the end is not an error; it is how an agent learns
            // it has read the whole range.
            data["range"] = range->ToA1();
            data["values"] = nlohmann::json::array();
            data["cells"] = nlohmann::json::array();
            data["csv"] = std::string();
            data["nextOffset"] = 0u;
            return ResultBuilder("The offset is past the end of " + range->ToA1() + "; there is nothing to read.")
                .WithData(std::move(data))
                .Build();
        }

        const UInt32 startRow = firstRow + static_cast<UInt32>(offset);
        UInt32 endRow = lastRow;
        if (limit > 0 && offset + limit < rowCount)
        {
            endRow = startRow + static_cast<UInt32>(limit) - 1;
        }

        bool truncated = false;
        if (columnCount > 0 && (static_cast<Size>(endRow - startRow) + 1) * columnCount > MaximumCellsPerRead)
        {
            const UInt32 rowBudget = std::max<UInt32>(1, static_cast<UInt32>(MaximumCellsPerRead / columnCount));
            endRow = startRow + rowBudget - 1;
            truncated = true;
        }

        const auto mode = arguments.value("mode", std::string("values"));
        nlohmann::json values = nlohmann::json::array();
        nlohmann::json cells = nlohmann::json::array();
        std::string csv;

        for (UInt32 row = startRow; row <= endRow && row <= lastRow; ++row)
        {
            nlohmann::json rowValues = nlohmann::json::array();
            for (UInt32 column = firstColumn; column <= lastColumn; ++column)
            {
                const auto address = Excel::CellAddress::TryCreate(row, column);
                if (!address.has_value())
                {
                    continue;
                }

                const auto stored = sheet->GetCellValue(*address);
                const auto value = stored.has_value() ? *stored : Excel::ExcelCellValue::Blank();

                if (mode == "values")
                {
                    rowValues.push_back(ExcelAddressing::CellValueToJson(value, sharedStrings));
                }
                else if (mode == "csv")
                {
                    if (column != firstColumn)
                    {
                        csv.push_back(',');
                    }

                    csv.append(EscapeCsv(ExcelAddressing::CellValueToText(value, sharedStrings)));
                }
                else if (stored.has_value() && !value.IsBlank())
                {
                    nlohmann::json entry = nlohmann::json::object();
                    entry["address"] = address->ToA1();
                    entry["type"] = ExcelAddressing::CellKindToken(value.Kind());
                    entry["value"] = ExcelAddressing::CellValueToJson(value, sharedStrings);
                    if (arguments.value("include_formulas", false) &&
                        value.Kind() == Excel::CellValueKind::Formula)
                    {
                        entry["formula"] = value.FormulaValue().Formula;
                    }

                    cells.push_back(std::move(entry));
                }
            }

            if (mode == "values")
            {
                values.push_back(std::move(rowValues));
            }
            else if (mode == "csv")
            {
                csv.append("\r\n");
            }
        }

        truncated = TruncateArrayToBudget(values) || truncated;
        truncated = TruncateArrayToBudget(cells) || truncated;
        truncated = TruncateTextToBudget(csv) || truncated;

        data["range"] = range->ToA1();
        data["values"] = std::move(values);
        data["cells"] = std::move(cells);
        data["csv"] = std::move(csv);
        data["nextOffset"] = endRow < lastRow ? static_cast<UInt64>(endRow - firstRow + 1) : 0u;

        return ResultBuilder("Read rows " + std::to_string(startRow) + " to " + std::to_string(endRow) + " of " +
                             sheet->Name() + ".")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static std::string EscapeCsv(const std::string& value)
    {
        if (value.find_first_of(",\"\r\n") == std::string::npos)
        {
            return value;
        }

        std::string escaped = "\"";
        for (const char character : value)
        {
            if (character == '"')
            {
                escaped.push_back('"');
            }

            escaped.push_back(character);
        }

        escaped.push_back('"');
        return escaped;
    }

    static void RegisterWriteCells(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["cells"] = Schema::Array(
            "Cells to write, addressed individually.",
            Schema::Object("One cell assignment.", {"address", "value"},
                           nlohmann::json{{"address", Schema::String("A1 cell address.")},
                                          {"value", ExcelAddressing::CellValueSchema()}}));

        auto definition = MakeDefinition("write_cells", "Write individual cells",
                                         "Write a sparse set of cells addressed individually. A null value leaves "
                                         "the existing cell untouched; call clear_range to erase cells. Use "
                                         "write_range for a contiguous block.",
                                         "cells");
        definition.InputSchema =
            Schema::Object("Arguments of write_cells.", {"documentId", "cells"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Write result.", {"written"},
                                            nlohmann::json{{"written", Schema::Integer("Number of written cells.")},
                                                           {"skipped", Schema::Integer("Cells left untouched because "
                                                                                       "their value was null.")},
                                                           {"sheet", Schema::String("Worksheet name.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"sheet", "Sheet1"},
            {"cells", nlohmann::json::array({nlohmann::json{{"address", "A1"}, {"value", "Region"}},
                                             nlohmann::json{{"address", "B1"}, {"value", 42}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return WriteCells(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome WriteCells(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        Size written = 0;
        Size skipped = 0;
        for (const auto& entry : arguments.at("cells"))
        {
            const auto addressText = entry.value("address", std::string());
            const auto address = ExcelAddressing::ParseCell(addressText, failure);
            if (!address.has_value())
            {
                return failure;
            }

            // A null value leaves the cell alone, exactly as in write_range;
            // the address is still validated so a typo is never silently
            // swallowed along with the skipped write.
            const auto member = entry.find("value");
            if (member == entry.end() || member->is_null())
            {
                ++skipped;
                continue;
            }

            Excel::ExcelCellValue value;
            if (!ExcelAddressing::ParseCellValue(*member, value, failure))
            {
                return failure;
            }

            if (!WriteCell(*sheet, *address, value))
            {
                return MakeError(ErrorCode::OperationFailed, "Cell " + addressText + " could not be written.",
                                 addressText);
            }

            ++written;
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["written"] = static_cast<UInt64>(written);
        data["skipped"] = static_cast<UInt64>(skipped);
        data["sheet"] = sheet->Name();

        return ResultBuilder("Wrote " + std::to_string(written) + " cell(s) to " + sheet->Name() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /// Writes one cell, routing formulas through the formula-specific setter.
    static bool WriteCell(Excel::Worksheet& sheet, Excel::CellAddress address, const Excel::ExcelCellValue& value)
    {
        if (value.Kind() == Excel::CellValueKind::Formula)
        {
            return sheet.SetCellFormula(address, value.FormulaValue());
        }

        return sheet.SetCellValue(address, value);
    }

    static void RegisterWriteRange(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["origin"] = Schema::String("A1 address of the top-left cell of the block.");
        properties["values"] = Schema::Array("Row-major block of values; a null entry leaves that cell untouched.",
                                             Schema::Array("One row.", ExcelAddressing::CellValueSchema()));

        auto definition = MakeDefinition("write_range", "Write a block of cells",
                                         "Write a rectangular block of values starting at an origin cell. A null "
                                         "entry leaves the existing cell untouched; call clear_range to erase "
                                         "cells.",
                                         "cells");
        definition.InputSchema = Schema::Object("Arguments of write_range.", {"documentId", "origin", "values"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Write result.", {"range", "written"},
                                            nlohmann::json{{"range", Schema::String("A1 range that was written.")},
                                                           {"written", Schema::Integer("Number of written cells.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"origin", "A1"},
            {"values", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                              nlohmann::json::array({"North", 1200})})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return WriteRange(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome WriteRange(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto origin = ExcelAddressing::ParseCell(arguments.value("origin", std::string()), failure);
        if (!origin.has_value())
        {
            return failure;
        }

        const auto& values = arguments.at("values");

        MutationGuard guard(session.Session());

        Size written = 0;
        UInt32 lastRow = origin->Row().Value();
        UInt32 lastColumn = origin->Column().Value();
        for (Size row = 0; row < values.size(); ++row)
        {
            const auto& rowValues = values[row];
            if (!rowValues.is_array())
            {
                return MakeError(ErrorCode::InputInvalid, "Every entry of 'values' must be an array of cells.");
            }

            for (Size column = 0; column < rowValues.size(); ++column)
            {
                if (rowValues[column].is_null())
                {
                    continue;
                }

                const auto address = Excel::CellAddress::TryCreate(
                    origin->Row().Value() + static_cast<UInt32>(row),
                    origin->Column().Value() + static_cast<UInt32>(column));
                if (!address.has_value())
                {
                    return MakeError(ErrorCode::RangeInvalid, "The block extends beyond the worksheet limits.");
                }

                Excel::ExcelCellValue value;
                if (!ExcelAddressing::ParseCellValue(rowValues[column], value, failure))
                {
                    return failure;
                }

                if (!WriteCell(*sheet, *address, value))
                {
                    return MakeError(ErrorCode::OperationFailed,
                                     "Cell " + address->ToA1() + " could not be written.", address->ToA1());
                }

                ++written;
                lastRow = std::max(lastRow, address->Row().Value());
                lastColumn = std::max(lastColumn, address->Column().Value());
            }
        }

        guard.Commit();

        const auto last = Excel::CellAddress::TryCreate(lastRow, lastColumn);
        const Excel::CellRange range(*origin, last.value_or(*origin));

        nlohmann::json data = nlohmann::json::object();
        data["range"] = range.ToA1();
        data["written"] = static_cast<UInt64>(written);

        return ResultBuilder("Wrote " + std::to_string(written) + " cell(s) into " + range.ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterClearRange(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range to clear.");
        properties["what"] = Schema::EnumerationWithDefault("What to remove.", {"contents", "formats", "all"},
                                                            "contents");

        auto definition = MakeDefinition("clear_range", "Clear cell range",
                                         "Clear the contents, the formatting, or both from a range of cells.",
                                         "cells");
        definition.InputSchema =
            Schema::Object("Arguments of clear_range.", {"documentId", "range"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Clear result.", {"range"},
                                            nlohmann::json{{"range", Schema::String("Cleared A1 range.")},
                                                           {"cleared", Schema::Integer("Cells affected.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"range", "A1:C10"}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ClearRange(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ClearRange(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto rangeText = arguments.value("range", std::string());
        const auto range = ExcelAddressing::ParseRange(rangeText, failure);
        if (!range.has_value())
        {
            return failure;
        }

        const auto what = arguments.value("what", std::string("contents"));

        MutationGuard guard(session.Session());

        Size cleared = 0;
        if (what == "contents" || what == "all")
        {
            const auto result = sheet->ClearRange(*range);
            if (!result.Succeeded())
            {
                return MakeError(ErrorCode::OperationFailed, result.Message, rangeText);
            }

            cleared = result.AffectedCellCount;
        }

        if (what == "formats" || what == "all")
        {
            // Style index 0 is the workbook's default format, so applying it is
            // how a range loses its explicit formatting.
            auto styles = session.Editor().Styles();
            const auto result = styles.ApplyToRange(*sheet, *range, 0);
            if (!result.Succeeded())
            {
                return MakeError(ErrorCode::OperationFailed, result.Message, rangeText);
            }

            cleared = std::max(cleared, result.AffectedCellCount);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["range"] = range->ToA1();
        data["cleared"] = static_cast<UInt64>(cleared);

        return ResultBuilder("Cleared " + what + " from " + range->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterModifySheetStructure(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["operation"] = Schema::Enumeration(
            "Structural change to apply.", {"insert_rows", "delete_rows", "insert_columns", "delete_columns"});
        properties["at"] = Schema::Integer("1-based row or column the operation starts at.", 1);
        properties["count"] = Schema::IntegerWithDefault("Number of rows or columns.", 1, 1, 10000);

        auto definition = MakeDefinition("modify_sheet_structure", "Insert or delete rows and columns",
                                         "Insert or delete whole rows or columns. Unqualified A1 references in "
                                         "formulas are rewritten to follow the shift.",
                                         "cells");
        definition.InputSchema = Schema::Object("Arguments of modify_sheet_structure.",
                                                {"documentId", "operation", "at"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Structure change result.", {"affectedCells"},
                           nlohmann::json{{"affectedCells", Schema::Integer("Cells the change moved or removed.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"operation", "insert_rows"}, {"at", 2}, {"count", 3}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ModifySheetStructure(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ModifySheetStructure(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto operation = arguments.value("operation", std::string());
        const auto at = arguments.value("at", static_cast<UInt32>(1));
        const auto count = arguments.value("count", static_cast<UInt32>(1));

        MutationGuard guard(session.Session());

        Excel::RangeOperationResult result;
        if (operation == "insert_rows")
        {
            result = sheet->InsertRows(at, count);
        }
        else if (operation == "delete_rows")
        {
            result = sheet->DeleteRows(at, count);
        }
        else if (operation == "insert_columns")
        {
            result = sheet->InsertColumns(at, count);
        }
        else if (operation == "delete_columns")
        {
            result = sheet->DeleteColumns(at, count);
        }
        else
        {
            return MakeError(ErrorCode::InputInvalid, "Unknown operation '" + operation + "'.", operation);
        }

        if (!result.Succeeded())
        {
            return MakeError(ErrorCode::OperationFailed, result.Message, operation);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["affectedCells"] = static_cast<UInt64>(result.AffectedCellCount);

        return ResultBuilder("Applied '" + operation + "' to " + sheet->Name() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetHyperlink(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["cell"] = Schema::String("A1 address of the cell that carries the link.");
        properties["target"] = Schema::String("External URL; omit together with 'location' to remove the link.");
        properties["location"] = Schema::String("In-workbook location such as \"Sheet2!A1\".");
        properties["tooltip"] = Schema::String("Tooltip shown on hover.");

        auto definition = MakeDefinition("set_hyperlink", "Set cell hyperlink",
                                         "Attach a hyperlink to a cell, or remove it by passing neither target nor "
                                         "location.",
                                         "cells");
        definition.InputSchema =
            Schema::Object("Arguments of set_hyperlink.", {"documentId", "cell"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Hyperlink result.", {"cell"},
                                            nlohmann::json{{"cell", Schema::String("A1 address.")},
                                                           {"removed", Schema::Boolean("True when the link was "
                                                                                       "removed.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"cell", "A1"}, {"target", "https://example.com"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetHyperlink(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetHyperlink(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto cellText = arguments.value("cell", std::string());
        const auto address = ExcelAddressing::ParseCell(cellText, failure);
        if (!address.has_value())
        {
            return failure;
        }

        const auto target = arguments.value("target", std::string());
        const auto location = arguments.value("location", std::string());

        MutationGuard guard(session.Session());

        bool removed = false;
        if (target.empty() && location.empty())
        {
            removed = sheet->RemoveHyperlink(*address);
        }
        else
        {
            Excel::ExcelHyperlink hyperlink;
            hyperlink.Address = *address;
            hyperlink.Target = target;
            hyperlink.Location = location;
            hyperlink.Tooltip = arguments.value("tooltip", std::string());
            if (!sheet->SetHyperlink(hyperlink))
            {
                return MakeError(ErrorCode::OperationFailed, "The hyperlink could not be written.", cellText);
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["cell"] = address->ToA1();
        data["removed"] = removed;

        return ResultBuilder(removed ? "Removed the hyperlink from " + address->ToA1() + "."
                                     : "Linked " + address->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterRecalculate(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();

        nlohmann::json formulaError =
            Schema::Object("One formula cell that evaluates to a worksheet error.", {"sheet", "address", "error"},
                           nlohmann::json{{"sheet", Schema::String("Worksheet name.")},
                                          {"address", Schema::String("A1 cell address.")},
                                          {"formula", Schema::String("Formula text.")},
                                          {"error", Schema::String("Worksheet error literal such as \"#DIV/0!\".")}});

        auto definition = MakeDefinition("recalculate", "Recalculate formulas",
                                         "Recompute every formula cell and rewrite the cached results, so the "
                                         "workbook shows current values when opened. Circular references are "
                                         "reported and keep their previous values; cells whose result is a "
                                         "worksheet error are reported in formulaErrors.",
                                         "cells");
        definition.InputSchema = Schema::Object("Arguments of recalculate.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Recalculation result.", {"recalculatedCells"},
                           nlohmann::json{{"recalculatedCells", Schema::Integer("Formula cells recomputed.")},
                                          {"circularReferences", Schema::Array("Detected cycles.",
                                                                               Schema::String("Cycle as a chain of "
                                                                                              "cell addresses."))},
                                          {"formulaErrors", Schema::Array("Formula cells whose recomputed result is "
                                                                          "a worksheet error.",
                                                                          std::move(formulaError))}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return Recalculate(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome Recalculate(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        auto document = session.Editor().GetDocument();
        if (document == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The workbook has no package.", session.Session().Id());
        }

        std::string sheetName;
        if (arguments.contains("sheet"))
        {
            ToolOutcome failure;
            auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
            if (sheet == nullptr)
            {
                return failure;
            }

            sheetName = sheet->Name();
        }

        MutationGuard guard(session.Session());

        // The editor owns a live formula engine, so the recalculation runs in
        // memory; the path-based RecalculateWorkbook is never needed here.
        Excel::FormulaEngine engine(document);
        if (!engine.IsValid())
        {
            return MakeError(ErrorCode::OperationFailed, "The formula engine could not be created.");
        }

        const auto result = sheetName.empty() ? engine.Recalculate() : engine.RecalculateSheet(sheetName);
        if (!result.Succeeded())
        {
            return MakeError(ErrorCode::OperationFailed, result.Status.Message, sheetName);
        }

        guard.Commit();

        nlohmann::json cycles = nlohmann::json::array();
        for (const auto& cycle : result.CircularReferenceCycles)
        {
            std::string rendered;
            for (const auto& member : cycle)
            {
                if (!rendered.empty())
                {
                    rendered.append(" -> ");
                }

                rendered.append(member.Sheet).append("!").append(member.Address.ToA1());
            }

            cycles.push_back(std::move(rendered));
        }

        bool truncated = false;
        auto formulaErrors = CollectFormulaErrors(session.Editor(), sheetName, truncated);
        const auto errorCount = formulaErrors.size();

        nlohmann::json data = nlohmann::json::object();
        data["recalculatedCells"] = static_cast<UInt64>(result.RecalculatedCellCount);
        data["circularReferences"] = std::move(cycles);
        data["formulaErrors"] = std::move(formulaErrors);

        std::string summary = "Recalculated " + std::to_string(result.RecalculatedCellCount) + " formula cell(s)";
        if (errorCount > 0)
        {
            summary.append("; ").append(std::to_string(errorCount)).append(" evaluate to a worksheet error");
        }

        summary.push_back('.');

        return ResultBuilder(std::move(summary))
            .WithSession(session.Session())
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    /// Cap on the number of failing formula cells one recalculation reports.
    static constexpr Size MaximumReportedFormulaErrors = 200;

    /**
     * @brief Formula cells whose cached result is a worksheet error.
     *
     * RecalculationResult reports circular references but not per-cell error
     * results, so the failing cells are collected from the cached values the
     * engine has just written back.
     *
     * @param sheetName Restricts the scan to one worksheet; empty scans all.
     * @param truncated Set when the cap cut the list short.
     */
    static nlohmann::json CollectFormulaErrors(Excel::ExcelDocumentEditor& editor, const std::string& sheetName,
                                               bool& truncated)
    {
        nlohmann::json errors = nlohmann::json::array();
        for (const auto& sheet : editor.Worksheets())
        {
            if (sheet == nullptr)
            {
                continue;
            }

            if (!sheetName.empty() && !AsciiText::EqualsIgnoreCase(sheet->Name(), sheetName))
            {
                continue;
            }

            for (const auto& address : sheet->StoredCellAddresses())
            {
                const auto stored = sheet->GetCellValue(address);
                if (!stored.has_value() || stored->Kind() != Excel::CellValueKind::Formula)
                {
                    continue;
                }

                const auto formula = stored->FormulaValue();
                if (formula.CachedKind != Excel::FormulaCachedValueKind::Error)
                {
                    continue;
                }

                if (errors.size() >= MaximumReportedFormulaErrors)
                {
                    truncated = true;
                    return errors;
                }

                nlohmann::json entry = nlohmann::json::object();
                entry["sheet"] = sheet->Name();
                entry["address"] = address.ToA1();
                entry["formula"] = formula.Formula;
                entry["error"] = formula.CachedText;
                errors.push_back(std::move(entry));
            }
        }

        return errors;
    }

    // --- formatting ---------------------------------------------------------

    static void RegisterMergeCells(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range to merge or unmerge.");
        properties["unmerge"] = Schema::BooleanWithDefault("Split an existing merged range instead.", false);

        auto definition = MakeDefinition("merge_cells", "Merge or unmerge cells",
                                         "Merge a rectangular range into one cell, or split it again.",
                                         "formatting");
        definition.InputSchema =
            Schema::Object("Arguments of merge_cells.", {"documentId", "range"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Merge result.", {"range"},
                                            nlohmann::json{{"range", Schema::String("Affected A1 range.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"range", "A1:C1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return MergeCells(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome MergeCells(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto range = ExcelAddressing::ParseRange(arguments.value("range", std::string()), failure);
        if (!range.has_value())
        {
            return failure;
        }

        const bool unmerge = arguments.value("unmerge", false);

        MutationGuard guard(session.Session());

        const auto result = unmerge ? sheet->UnmergeRange(*range) : sheet->MergeRange(*range);
        if (!result.Succeeded())
        {
            return MakeError(ErrorCode::OperationFailed, result.Message, range->ToA1());
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["range"] = range->ToA1();

        return ResultBuilder((unmerge ? "Unmerged " : "Merged ") + range->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterFormatRange(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range to format.");
        properties["number_format"] = Schema::String("Number format code, for example \"#,##0.00\".");
        properties["font"] = Schema::Object("Font settings.", {},
                                            nlohmann::json{{"name", Schema::String("Font family.")},
                                                           {"sizePt", Schema::Number("Font size in points.")},
                                                           {"bold", Schema::Boolean("Bold text.")},
                                                           {"italic", Schema::Boolean("Italic text.")},
                                                           {"color", Schema::String("Font color as \"#RRGGBB\".")}});
        properties["fill"] = Schema::Object("Solid cell background.", {"color"},
                                            nlohmann::json{{"color", Schema::String("Fill color as \"#RRGGBB\".")}});
        properties["border"] = Schema::Object(
            "Outline border applied to all four sides.", {},
            nlohmann::json{{"style", Schema::Enumeration("Border line style.",
                                                         {"none", "thin", "medium", "thick", "dashed", "dotted",
                                                          "double"})},
                           {"color", Schema::String("Border color as \"#RRGGBB\".")}});
        properties["alignment"] = Schema::Object(
            "Cell alignment.", {},
            nlohmann::json{{"horizontal", Schema::Enumeration("Horizontal alignment.",
                                                              {"general", "left", "center", "right", "fill",
                                                               "justify", "distributed"})},
                           {"vertical", Schema::Enumeration("Vertical alignment.",
                                                            {"top", "center", "bottom", "justify", "distributed"})},
                           {"wrap", Schema::Boolean("Wrap text inside the cell.")}});

        auto definition = MakeDefinition("format_range", "Format cell range",
                                         "Apply a number format, font, fill, border, and alignment to a range. "
                                         "Only the members you pass take part in the resulting cell format. Every "
                                         "color is an opaque \"#RRGGBB\" value; an unreadable one is reported "
                                         "rather than ignored.",
                                         "formatting");
        definition.InputSchema =
            Schema::Object("Arguments of format_range.", {"documentId", "range"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Formatting result.", {"range", "styleIndex"},
                                            nlohmann::json{{"range", Schema::String("Formatted A1 range.")},
                                                           {"styleIndex", Schema::Integer("Registered style index.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"},
                                            {"range", "A1:C1"},
                                            {"font", nlohmann::json{{"bold", true}}}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return FormatRange(context, arguments); };
        registry.Add(std::move(definition));
    }

    /**
     * @brief Reads an optional "#RRGGBB" color member.
     *
     * A member that is present but unreadable is reported rather than dropped:
     * a mistyped color is a user mistake, and silently ignoring it would hand
     * back a document that does not match what was asked for. An absent member
     * leaves @p result empty and succeeds.
     *
     * @param label Names the member in the error message, for example "font".
     */
    static bool ReadColor(const nlohmann::json& owner, const char* name, const char* label,
                          std::optional<Excel::ExcelColor>& result, ToolOutcome& failure)
    {
        result.reset();

        const auto member = owner.find(name);
        if (member == owner.end() || member->is_null())
        {
            return true;
        }

        const std::string text = member->is_string() ? member->get<std::string>() : member->dump();
        std::optional<Color> color;
        if (member->is_string())
        {
            color = ParseColor(text);
        }

        if (!color.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid,
                                "The " + std::string(label) + " color is not a valid \"#RRGGBB\" value.", text,
                                "Use a hexadecimal color such as \"#1F4E79\".");
            return false;
        }

        // Excel stores colors as ARGB; the tools accept opaque RGB only.
        auto hex = color->ToHexString();
        if (!hex.empty() && hex.front() == '#')
        {
            hex.erase(hex.begin());
        }

        result = Excel::ExcelColor::Rgb("FF" + hex);
        return true;
    }

    static Excel::ExcelBorderStyle ParseBorderStyle(const std::string& token)
    {
        if (token == "thin")
        {
            return Excel::ExcelBorderStyle::Thin;
        }

        if (token == "medium")
        {
            return Excel::ExcelBorderStyle::Medium;
        }

        if (token == "thick")
        {
            return Excel::ExcelBorderStyle::Thick;
        }

        if (token == "dashed")
        {
            return Excel::ExcelBorderStyle::Dashed;
        }

        if (token == "dotted")
        {
            return Excel::ExcelBorderStyle::Dotted;
        }

        if (token == "double")
        {
            return Excel::ExcelBorderStyle::Double;
        }

        return Excel::ExcelBorderStyle::None;
    }

    static std::optional<Excel::ExcelHorizontalAlignment> ParseHorizontalAlignment(const std::string& token)
    {
        if (token == "left")
        {
            return Excel::ExcelHorizontalAlignment::Left;
        }

        if (token == "center")
        {
            return Excel::ExcelHorizontalAlignment::Center;
        }

        if (token == "right")
        {
            return Excel::ExcelHorizontalAlignment::Right;
        }

        if (token == "fill")
        {
            return Excel::ExcelHorizontalAlignment::Fill;
        }

        if (token == "justify")
        {
            return Excel::ExcelHorizontalAlignment::Justify;
        }

        if (token == "distributed")
        {
            return Excel::ExcelHorizontalAlignment::Distributed;
        }

        if (token == "general")
        {
            return Excel::ExcelHorizontalAlignment::General;
        }

        return std::nullopt;
    }

    static std::optional<Excel::ExcelVerticalAlignment> ParseVerticalAlignment(const std::string& token)
    {
        if (token == "top")
        {
            return Excel::ExcelVerticalAlignment::Top;
        }

        if (token == "center")
        {
            return Excel::ExcelVerticalAlignment::Center;
        }

        if (token == "bottom")
        {
            return Excel::ExcelVerticalAlignment::Bottom;
        }

        if (token == "justify")
        {
            return Excel::ExcelVerticalAlignment::Justify;
        }

        if (token == "distributed")
        {
            return Excel::ExcelVerticalAlignment::Distributed;
        }

        return std::nullopt;
    }

    static ToolOutcome FormatRange(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto range = ExcelAddressing::ParseRange(arguments.value("range", std::string()), failure);
        if (!range.has_value())
        {
            return failure;
        }

        Excel::ExcelStyle style;

        const auto numberFormat = arguments.value("number_format", std::string());
        if (!numberFormat.empty())
        {
            auto format = Excel::ExcelNumberFormat::Custom(numberFormat);
            if (!format.has_value())
            {
                return MakeError(ErrorCode::Unsupported, "The number format code was rejected.", numberFormat,
                                 "Use an Excel number format such as \"#,##0.00\".");
            }

            style.NumberFormat = *format;
        }

        const auto font = arguments.find("font");
        if (font != arguments.end() && font->is_object())
        {
            Excel::ExcelFont value;
            const auto name = font->value("name", std::string());
            if (!name.empty())
            {
                value.Name = name;
            }

            const auto size = font->value("sizePt", 0.0);
            if (size > 0.0)
            {
                value.Size = size;
            }

            value.Bold = font->value("bold", false);
            value.Italic = font->value("italic", false);
            if (!ReadColor(*font, "color", "font", value.Color, failure))
            {
                return failure;
            }

            style.Font = value;
        }

        const auto fill = arguments.find("fill");
        if (fill != arguments.end() && fill->is_object())
        {
            std::optional<Excel::ExcelColor> color;
            if (!ReadColor(*fill, "color", "fill", color, failure))
            {
                return failure;
            }

            if (!color.has_value())
            {
                return MakeError(ErrorCode::InputInvalid, "A fill needs a \"color\".", {},
                                 "Pass fill.color as \"#RRGGBB\", for example \"#FFFF00\".");
            }

            Excel::ExcelFill value;
            value.Kind = Excel::ExcelFillKind::Pattern;
            value.Pattern = Excel::ExcelFillPattern::Solid;
            value.Foreground = color;
            style.Fill = value;
        }

        const auto border = arguments.find("border");
        if (border != arguments.end() && border->is_object())
        {
            Excel::ExcelBorderSide side;
            side.Style = ParseBorderStyle(border->value("style", std::string("thin")));
            if (!ReadColor(*border, "color", "border", side.Color, failure))
            {
                return failure;
            }

            Excel::ExcelBorder value;
            value.Left = side;
            value.Right = side;
            value.Top = side;
            value.Bottom = side;
            style.Border = value;
        }

        const auto alignment = arguments.find("alignment");
        if (alignment != arguments.end() && alignment->is_object())
        {
            Excel::ExcelAlignment value;
            const auto horizontal = alignment->value("horizontal", std::string());
            if (!horizontal.empty())
            {
                value.Horizontal = ParseHorizontalAlignment(horizontal);
            }

            const auto vertical = alignment->value("vertical", std::string());
            if (!vertical.empty())
            {
                value.Vertical = ParseVerticalAlignment(vertical);
            }

            if (alignment->contains("wrap"))
            {
                value.WrapText = alignment->value("wrap", false);
            }

            style.Alignment = value;
        }

        MutationGuard guard(session.Session());

        auto styles = session.Editor().Styles();
        const auto registration = styles.GetOrAdd(style);
        if (!registration.Succeeded())
        {
            return MakeError(ErrorCode::Unsupported, registration.Status.Message, range->ToA1(),
                             "Simplify the requested formatting; not every combination is expressible.");
        }

        const auto applied = styles.ApplyToRange(*sheet, *range, registration.StyleIndex);
        if (!applied.Succeeded())
        {
            return MakeError(ErrorCode::OperationFailed, applied.Message, range->ToA1());
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["range"] = range->ToA1();
        data["styleIndex"] = registration.StyleIndex;

        return ResultBuilder("Formatted " + range->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetColumnWidth(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["columns"] = Schema::String("Column or column band, for example \"B\" or \"B:D\".");
        properties["width"] = Schema::Number("Width in characters of the default font.");

        auto definition = MakeDefinition("set_column_width", "Set column width",
                                         "Set the width of one column or a band of columns, in character units.",
                                         "formatting");
        definition.InputSchema = Schema::Object("Arguments of set_column_width.", {"documentId", "columns", "width"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Column widths.", {"columns"},
                                            nlohmann::json{{"columns", Schema::Integer("Columns changed.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"columns", "A:C"}, {"width", 18.0}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetColumnWidth(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetColumnWidth(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto band = arguments.value("columns", std::string());
        UInt32 first = 0;
        UInt32 last = 0;
        if (!ExcelAddressing::ParseColumnBand(band, first, last))
        {
            return MakeError(ErrorCode::RangeInvalid, "'" + band + "' is not a column or column band.", band,
                             "Use \"B\" or \"B:D\".");
        }

        MutationGuard guard(session.Session());

        const auto width = arguments.value("width", 0.0);
        Size changed = 0;
        for (UInt32 column = first; column <= last; ++column)
        {
            auto dimension = sheet->GetColumnDimension(column).value_or(Excel::ColumnDimension{});
            dimension.Width = width;
            if (sheet->SetColumnDimension(column, dimension))
            {
                ++changed;
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["columns"] = static_cast<UInt64>(changed);

        return ResultBuilder("Set the width of " + std::to_string(changed) + " column(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetRowHeight(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["rows"] = Schema::String("Row or row band, for example \"2\" or \"2:5\".");
        properties["height"] = Schema::Number("Row height in points.");

        auto definition = MakeDefinition("set_row_height", "Set row height",
                                         "Set the height of one row or a band of rows, in points.", "formatting");
        definition.InputSchema =
            Schema::Object("Arguments of set_row_height.", {"documentId", "rows", "height"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Row heights.", {"rows"}, nlohmann::json{{"rows", Schema::Integer("Rows changed.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"rows", "1"}, {"height", 24.0}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetRowHeight(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetRowHeight(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto band = arguments.value("rows", std::string());
        UInt32 first = 0;
        UInt32 last = 0;
        if (!ExcelAddressing::ParseRowBand(band, first, last))
        {
            return MakeError(ErrorCode::RangeInvalid, "'" + band + "' is not a row or row band.", band,
                             "Use \"2\" or \"2:5\".");
        }

        MutationGuard guard(session.Session());

        const auto height = arguments.value("height", 0.0);
        Size changed = 0;
        for (UInt32 row = first; row <= last; ++row)
        {
            auto dimension = sheet->GetRowDimension(row).value_or(Excel::RowDimension{});
            dimension.Height = height;
            if (sheet->SetRowDimension(row, dimension))
            {
                ++changed;
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["rows"] = static_cast<UInt64>(changed);

        return ResultBuilder("Set the height of " + std::to_string(changed) + " row(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterFreezePanes(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["cell"] = Schema::String("Top-left cell of the scrolling area; \"A1\" removes the freeze.");

        auto definition = MakeDefinition("freeze_panes", "Freeze panes",
                                         "Freeze the rows above and the columns left of a cell. Pass \"A1\" to "
                                         "unfreeze.",
                                         "formatting");
        definition.InputSchema =
            Schema::Object("Arguments of freeze_panes.", {"documentId", "cell"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Freeze state.", {"frozenRows", "frozenColumns"},
                           nlohmann::json{{"frozenRows", Schema::Integer("Rows kept visible.")},
                                          {"frozenColumns", Schema::Integer("Columns kept visible.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"cell", "B2"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return FreezePanes(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome FreezePanes(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto address = ExcelAddressing::ParseCell(arguments.value("cell", std::string()), failure);
        if (!address.has_value())
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        auto view = sheet->GetView();
        view.FrozenRows = address->Row().Value() - 1;
        view.FrozenColumns = address->Column().Value() - 1;
        if (!sheet->SetView(view))
        {
            return MakeError(ErrorCode::OperationFailed, "The worksheet view could not be updated.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["frozenRows"] = view.FrozenRows;
        data["frozenColumns"] = view.FrozenColumns;

        return ResultBuilder(view.FrozenRows == 0 && view.FrozenColumns == 0
                                 ? "Removed the frozen panes."
                                 : "Froze " + std::to_string(view.FrozenRows) + " row(s) and " +
                                       std::to_string(view.FrozenColumns) + " column(s).")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    // --- analysis -----------------------------------------------------------

    static void RegisterAddTable(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range covered by the table, including the header row.");
        properties["name"] = Schema::String("Table name; generated when omitted.");
        properties["header_row"] =
            Schema::BooleanWithDefault("Take the column names from the first row of the range.", true);

        auto definition = MakeDefinition("add_table", "Add table",
                                         "Turn a range into a structured table (list object) with named columns.",
                                         "analysis");
        definition.InputSchema =
            Schema::Object("Arguments of add_table.", {"documentId", "range"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New table.", {"name", "range"},
                                            nlohmann::json{{"name", Schema::String("Table name.")},
                                                           {"range", Schema::String("A1 range of the table.")},
                                                           {"columns", Schema::Array("Column names.",
                                                                                     Schema::String("Column name."))}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"range", "A1:C10"}, {"name", "Sales"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddTable(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto range = ExcelAddressing::ParseRange(arguments.value("range", std::string()), failure);
        if (!range.has_value())
        {
            return failure;
        }

        auto name = arguments.value("name", std::string());
        if (name.empty())
        {
            name = "Table" + std::to_string(sheet->Tables().size() + 1);
        }

        if (!Excel::IsValidExcelTableName(name))
        {
            return MakeError(ErrorCode::InputInvalid, "'" + name + "' is not a valid table name.", name,
                             "Use a name that starts with a letter and contains no spaces.");
        }

        const auto sharedStrings = session.Editor().SharedStrings();
        const bool headerRow = arguments.value("header_row", true);

        std::vector<Excel::ExcelTableColumn> columns;
        nlohmann::json columnNames = nlohmann::json::array();
        UInt32 columnId = 1;
        for (UInt32 column = range->First().Column().Value(); column <= range->Last().Column().Value(); ++column)
        {
            Excel::ExcelTableColumn definition;
            definition.Id = columnId;
            definition.Name = "Column" + std::to_string(columnId);
            if (headerRow)
            {
                const auto address = Excel::CellAddress::TryCreate(range->First().Row().Value(), column);
                if (address.has_value())
                {
                    const auto stored = sheet->GetCellValue(*address);
                    if (stored.has_value())
                    {
                        const auto text = ExcelAddressing::CellValueToText(*stored, sharedStrings);
                        if (!text.empty())
                        {
                            definition.Name = text;
                        }
                    }
                }
            }

            columnNames.push_back(definition.Name);
            columns.push_back(std::move(definition));
            ++columnId;
        }

        MutationGuard guard(session.Session());

        auto table = sheet->CreateTable(name, *range, columns);
        if (table == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The table could not be created.", name,
                             "The name may already be in use, or the range may overlap another table.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = table->Name();
        data["range"] = range->ToA1();
        data["columns"] = std::move(columnNames);

        return ResultBuilder("Created table '" + table->Name() + "' over " + range->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddNamedRange(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["name"] = Schema::String("Defined name, unique in its scope.");
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range the name refers to.");
        properties["scope"] = Schema::EnumerationWithDefault("Where the name is visible.", {"workbook", "sheet"},
                                                             "workbook");

        auto definition = MakeDefinition("add_named_range", "Add named range",
                                         "Define a workbook or worksheet name for a range so formulas can refer to "
                                         "it by name.",
                                         "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_named_range.", {"documentId", "name", "range"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New name.", {"name", "formula"},
                                            nlohmann::json{{"name", Schema::String("Defined name.")},
                                                           {"formula", Schema::String("Reference the name resolves "
                                                                                      "to.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"name", "SalesData"}, {"range", "A1:C10"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddNamedRange(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddNamedRange(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto range = ExcelAddressing::ParseRange(arguments.value("range", std::string()), failure);
        if (!range.has_value())
        {
            return failure;
        }

        const auto name = arguments.value("name", std::string());
        if (!Excel::NamedRangeManager::IsValidName(name))
        {
            return MakeError(ErrorCode::InputInvalid, "'" + name + "' is not a valid defined name.", name,
                             "A name starts with a letter or underscore and contains no spaces.");
        }

        MutationGuard guard(session.Session());

        Excel::NamedRangeManager manager(session.Editor().GetDocument());
        const auto scope = arguments.value("scope", std::string("workbook"));
        const Excel::SheetCellRange reference(sheet->Name(), *range);
        const auto result =
            scope == "sheet"
                ? manager.Create(name, reference, Excel::NamedRangeScope::Sheet, sheet->Name())
                : manager.Create(name, reference, Excel::NamedRangeScope::Workbook);
        if (!result.Succeeded())
        {
            return MakeError(ErrorCode::OperationFailed, result.Message, name);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = name;
        data["formula"] = reference.ToFormula();

        return ResultBuilder("Defined the name '" + name + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddDataValidation(ToolRegistry& registry)
    {
        nlohmann::json rule = Schema::Object(
            "Validation rule.", {"type"},
            nlohmann::json{
                {"type", Schema::Enumeration("What the rule constrains.",
                                             {"list", "whole", "decimal", "date", "time", "textLength", "custom"})},
                {"operator", Schema::Enumeration("Comparison for scalar rules.",
                                                 {"between", "notBetween", "equal", "notEqual", "lessThan",
                                                  "lessThanOrEqual", "greaterThan", "greaterThanOrEqual"})},
                {"formula1", Schema::String("First bound, list source, or custom formula.")},
                {"formula2", Schema::String("Second bound for between and notBetween.")},
                {"values", Schema::Array("Inline list entries; an alternative to formula1 for a list rule.",
                                         Schema::String("One allowed value."))},
                {"allow_blank", Schema::Boolean("Accept an empty cell.")},
                {"input_message", Schema::String("Message shown when the cell is selected.")},
                {"error_message", Schema::String("Message shown when input is rejected.")}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range the rule applies to.");
        properties["rule"] = std::move(rule);

        auto definition = MakeDefinition("add_data_validation", "Add data validation",
                                         "Constrain what a range accepts: a value list, a numeric or date range, a "
                                         "text length, or a custom formula.",
                                         "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_data_validation.",
                                                {"documentId", "range", "rule"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New validation.", {"range"},
                                            nlohmann::json{{"range", Schema::String("Constrained A1 range.")},
                                                           {"type", Schema::String("Rule type applied.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"range", "B2:B20"},
            {"rule", nlohmann::json{{"type", "list"}, {"values", nlohmann::json::array({"North", "South"})}}}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddDataValidation(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::optional<Excel::DataValidationType> ParseValidationType(const std::string& token)
    {
        if (token == "list")
        {
            return Excel::DataValidationType::List;
        }

        if (token == "whole")
        {
            return Excel::DataValidationType::Whole;
        }

        if (token == "decimal")
        {
            return Excel::DataValidationType::Decimal;
        }

        if (token == "date")
        {
            return Excel::DataValidationType::Date;
        }

        if (token == "time")
        {
            return Excel::DataValidationType::Time;
        }

        if (token == "textLength")
        {
            return Excel::DataValidationType::TextLength;
        }

        if (token == "custom")
        {
            return Excel::DataValidationType::Custom;
        }

        return std::nullopt;
    }

    static std::optional<Excel::DataValidationOperator> ParseValidationOperator(const std::string& token)
    {
        if (token == "between")
        {
            return Excel::DataValidationOperator::Between;
        }

        if (token == "notBetween")
        {
            return Excel::DataValidationOperator::NotBetween;
        }

        if (token == "equal")
        {
            return Excel::DataValidationOperator::Equal;
        }

        if (token == "notEqual")
        {
            return Excel::DataValidationOperator::NotEqual;
        }

        if (token == "lessThan")
        {
            return Excel::DataValidationOperator::LessThan;
        }

        if (token == "lessThanOrEqual")
        {
            return Excel::DataValidationOperator::LessThanOrEqual;
        }

        if (token == "greaterThan")
        {
            return Excel::DataValidationOperator::GreaterThan;
        }

        if (token == "greaterThanOrEqual")
        {
            return Excel::DataValidationOperator::GreaterThanOrEqual;
        }

        return std::nullopt;
    }

    static ToolOutcome AddDataValidation(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto range = ExcelAddressing::ParseRange(arguments.value("range", std::string()), failure);
        if (!range.has_value())
        {
            return failure;
        }

        const auto& rule = arguments.at("rule");
        const auto typeToken = rule.value("type", std::string());
        const auto type = ParseValidationType(typeToken);
        if (!type.has_value())
        {
            return MakeError(ErrorCode::InputInvalid, "Unknown validation type '" + typeToken + "'.", typeToken);
        }

        Excel::ExcelDataValidationDefinition definition;
        definition.Type = *type;
        definition.Ranges.push_back(*range);
        definition.AllowBlank = rule.value("allow_blank", false);

        const auto operation = rule.value("operator", std::string());
        if (!operation.empty())
        {
            definition.Operation = ParseValidationOperator(operation);
            if (!definition.Operation.has_value())
            {
                return MakeError(ErrorCode::InputInvalid, "Unknown validation operator '" + operation + "'.",
                                 operation);
            }
        }

        const auto values = rule.find("values");
        if (values != rule.end() && values->is_array() && !values->empty())
        {
            std::string inline_;
            for (const auto& value : *values)
            {
                if (!inline_.empty())
                {
                    inline_.push_back(',');
                }

                inline_.append(value.get<std::string>());
            }

            definition.Formula1 = "\"" + inline_ + "\"";
        }
        else if (rule.contains("formula1"))
        {
            definition.Formula1 = rule.value("formula1", std::string());
        }

        if (rule.contains("formula2"))
        {
            definition.Formula2 = rule.value("formula2", std::string());
        }

        const auto inputMessage = rule.value("input_message", std::string());
        if (!inputMessage.empty())
        {
            definition.ShowInputMessage = true;
            definition.Prompt = inputMessage;
        }

        const auto errorMessage = rule.value("error_message", std::string());
        if (!errorMessage.empty())
        {
            definition.ShowErrorMessage = true;
            definition.Error = errorMessage;
        }

        if (!Excel::IsValidExcelDataValidation(definition))
        {
            return MakeError(ErrorCode::InputInvalid, "The validation rule is incomplete for its type.", typeToken,
                             "A scalar rule needs an operator and one or two formulas; a list rule needs values or "
                             "formula1.");
        }

        MutationGuard guard(session.Session());

        if (sheet->CreateDataValidation(definition) == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The data validation could not be created.",
                             range->ToA1());
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["range"] = range->ToA1();
        data["type"] = typeToken;

        return ResultBuilder("Added a '" + typeToken + "' validation to " + range->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddConditionalFormatting(ToolRegistry& registry)
    {
        nlohmann::json rule = Schema::Object(
            "Conditional formatting rule.", {"type"},
            nlohmann::json{
                {"type", Schema::Enumeration("Rule kind.",
                                             {"cellIs", "expression", "containsText", "notContainsText",
                                              "beginsWith", "endsWith", "uniqueValues", "duplicateValues",
                                              "containsBlanks", "notContainsBlanks"})},
                {"operator", Schema::Enumeration("Comparison for cellIs rules.",
                                                 {"lessThan", "lessThanOrEqual", "equal", "notEqual",
                                                  "greaterThanOrEqual", "greaterThan", "between", "notBetween"})},
                {"formula1", Schema::String("First formula or bound.")},
                {"formula2", Schema::String("Second bound for between and notBetween.")},
                {"text", Schema::String("Text for the text-matching rules.")},
                {"stop_if_true", Schema::Boolean("Stop evaluating further rules when this one matches.")}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range the rule applies to.");
        properties["rule"] = std::move(rule);

        auto definition = MakeDefinition(
            "add_conditional_formatting", "Add conditional formatting",
            "Add a conditional formatting rule to a range. Color scales and data bars are not offered in this "
            "version; use a cellIs or expression rule instead.",
            "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_conditional_formatting.",
                                                {"documentId", "range", "rule"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New rule.", {"range"},
                                            nlohmann::json{{"range", Schema::String("A1 range.")},
                                                           {"type", Schema::String("Rule kind applied.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"range", "B2:B20"},
            {"rule", nlohmann::json{{"type", "cellIs"}, {"operator", "greaterThan"}, {"formula1", "100"}}}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddConditionalFormatting(context, arguments); };
        registry.Add(std::move(definition));
    }

    static std::optional<Excel::ConditionalFormattingOperator> ParseFormattingOperator(const std::string& token)
    {
        if (token == "lessThan")
        {
            return Excel::ConditionalFormattingOperator::LessThan;
        }

        if (token == "lessThanOrEqual")
        {
            return Excel::ConditionalFormattingOperator::LessThanOrEqual;
        }

        if (token == "equal")
        {
            return Excel::ConditionalFormattingOperator::Equal;
        }

        if (token == "notEqual")
        {
            return Excel::ConditionalFormattingOperator::NotEqual;
        }

        if (token == "greaterThanOrEqual")
        {
            return Excel::ConditionalFormattingOperator::GreaterThanOrEqual;
        }

        if (token == "greaterThan")
        {
            return Excel::ConditionalFormattingOperator::GreaterThan;
        }

        if (token == "between")
        {
            return Excel::ConditionalFormattingOperator::Between;
        }

        if (token == "notBetween")
        {
            return Excel::ConditionalFormattingOperator::NotBetween;
        }

        return std::nullopt;
    }

    static ToolOutcome AddConditionalFormatting(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto range = ExcelAddressing::ParseRange(arguments.value("range", std::string()), failure);
        if (!range.has_value())
        {
            return failure;
        }

        const auto& rule = arguments.at("rule");
        const auto type = rule.value("type", std::string());
        const std::vector<Excel::CellRange> ranges{*range};
        const auto formula1 = rule.value("formula1", std::string());
        const auto formula2 = rule.value("formula2", std::string());
        const auto text = rule.value("text", std::string());

        std::optional<Excel::ExcelConditionalFormattingDefinition> definition;
        if (type == "expression")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::Expression(ranges, formula1);
        }
        else if (type == "cellIs")
        {
            const auto operation = ParseFormattingOperator(rule.value("operator", std::string()));
            if (!operation.has_value())
            {
                return MakeError(ErrorCode::InputInvalid, "A cellIs rule needs a known 'operator'.", type);
            }

            if (*operation == Excel::ConditionalFormattingOperator::Between)
            {
                definition = Excel::ExcelConditionalFormattingDefinition::Between(ranges, formula1, formula2);
            }
            else if (*operation == Excel::ConditionalFormattingOperator::NotBetween)
            {
                definition = Excel::ExcelConditionalFormattingDefinition::NotBetween(ranges, formula1, formula2);
            }
            else
            {
                definition = Excel::ExcelConditionalFormattingDefinition::CellIs(ranges, *operation, formula1);
            }
        }
        else if (type == "containsText")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::ContainsText(ranges, text);
        }
        else if (type == "notContainsText")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::NotContainsText(ranges, text);
        }
        else if (type == "beginsWith")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::BeginsWith(ranges, text);
        }
        else if (type == "endsWith")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::EndsWith(ranges, text);
        }
        else if (type == "uniqueValues")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::UniqueValues(ranges);
        }
        else if (type == "duplicateValues")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::DuplicateValues(ranges);
        }
        else if (type == "containsBlanks")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::ContainsBlanks(ranges);
        }
        else if (type == "notContainsBlanks")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::NotContainsBlanks(ranges);
        }
        else
        {
            return MakeError(ErrorCode::Unsupported, "The rule kind '" + type + "' is not offered by this server.",
                             type, "Use cellIs or expression, or set the formatting with format_range.");
        }

        definition->StopIfTrue = rule.value("stop_if_true", false);

        MutationGuard guard(session.Session());

        if (sheet->CreateConditionalFormatting(*definition) == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The conditional formatting could not be created.",
                             range->ToA1());
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["range"] = range->ToA1();
        data["type"] = type;

        return ResultBuilder("Added a '" + type + "' rule to " + range->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddChart(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["type"] = Schema::Enumeration("Chart type.", {"bar", "column", "line", "pie", "scatter", "area"});
        properties["data_range"] = Schema::String(
            "A1 range holding the series values. A range spanning several columns becomes one series per column; "
            "see series_in.");
        properties["series_in"] = Schema::EnumerationWithDefault(
            "Whether each column or each row of data_range is one series.", {"columns", "rows"}, "columns");
        properties["series_names"] =
            Schema::Array("Series names in data order; generated names are used for the rest.",
                          Schema::String("One series name."));
        properties["categories_range"] = Schema::String(
            "A1 range holding the category labels, shared by every series; the X values of a scatter chart.");
        properties["anchor_cell"] = Schema::String("A1 cell the chart's top-left corner sits on.");
        properties["width"] = Schema::Length("Chart width; defaults to about 15 cm.");
        properties["height"] = Schema::Length("Chart height; defaults to about 8 cm.");
        properties["title"] = Schema::String("Chart title.");

        auto definition = MakeDefinition("add_chart", "Add chart",
                                         "Add a chart driven by a range of values and optional category labels. A "
                                         "multi-column data range plots one series per column, or per row when "
                                         "series_in is \"rows\". A pie chart plots one series and warns when the "
                                         "range holds more. This version offers the basic chart types only.",
                                         "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_chart.",
                                                {"documentId", "type", "data_range", "anchor_cell"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New chart.", {"chartId"},
                                            nlohmann::json{{"chartId", Schema::Integer("Chart identifier.")},
                                                           {"anchor", Schema::String("A1 anchor cell.")},
                                                           {"seriesCount", Schema::Integer("Series the chart "
                                                                                           "plots.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"},
                                            {"type", "column"},
                                            {"data_range", "B2:B10"},
                                            {"anchor_cell", "E2"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddChart(context, arguments); };
        registry.Add(std::move(definition));
    }

    static Excel::ExcelChartType ParseChartType(const std::string& token)
    {
        if (token == "bar")
        {
            return Excel::ExcelChartType::Bar;
        }

        if (token == "line")
        {
            return Excel::ExcelChartType::Line;
        }

        if (token == "pie")
        {
            return Excel::ExcelChartType::Pie;
        }

        if (token == "scatter")
        {
            return Excel::ExcelChartType::XyScatter;
        }

        if (token == "area")
        {
            return Excel::ExcelChartType::Area;
        }

        return Excel::ExcelChartType::Column;
    }

    /**
     * @brief Splits a data range into one value range per column, or per row.
     *
     * A chart definition carries a vector of series, so a block of data is
     * expressible as several series rather than one; which axis of the block
     * separates them is the caller's choice.
     */
    static std::vector<Excel::CellRange> SplitSeriesRanges(const Excel::CellRange& range, bool byColumns)
    {
        const UInt32 firstRow = range.First().Row().Value();
        const UInt32 lastRow = range.Last().Row().Value();
        const UInt32 firstColumn = range.First().Column().Value();
        const UInt32 lastColumn = range.Last().Column().Value();

        std::vector<Excel::CellRange> ranges;
        if (byColumns)
        {
            for (UInt32 column = firstColumn; column <= lastColumn; ++column)
            {
                const auto first = Excel::CellAddress::TryCreate(firstRow, column);
                const auto last = Excel::CellAddress::TryCreate(lastRow, column);
                if (first.has_value() && last.has_value())
                {
                    ranges.emplace_back(*first, *last);
                }
            }

            return ranges;
        }

        for (UInt32 row = firstRow; row <= lastRow; ++row)
        {
            const auto first = Excel::CellAddress::TryCreate(row, firstColumn);
            const auto last = Excel::CellAddress::TryCreate(row, lastColumn);
            if (first.has_value() && last.has_value())
            {
                ranges.emplace_back(*first, *last);
            }
        }

        return ranges;
    }

    /// Chart anchors are a cell pair, so a size in length units becomes a span
    /// of columns and rows at the default column width and row height.
    static Excel::CellAddress ChartAnchorEnd(Excel::CellAddress from, const nlohmann::json& arguments)
    {
        constexpr Real DefaultColumnWidthPt = 48.0;
        constexpr Real DefaultRowHeightPt = 15.0;

        Real widthPt = 425.0;
        Real heightPt = 227.0;
        const auto width = arguments.find("width");
        if (width != arguments.end())
        {
            const auto parsed = ParseLength(*width);
            if (parsed.has_value())
            {
                widthPt = ToPointValue(*parsed);
            }
        }

        const auto height = arguments.find("height");
        if (height != arguments.end())
        {
            const auto parsed = ParseLength(*height);
            if (parsed.has_value())
            {
                heightPt = ToPointValue(*parsed);
            }
        }

        const auto columns = std::max<UInt32>(1, static_cast<UInt32>(widthPt / DefaultColumnWidthPt));
        const auto rows = std::max<UInt32>(1, static_cast<UInt32>(heightPt / DefaultRowHeightPt));
        const auto end =
            Excel::CellAddress::TryCreate(from.Row().Value() + rows, from.Column().Value() + columns);
        return end.value_or(from);
    }

    static ToolOutcome AddChart(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        const auto dataRange = ExcelAddressing::ParseRange(arguments.value("data_range", std::string()), failure);
        if (!dataRange.has_value())
        {
            return failure;
        }

        const auto anchor = ExcelAddressing::ParseCell(arguments.value("anchor_cell", std::string()), failure);
        if (!anchor.has_value())
        {
            return failure;
        }

        Excel::ExcelChartDefinition chart;
        chart.Type = ParseChartType(arguments.value("type", std::string("column")));
        chart.Title = arguments.value("title", std::string());
        chart.From = *anchor;
        chart.To = ChartAnchorEnd(*anchor, arguments);

        std::optional<Excel::CellRange> categoryRange;
        const auto categories = arguments.value("categories_range", std::string());
        if (!categories.empty())
        {
            categoryRange = ExcelAddressing::ParseRange(categories, failure);
            if (!categoryRange.has_value())
            {
                return failure;
            }
        }

        const bool byColumns = arguments.value("series_in", std::string("columns")) != "rows";
        auto seriesRanges = SplitSeriesRanges(*dataRange, byColumns);
        if (seriesRanges.empty())
        {
            seriesRanges.push_back(*dataRange);
        }

        std::string warning;
        if (chart.Type == Excel::ExcelChartType::Pie && seriesRanges.size() > 1)
        {
            // A pie chart plots exactly one series; charting the rest anyway
            // would quietly produce something the caller did not ask for.
            warning = std::string("A pie chart plots one series, so only the first ") +
                      (byColumns ? "column" : "row") + " of " + dataRange->ToA1() + " is charted.";
            seriesRanges.resize(1);
        }

        const auto seriesNames = arguments.find("series_names");
        for (Size index = 0; index < seriesRanges.size(); ++index)
        {
            Excel::ExcelChartSeries series;
            series.Name = "Series " + std::to_string(index + 1);
            if (seriesNames != arguments.end() && seriesNames->is_array() && index < seriesNames->size() &&
                (*seriesNames)[index].is_string())
            {
                series.Name = (*seriesNames)[index].get<std::string>();
            }
            else if (seriesRanges.size() == 1 && !chart.Title.empty())
            {
                series.Name = chart.Title;
            }

            series.Values = seriesRanges[index];
            if (categoryRange.has_value())
            {
                // Scatter and bubble charts pair X values with the values;
                // the category axis belongs to the other chart types.
                if (chart.Type == Excel::ExcelChartType::XyScatter ||
                    chart.Type == Excel::ExcelChartType::Bubble)
                {
                    series.XValues = *categoryRange;
                }
                else
                {
                    series.Categories = *categoryRange;
                }
            }

            chart.Series.push_back(std::move(series));
        }

        MutationGuard guard(session.Session());

        const auto id = sheet->AddChart(chart);
        if (!id.has_value())
        {
            return MakeError(ErrorCode::OperationFailed, "The chart could not be created.", dataRange->ToA1(),
                             "Check that the data range is inside the worksheet and the anchor does not overlap "
                             "another drawing.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["chartId"] = *id;
        data["anchor"] = anchor->ToA1();
        data["seriesCount"] = static_cast<UInt64>(chart.Series.size());

        ResultBuilder builder("Added a chart with " + std::to_string(chart.Series.size()) +
                              " series anchored at " + anchor->ToA1() + ".");
        builder.WithSession(session.Session()).WithData(std::move(data));
        if (!warning.empty())
        {
            builder.WithWarning("chart_series_dropped", warning, dataRange->ToA1());
        }

        return builder.Build();
    }

    static void RegisterAddPivotTable(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["source_sheet"] = SheetReferenceProperty(
            "Worksheet holding the source data, by name or 1-based index; defaults to target_sheet.");
        properties["source_range"] = Schema::String("A1 range of the source data, including its header row.");
        properties["target_sheet"] =
            SheetReferenceProperty("Worksheet the report is written to, by name or 1-based index.");
        properties["target_cell"] = Schema::String("A1 cell of the report's top-left corner.");
        properties["name"] = Schema::String("Pivot table name; generated when omitted.");
        properties["rows"] = Schema::Array("Source field names placed on the row axis.",
                                           Schema::String("Field name from the header row."));
        properties["columns"] = Schema::Array("Source field names placed on the column axis.",
                                              Schema::String("Field name from the header row."));
        properties["values"] = Schema::Array(
            "Aggregated value fields.",
            Schema::Object("One value field.", {"field"},
                           nlohmann::json{{"field", Schema::String("Source field name.")},
                                          {"aggregate", Schema::Enumeration("Aggregate function.",
                                                                            {"sum", "count", "countNumbers",
                                                                             "average", "min", "max", "product"})},
                                          {"name", Schema::String("Caption of the value column.")}}));

        auto definition = MakeDefinition("add_pivot_table", "Add pivot table",
                                         "Build a pivot table from a source range, with row fields, column fields, "
                                         "and aggregated value fields.",
                                         "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_pivot_table.",
                                                {"documentId", "source_range", "target_sheet", "target_cell"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New pivot table.", {"name"},
                                            nlohmann::json{{"name", Schema::String("Pivot table name.")},
                                                           {"targetCell", Schema::String("A1 anchor of the report.")}}),
                             true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"source_range", "A1:C20"},
            {"target_sheet", "Report"},
            {"target_cell", "A1"},
            {"rows", nlohmann::json::array({"Region"})},
            {"values", nlohmann::json::array({nlohmann::json{{"field", "Revenue"}, {"aggregate", "sum"}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddPivotTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static Excel::PivotAggregateFunction ParseAggregate(const std::string& token)
    {
        if (token == "count")
        {
            return Excel::PivotAggregateFunction::Count;
        }

        if (token == "countNumbers")
        {
            return Excel::PivotAggregateFunction::CountNumbers;
        }

        if (token == "average")
        {
            return Excel::PivotAggregateFunction::Average;
        }

        if (token == "min")
        {
            return Excel::PivotAggregateFunction::Minimum;
        }

        if (token == "max")
        {
            return Excel::PivotAggregateFunction::Maximum;
        }

        if (token == "product")
        {
            return Excel::PivotAggregateFunction::Product;
        }

        return Excel::PivotAggregateFunction::Sum;
    }

    static ToolOutcome AddPivotTable(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        ToolOutcome failure;
        const auto targetMember = arguments.find("target_sheet");
        const auto targetToken = ExcelAddressing::SheetToken(arguments, "target_sheet");
        auto target = targetMember == arguments.end()
                          ? nullptr
                          : ExcelAddressing::FindSheetValue(session.Editor(), *targetMember);
        if (target == nullptr)
        {
            return MakeError(ErrorCode::SheetNotFound, "The workbook has no worksheet '" + targetToken + "'.",
                             targetToken, "Call add_sheet to create the report sheet first.");
        }

        // An absent source sheet means the report reads from its own sheet.
        auto source = target;
        const auto sourceMember = arguments.find("source_sheet");
        if (sourceMember != arguments.end() && !sourceMember->is_null())
        {
            source = ExcelAddressing::FindSheetValue(session.Editor(), *sourceMember);
            if (source == nullptr)
            {
                const auto sourceToken = ExcelAddressing::SheetToken(arguments, "source_sheet");
                return MakeError(ErrorCode::SheetNotFound, "The workbook has no worksheet '" + sourceToken + "'.",
                                 sourceToken, "Call list_sheets to see the available worksheets.");
            }
        }

        const auto sourceRange = ExcelAddressing::ParseRange(arguments.value("source_range", std::string()), failure);
        if (!sourceRange.has_value())
        {
            return failure;
        }

        const auto targetCell = ExcelAddressing::ParseCell(arguments.value("target_cell", std::string()), failure);
        if (!targetCell.has_value())
        {
            return failure;
        }

        Excel::ExcelPivotTableDefinition definition;
        definition.Name = arguments.value("name", std::string());
        if (definition.Name.empty())
        {
            definition.Name = "PivotTable" + std::to_string(target->PivotTables().size() + 1);
        }

        definition.SourceSheet = source->Name();
        definition.SourceRange = *sourceRange;
        definition.TargetCell = *targetCell;

        const auto addFields = [&definition](const nlohmann::json& owner, const char* name, Excel::PivotAxis axis)
        {
            const auto member = owner.find(name);
            if (member == owner.end() || !member->is_array())
            {
                return;
            }

            for (const auto& field : *member)
            {
                Excel::ExcelPivotField entry;
                entry.Name = field.get<std::string>();
                entry.Axis = axis;
                definition.Fields.push_back(std::move(entry));
            }
        };

        addFields(arguments, "rows", Excel::PivotAxis::Row);
        addFields(arguments, "columns", Excel::PivotAxis::Column);

        const auto values = arguments.find("values");
        if (values != arguments.end() && values->is_array())
        {
            for (const auto& value : *values)
            {
                Excel::ExcelPivotDataField field;
                field.SourceField = value.value("field", std::string());
                field.Name = value.value("name", field.SourceField);
                field.Function = ParseAggregate(value.value("aggregate", std::string("sum")));
                definition.DataFields.push_back(std::move(field));
            }
        }

        MutationGuard guard(session.Session());

        const auto result = target->CreatePivotTable(definition);
        if (!result.Succeeded())
        {
            return MakeError(ErrorCode::OperationFailed, result.Status.Message, definition.Name,
                             "Field names must match the header row of the source range.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = definition.Name;
        data["targetCell"] = targetCell->ToA1();

        return ResultBuilder("Created pivot table '" + definition.Name + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }
};

void RegisterExcelToolset(ToolRegistry& registry)
{
    ExcelTools::Register(registry);
}

} // namespace ExyokiOffice::Mcp
