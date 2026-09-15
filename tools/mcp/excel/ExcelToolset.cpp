// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/ThemeService.hpp"
#include "ExcelToolset.hpp"

#include "ExcelAddressing.hpp"
#include "SharedToolset.hpp"
#include "Units.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/Excel/ExcelSlicer.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <fstream>
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

nlohmann::json ExcelDocumentHandle::Protection() const
{
    if (!m_editor)
    {
        return {};
    }

    nlohmann::json sheets = nlohmann::json::array();
    for (const auto& sheet : m_editor->Worksheets())
    {
        const auto info = sheet ? sheet->GetProtection() : std::nullopt;
        if (!info.has_value())
        {
            continue;
        }

        nlohmann::json entry = nlohmann::json::object();
        entry["sheet"] = sheet->Name();
        entry["hasPassword"] = info->HasPassword;
        sheets.push_back(std::move(entry));
    }

    const auto workbook = m_editor->GetWorkbookProtection();
    if (!workbook.has_value() && sheets.empty())
    {
        return {};
    }

    nlohmann::json data = nlohmann::json::object();
    if (workbook.has_value())
    {
        nlohmann::json structure = nlohmann::json::object();
        structure["lockStructure"] = workbook->Options.LockStructure;
        structure["lockWindows"] = workbook->Options.LockWindows;
        structure["hasPassword"] = workbook->HasPassword;
        data["workbook"] = std::move(structure);
    }
    if (!sheets.empty())
    {
        // Which operations each protected sheet still permits is a long list
        // nobody reads in an overview; set_protection writes it and the sheet
        // itself carries it.
        data["sheets"] = std::move(sheets);
    }
    return data;
}

std::shared_ptr<Packaging::ThemePart> ExcelDocumentHandle::Theme() const
{
    const auto document = m_editor ? m_editor->GetDocument() : nullptr;
    const auto main = document ? document->GetWorkbookPart() : nullptr;
    return main ? main->GetThemePart() : nullptr;
}

std::shared_ptr<Packaging::ThemePart> ExcelDocumentHandle::EnsureTheme()
{
    const auto document = m_editor ? m_editor->GetDocument() : nullptr;
    const auto main = document ? document->GetWorkbookPart() : nullptr;
    if (!main)
    {
        return nullptr;
    }
    if (const auto existing = main->GetThemePart())
    {
        return existing;
    }
    const auto created = main->AddThemePart();
    return created && ThemeService::WriteDefaultTheme(created) ? created : nullptr;
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
        RegisterMoveSheet(registry);
        RegisterCopySheet(registry);
        RegisterReadRange(registry);
        RegisterWriteCells(registry);
        RegisterWriteRange(registry);
        RegisterClearRange(registry);
        RegisterModifySheetStructure(registry);
        RegisterCopyRange(registry);
        RegisterSetHyperlink(registry);
        RegisterSetProtection(registry);
        RegisterSetPrintSetup(registry);
        RegisterAddImage(registry);
        RegisterAddComment(registry);
        RegisterListComments(registry);
        RegisterDeleteComment(registry);
        RegisterRecalculate(registry);
        RegisterMergeCells(registry);
        RegisterFormatRange(registry);
        RegisterSetColumnWidth(registry);
        RegisterSetRowHeight(registry);
        RegisterFreezePanes(registry);
        RegisterAddTable(registry);
        RegisterListTables(registry);
        RegisterUpdateTable(registry);
        RegisterGetVbaProject(registry);
        RegisterSetVbaProject(registry);
        RegisterRemoveVbaProject(registry);
        RegisterAddNamedRange(registry);
        RegisterAddDataValidation(registry);
        RegisterAddConditionalFormatting(registry);
        RegisterAddChart(registry);
        RegisterAddPivotTable(registry);
        RegisterAddSlicer(registry);
        RegisterListSlicers(registry);
        RegisterSetSlicerSelection(registry);
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

    /**
     * @brief A range argument that takes one rectangle or several.
     *
     * A conditional format applies to a set of rectangles, not only to one, and
     * a rule repeated per rectangle is not the same thing: rules that rank or
     * average read their population from the whole set.
     */
    static nlohmann::json RangeListProperty()
    {
        nlohmann::json schema = nlohmann::json::object();
        schema["description"] = "A1 range the rule applies to, or a list of them evaluated as one population.";
        schema["type"] = nlohmann::json::array({"string", "array"});
        schema["items"] = Schema::String("One A1 range.");
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
                                          {"value", Schema::Any("Cell value; for a formula, its cached result "
                                                                "typed like a plain cell (a number as a number, "
                                                                "a boolean as a boolean, null when none is "
                                                                "cached).")},
                                          {"formula", Schema::String("Formula text, when requested.")}});

        auto definition = MakeDefinition("read_range", "Read cell range",
                                         "Read cells as a value matrix, as detailed cell records, or as CSV. A "
                                         "formula cell reports its cached result typed like a plain cell, so "
                                         "=1+1 reads as the number 2; call recalculate first when the cache may "
                                         "be stale. Reads are capped per call; page with offset and limit for "
                                         "large ranges.",
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

        // An interval off the grid is a mistake in the request, and the caller
        // needs the grid size to fix it; the library would only say it failed.
        const bool rows = operation == "insert_rows" || operation == "delete_rows";
        const UInt32 limit = rows ? Excel::MaxRowIndex : Excel::MaxColumnIndex;
        if (at > limit)
        {
            return MakeError(ErrorCode::RangeInvalid,
                             std::string(rows ? "Row " : "Column ") + std::to_string(at) +
                                 " is outside the worksheet, which has " + std::to_string(limit) +
                                 (rows ? " rows." : " columns."),
                             std::to_string(at), "Pass an 'at' within the worksheet grid.");
        }

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

    static void RegisterMoveSheet(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["to_index"] = Schema::Integer("1-based position the sheet moves to.", 1);

        auto definition = MakeDefinition("move_sheet", "Move worksheet",
                                         "Move a worksheet to another position in the workbook.", "sheets");
        definition.InputSchema =
            Schema::Object("Arguments of move_sheet.", {"documentId", "to_index"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Moved sheet.", {"name", "index"},
                                            nlohmann::json{{"name", Schema::String("Worksheet name.")},
                                                           {"index", Schema::Integer("New 1-based position.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"sheet", "Data"}, {"to_index", 1}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return MoveSheet(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome MoveSheet(ToolContext& context, const nlohmann::json& arguments)
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

        const auto name = sheet->Name();
        const auto from = IndexOfSheet(session.Editor(), *sheet);
        const Size to = arguments.value("to_index", static_cast<Size>(1));
        const auto count = session.Editor().Worksheets().size();
        if (to == 0 || to > count)
        {
            return MakeError(ErrorCode::InputInvalid,
                             "The workbook has " + std::to_string(count) + " worksheet(s).", std::to_string(to));
        }

        MutationGuard guard(session.Session());

        if (!session.Editor().MoveWorksheet(from, to - 1))
        {
            return MakeError(ErrorCode::OperationFailed, "The worksheet could not be moved.", name);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = name;
        data["index"] = static_cast<UInt64>(to);

        return ResultBuilder("Moved " + name + " to position " + std::to_string(to) + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterCopySheet(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["name"] = Schema::String("Name for the copy; one is generated when omitted.");
        properties["source_path"] =
            Schema::String("Workspace-relative workbook to copy the sheet from; omit to copy within this one.");

        auto definition = MakeDefinition(
            "copy_sheet", "Copy worksheet",
            "Copy a worksheet, either within this workbook or from another one in the workspace. The copy is "
            "appended at the end.",
            "sheets");
        definition.InputSchema =
            Schema::Object("Arguments of copy_sheet.", {"documentId"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New sheet.", {"name", "index"},
                                            nlohmann::json{{"name", Schema::String("Name of the copy.")},
                                                           {"index", Schema::Integer("1-based position.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"sheet", "Data"}, {"name", "Data backup"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return CopySheet(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome CopySheet(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto name = arguments.value("name", std::string());
        const auto sourcePath = arguments.value("source_path", std::string());

        // The name is the one thing the caller can get wrong here, so it is
        // checked up front and reported for what it is.
        if (!name.empty() && !IsValidSheetName(name))
        {
            return MakeError(ErrorCode::InputInvalid, "'" + name + "' is not a valid worksheet name.", name,
                             "A name is 1 to 31 characters and contains none of : \\ / ? * [ ].");
        }
        if (!name.empty() && session.Editor().GetWorksheet(name) != nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "A worksheet named '" + name + "' already exists.", name,
                             "Pick another name, or omit it to have one generated.");
        }

        MutationGuard guard(session.Session());

        Excel::Worksheet::Ptr copy;
        if (sourcePath.empty())
        {
            ToolOutcome failure;
            auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
            if (sheet == nullptr)
            {
                return failure;
            }

            copy = session.Editor().CopyWorksheet(IndexOfSheet(session.Editor(), *sheet), name);
        }
        else
        {
            // A source workbook is an ordinary workspace file, so it goes
            // through the same path resolution every file-to-file tool uses.
            ToolOutcome failure;
            const auto resolved = ToolSupport::ResolveExistingFile(context, sourcePath, failure);
            if (!resolved.has_value())
            {
                return failure;
            }

            // A source workbook is opened under the configured safety limits
            // like any other, so a decompression bomb cannot arrive this way
            // either.
            auto source =
                Excel::ExcelDocumentEditor::Open(*resolved, SettingsWithLimits(context.Options().PackageLimits));
            if (source == nullptr)
            {
                return MakeError(ErrorCode::PackageLoadFailed, "The source workbook could not be opened.",
                                 sourcePath);
            }

            ToolOutcome sourceFailure;
            auto sheet = ExcelAddressing::FindSheet(*source, arguments, sourceFailure);
            if (sheet == nullptr)
            {
                return sourceFailure;
            }

            copy = session.Editor().CopyWorksheetFrom(*source, IndexOfSheet(*source, *sheet), name);
        }

        if (copy == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The worksheet could not be copied.", name,
                             "The source sheet's parts could not be brought into this workbook.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = copy->Name();
        data["index"] = static_cast<UInt64>(session.Editor().Worksheets().size());

        return ResultBuilder("Copied the worksheet as " + copy->Name() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterCopyRange(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["source"] = Schema::String("A1 range to copy.");
        properties["destination"] = Schema::String("A1 cell the range's top-left corner lands on.");
        properties["move"] =
            Schema::BooleanWithDefault("Clear the source after copying, and rewrite formulas that referred to "
                                       "it.",
                                       false);

        auto definition = MakeDefinition(
            "copy_range", "Copy or move range",
            "Copy a rectangular range to another position on the same sheet, or move it. Source and "
            "destination may overlap. A copy writes formulas verbatim, without adjusting their references; a "
            "move retargets the local A1 references that pointed into the source and clears it.",
            "cells");
        definition.InputSchema = Schema::Object("Arguments of copy_range.",
                                                {"documentId", "source", "destination"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Copied range.", {"source", "destination"},
                                            nlohmann::json{{"source", Schema::String("A1 source range.")},
                                                           {"destination", Schema::String("A1 destination "
                                                                                          "range.")},
                                                           {"moved", Schema::Boolean("True when the source was "
                                                                                     "cleared.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"source", "A1:C5"}, {"destination", "E1"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return CopyRange(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome CopyRange(ToolContext& context, const nlohmann::json& arguments)
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

        const auto sourceText = arguments.value("source", std::string());
        const auto source = ExcelAddressing::ParseRange(sourceText, failure);
        if (!source.has_value())
        {
            return failure;
        }

        const auto destinationText = arguments.value("destination", std::string());
        const auto destination = ExcelAddressing::ParseCell(destinationText, failure);
        if (!destination.has_value())
        {
            return failure;
        }

        const bool move = arguments.value("move", false);

        MutationGuard guard(session.Session());

        const auto result = move ? sheet->MoveRange(*source, *destination)
                                 : sheet->CopyRange(*source, *destination);
        if (!result.Succeeded())
        {
            return MakeError(ErrorCode::RangeInvalid,
                             move ? "The range could not be moved." : "The range could not be copied.",
                             sourceText + " -> " + destinationText,
                             "A destination that would run past the Excel grid is refused; an overlapping one "
                             "is not.");
        }

        guard.Commit();

        // The destination range is the source rectangle translated onto the new
        // top-left corner, which is what the caller wants to address next.
        const auto width = source->Last().Column().Value() - source->First().Column().Value();
        const auto height = source->Last().Row().Value() - source->First().Row().Value();
        const auto last = Excel::CellAddress::TryCreate(destination->Row().Value() + height,
                                                        destination->Column().Value() + width);

        nlohmann::json data = nlohmann::json::object();
        data["source"] = source->ToA1();
        data["destination"] = last.has_value() ? Excel::CellRange(*destination, *last).ToA1()
                                               : destination->ToA1();
        data["moved"] = move;

        return ResultBuilder((move ? std::string("Moved ") : std::string("Copied ")) + source->ToA1() + " to " +
                             destination->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterSetProtection(ToolRegistry& registry)
    {
        nlohmann::json allow = Schema::Object(
            "Operations that stay available while sheet protection is active.", {},
            nlohmann::json{{"format_cells", Schema::Boolean("Permit changing cell formats.")},
                           {"format_columns", Schema::Boolean("Permit changing column widths and formats.")},
                           {"format_rows", Schema::Boolean("Permit changing row heights and formats.")},
                           {"insert_columns", Schema::Boolean("Permit inserting columns.")},
                           {"insert_rows", Schema::Boolean("Permit inserting rows.")},
                           {"insert_hyperlinks", Schema::Boolean("Permit inserting hyperlinks.")},
                           {"delete_columns", Schema::Boolean("Permit deleting unlocked columns.")},
                           {"delete_rows", Schema::Boolean("Permit deleting unlocked rows.")},
                           {"select_locked_cells", Schema::Boolean("Permit selecting locked cells.")},
                           {"select_unlocked_cells", Schema::Boolean("Permit selecting unlocked cells.")},
                           {"sort", Schema::Boolean("Permit sorting unlocked ranges.")},
                           {"auto_filter", Schema::Boolean("Permit changing auto-filter criteria.")},
                           {"pivot_tables", Schema::Boolean("Permit interacting with pivot tables.")}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["scope"] =
            Schema::EnumerationWithDefault("What to protect.", {"sheet", "workbook"}, "sheet");
        properties["sheet"] = SheetProperty();
        properties["protect"] =
            Schema::BooleanWithDefault("True to apply protection, false to remove it.", true);
        properties["password"] = Schema::String("Password; the same one is required to remove the protection.");
        properties["allow"] = std::move(allow);
        properties["lock_structure"] = Schema::BooleanWithDefault(
            "Workbook scope: prevent adding, deleting, renaming, hiding, moving and copying sheets.", true);
        properties["lock_windows"] =
            Schema::BooleanWithDefault("Workbook scope: prevent moving and resizing the workbook windows.",
                                       false);

        auto definition = MakeDefinition(
            "set_protection", "Set protection",
            "Protect a worksheet or the workbook structure, or remove that protection. This is an editing "
            "restriction with a password verifier, not encryption: every part stays readable and any tool "
            "that ignores the setting can still rewrite the document.",
            "review");
        definition.InputSchema =
            Schema::Object("Arguments of set_protection.", {"documentId"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Protection state.", {"scope", "protected"},
                                            nlohmann::json{{"scope", Schema::String("sheet or workbook.")},
                                                           {"protected", Schema::Boolean("True when protection "
                                                                                         "is now active.")},
                                                           {"sheet", Schema::String("Worksheet name, for sheet "
                                                                                    "scope.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"scope", "sheet"}, {"password", "secret"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetProtection(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetProtection(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto scope = arguments.value("scope", std::string("sheet"));
        const bool protect = arguments.value("protect", true);
        const auto password = arguments.value("password", std::string());

        nlohmann::json data = nlohmann::json::object();
        data["scope"] = scope;
        data["protected"] = protect;
        data["sheet"] = std::string();

        MutationGuard guard(session.Session());

        if (scope == "workbook")
        {
            Excel::WorkbookProtectionOptions options;
            options.LockStructure = arguments.value("lock_structure", true);
            options.LockWindows = arguments.value("lock_windows", false);

            const auto result = protect ? session.Editor().ProtectWorkbook(options, password)
                                        : session.Editor().UnprotectWorkbook(password);
            if (!result.Succeeded())
            {
                return MakeError(ErrorCode::OperationFailed,
                                 protect ? "The workbook protection could not be written."
                                         : "The workbook protection could not be removed; the password may not "
                                           "match.",
                                 scope);
            }
        }
        else
        {
            ToolOutcome failure;
            auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
            if (sheet == nullptr)
            {
                return failure;
            }

            data["sheet"] = sheet->Name();

            Excel::SheetProtectionOptions options;
            if (const auto allow = arguments.find("allow"); allow != arguments.end())
            {
                options.AllowFormatCells = allow->value("format_cells", options.AllowFormatCells);
                options.AllowFormatColumns = allow->value("format_columns", options.AllowFormatColumns);
                options.AllowFormatRows = allow->value("format_rows", options.AllowFormatRows);
                options.AllowInsertColumns = allow->value("insert_columns", options.AllowInsertColumns);
                options.AllowInsertRows = allow->value("insert_rows", options.AllowInsertRows);
                options.AllowInsertHyperlinks = allow->value("insert_hyperlinks", options.AllowInsertHyperlinks);
                options.AllowDeleteColumns = allow->value("delete_columns", options.AllowDeleteColumns);
                options.AllowDeleteRows = allow->value("delete_rows", options.AllowDeleteRows);
                options.AllowSelectLockedCells =
                    allow->value("select_locked_cells", options.AllowSelectLockedCells);
                options.AllowSelectUnlockedCells =
                    allow->value("select_unlocked_cells", options.AllowSelectUnlockedCells);
                options.AllowSort = allow->value("sort", options.AllowSort);
                options.AllowAutoFilter = allow->value("auto_filter", options.AllowAutoFilter);
                options.AllowPivotTables = allow->value("pivot_tables", options.AllowPivotTables);
            }

            const auto result = protect ? sheet->Protect(options, password) : sheet->Unprotect(password);
            if (!result.Succeeded())
            {
                return MakeError(ErrorCode::OperationFailed,
                                 protect ? "The sheet protection could not be written."
                                         : "The sheet protection could not be removed; the password may not "
                                           "match.",
                                 sheet->Name());
            }
        }

        guard.Commit();

        return ResultBuilder(protect ? "Protected the " + scope + "." : "Removed the " + scope + " protection.")
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

    /**
     * @brief The `threaded` flag, shared by the three comment tools.
     *
     * SpreadsheetML carries two unrelated comment models, and which one a call
     * means cannot be inferred from the arguments: a plain comment is keyed by
     * its cell, a threaded one by an identifier, and a cell can hold both. The
     * flag is therefore explicit everywhere rather than guessed.
     *
     * It defaults to the threaded model, which is the one Excel writes today and
     * the one a reply can be attached to.
     */
    static nlohmann::json ThreadedProperty()
    {
        return Schema::BooleanWithDefault("Address the modern threaded comment model rather than the plain "
                                          "note model.",
                                          true);
    }

    /// The paper sizes SpreadsheetML names, as the tokens the schema publishes.
    static const std::vector<std::pair<const char*, Excel::PaperSize>>& PaperSizes()
    {
        static const std::vector<std::pair<const char*, Excel::PaperSize>> sizes{
            {"letter", Excel::PaperSize::Letter}, {"letter_small", Excel::PaperSize::LetterSmall}, {"tabloid", Excel::PaperSize::Tabloid}, {"ledger", Excel::PaperSize::Ledger}, {"legal", Excel::PaperSize::Legal}, {"statement", Excel::PaperSize::Statement}, {"executive", Excel::PaperSize::Executive}, {"a3", Excel::PaperSize::A3}, {"a4", Excel::PaperSize::A4}, {"a4_small", Excel::PaperSize::A4Small}, {"a5", Excel::PaperSize::A5}, {"b4", Excel::PaperSize::B4}, {"b5", Excel::PaperSize::B5}, {"folio", Excel::PaperSize::Folio}};
        return sizes;
    }

    static std::string PaperSizeToken(Excel::PaperSize size)
    {
        for (const auto& [token, value] : PaperSizes())
        {
            if (value == size)
            {
                return token;
            }
        }

        return "letter";
    }

    /**
     * @brief Reads a row or column band such as `"1:2"` or `"A:B"`.
     *
     * Print titles are a pair of bands rather than a cell range. The band
     * grammar is the one the sizing tools already take — a single position or
     * a pair, in either order — so an agent writes a band the same way here as
     * it does for `set_row_height`.
     */
    static bool ParsePrintTitleBand(const std::string& text, bool columns, std::pair<UInt32, UInt32>& band,
                                    ToolOutcome& failure)
    {
        UInt32 first = 0;
        UInt32 last = 0;
        const bool parsed = columns ? ExcelAddressing::ParseColumnBand(text, first, last)
                                    : ExcelAddressing::ParseRowBand(text, first, last);
        if (!parsed)
        {
            failure = MakeError(ErrorCode::RangeInvalid,
                                columns ? "A column band looks like \"A\" or \"A:B\"."
                                        : "A row band looks like \"1\" or \"1:2\".",
                                text);
            return false;
        }

        band = {first, last};
        return true;
    }

    static void RegisterSetPrintSetup(ToolRegistry& registry)
    {
        std::vector<std::string> paperTokens;
        for (const auto& [token, value] : PaperSizes())
        {
            static_cast<void>(value);
            paperTokens.emplace_back(token);
        }

        nlohmann::json margins =
            Schema::Object("Page margins; omitted members keep their current value.", {},
                           nlohmann::json{{"left", Schema::Length("Left margin.")},
                                          {"right", Schema::Length("Right margin.")},
                                          {"top", Schema::Length("Top margin.")},
                                          {"bottom", Schema::Length("Bottom margin.")},
                                          {"header", Schema::Length("Header margin.")},
                                          {"footer", Schema::Length("Footer margin.")}});

        nlohmann::json headerFooter = Schema::Object(
            "Header and footer strings, using Excel's formatting codes such as \"&P\" for the page number.", {},
            nlohmann::json{{"odd_header", Schema::String("Header of every page, or of odd pages.")},
                           {"odd_footer", Schema::String("Footer of every page, or of odd pages.")},
                           {"even_header", Schema::String("Header of even pages.")},
                           {"even_footer", Schema::String("Footer of even pages.")},
                           {"first_header", Schema::String("Header of the first page.")},
                           {"first_footer", Schema::String("Footer of the first page.")},
                           {"different_odd_even", Schema::Boolean("Use the even-page strings.")},
                           {"different_first", Schema::Boolean("Use the first-page strings.")}});

        nlohmann::json options =
            Schema::Object("Print decorations and alignment.", {},
                           nlohmann::json{{"horizontal_centered", Schema::Boolean("Center the page horizontally.")},
                                          {"vertical_centered", Schema::Boolean("Center the page vertically.")},
                                          {"headings", Schema::Boolean("Print row and column headings.")},
                                          {"grid_lines", Schema::Boolean("Print grid lines.")}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["orientation"] = Schema::Enumeration("Page orientation.", {"portrait", "landscape"});
        properties["paper_size"] = Schema::Enumeration("Paper size.", std::move(paperTokens));
        properties["scale"] = Schema::Integer("Print scale in percent; ignored when a fit-to-page value is set.",
                                              10, 400);
        properties["fit_to_width"] = Schema::Integer("Pages to fit the width into; 0 means unrestricted.", 0, 32767);
        properties["fit_to_height"] =
            Schema::Integer("Pages to fit the height into; 0 means unrestricted.", 0, 32767);
        properties["margins"] = std::move(margins);
        properties["print_area"] =
            Schema::Array("A1 ranges printed from this sheet; an empty array clears the print area.",
                          Schema::String("One A1 range."));
        properties["repeat_rows"] = Schema::String("Row band repeated on every page, such as \"1:2\"; an empty "
                                                   "string clears it.");
        properties["repeat_columns"] = Schema::String("Column band repeated on every page, such as \"A:B\"; an "
                                                      "empty string clears it.");
        properties["header_footer"] = std::move(headerFooter);
        properties["options"] = std::move(options);

        auto definition =
            MakeDefinition("set_print_setup", "Set print setup",
                           "Set how a worksheet prints: orientation, paper, scaling, margins, print area, "
                           "repeated titles, headers and footers. Every member is optional and the ones left "
                           "out keep their current value.",
                           "layout");
        definition.InputSchema =
            Schema::Object("Arguments of set_print_setup.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Effective print setup.", {"sheet"},
                           nlohmann::json{{"sheet", Schema::String("Worksheet name.")},
                                          {"orientation", Schema::String("Page orientation.")},
                                          {"paperSize", Schema::String("Paper size token.")},
                                          {"printArea", Schema::Array("A1 ranges printed from this sheet.",
                                                                      Schema::String("One A1 range."))}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"orientation", "landscape"}, {"fit_to_width", 1}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetPrintSetup(context, arguments); };
        registry.Add(std::move(definition));
    }

    /// Applies the members of @p source that are present onto @p margins.
    static bool ApplyMargins(const nlohmann::json& source, Excel::PageMargins& margins, ToolOutcome& failure)
    {
        const auto apply = [&source, &failure](const char* name, MeasuringUnits& target)
        {
            const auto value = source.find(name);
            if (value == source.end())
            {
                return true;
            }

            const auto parsed = ParseLength(*value);
            if (!parsed.has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid, "The margin is not a length.", name);
                return false;
            }

            // A negative margin passes the length parser but not the writer,
            // which would fail the whole call with nothing to point at.
            if (ToPointValue(*parsed) < 0.0)
            {
                failure = MakeError(ErrorCode::InputInvalid, "A margin cannot be negative.", name);
                return false;
            }

            target = *parsed;
            return true;
        };

        return apply("left", margins.Left) && apply("right", margins.Right) && apply("top", margins.Top) &&
               apply("bottom", margins.Bottom) && apply("header", margins.Header) &&
               apply("footer", margins.Footer);
    }

    static ToolOutcome SetPrintSetup(ToolContext& context, const nlohmann::json& arguments)
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

        // Everything is parsed and validated before anything is written, so a
        // rejected member does not leave half a print setup behind.
        auto setup = sheet->GetPageSetup();
        if (const auto orientation = arguments.find("orientation"); orientation != arguments.end())
        {
            setup.Orientation = orientation->get<std::string>() == "landscape" ? Excel::PageOrientation::Landscape
                                                                               : Excel::PageOrientation::Portrait;
        }

        if (const auto paper = arguments.find("paper_size"); paper != arguments.end())
        {
            const auto token = paper->get<std::string>();
            for (const auto& [name, value] : PaperSizes())
            {
                if (token == name)
                {
                    setup.PaperSize = value;
                    break;
                }
            }
        }

        if (const auto scale = arguments.find("scale"); scale != arguments.end())
        {
            setup.Scale = scale->get<UInt32>();
        }

        if (const auto fit = arguments.find("fit_to_width"); fit != arguments.end())
        {
            setup.FitToWidth = fit->get<UInt32>();
        }

        if (const auto fit = arguments.find("fit_to_height"); fit != arguments.end())
        {
            setup.FitToHeight = fit->get<UInt32>();
        }

        auto margins = sheet->GetPageMargins();
        if (const auto source = arguments.find("margins"); source != arguments.end())
        {
            if (!ApplyMargins(*source, margins, failure))
            {
                return failure;
            }
        }

        std::optional<std::vector<Excel::CellRange>> printArea;
        if (const auto areas = arguments.find("print_area"); areas != arguments.end())
        {
            std::vector<Excel::CellRange> parsed;
            for (const auto& entry : *areas)
            {
                const auto text = entry.get<std::string>();
                const auto range = ExcelAddressing::ParseRange(text, failure);
                if (!range.has_value())
                {
                    return failure;
                }

                parsed.push_back(*range);
            }

            printArea = std::move(parsed);
        }

        auto titles = sheet->GetPrintTitles();
        if (const auto rows = arguments.find("repeat_rows"); rows != arguments.end())
        {
            const auto text = rows->get<std::string>();
            if (text.empty())
            {
                titles.Rows.reset();
            }
            else
            {
                std::pair<UInt32, UInt32> band;
                if (!ParsePrintTitleBand(text, false, band, failure))
                {
                    return failure;
                }

                titles.Rows = band;
            }
        }

        if (const auto columns = arguments.find("repeat_columns"); columns != arguments.end())
        {
            const auto text = columns->get<std::string>();
            if (text.empty())
            {
                titles.Columns.reset();
            }
            else
            {
                std::pair<UInt32, UInt32> band;
                if (!ParsePrintTitleBand(text, true, band, failure))
                {
                    return failure;
                }

                titles.Columns = band;
            }
        }

        auto headerFooter = sheet->GetHeaderFooter();
        if (const auto source = arguments.find("header_footer"); source != arguments.end())
        {
            headerFooter.OddHeader = source->value("odd_header", headerFooter.OddHeader);
            headerFooter.OddFooter = source->value("odd_footer", headerFooter.OddFooter);
            headerFooter.EvenHeader = source->value("even_header", headerFooter.EvenHeader);
            headerFooter.EvenFooter = source->value("even_footer", headerFooter.EvenFooter);
            headerFooter.FirstHeader = source->value("first_header", headerFooter.FirstHeader);
            headerFooter.FirstFooter = source->value("first_footer", headerFooter.FirstFooter);
            headerFooter.DifferentOddEven = source->value("different_odd_even", headerFooter.DifferentOddEven);
            headerFooter.DifferentFirst = source->value("different_first", headerFooter.DifferentFirst);
        }

        auto options = sheet->GetPrintOptions();
        if (const auto source = arguments.find("options"); source != arguments.end())
        {
            options.HorizontalCentered = source->value("horizontal_centered", options.HorizontalCentered);
            options.VerticalCentered = source->value("vertical_centered", options.VerticalCentered);
            options.Headings = source->value("headings", options.Headings);
            options.GridLines = source->value("grid_lines", options.GridLines);
        }

        MutationGuard guard(session.Session());

        if (!sheet->SetPageSetup(setup) || !sheet->SetPageMargins(margins) || !sheet->SetPrintTitles(titles) ||
            !sheet->SetHeaderFooter(headerFooter) || !sheet->SetPrintOptions(options))
        {
            return MakeError(ErrorCode::OperationFailed, "The print setup could not be written.", sheet->Name());
        }

        if (printArea.has_value() && !sheet->SetPrintArea(*printArea))
        {
            return MakeError(ErrorCode::OperationFailed, "The print area could not be written.", sheet->Name());
        }

        guard.Commit();

        nlohmann::json areas = nlohmann::json::array();
        for (const auto& range : sheet->GetPrintArea())
        {
            areas.push_back(range.ToA1());
        }

        nlohmann::json data = nlohmann::json::object();
        data["sheet"] = sheet->Name();
        data["orientation"] =
            setup.Orientation == Excel::PageOrientation::Landscape ? "landscape" : "portrait";
        data["paperSize"] = setup.PaperSize.has_value() ? PaperSizeToken(*setup.PaperSize) : std::string();
        data["printArea"] = std::move(areas);

        return ResultBuilder("Set the print setup of " + sheet->Name() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    /**
     * @brief Maps a detected media type onto the worksheet image formats.
     *
     * A worksheet drawing stores the encoding as an enumeration rather than as
     * a content type, so a payload the library cannot name here has nowhere to
     * go and is refused instead of being written as something it is not.
     */
    static std::optional<Excel::ExcelImageFormat> ParseImageFormat(const std::string& contentType)
    {
        if (contentType == "image/png")
        {
            return Excel::ExcelImageFormat::Png;
        }

        if (contentType == "image/jpeg")
        {
            return Excel::ExcelImageFormat::Jpeg;
        }

        if (contentType == "image/gif")
        {
            return Excel::ExcelImageFormat::Gif;
        }

        if (contentType == "image/bmp")
        {
            return Excel::ExcelImageFormat::Bmp;
        }

        if (contentType == "image/tiff")
        {
            return Excel::ExcelImageFormat::Tiff;
        }

        return std::nullopt;
    }

    static void RegisterAddImage(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["anchor_cell"] = Schema::String("A1 cell the image's top-left corner sits on.");
        properties["path"] = Schema::String("Workspace-relative image file; mutually exclusive with dataBase64.");
        properties["dataBase64"] = Schema::String("Base64 image payload; mutually exclusive with path.");
        properties["contentType"] = Schema::String("Media type; detected when omitted.");
        properties["width"] = Schema::Length("Image width; defaults to about 10 cm.");
        properties["height"] = Schema::Length("Image height; defaults to about 7.5 cm.");
        properties["alt"] = Schema::String("Alternative text for accessibility.");
        properties["name"] = Schema::String("Non-visual drawing name.");

        auto definition = MakeDefinition("add_image", "Add image",
                                         "Place a picture on the worksheet, anchored to a cell rectangle. PNG, "
                                         "JPEG, GIF, BMP, and TIFF payloads are accepted.",
                                         "media");
        definition.InputSchema = Schema::Object("Arguments of add_image.", {"documentId", "anchor_cell"},
                                                std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("New image.", {"imageId"},
                           nlohmann::json{{"imageId", Schema::Integer("Drawing object identifier.")},
                                          {"anchor", Schema::String("A1 anchor cell.")},
                                          {"contentType", Schema::String("Media type that was stored.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"anchor_cell", "D2"}, {"path", "logo.png"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddImage(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddImage(ToolContext& context, const nlohmann::json& arguments)
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

        const auto anchorText = arguments.value("anchor_cell", std::string());
        const auto anchor = ExcelAddressing::ParseCell(anchorText, failure);
        if (!anchor.has_value())
        {
            return failure;
        }

        std::vector<Byte> bytes;
        std::string contentType;
        if (!ToolSupport::LoadImagePayload(context, arguments, bytes, contentType, failure))
        {
            return failure;
        }

        const auto format = ParseImageFormat(contentType);
        if (!format.has_value())
        {
            return MakeError(ErrorCode::Unsupported,
                             "A worksheet image must be PNG, JPEG, GIF, BMP, or TIFF.", contentType);
        }

        const auto placement = ResolveAnchor(*sheet, *anchor, arguments, 283.0, 212.0, failure);
        if (!placement.has_value())
        {
            return failure;
        }

        MutationGuard guard(session.Session());

        Excel::ExcelWorksheetImage image;
        image.Name = arguments.value("name", std::string());
        image.Description = arguments.value("alt", std::string());
        image.From = placement->From;
        image.FromOffset = placement->FromOffset;
        image.To = placement->To;
        image.ToOffset = placement->ToOffset;
        image.Extent = placement->Extent;
        image.Data = std::move(bytes);
        image.Format = *format;

        const auto written = sheet->AddImage(std::move(image));
        if (!written.has_value())
        {
            return MakeError(ErrorCode::OperationFailed, "The image could not be placed.", anchorText);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["imageId"] = static_cast<UInt64>(*written);
        data["anchor"] = anchor->ToA1();
        data["contentType"] = contentType;

        return ResultBuilder("Placed an image at " + anchor->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddComment(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["cell"] = Schema::String("A1 address of the annotated cell.");
        properties["text"] = Schema::String("Comment text.");
        properties["author"] = Schema::StringWithDefault("Author display name.", "ExyokiOffice");
        properties["threaded"] = ThreadedProperty();
        properties["reply_to"] =
            Schema::String("Identifier of the thread entry this one replies to; implies a threaded comment.");

        auto definition = MakeDefinition("add_comment", "Add comment",
                                         "Attach a comment to a cell, as a plain note by default or as a "
                                         "threaded comment. Pass reply_to to answer an existing thread entry.",
                                         "review");
        definition.InputSchema =
            Schema::Object("Arguments of add_comment.", {"documentId", "cell", "text"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("New comment.", {"cell"},
                           nlohmann::json{{"cell", Schema::String("A1 address.")},
                                          {"commentId", Schema::String("Thread entry identifier; empty for a "
                                                                       "plain note.")},
                                          {"threaded", Schema::Boolean("True for a threaded comment.")}}),
            true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"}, {"cell", "B4"}, {"text", "Check this number."}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddComment(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddComment(ToolContext& context, const nlohmann::json& arguments)
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

        const auto text = arguments.value("text", std::string());
        if (text.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "A comment needs text.", "text");
        }

        const auto replyTo = arguments.value("reply_to", std::string());
        // The fallback here has to match the schema's published default: the
        // schema documents the default, it does not fill it in.
        const bool threaded = !replyTo.empty() || arguments.value("threaded", true);

        // A reply has to name an entry that exists. Without the check the
        // orphaned parent identifier would be written out and Excel would drop
        // the reply when it repaired the file, which reports success here and
        // loses the comment there.
        if (!replyTo.empty())
        {
            const auto existing = sheet->ThreadedComments();
            const auto parent = std::find_if(existing.begin(), existing.end(),
                                             [&replyTo](const Excel::ExcelThreadedComment& entry)
                                             { return entry.Id == replyTo; });
            if (parent == existing.end())
            {
                return MakeError(ErrorCode::CommentNotFound, "No thread entry has that identifier.", replyTo);
            }
        }

        MutationGuard guard(session.Session());

        std::string commentId;
        if (threaded)
        {
            Excel::ExcelThreadedComment comment;
            comment.Address = *address;
            comment.PersonName = arguments.value("author", std::string("ExyokiOffice"));
            comment.Text = text;
            comment.ParentId = replyTo;

            const auto written = sheet->AddThreadedComment(std::move(comment));
            if (!written.has_value())
            {
                return MakeError(ErrorCode::OperationFailed, "The comment could not be written.", cellText);
            }

            commentId = *written;
        }
        else
        {
            Excel::ExcelComment comment;
            comment.Address = *address;
            comment.Author = arguments.value("author", std::string("ExyokiOffice"));
            comment.Text = text;
            if (!sheet->SetComment(comment))
            {
                return MakeError(ErrorCode::OperationFailed, "The comment could not be written.", cellText);
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["cell"] = address->ToA1();
        data["commentId"] = commentId;
        data["threaded"] = threaded;

        return ResultBuilder("Commented " + address->ToA1() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterListComments(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["sheet"] = SheetReferenceProperty(
            "Worksheet name (case-insensitive) or 1-based index; omit to list every sheet.");

        nlohmann::json comment =
            Schema::Object("One comment.", {"sheet", "cell", "text"},
                           nlohmann::json{{"sheet", Schema::String("Worksheet name.")},
                                          {"cell", Schema::String("A1 address.")},
                                          {"id", Schema::String("Thread entry identifier; empty for a plain "
                                                                "note.")},
                                          {"author", Schema::String("Author display name.")},
                                          {"text", Schema::String("Comment text.")},
                                          {"threaded", Schema::Boolean("True for a threaded comment.")},
                                          {"parentId", Schema::String("Identifier of the entry this one "
                                                                      "replies to.")}});

        auto definition = MakeDefinition("list_comments", "List comments",
                                         "List the comments of the workbook, optionally narrowed to one sheet. "
                                         "Both threaded comments and plain notes are reported.",
                                         "review");
        definition.InputSchema = Schema::Object("Arguments of list_comments.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Comments.", {"comments"},
                           nlohmann::json{{"comments", Schema::Array("Comments.", std::move(comment))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListComments(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListComments(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        // `sheet` narrows the listing but is optional, and FindSheet falls back
        // to the first sheet when it is absent. Resolving it only when present
        // keeps "omitted" meaning every sheet rather than the first one.
        std::string only;
        if (arguments.contains("sheet"))
        {
            ToolOutcome failure;
            auto sheet = ExcelAddressing::FindSheet(reader.Editor(), arguments, failure);
            if (sheet == nullptr)
            {
                return failure;
            }

            only = sheet->Name();
        }

        nlohmann::json comments = nlohmann::json::array();
        for (const auto& sheet : reader.Editor().Worksheets())
        {
            if (sheet == nullptr || (!only.empty() && sheet->Name() != only))
            {
                continue;
            }

            for (const auto& note : sheet->Comments())
            {
                nlohmann::json entry = nlohmann::json::object();
                entry["sheet"] = sheet->Name();
                entry["cell"] = note.Address.ToA1();
                entry["id"] = std::string();
                entry["author"] = note.Author;
                entry["text"] = note.Text;
                entry["threaded"] = false;
                entry["parentId"] = std::string();
                comments.push_back(std::move(entry));
            }

            for (const auto& thread : sheet->ThreadedComments())
            {
                nlohmann::json entry = nlohmann::json::object();
                entry["sheet"] = sheet->Name();
                entry["cell"] = thread.Address.ToA1();
                entry["id"] = thread.Id;
                entry["author"] = thread.PersonName;
                entry["text"] = thread.Text;
                entry["threaded"] = true;
                entry["parentId"] = thread.ParentId;
                comments.push_back(std::move(entry));
            }
        }

        const bool truncated = TruncateArrayToBudget(comments);

        nlohmann::json data = nlohmann::json::object();
        const auto count = comments.size();
        data["comments"] = std::move(comments);

        return ResultBuilder("The workbook holds " + std::to_string(count) + " comment(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterDeleteComment(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["cell"] = Schema::String("A1 address whose plain note is removed.");
        properties["comment_id"] = Schema::String("Thread entry identifier reported by list_comments.");

        auto definition = MakeDefinition("delete_comment", "Delete comment",
                                         "Remove one comment: a thread entry by its identifier, or the plain note "
                                         "of a cell. Pass exactly one of comment_id and cell.",
                                         "review");
        definition.InputSchema = Schema::Object("Arguments of delete_comment.", {"documentId"},
                                                std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Deleted comment.", {"removed"},
                           nlohmann::json{{"removed", Schema::Boolean("True when a comment was removed.")},
                                          {"cell", Schema::String("A1 address, when one was named.")},
                                          {"commentId", Schema::String("Identifier, when one was named.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"cell", "B4"}};
        definition.Annotations.Destructive = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return DeleteComment(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome DeleteComment(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto cellText = arguments.value("cell", std::string());
        const auto commentId = arguments.value("comment_id", std::string());

        // Neither argument is required on its own, so the pair has to be
        // checked here. Accepting both would leave it to the reader of the log
        // to work out which comment actually went.
        if (cellText.empty() == commentId.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "Pass exactly one of comment_id and cell.", "comment_id");
        }

        ToolOutcome failure;
        auto sheet = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
        if (sheet == nullptr)
        {
            return failure;
        }

        nlohmann::json data = nlohmann::json::object();
        data["cell"] = std::string();
        data["commentId"] = std::string();

        MutationGuard guard(session.Session());

        bool removed = false;
        if (!commentId.empty())
        {
            removed = sheet->RemoveThreadedComment(commentId);
            data["commentId"] = commentId;
            if (!removed)
            {
                return MakeError(ErrorCode::CommentNotFound, "No thread entry has that identifier.", commentId);
            }
        }
        else
        {
            const auto address = ExcelAddressing::ParseCell(cellText, failure);
            if (!address.has_value())
            {
                return failure;
            }

            removed = sheet->RemoveComment(*address);
            data["cell"] = address->ToA1();
            if (!removed)
            {
                return MakeError(ErrorCode::CommentNotFound, "The cell carries no plain note.", cellText);
            }
        }

        guard.Commit();
        data["removed"] = removed;

        return ResultBuilder("Removed one comment.")
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

    /// Whether two ranges share at least one cell.
    static bool RangesIntersect(const Excel::CellRange& left, const Excel::CellRange& right)
    {
        return left.First().Row().Value() <= right.Last().Row().Value() &&
               right.First().Row().Value() <= left.Last().Row().Value() &&
               left.First().Column().Value() <= right.Last().Column().Value() &&
               right.First().Column().Value() <= left.Last().Column().Value();
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
        if (!unmerge)
        {
            for (const auto& table : sheet->Tables())
            {
                const auto area = table->Range();
                if (area.has_value() && RangesIntersect(*area, *range))
                {
                    return MakeError(ErrorCode::InputInvalid,
                                     "The range " + range->ToA1() + " overlaps the table '" + table->Name() +
                                         "', and a table cannot hold merged cells.",
                                     range->ToA1(), "Merge cells outside the table, or center the text instead.");
                }
            }
        }

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

    /**
     * @brief The appearance members format_range and a conditional format share.
     *
     * The appearance a rule paints over the cells it matches is the same
     * vocabulary as the appearance a range carries, so the two tools publish
     * one schema instead of two that drift apart.
     */
    static nlohmann::json StyleProperties()
    {
        nlohmann::json properties = nlohmann::json::object();
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
        return properties;
    }

    static void RegisterFormatRange(ToolRegistry& registry)
    {
        nlohmann::json properties = StyleProperties();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range to format.");

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

    /**
     * @brief Reads the members StyleProperties() publishes into a style definition.
     *
     * Absent members are left out of @p style rather than defaulted, which is
     * what lets the same reader serve a cell format (where a missing member
     * means "the workbook default") and a differential format (where it means
     * "leave this alone").
     */
    static bool ReadStyleArguments(const nlohmann::json& owner, Excel::ExcelStyle& style, ToolOutcome& failure)
    {
        const auto numberFormat = owner.value("number_format", std::string());
        if (!numberFormat.empty())
        {
            auto format = Excel::ExcelNumberFormat::Custom(numberFormat);
            if (!format.has_value())
            {
                failure = MakeError(ErrorCode::Unsupported, "The number format code was rejected.", numberFormat,
                                    "Use an Excel number format such as \"#,##0.00\".");
                return false;
            }

            style.NumberFormat = *format;
        }

        const auto font = owner.find("font");
        if (font != owner.end() && font->is_object())
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
                return false;
            }

            style.Font = value;
        }

        const auto fill = owner.find("fill");
        if (fill != owner.end() && fill->is_object())
        {
            std::optional<Excel::ExcelColor> color;
            if (!ReadColor(*fill, "color", "fill", color, failure))
            {
                return false;
            }

            if (!color.has_value())
            {
                failure = MakeError(ErrorCode::InputInvalid, "A fill needs a \"color\".", {},
                                    "Pass fill.color as \"#RRGGBB\", for example \"#FFFF00\".");
                return false;
            }

            Excel::ExcelFill value;
            value.Kind = Excel::ExcelFillKind::Pattern;
            value.Pattern = Excel::ExcelFillPattern::Solid;
            value.Foreground = color;
            style.Fill = value;
        }

        const auto border = owner.find("border");
        if (border != owner.end() && border->is_object())
        {
            Excel::ExcelBorderSide side;
            side.Style = ParseBorderStyle(border->value("style", std::string("thin")));
            if (!ReadColor(*border, "color", "border", side.Color, failure))
            {
                return false;
            }

            Excel::ExcelBorder value;
            value.Left = side;
            value.Right = side;
            value.Top = side;
            value.Bottom = side;
            style.Border = value;
        }

        const auto alignment = owner.find("alignment");
        if (alignment != owner.end() && alignment->is_object())
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

        return true;
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
        if (!ReadStyleArguments(arguments, style, failure))
        {
            return failure;
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

        const auto width = arguments.value("width", 0.0);
        Excel::ColumnDimension probe;
        probe.Width = width;
        if (!Excel::IsValidColumnDimension(probe))
        {
            return MakeError(ErrorCode::InputInvalid,
                             "A column width has to be between 0 and 255 characters.", "width",
                             "Widths are in characters of the default font; Excel's default column is 8.43.");
        }

        MutationGuard guard(session.Session());

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

        const auto height = arguments.value("height", 0.0);
        Excel::RowDimension probe;
        probe.Height = height;
        if (!Excel::IsValidRowDimension(probe))
        {
            return MakeError(ErrorCode::InputInvalid,
                             "A row height has to be greater than 0 and at most 409.5 points.", "height",
                             "Excel's default row is 15 points.");
        }

        MutationGuard guard(session.Session());

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

    /**
     * @brief Finds a table of the workbook by name, and the sheet that owns it.
     *
     * Table names are unique workbook-wide, so a `sheet` argument only narrows
     * the search; a name that exists on another sheet is then not found, which
     * is what makes a mistaken sheet report itself rather than land elsewhere.
     */
    static Excel::ExcelTable::Ptr FindTable(Excel::ExcelDocumentEditor& editor, const std::string& name,
                                            const std::string& only, std::string& hostSheet)
    {
        for (const auto& sheet : editor.Worksheets())
        {
            if (sheet == nullptr || (!only.empty() && sheet->Name() != only))
            {
                continue;
            }

            for (const auto& table : sheet->Tables())
            {
                if (table != nullptr && AsciiText::EqualsIgnoreCase(table->Name(), name))
                {
                    hostSheet = sheet->Name();
                    return table;
                }
            }
        }

        return nullptr;
    }

    /// Reports one table the way both table tools describe it.
    static nlohmann::json DescribeTable(const Excel::ExcelTable::Ptr& table, const std::string& sheetName)
    {
        const auto columns = table->Columns();

        nlohmann::json columnList = nlohmann::json::array();
        for (const auto& column : columns)
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = column.Name;
            entry["id"] = column.Id;
            if (column.TotalsRowLabel.has_value())
            {
                entry["totalsLabel"] = *column.TotalsRowLabel;
            }

            columnList.push_back(std::move(entry));
        }

        nlohmann::json filters = nlohmann::json::array();
        for (const auto& filter : table->ValueFilters())
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["column"] = filter.ColumnIndex < columns.size() ? columns[filter.ColumnIndex].Name : std::string();
            entry["columnIndex"] = filter.ColumnIndex + 1;
            entry["values"] = filter.Values;
            entry["includeBlank"] = filter.IncludeBlank;
            filters.push_back(std::move(entry));
        }

        const auto range = table->Range();

        nlohmann::json entry = nlohmann::json::object();
        entry["name"] = table->Name();
        entry["sheet"] = sheetName;
        entry["range"] = range.has_value() ? range->ToA1() : std::string();
        entry["autoFilter"] = table->AutoFilterEnabled();
        entry["totalsRow"] = table->TotalsRowShown();
        entry["columns"] = std::move(columnList);
        entry["filters"] = std::move(filters);
        return entry;
    }

    /**
     * @brief Hides the table rows its value filters exclude, and shows the rest.
     *
     * The filter criteria and the hidden rows are two separate things in the
     * file: the criteria say what the funnel button offers, the `hidden` flag on
     * each row is what a reader actually sees. Excel writes both and recomputes
     * neither on open, so a file carrying criteria alone shows a column marked
     * as filtered with every row still in view.
     */
    static void ApplyTableFilters(const Excel::Worksheet::Ptr& sheet, const Excel::ExcelTable::Ptr& table,
                                  const Excel::SharedStringTableService& sharedStrings)
    {
        const auto range = table->Range();
        if (sheet == nullptr || !range.has_value())
        {
            return;
        }

        const auto filters = table->ValueFilters();
        const auto firstColumn = range->First().Column().Value();
        const auto firstRow = range->First().Row().Value() + 1;
        const auto lastRow = range->Last().Row().Value() - (table->TotalsRowShown() ? 1 : 0);

        for (auto row = firstRow; row <= lastRow; ++row)
        {
            bool visible = true;
            for (const auto& filter : filters)
            {
                const auto value = sheet->GetCellValue(row, firstColumn + filter.ColumnIndex);
                const auto text = value.has_value()
                                      ? ExcelAddressing::CellValueToText(*value, sharedStrings)
                                      : std::string();
                if (text.empty())
                {
                    visible = filter.IncludeBlank;
                }
                else
                {
                    visible = std::find(filter.Values.begin(), filter.Values.end(), text) != filter.Values.end();
                }

                if (!visible)
                {
                    break;
                }
            }

            auto dimension = sheet->GetRowDimension(row).value_or(Excel::RowDimension{});
            if (dimension.Hidden == !visible)
            {
                continue;
            }

            dimension.Hidden = !visible;
            sheet->SetRowDimension(row, dimension);
        }
    }

    // -----------------------------------------------------------------------
    // VBA project
    //
    // The project is carried as opaque bytes throughout: nothing here parses,
    // rewrites, or runs the code it contains. A workbook that gains one becomes
    // macro-enabled, which is a property of the package rather than of the file
    // name, so the tools say so rather than renaming anything behind the
    // caller's back.
    // -----------------------------------------------------------------------

    static void RegisterGetVbaProject(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        // Not `path`: a reading tool already publishes that as the document to
        // open, and the two would be the same argument.
        properties["output_path"] = Schema::String(
            "Workspace file to write vbaProject.bin to; omitted, only the report is returned.");
        properties["overwrite"] = Schema::BooleanWithDefault("Replace an existing file at that path.", false);

        auto definition = MakeDefinition(
            "get_vba_project", "Get VBA project",
            "Report whether the workbook carries a VBA project and, when a path is given, write the opaque "
            "vbaProject.bin to it. The payload is never parsed or executed.",
            "vba");
        definition.InputSchema = Schema::Object("Arguments of get_vba_project.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("VBA project.", {"present"},
                           nlohmann::json{{"present", Schema::Boolean("A project is embedded.")},
                                          {"bytes", Schema::Integer("Size of the project payload.")},
                                          {"path", Schema::String("Workspace file it was written to.")}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"output_path", "macros.bin"}};
        definition.Annotations.ReadOnly = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return GetVbaProject(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome GetVbaProject(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        const auto document = reader.Editor().GetDocument();
        if (document == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The workbook has no package.");
        }

        nlohmann::json data = nlohmann::json::object();
        data["present"] = document->HasVbaProject();
        if (!document->HasVbaProject())
        {
            return ResultBuilder("The workbook carries no VBA project.").WithData(std::move(data)).Build();
        }

        const auto payload = document->GetVbaProjectData();
        data["bytes"] = static_cast<UInt64>(payload.size());

        const auto path = arguments.value("output_path", std::string());
        if (path.empty())
        {
            return ResultBuilder("The workbook carries a VBA project of " + std::to_string(payload.size()) +
                                 " bytes.")
                .WithData(std::move(data))
                .Build();
        }

        ToolOutcome failure;
        const auto resolved =
            ToolSupport::ResolveOutputPath(context, path, arguments.value("overwrite", false), failure);
        if (!resolved.has_value())
        {
            return failure;
        }

        std::ofstream stream(*resolved, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return MakeError(ErrorCode::OperationFailed, "The file could not be written.", path);
        }

        stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!stream)
        {
            return MakeError(ErrorCode::OperationFailed, "The file could not be written.", path);
        }

        data["path"] = context.GetWorkspace().Relativize(*resolved);

        return ResultBuilder("Wrote the VBA project to " + path + ".").WithData(std::move(data)).Build();
    }

    static void RegisterSetVbaProject(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["source_path"] = Schema::String("Workspace file holding the vbaProject.bin to embed.");

        auto definition = MakeDefinition(
            "set_vba_project", "Set VBA project",
            "Embed or replace the workbook's VBA project from a workspace file, which makes the workbook "
            "macro-enabled. The bytes are stored exactly as given: nothing here parses, rewrites or runs them, "
            "and saving under an .xlsx name produces a macro-enabled package a reader will question.",
            "vba");
        definition.InputSchema =
            Schema::Object("Arguments of set_vba_project.", {"documentId", "source_path"},
                           std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Embedded project.", {"bytes"},
                           nlohmann::json{{"bytes", Schema::Integer("Size of the embedded payload.")},
                                          {"macroEnabled", Schema::Boolean("The package is now macro-enabled.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}, {"source_path", "macros.bin"}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetVbaProject(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetVbaProject(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto path = arguments.value("source_path", std::string());
        ToolOutcome failure;
        const auto resolved = ToolSupport::ResolveExistingFile(context, path, failure);
        if (!resolved.has_value())
        {
            return failure;
        }

        std::ifstream stream(*resolved, std::ios::binary);
        if (!stream)
        {
            return MakeError(ErrorCode::OperationFailed, "The file could not be read.", path);
        }

        const std::vector<Byte> payload((std::istreambuf_iterator<char>(stream)),
                                        std::istreambuf_iterator<char>());
        if (payload.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "The file is empty.", path,
                             "A VBA project is the vbaProject.bin taken out of a macro-enabled workbook.");
        }

        const auto document = session.Editor().GetDocument();
        if (document == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The workbook has no package.", session.Session().Id());
        }

        MutationGuard guard(session.Session());

        if (!document->SetVbaProjectData(payload))
        {
            return MakeError(ErrorCode::OperationFailed, "The VBA project could not be embedded.", path);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["bytes"] = static_cast<UInt64>(payload.size());
        data["macroEnabled"] = document->HasVbaProject();

        return ResultBuilder("Embedded a VBA project of " + std::to_string(payload.size()) + " bytes.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterRemoveVbaProject(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);

        auto definition = MakeDefinition(
            "remove_vba_project", "Remove VBA project",
            "Remove the workbook's VBA project and turn the package back into its macro-free counterpart.",
            "vba");
        definition.InputSchema =
            Schema::Object("Arguments of remove_vba_project.", {"documentId"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Removal.", {"removed"},
                           nlohmann::json{{"removed", Schema::Boolean("A project was there and is gone.")}}),
            true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.Destructive = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return RemoveVbaProject(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome RemoveVbaProject(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto document = session.Editor().GetDocument();
        if (document == nullptr)
        {
            return MakeError(ErrorCode::InternalError, "The workbook has no package.", session.Session().Id());
        }

        if (!document->HasVbaProject())
        {
            // Removing what is not there is the state the caller asked for, so
            // it is reported as done with nothing removed rather than refused.
            nlohmann::json data = nlohmann::json::object();
            data["removed"] = false;
            return ResultBuilder("The workbook carries no VBA project; there was nothing to remove.")
                .WithSession(session.Session())
                .WithData(std::move(data))
                .Build();
        }

        MutationGuard guard(session.Session());

        if (!document->RemoveVbaProject())
        {
            return MakeError(ErrorCode::OperationFailed, "The VBA project could not be removed.",
                             session.Session().Id());
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["removed"] = true;

        return ResultBuilder("Removed the VBA project.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterListTables(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["sheet"] = SheetReferenceProperty(
            "Worksheet name (case-insensitive) or 1-based index; omit to list every sheet.");

        nlohmann::json column =
            Schema::Object("One table column.", {"name"},
                           nlohmann::json{{"name", Schema::String("Column name.")},
                                          {"id", Schema::Integer("Stable column identifier.")},
                                          {"totalsLabel", Schema::String("Label shown in the totals row.")}});

        nlohmann::json filter = Schema::Object(
            "One column filter.", {"column", "values"},
            nlohmann::json{{"column", Schema::String("Filtered column.")},
                           {"columnIndex", Schema::Integer("1-based column position.")},
                           {"values", Schema::Array("Values kept visible.", Schema::String("One value."))},
                           {"includeBlank", Schema::Boolean("Blank cells are kept too.")}});

        nlohmann::json table = Schema::Object(
            "One table.", {"name", "sheet", "range"},
            nlohmann::json{{"name", Schema::String("Table name.")},
                           {"sheet", Schema::String("Worksheet that holds it.")},
                           {"range", Schema::String("A1 range, header and totals rows included.")},
                           {"autoFilter", Schema::Boolean("The table carries filter buttons.")},
                           {"totalsRow", Schema::Boolean("The totals row is visible.")},
                           {"columns", Schema::Array("Columns in worksheet order.", std::move(column))},
                           {"filters", Schema::Array("Active column filters.", std::move(filter))}});

        auto definition = MakeDefinition("list_tables", "List tables",
                                         "List the structured tables of the workbook with their columns and the "
                                         "filters currently applied to them.",
                                         "analysis");
        definition.InputSchema = Schema::Object("Arguments of list_tables.", {}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Tables.", {"tables"},
                                            nlohmann::json{{"tables", Schema::Array("Tables.", std::move(table))}}),
                             false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListTables(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListTables(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        std::string only;
        if (arguments.contains("sheet"))
        {
            ToolOutcome failure;
            auto sheet = ExcelAddressing::FindSheet(reader.Editor(), arguments, failure);
            if (sheet == nullptr)
            {
                return failure;
            }

            only = sheet->Name();
        }

        nlohmann::json tables = nlohmann::json::array();
        for (const auto& sheet : reader.Editor().Worksheets())
        {
            if (sheet == nullptr || (!only.empty() && sheet->Name() != only))
            {
                continue;
            }

            for (const auto& table : sheet->Tables())
            {
                if (table != nullptr)
                {
                    tables.push_back(DescribeTable(table, sheet->Name()));
                }
            }
        }

        const auto count = tables.size();

        nlohmann::json data = nlohmann::json::object();
        data["tables"] = std::move(tables);

        return ResultBuilder("Listed " + std::to_string(count) + " table(s).").WithData(std::move(data)).Build();
    }

    static void RegisterUpdateTable(ToolRegistry& registry)
    {
        nlohmann::json filter = Schema::Object(
            "One column filter.", {"column"},
            nlohmann::json{
                {"column", SheetReferenceProperty("Column name (case-insensitive) or 1-based position.")},
                {"values", Schema::Array("Values to keep visible; the others are hidden.",
                                         Schema::String("One value, as it is written in the cell."))},
                {"include_blank", Schema::BooleanWithDefault("Keep blank cells visible as well.", false)},
                {"clear", Schema::BooleanWithDefault("Drop this column's filter instead of setting one.", false)}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetReferenceProperty(
            "Worksheet holding the table, by name or 1-based index; omit to search the whole workbook.");
        properties["table"] = Schema::String("Name of the table to update, matched case-insensitively.");
        properties["name"] = Schema::String("New table name.");
        properties["auto_filter"] = Schema::Boolean("Show or hide the column filter buttons.");
        properties["totals_row"] = Schema::Boolean("Show or hide the totals row; showing it needs two data rows.");
        properties["filters"] = Schema::Array("Column filters to set or drop.", std::move(filter));
        properties["clear_filters"] = Schema::BooleanWithDefault("Drop every column filter first.", false);

        auto definition = MakeDefinition(
            "update_table", "Update table",
            "Rename a table, show or hide its filter buttons and totals row, and set which values each column "
            "keeps visible. Rows the filters exclude are hidden, not deleted; clearing the filters brings them "
            "back.",
            "analysis");
        definition.InputSchema =
            Schema::Object("Arguments of update_table.", {"documentId", "table"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Updated table.", {"name", "sheet"},
                                            nlohmann::json{
                                                {"name", Schema::String("Table name.")},
                                                {"sheet", Schema::String("Worksheet that holds it.")},
                                                {"range", Schema::String("A1 range of the table.")},
                                                {"autoFilter", Schema::Boolean("Filter buttons are shown.")},
                                                {"totalsRow", Schema::Boolean("The totals row is shown.")},
                                                {"filterCount", Schema::Integer("Column filters now in force.")}}),
                             true);
        definition.Example =
            nlohmann::json{{"documentId", "doc-1"},
                           {"table", "Sales"},
                           {"filters", nlohmann::json::array({nlohmann::json{
                                           {"column", "Region"},
                                           {"values", nlohmann::json::array({"North", "South"})}}})}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return UpdateTable(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome UpdateTable(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        std::string only;
        if (arguments.contains("sheet"))
        {
            ToolOutcome failure;
            auto hosting = ExcelAddressing::FindSheet(session.Editor(), arguments, failure);
            if (hosting == nullptr)
            {
                return failure;
            }

            only = hosting->Name();
        }

        const auto tableName = arguments.value("table", std::string());
        std::string sheetName;
        auto table = FindTable(session.Editor(), tableName, only, sheetName);
        if (table == nullptr)
        {
            // A table is a block of the sheet, not a media object.
            return MakeError(ErrorCode::BlockNotFound, "No table named '" + tableName + "'.", tableName,
                             "Call list_tables to see the tables the workbook holds.");
        }

        auto sheet = session.Editor().GetWorksheet(sheetName);
        const auto columns = table->Columns();

        // Every filter is resolved against the table before anything is
        // written, so one naming a column that does not exist leaves the table
        // exactly as it was rather than half updated.
        struct PendingFilter
        {
            UInt32 Index = 0;
            bool Clear = false;
            Excel::ExcelTableValueFilter Filter;
        };

        std::vector<PendingFilter> pending;
        const auto filters = arguments.find("filters");
        if (filters != arguments.end())
        {
            for (const auto& entry : *filters)
            {
                const auto& column = entry.at("column");
                std::optional<UInt32> index;
                if (column.is_number_integer())
                {
                    const auto position = column.get<Int64>();
                    if (position >= 1 && static_cast<Size>(position) <= columns.size())
                    {
                        index = static_cast<UInt32>(position - 1);
                    }
                }
                else
                {
                    const auto wanted = column.get<std::string>();
                    for (Size candidate = 0; candidate < columns.size(); ++candidate)
                    {
                        if (AsciiText::EqualsIgnoreCase(columns[candidate].Name, wanted))
                        {
                            index = static_cast<UInt32>(candidate);
                            break;
                        }
                    }
                }

                if (!index.has_value())
                {
                    return MakeError(ErrorCode::InputInvalid, "The table has no column " + column.dump() + ".",
                                     tableName, "Name a column of the table, or give its 1-based position.");
                }

                PendingFilter item;
                item.Index = *index;
                item.Clear = entry.value("clear", false);
                item.Filter.ColumnIndex = *index;
                item.Filter.IncludeBlank = entry.value("include_blank", false);
                const auto values = entry.find("values");
                if (values != entry.end())
                {
                    for (const auto& value : *values)
                    {
                        item.Filter.Values.push_back(value.get<std::string>());
                    }
                }

                if (!item.Clear && item.Filter.Values.empty() && !item.Filter.IncludeBlank)
                {
                    return MakeError(ErrorCode::InputInvalid, "A filter needs values, include_blank, or clear.",
                                     columns[*index].Name,
                                     "An empty filter would hide every row; pass clear to drop the filter "
                                     "instead.");
                }

                pending.push_back(std::move(item));
            }
        }

        MutationGuard guard(session.Session());

        if (arguments.contains("name"))
        {
            const auto newName = arguments.value("name", std::string());
            if (sheet == nullptr || !sheet->RenameTable(table, newName))
            {
                return MakeError(ErrorCode::OperationFailed, "The table could not be renamed to '" + newName + "'.",
                                 newName,
                                 "A table name is unique in the workbook, starts with a letter or an underscore, "
                                 "and carries no spaces.");
            }
        }

        if (arguments.contains("auto_filter") && !table->SetAutoFilterEnabled(arguments.value("auto_filter", true)))
        {
            return MakeError(ErrorCode::OperationFailed, "The filter buttons could not be changed.", tableName);
        }

        if (arguments.value("clear_filters", false) && !table->ClearValueFilters())
        {
            return MakeError(ErrorCode::OperationFailed, "The column filters could not be cleared.", tableName);
        }

        for (const auto& item : pending)
        {
            if (item.Clear)
            {
                table->RemoveValueFilter(item.Index);
                continue;
            }

            if (!table->SetValueFilter(item.Filter))
            {
                return MakeError(ErrorCode::OperationFailed,
                                 "The filter for column " + std::to_string(item.Index + 1) + " could not be set.",
                                 tableName,
                                 "Values have to be distinct, and the table needs its filter buttons shown.");
            }
        }

        if (arguments.contains("totals_row") && !table->SetTotalsRowShown(arguments.value("totals_row", false)))
        {
            return MakeError(ErrorCode::OperationFailed, "The totals row could not be changed.", tableName,
                             "Showing a totals row needs a table with at least two rows.");
        }

        ApplyTableFilters(sheet, table, session.Editor().SharedStrings());

        guard.Commit();

        const auto range = table->Range();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = table->Name();
        data["sheet"] = sheetName;
        data["range"] = range.has_value() ? range->ToA1() : std::string();
        data["autoFilter"] = table->AutoFilterEnabled();
        data["totalsRow"] = table->TotalsRowShown();
        data["filterCount"] = table->ValueFilters().size();

        return ResultBuilder("Updated table '" + table->Name() + "'.")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddTable(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = Schema::String("A1 range covered by the table, including the header row.");
        properties["name"] = Schema::String("Table name; generated when omitted.");
        properties["header_row"] =
            Schema::BooleanWithDefault(
                "Take the column names from the first row of the range. The first row is the table's header row "
                "either way: an empty header cell, or every header cell when this is false, is given a generated "
                "name such as Column1, and the result warns when that overwrote a value.",
                true);

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

        // Excel keeps merged cells out of tables: it refuses to make one over
        // them and will not open a workbook that has one.
        for (const auto& merged : sheet->MergedRanges())
        {
            if (RangesIntersect(merged, *range))
            {
                return MakeError(ErrorCode::InputInvalid,
                                 "The range " + range->ToA1() + " contains the merged cells " + merged.ToA1() +
                                     ", and a table cannot hold merged cells.",
                                 merged.ToA1(), "Unmerge them first with merge_cells and unmerge set to true.");
            }
        }

        // Nor may two tables share a cell: Excel refuses the workbook, and the
        // header row written for the new table would have overwritten the old
        // table's data first.
        for (const auto& existing : sheet->Tables())
        {
            const auto area = existing != nullptr ? existing->Range() : std::nullopt;
            if (area.has_value() && RangesIntersect(*area, *range))
            {
                return MakeError(ErrorCode::RangeInvalid,
                                 "The range " + range->ToA1() + " overlaps the table '" + existing->Name() +
                                     "' at " + area->ToA1() + ".",
                                 existing->Name(),
                                 "Tables cannot share cells; choose a range outside " + area->ToA1() +
                                     ", or extend that table instead.");
            }
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

        // The library writes each column name into its header cell, because
        // Excel refuses a table whose header cells disagree with its columns.
        // A header cell that held something else is worth telling the caller.
        std::vector<std::string> overwritten;
        for (Size index = 0; index < columns.size(); ++index)
        {
            const auto address = Excel::CellAddress::TryCreate(
                range->First().Row().Value(), range->First().Column().Value() + static_cast<UInt32>(index));
            const auto stored = address.has_value() ? sheet->GetCellValue(*address) : std::nullopt;
            if (stored.has_value())
            {
                const auto text = ExcelAddressing::CellValueToText(*stored, sharedStrings);
                if (!text.empty() && text != columns[index].Name)
                {
                    overwritten.push_back(address->ToA1());
                }
            }
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

        ResultBuilder builder("Created table '" + table->Name() + "' over " + range->ToA1() + ".");
        builder.WithSession(session.Session()).WithData(std::move(data));
        if (!overwritten.empty())
        {
            std::string cells;
            for (const auto& cell : overwritten)
            {
                cells += (cells.empty() ? "" : ", ") + cell;
            }
            builder.WithWarning("table_header_rewritten",
                                "The header row now holds the column names, replacing the values in " + cells + ".",
                                range->ToA1());
        }

        return builder.Build();
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
                                              "containsBlanks", "notContainsBlanks", "containsErrors",
                                              "notContainsErrors", "top", "bottom", "aboveAverage",
                                              "belowAverage"})},
                {"operator", Schema::Enumeration("Comparison for cellIs rules.",
                                                 {"lessThan", "lessThanOrEqual", "equal", "notEqual",
                                                  "greaterThanOrEqual", "greaterThan", "between", "notBetween"})},
                {"formula1", Schema::String("First formula or bound.")},
                {"formula2", Schema::String("Second bound for between and notBetween.")},
                {"text", Schema::String("Text for the text-matching rules.")},
                {"rank", Schema::IntegerWithDefault("How many items a top or bottom rule selects.", 10, 1, 1000)},
                {"percent", Schema::BooleanWithDefault(
                                "Read 'rank' as a percentage of the range rather than a count of items.", false)},
                {"equal_average",
                 Schema::BooleanWithDefault("Include values equal to the average in an average rule.", false)},
                {"standard_deviation",
                 Schema::Integer("Shift an average rule's boundary by this many standard deviations.", 0, 3)},
                {"stop_if_true", Schema::Boolean("Stop evaluating further rules when this one matches.")}});

        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["range"] = RangeListProperty();
        properties["rule"] = std::move(rule);
        properties["format"] = Schema::Object(
            "Appearance painted over the cells the rule matches. Only the members you pass take part; every "
            "other aspect of a matching cell keeps the look it already had.",
            {}, StyleProperties());

        auto definition = MakeDefinition(
            "add_conditional_formatting", "Add conditional formatting",
            "Add a conditional formatting rule to one or several ranges. The rule decides which cells match and "
            "'format' decides what they then look like; a rule without a format matches cells and changes "
            "nothing about them. Color scales, data bars and icon sets are not offered by this version.",
            "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_conditional_formatting.",
                                                {"documentId", "range", "rule"}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("New rule.", {"range"},
                           nlohmann::json{{"range", Schema::String("A1 range.")},
                                          {"type", Schema::String("Rule kind applied.")},
                                          {"differentialFormatId",
                                           Schema::Integer("Workbook differential format the rule paints with; "
                                                           "absent when the rule carries no appearance.")}}),
            true);
        definition.Example = nlohmann::json{
            {"documentId", "doc-1"},
            {"range", "B2:B20"},
            {"rule", nlohmann::json{{"type", "cellIs"}, {"operator", "greaterThan"}, {"formula1", "100"}}},
            {"format", nlohmann::json{{"fill", nlohmann::json{{"color", "#FFC7CE"}}}}}};
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

        std::vector<std::string> rangeTokens;
        const auto& rangeArgument = arguments.at("range");
        if (rangeArgument.is_array())
        {
            for (const auto& entry : rangeArgument)
            {
                if (!entry.is_string())
                {
                    return MakeError(ErrorCode::InputInvalid, "Every entry of 'range' has to be an A1 range.",
                                     entry.dump());
                }
                rangeTokens.push_back(entry.get<std::string>());
            }
        }
        else
        {
            rangeTokens.push_back(rangeArgument.get<std::string>());
        }

        if (rangeTokens.empty())
        {
            return MakeError(ErrorCode::InputInvalid, "A rule needs at least one range.", "range");
        }

        std::vector<Excel::CellRange> ranges;
        std::string rangeText;
        for (const auto& token : rangeTokens)
        {
            const auto parsed = ExcelAddressing::ParseRange(token, failure);
            if (!parsed.has_value())
            {
                return failure;
            }
            ranges.push_back(*parsed);
            if (!rangeText.empty())
            {
                rangeText.push_back(' ');
            }
            rangeText.append(parsed->ToA1());
        }

        const auto& rule = arguments.at("rule");
        const auto type = rule.value("type", std::string());
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
        else if (type == "containsErrors")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::ContainsErrors(ranges);
        }
        else if (type == "notContainsErrors")
        {
            definition = Excel::ExcelConditionalFormattingDefinition::NotContainsErrors(ranges);
        }
        else if (type == "top" || type == "bottom")
        {
            // The schema bounds the rank, so a value outside it never arrives;
            // the fallback only has to match the published default.
            const auto rank = static_cast<UInt32>(rule.value("rank", 10));
            const auto percent = rule.value("percent", false);
            definition = type == "top" ? Excel::ExcelConditionalFormattingDefinition::Top(ranges, rank, percent)
                                       : Excel::ExcelConditionalFormattingDefinition::Bottom(ranges, rank, percent);
        }
        else if (type == "aboveAverage" || type == "belowAverage")
        {
            const auto equalAverage = rule.value("equal_average", false);
            std::optional<Int32> deviation;
            if (rule.contains("standard_deviation"))
            {
                deviation = static_cast<Int32>(rule.value("standard_deviation", 0));
            }
            definition = type == "aboveAverage"
                             ? Excel::ExcelConditionalFormattingDefinition::AboveAverage(ranges, equalAverage,
                                                                                         deviation)
                             : Excel::ExcelConditionalFormattingDefinition::BelowAverage(ranges, equalAverage,
                                                                                         deviation);
        }
        else
        {
            return MakeError(ErrorCode::Unsupported, "The rule kind '" + type + "' is not offered by this server.",
                             type,
                             "Color scales, data bars and icon sets are not written by this version; use cellIs "
                             "or expression and pass the appearance in 'format'.");
        }

        definition->StopIfTrue = rule.value("stop_if_true", false);

        if (!Excel::IsValidExcelConditionalFormatting(*definition))
        {
            return MakeError(ErrorCode::InputInvalid, "The rule is incomplete for its kind.", type,
                             "A cellIs rule needs one formula, or two for between and notBetween; a text rule "
                             "needs 'text'; the remaining kinds take neither.");
        }

        MutationGuard guard(session.Session());

        // The appearance is registered as a workbook differential format and
        // the rule refers to it by index; that reference is the whole of what
        // a matching cell ends up looking like.
        const auto format = arguments.find("format");
        if (format != arguments.end() && format->is_object())
        {
            Excel::ExcelStyle appearance;
            if (!ReadStyleArguments(*format, appearance, failure))
            {
                return failure;
            }

            auto styles = session.Editor().Styles();
            const auto registration = styles.GetOrAddDifferentialFormat(appearance);
            if (!registration.Succeeded())
            {
                return MakeError(ErrorCode::InputInvalid, registration.Status.Message, "format",
                                 "Pass at least one of number_format, font, fill, border or alignment.");
            }

            definition->DifferentialFormatId = registration.StyleIndex;
        }

        if (sheet->CreateConditionalFormatting(*definition) == nullptr)
        {
            return MakeError(ErrorCode::OperationFailed, "The conditional formatting could not be created.",
                             rangeText);
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["range"] = rangeText;
        data["type"] = type;
        if (definition->DifferentialFormatId.has_value())
        {
            data["differentialFormatId"] = *definition->DifferentialFormatId;
        }

        return ResultBuilder("Added a '" + type + "' rule to " + rangeText + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterAddChart(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["type"] = Schema::Enumeration("Chart type.", ToolSupport::ChartTypeTokens());
        properties["data_range"] = Schema::String(
            "A1 range holding the series values, optionally on another worksheet (Data!B2:D10, or 'My Data'!B2:D10 "
            "for a name with spaces); unqualified, it is read from the chart's own sheet. A range spanning several "
            "columns becomes one series per column; see series_in.");
        properties["series_in"] = Schema::EnumerationWithDefault(
            "Whether each column or each row of data_range is one series.", {"columns", "rows"}, "columns");
        properties["series_names"] =
            Schema::Array("Series names in data order; generated names are used for the rest.",
                          Schema::String("One series name."));
        properties["series_options"] = Schema::Array(
            "How each series is drawn, in data order; series beyond the list keep the chart's type on the primary "
            "axis. Giving a series another type makes a combination chart, such as columns with a line over them.",
            Schema::Object(
                "Drawing of one series.", {},
                nlohmann::json{
                    {"type", Schema::Enumeration("Type this series is drawn as. Column, line and area combine with "
                                                 "each other; bar, scatter, bubble and pie only with their own kind.",
                                                 ToolSupport::ChartTypeTokens())},
                    {"secondary_axis",
                     Schema::BooleanWithDefault("Plot the series against a secondary value axis on the opposite "
                                                "side, for values on a different scale. At least one series has "
                                                "to stay on the primary axis.",
                                                false)}}));
        properties["categories_range"] =
            Schema::String("A1 range holding the category labels, shared by every series; the X values of a scatter "
                           "or bubble chart. Must be on the same worksheet as data_range.");
        properties["sizes_range"] =
            Schema::String("Bubble sizes, shaped like data_range and on the same worksheet. Required for a bubble "
                           "chart and refused for any other.");
        properties["anchor_cell"] = Schema::String("A1 cell the chart's top-left corner sits on.");
        properties["width"] = Schema::Length("Chart width; defaults to about 15 cm.");
        properties["height"] = Schema::Length("Chart height; defaults to about 8 cm.");
        properties["title"] = Schema::String("Chart title.");
        ToolSupport::AddChartAppearanceProperties(properties);

        auto definition = MakeDefinition(
            "add_chart", "Add chart",
            "Add a chart driven by a range of values and optional category labels, from this worksheet or another. "
            "A multi-column data range plots one series per column, or per row when series_in is \"rows\"; "
            "series_options draws a series as another type or against a secondary axis. A pie chart plots one "
            "series and warns when the range holds more.",
            "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_chart.",
                                                {"documentId", "type", "data_range", "anchor_cell"},
                                                std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object(
                "New chart.", {"chartId"},
                nlohmann::json{{"chartId", Schema::Integer("Chart identifier.")},
                               {"anchor", Schema::String("A1 anchor cell.")},
                               {"seriesCount", Schema::Integer("Series the chart plots.")},
                               {"sourceSheet", Schema::String("Worksheet the series are read from.")}}),
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

        if (token == "bubble")
        {
            return Excel::ExcelChartType::Bubble;
        }

        return Excel::ExcelChartType::Column;
    }

    static Excel::ExcelLegendPosition ParseLegendPosition(const std::string& token)
    {
        if (token == "left")
        {
            return Excel::ExcelLegendPosition::Left;
        }

        if (token == "top")
        {
            return Excel::ExcelLegendPosition::Top;
        }

        if (token == "bottom")
        {
            return Excel::ExcelLegendPosition::Bottom;
        }

        return token == "none" ? Excel::ExcelLegendPosition::None : Excel::ExcelLegendPosition::Right;
    }

    /**
     * @brief Parses a chart source range that may name its worksheet.
     *
     * A chart often sits on a summary sheet and plots data kept elsewhere, so
     * the range carries the worksheet rather than the chart. @p sheetName is
     * the resolved worksheet's own name, or empty for an unqualified range,
     * which is read from the chart's sheet.
     */
    static std::optional<Excel::CellRange> ParseChartRange(Excel::ExcelDocumentEditor& editor,
                                                           const std::string& text, std::string& sheetName,
                                                           ToolOutcome& failure)
    {
        sheetName.clear();
        if (text.find('!') == std::string::npos)
        {
            return ExcelAddressing::ParseRange(text, failure);
        }

        const auto qualified = Excel::SheetCellRange::Parse(text);
        if (!qualified.has_value() || !qualified->Range().IsValid())
        {
            failure = MakeError(ErrorCode::RangeInvalid, "'" + text + "' is not a valid sheet-qualified A1 range.",
                                text, "Write it as Data!B2:B10, quoting a name with spaces: 'My Data'!B2:B10.");
            return std::nullopt;
        }

        const auto sheet = ExcelAddressing::FindSheet(editor, nlohmann::json{{"sheet", qualified->Sheet()}}, failure);
        if (sheet == nullptr)
        {
            return std::nullopt;
        }

        sheetName = sheet->Name();
        return qualified->Range();
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

    /**
     * @brief Resolves where an object of the requested size sits on the sheet.
     *
     * The size comes from `width` and `height`, falling back to the defaults,
     * and the library turns it into an anchor from the sheet's real column
     * widths and row heights plus the exact extent. Pictures and charts store
     * the extent, so they keep the requested size on any screen; slicers span
     * the computed cells.
     *
     * @return The anchor, or std::nullopt with @p failure set for a size that
     *         is not a non-negative length.
     */
    static std::optional<Excel::DrawingAnchor> ResolveAnchor(const Excel::Worksheet& sheet,
                                                             Excel::CellAddress from,
                                                             const nlohmann::json& arguments, Real defaultWidthPt,
                                                             Real defaultHeightPt, ToolOutcome& failure)
    {
        MeasuringUnits width(defaultWidthPt, MeasurementUnit::Point);
        MeasuringUnits height(defaultHeightPt, MeasurementUnit::Point);
        const auto readLength = [&](const char* member, MeasuringUnits& target)
        {
            const auto value = arguments.find(member);
            if (value == arguments.end())
            {
                return true;
            }
            const auto parsed = ParseLength(*value);
            if (!parsed.has_value() || ToPointValue(*parsed) < 0.0)
            {
                failure = MakeError(ErrorCode::InputInvalid,
                                    std::string("'") + member + "' has to be a non-negative length.", member,
                                    "Pass a number of points or a string with a unit, such as \"4cm\".");
                return false;
            }
            target = *parsed;
            return true;
        };
        if (!readLength("width", width) || !readLength("height", height))
        {
            return std::nullopt;
        }

        auto anchor = sheet.DrawingAnchorForSize(from, width, height);
        if (!anchor.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid, "The size could not be placed on the worksheet.",
                                from.ToA1());
            return std::nullopt;
        }
        return anchor;
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

        std::string dataSheet;
        const auto dataRange =
            ParseChartRange(session.Editor(), arguments.value("data_range", std::string()), dataSheet, failure);
        if (!dataRange.has_value())
        {
            return failure;
        }

        const auto anchor = ExcelAddressing::ParseCell(arguments.value("anchor_cell", std::string()), failure);
        if (!anchor.has_value())
        {
            return failure;
        }

        const auto type = arguments.value("type", std::string("column"));
        const auto legend = arguments.value("legend", std::string("right"));
        Excel::ExcelChartDefinition chart;
        chart.Type = ParseChartType(type);
        chart.Title = arguments.value("title", std::string());
        chart.CategoryAxisTitle = arguments.value("category_axis_title", std::string());
        chart.ValueAxisTitle = arguments.value("value_axis_title", std::string());
        chart.SecondaryValueAxisTitle = arguments.value("secondary_axis_title", std::string());
        chart.ShowLegend = legend != "none";
        chart.LegendPosition = ParseLegendPosition(legend);
        chart.ShowGridLines = arguments.value("gridlines", true);
        const auto placement = ResolveAnchor(*sheet, *anchor, arguments, 425.0, 227.0, failure);
        if (!placement.has_value())
        {
            return failure;
        }
        chart.From = placement->From;
        chart.FromOffset = placement->FromOffset;
        chart.To = placement->To;
        chart.ToOffset = placement->ToOffset;
        chart.Extent = placement->Extent;

        // A series names one worksheet for all of its ranges, so the labels and
        // sizes have to come from the sheet the values come from.
        const std::string source = dataSheet.empty() ? sheet->Name() : dataSheet;
        const auto readCompanionRange = [&](const std::string& member, std::optional<Excel::CellRange>& range)
        {
            const auto text = arguments.value(member, std::string());
            if (text.empty())
            {
                return true;
            }

            std::string rangeSheet;
            range = ParseChartRange(session.Editor(), text, rangeSheet, failure);
            if (!range.has_value())
            {
                return false;
            }

            if ((rangeSheet.empty() ? sheet->Name() : rangeSheet) != source)
            {
                failure = MakeError(ErrorCode::InputInvalid,
                                    "'" + member + "' is on a different worksheet from data_range, which reads from '" +
                                        source + "'.",
                                    text, "Qualify both ranges with the same worksheet name.");
                return false;
            }

            return true;
        };

        std::optional<Excel::CellRange> categoryRange;
        if (!readCompanionRange("categories_range", categoryRange))
        {
            return failure;
        }

        std::optional<Excel::CellRange> sizesRange;
        if (!readCompanionRange("sizes_range", sizesRange))
        {
            return failure;
        }

        const bool bubble = chart.Type == Excel::ExcelChartType::Bubble;
        if (bubble != sizesRange.has_value())
        {
            return bubble ? MakeError(ErrorCode::InputInvalid, "A bubble chart needs 'sizes_range'.", "sizes_range",
                                      "Pass a range shaped like data_range holding the size of each bubble.")
                          : MakeError(ErrorCode::InputInvalid, "'sizes_range' applies only to a bubble chart.",
                                      "sizes_range", "Remove sizes_range, or set type to \"bubble\".");
        }

        const bool byColumns = arguments.value("series_in", std::string("columns")) != "rows";
        auto seriesRanges = SplitSeriesRanges(*dataRange, byColumns);
        if (seriesRanges.empty())
        {
            seriesRanges.push_back(*dataRange);
        }

        std::vector<Excel::CellRange> sizeRanges;
        if (sizesRange.has_value())
        {
            sizeRanges = SplitSeriesRanges(*sizesRange, byColumns);
            if (sizeRanges.size() != seriesRanges.size())
            {
                return MakeError(ErrorCode::InputInvalid,
                                 "'sizes_range' holds " + std::to_string(sizeRanges.size()) +
                                     " series of sizes, but data_range holds " + std::to_string(seriesRanges.size()) +
                                     " series of values.",
                                 sizesRange->ToA1(), "Give sizes_range the same shape as data_range.");
            }
        }

        std::vector<ToolSupport::ChartSeriesPlan> plans(seriesRanges.size());
        const auto options = arguments.find("series_options");
        if (options != arguments.end())
        {
            if (options->size() > seriesRanges.size())
            {
                return MakeError(ErrorCode::InputInvalid,
                                 "'series_options' has " + std::to_string(options->size()) +
                                     " entries, but data_range holds " + std::to_string(seriesRanges.size()) +
                                     " series.",
                                 "series_options", "Give at most one entry per series, in data order.");
            }

            for (Size index = 0; index < options->size(); ++index)
            {
                plans[index].Type = (*options)[index].value("type", std::string());
                plans[index].SecondaryAxis = (*options)[index].value("secondary_axis", false);
            }
        }

        std::string warning;
        if (chart.Type == Excel::ExcelChartType::Pie && seriesRanges.size() > 1)
        {
            // A pie chart plots exactly one series; charting the rest anyway
            // would quietly produce something the caller did not ask for.
            warning = std::string("A pie chart plots one series, so only the first ") +
                      (byColumns ? "column" : "row") + " of " + dataRange->ToA1() + " is charted.";
            seriesRanges.resize(1);
            plans.resize(1);
        }

        const auto combination = ToolSupport::ChartCombinationError(type, plans);
        if (!combination.empty())
        {
            return MakeError(ErrorCode::InputInvalid, combination, "series_options",
                             "Change the series type, or leave it out to draw the series as the chart's own type.");
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
            if (!plans[index].Type.empty())
            {
                series.Type = ParseChartType(plans[index].Type);
            }

            series.SecondaryAxis = plans[index].SecondaryAxis;
            if (source != sheet->Name())
            {
                series.SourceSheet = source;
            }

            if (!sizeRanges.empty())
            {
                series.BubbleSizes = sizeRanges[index];
            }

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
        data["sourceSheet"] = source;

        ResultBuilder builder("Added a chart with " + std::to_string(chart.Series.size()) +
                              " series anchored at " + anchor->ToA1() + ".");
        builder.WithSession(session.Session()).WithData(std::move(data));
        if (!warning.empty())
        {
            builder.WithWarning("chart_series_dropped", warning, dataRange->ToA1());
        }

        return builder.Build();
    }

    static std::string SlicerSortToken(Excel::SlicerSortOrder value)
    {
        return value == Excel::SlicerSortOrder::Descending ? "descending" : "ascending";
    }

    static void RegisterAddSlicer(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["sheet"] = SheetProperty();
        properties["source_kind"] =
            Schema::EnumerationWithDefault("What the slicer filters.", {"pivot_table", "table"}, "pivot_table");
        properties["source"] = Schema::String("Name of the pivot table or worksheet table to filter. A pivot "
                                              "table may live on any sheet of the workbook.");
        properties["field"] = Schema::String("Source column the buttons come from, matched case-insensitively.");
        properties["anchor_cell"] = Schema::String("A1 cell the slicer's top-left corner sits on.");
        properties["width"] = Schema::Length("Slicer width; defaults to about 4 cm.");
        properties["height"] = Schema::Length("Slicer height; defaults to about 5 cm.");
        properties["name"] = Schema::String("Slicer name, unique in the workbook; generated when omitted.");
        properties["caption"] = Schema::String("Header caption; the field name is used when omitted.");
        properties["columns"] = Schema::Integer("Number of button columns.", 1, 20000);
        properties["style"] = Schema::StringWithDefault("Slicer style name.", "SlicerStyleLight1");
        properties["sort_order"] =
            Schema::EnumerationWithDefault("Button order.", {"ascending", "descending"}, "ascending");
        properties["selected"] =
            Schema::Array("Captions of the selected items; an empty list selects everything.",
                          Schema::String("One item caption."));

        auto definition = MakeDefinition(
            "add_slicer", "Add slicer",
            "Add a slicer filtering a pivot table or a worksheet table by one of its columns. The source has "
            "to exist already; build it with add_pivot_table or add_table first.",
            "analysis");
        definition.InputSchema = Schema::Object("Arguments of add_slicer.",
                                                {"documentId", "source", "field", "anchor_cell"},
                                                std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("New slicer.", {"name"},
                                            nlohmann::json{{"name", Schema::String("Slicer name.")},
                                                           {"anchor", Schema::String("A1 anchor cell.")},
                                                           {"itemCount", Schema::Integer("Buttons the slicer "
                                                                                         "offers.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"},
                                            {"source", "SalesByRegion"},
                                            {"field", "Region"},
                                            {"anchor_cell", "F2"}};
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return AddSlicer(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome AddSlicer(ToolContext& context, const nlohmann::json& arguments)
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

        const auto anchorText = arguments.value("anchor_cell", std::string());
        const auto anchor = ExcelAddressing::ParseCell(anchorText, failure);
        if (!anchor.has_value())
        {
            return failure;
        }

        const auto placement = ResolveAnchor(*sheet, *anchor, arguments, 113.0, 142.0, failure);
        if (!placement.has_value())
        {
            return failure;
        }

        Excel::ExcelSlicerDefinition slicer;
        slicer.Name = arguments.value("name", std::string());
        slicer.Caption = arguments.value("caption", std::string());
        slicer.SourceField = arguments.value("field", std::string());
        slicer.From = placement->From;
        slicer.FromOffset = placement->FromOffset;
        slicer.To = placement->To;
        slicer.ToOffset = placement->ToOffset;
        slicer.ColumnCount = arguments.value("columns", 1U);
        slicer.Style = arguments.value("style", std::string("SlicerStyleLight1"));
        slicer.SortOrder = arguments.value("sort_order", std::string("ascending")) == "descending"
                               ? Excel::SlicerSortOrder::Descending
                               : Excel::SlicerSortOrder::Ascending;

        const auto source = arguments.value("source", std::string());
        if (arguments.value("source_kind", std::string("pivot_table")) == "table")
        {
            slicer.SourceKind = Excel::SlicerSourceKind::Table;
            slicer.TableName = source;
        }
        else
        {
            slicer.SourceKind = Excel::SlicerSourceKind::PivotTable;
            slicer.PivotTableName = source;
        }

        if (const auto selected = arguments.find("selected"); selected != arguments.end())
        {
            slicer.SelectedItems = selected->get<std::vector<std::string>>();
        }

        MutationGuard guard(session.Session());

        const auto result = sheet->CreateSlicer(slicer);
        if (result.Status.Error != Excel::SlicerError::None || result.Slicer == nullptr)
        {
            // The library distinguishes an unknown source from an unknown field
            // and from a bad anchor, and the difference is exactly what tells an
            // agent whether to build the pivot table or to fix the column name.
            return MakeError(ErrorCode::OperationFailed,
                             result.Status.Message.empty() ? "The slicer could not be created."
                                                           : result.Status.Message,
                             source + "." + slicer.SourceField,
                             "The pivot table or table has to exist in the workbook, and the field has to be "
                             "one of its columns.");
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = result.Slicer->Name();
        data["anchor"] = anchor->ToA1();
        data["itemCount"] = static_cast<UInt64>(result.Slicer->Items().size());

        return ResultBuilder("Added slicer " + result.Slicer->Name() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
    }

    static void RegisterListSlicers(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentSourceProperties(properties);
        properties["sheet"] = SheetReferenceProperty(
            "Worksheet name (case-insensitive) or 1-based index; omit to list every sheet.");

        nlohmann::json item =
            Schema::Object("One slicer button.", {"caption", "selected"},
                           nlohmann::json{{"caption", Schema::String("Button caption.")},
                                          {"selected", Schema::Boolean("True when the item is not filtered "
                                                                       "out.")},
                                          {"hasNoData", Schema::Boolean("True when no source row matches.")}});

        nlohmann::json slicer =
            Schema::Object("One slicer.", {"name", "sheet"},
                           nlohmann::json{{"name", Schema::String("Slicer name.")},
                                          {"sheet", Schema::String("Worksheet that hosts it.")},
                                          {"caption", Schema::String("Header caption.")},
                                          {"sourceKind", Schema::String("pivot_table or table.")},
                                          {"source", Schema::String("Name of the filtered object.")},
                                          {"field", Schema::String("Source column.")},
                                          {"anchor", Schema::String("A1 anchor cell.")},
                                          {"columns", Schema::Integer("Button columns.")},
                                          {"style", Schema::String("Slicer style name.")},
                                          {"sortOrder", Schema::String("ascending or descending.")},
                                          {"items", Schema::Array("Buttons.", std::move(item))}});

        auto definition = MakeDefinition("list_slicers", "List slicers",
                                         "List the slicers of the workbook with their buttons and which of "
                                         "them are selected.",
                                         "analysis");
        definition.InputSchema = Schema::Object("Arguments of list_slicers.", {}, std::move(properties));
        definition.OutputSchema = Schema::Envelope(
            Schema::Object("Slicers.", {"slicers"},
                           nlohmann::json{{"slicers", Schema::Array("Slicers.", std::move(slicer))}}),
            false);
        definition.Example = nlohmann::json{{"documentId", "doc-1"}};
        definition.Annotations.ReadOnly = true;
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return ListSlicers(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome ListSlicers(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelReader reader(context, arguments);
        if (!reader.IsValid())
        {
            return reader.Failure();
        }

        std::string only;
        if (arguments.contains("sheet"))
        {
            ToolOutcome failure;
            auto sheet = ExcelAddressing::FindSheet(reader.Editor(), arguments, failure);
            if (sheet == nullptr)
            {
                return failure;
            }

            only = sheet->Name();
        }

        nlohmann::json slicers = nlohmann::json::array();
        for (const auto& sheet : reader.Editor().Worksheets())
        {
            if (sheet == nullptr || (!only.empty() && sheet->Name() != only))
            {
                continue;
            }

            for (const auto& slicer : sheet->Slicers())
            {
                if (slicer == nullptr)
                {
                    continue;
                }

                nlohmann::json items = nlohmann::json::array();
                for (const auto& item : slicer->Items())
                {
                    items.push_back(nlohmann::json{{"caption", item.Caption},
                                                   {"selected", item.Selected},
                                                   {"hasNoData", item.HasNoData}});
                }

                const auto anchor = slicer->Anchor();

                nlohmann::json entry = nlohmann::json::object();
                entry["name"] = slicer->Name();
                entry["sheet"] = sheet->Name();
                entry["caption"] = slicer->Caption();
                entry["sourceKind"] =
                    slicer->SourceKind() == Excel::SlicerSourceKind::Table ? "table" : "pivot_table";
                entry["source"] = slicer->SourceObjectName();
                entry["field"] = slicer->SourceField();
                entry["anchor"] = anchor.has_value() ? anchor->first.ToA1() : std::string();
                entry["columns"] = static_cast<UInt64>(slicer->ColumnCount());
                entry["style"] = slicer->Style();
                entry["sortOrder"] = SlicerSortToken(slicer->SortOrder());
                entry["items"] = std::move(items);
                slicers.push_back(std::move(entry));
            }
        }

        const bool truncated = TruncateArrayToBudget(slicers);

        nlohmann::json data = nlohmann::json::object();
        const auto count = slicers.size();
        data["slicers"] = std::move(slicers);

        return ResultBuilder("The workbook holds " + std::to_string(count) + " slicer(s).")
            .WithData(std::move(data))
            .WithTruncated(truncated)
            .Build();
    }

    static void RegisterSetSlicerSelection(ToolRegistry& registry)
    {
        nlohmann::json properties = nlohmann::json::object();
        ToolSupport::AddDocumentIdProperty(properties);
        properties["slicer"] = Schema::String("Slicer name from list_slicers.");
        properties["selected"] =
            Schema::Array("Captions to select; an empty list selects everything, which is what clearing the "
                          "filter means.",
                          Schema::String("One item caption."));

        auto definition = MakeDefinition(
            "set_slicer_selection", "Set slicer selection",
            "Choose which of a slicer's buttons are selected, which is what the slicer filters its source "
            "down to. Items are named by caption, because cache indexes move when the source is refreshed.",
            "analysis");
        definition.InputSchema = Schema::Object("Arguments of set_slicer_selection.",
                                                {"documentId", "slicer", "selected"}, std::move(properties));
        definition.OutputSchema =
            Schema::Envelope(Schema::Object("Selection.", {"name", "selectedCount"},
                                            nlohmann::json{{"name", Schema::String("Slicer name.")},
                                                           {"selectedCount", Schema::Integer("Items now "
                                                                                             "selected.")}}),
                             true);
        definition.Example = nlohmann::json{{"documentId", "doc-1"},
                                            {"slicer", "Slicer1"},
                                            {"selected", nlohmann::json::array({"North", "South"})}};
        definition.Annotations.Idempotent = true;
        definition.Handler = [](ToolContext& context, const nlohmann::json& arguments)
        { return SetSlicerSelection(context, arguments); };
        registry.Add(std::move(definition));
    }

    static ToolOutcome SetSlicerSelection(ToolContext& context, const nlohmann::json& arguments)
    {
        ExcelSession session(context, arguments);
        if (!session.IsValid())
        {
            return session.Failure();
        }

        const auto name = arguments.value("slicer", std::string());

        // A slicer name is unique workbook-wide, so it is looked up across every
        // sheet rather than needing the caller to remember where it sits.
        Excel::ExcelSlicer::Ptr target;
        for (const auto& sheet : session.Editor().Worksheets())
        {
            if (sheet == nullptr)
            {
                continue;
            }

            for (const auto& slicer : sheet->Slicers())
            {
                if (slicer != nullptr && AsciiText::EqualsIgnoreCase(slicer->Name(), name))
                {
                    target = slicer;
                    break;
                }
            }
        }

        if (target == nullptr)
        {
            // A slicer is a drawing shape on the sheet.
            return MakeError(ErrorCode::ShapeNotFound, "No slicer has that name.", name,
                             "Call list_slicers to see them.");
        }

        const auto selected = arguments.value("selected", std::vector<std::string>());

        MutationGuard guard(session.Session());

        const auto result = target->SelectItems(selected);
        if (result.Error != Excel::SlicerError::None)
        {
            return MakeError(ErrorCode::InputInvalid,
                             result.Message.empty() ? "The selection could not be written." : result.Message,
                             name, "Every caption has to be one the slicer offers; list_slicers reports them.");
        }

        // A table slicer writes its selection as the table's column filter,
        // and a filter is only half of what a reader sees: the rows it
        // excludes have to be hidden as well, exactly as update_table does.
        if (target->SourceKind() == Excel::SlicerSourceKind::Table)
        {
            std::string hostSheet;
            auto table = FindTable(session.Editor(), target->SourceObjectName(), std::string(), hostSheet);
            if (table != nullptr)
            {
                ApplyTableFilters(session.Editor().GetWorksheet(hostSheet), table, session.Editor().SharedStrings());
            }
        }

        guard.Commit();

        nlohmann::json data = nlohmann::json::object();
        data["name"] = target->Name();
        data["selectedCount"] = static_cast<UInt64>(selected.size());

        return ResultBuilder("Set the selection of " + target->Name() + ".")
            .WithSession(session.Session())
            .WithData(std::move(data))
            .Build();
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
