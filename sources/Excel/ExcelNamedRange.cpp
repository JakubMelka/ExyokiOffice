// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <utility>

namespace ExyokiOffice::Excel
{

namespace NamedRangeDetail
{

namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

std::shared_ptr<Spreadsheet::Workbook> GetWorkbook(const ExcelDocument::Ptr& document)
{
    auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    return workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
}

std::shared_ptr<Spreadsheet::DefinedNames> GetDefinedNames(const ExcelDocument::Ptr& document, bool create)
{
    auto workbook = GetWorkbook(document);
    if (!workbook)
    {
        return nullptr;
    }
    auto definedNames = workbook->GetFirstChildOfType<Spreadsheet::DefinedNames>();
    if (definedNames || !create)
    {
        return definedNames;
    }
    // Schema-aware AppendChild places the collection at its schema position
    // (after the sheets, before calculation properties).
    return workbook->AppendChild<Spreadsheet::DefinedNames>();
}

/** Worksheet display names in workbook sheet-list order (localSheetId order). */
std::vector<std::string> SheetNames(const ExcelDocument::Ptr& document)
{
    std::vector<std::string> names;
    ExcelDocumentEditor editor(document);
    for (const auto& worksheet : editor.Worksheets())
    {
        names.push_back(worksheet ? worksheet->Name() : std::string());
    }
    return names;
}

std::optional<UInt32> SheetIndex(const std::vector<std::string>& sheetNames, std::string_view name)
{
    for (Size i = 0; i < sheetNames.size(); ++i)
    {
        if (AsciiText::EqualsIgnoreCase(sheetNames[i], name))
        {
            return static_cast<UInt32>(i);
        }
    }
    return std::nullopt;
}

NamedRange ToModel(const std::shared_ptr<Spreadsheet::DefinedName>& element,
                   const std::vector<std::string>& sheetNames)
{
    NamedRange model;
    model.Name = element->GetName().ToString();
    model.Formula = std::string(element->GetText());
    model.Hidden = element->GetHidden().ValueOr(false);
    model.Comment = element->GetComment().ToString();
    const auto localSheetId = element->GetLocalSheetId();
    if (localSheetId.IsDefined())
    {
        model.Scope = NamedRangeScope::Sheet;
        const auto index = localSheetId.Value();
        if (index < sheetNames.size())
        {
            model.ScopeSheet = sheetNames[index];
        }
    }
    return model;
}

/** True when the entry lives in the requested scope. */
bool MatchesScope(const std::shared_ptr<Spreadsheet::DefinedName>& element,
                  const std::vector<std::string>& sheetNames,
                  std::string_view scopeSheet)
{
    const auto localSheetId = element->GetLocalSheetId();
    if (scopeSheet.empty())
    {
        return !localSheetId.IsDefined();
    }
    if (!localSheetId.IsDefined())
    {
        return false;
    }
    const auto index = localSheetId.Value();
    return index < sheetNames.size() && AsciiText::EqualsIgnoreCase(sheetNames[index], scopeSheet);
}

std::shared_ptr<Spreadsheet::DefinedName> FindElement(const ExcelDocument::Ptr& document,
                                                      std::string_view name,
                                                      std::string_view scopeSheet,
                                                      const std::vector<std::string>& sheetNames)
{
    const auto definedNames = GetDefinedNames(document, false);
    if (!definedNames)
    {
        return nullptr;
    }
    for (const auto& element : definedNames->Elements<Spreadsheet::DefinedName>())
    {
        if (AsciiText::EqualsIgnoreCase(element->GetName().View(), name) &&
            MatchesScope(element, sheetNames, scopeSheet))
        {
            return element;
        }
    }
    return nullptr;
}

std::string StripEqualsPrefix(std::string_view formula)
{
    if (!formula.empty() && formula.front() == '=')
    {
        formula.remove_prefix(1);
    }
    return std::string(formula);
}

NamedRangeResult Failure(NamedRangeError error, std::string message)
{
    NamedRangeResult result;
    result.Error = error;
    result.Message = std::move(message);
    return result;
}

} // namespace NamedRangeDetail

std::optional<SheetCellRange> NamedRange::Range() const
{
    return SheetCellRange::Parse(Formula);
}

NamedRangeManager::NamedRangeManager(ExcelDocument::Ptr document) : m_document(std::move(document))
{
}

bool NamedRangeManager::IsValid() const noexcept
{
    return m_document != nullptr;
}

bool NamedRangeManager::IsValidName(std::string_view name)
{
    using namespace NamedRangeDetail;
    if (name.empty() || name.size() > 255)
    {
        return false;
    }
    const char first = name.front();
    const bool validFirst = AsciiText::IsAlpha(first) || AsciiText::IsNonAscii(first) ||
                            first == '_' || first == '\\';
    if (!validFirst)
    {
        return false;
    }
    for (const char c : name)
    {
        const bool valid = AsciiText::IsAlnum(c) || AsciiText::IsNonAscii(c) ||
                           c == '_' || c == '.' || c == '\\';
        if (!valid)
        {
            return false;
        }
    }
    // Single-letter C and R are reserved, and cell-reference spellings such
    // as A1 or R1C1 are forbidden.
    if (AsciiText::EqualsIgnoreCase(name, "C") || AsciiText::EqualsIgnoreCase(name, "R"))
    {
        return false;
    }
    if (CellAddress::ParseA1(name) || CellAddress::ParseR1C1(name))
    {
        return false;
    }
    return true;
}

NamedRangeResult NamedRangeManager::Create(std::string_view name,
                                           const SheetCellRange& range,
                                           NamedRangeScope scope,
                                           std::string_view scopeSheet)
{
    using namespace NamedRangeDetail;
    if (!range.Range().IsValid() || range.Sheet().empty())
    {
        return Failure(NamedRangeError::InvalidRange, "The target range is invalid.");
    }
    if (!IsValid())
    {
        return Failure(NamedRangeError::InvalidDocument, "The manager has no attached workbook document.");
    }
    const auto sheetNames = SheetNames(m_document);
    if (!SheetIndex(sheetNames, range.Sheet()))
    {
        return Failure(NamedRangeError::UnknownSheet,
                       "Worksheet '" + range.Sheet() + "' does not exist in the workbook.");
    }
    return CreateFromFormula(name, range.ToFormula(), scope, scopeSheet);
}

NamedRangeResult NamedRangeManager::CreateFromFormula(std::string_view name,
                                                      std::string_view formula,
                                                      NamedRangeScope scope,
                                                      std::string_view scopeSheet)
{
    using namespace NamedRangeDetail;
    if (!IsValid())
    {
        return Failure(NamedRangeError::InvalidDocument, "The manager has no attached workbook document.");
    }
    if (!IsValidName(name))
    {
        return Failure(NamedRangeError::InvalidName,
                       "'" + std::string(name) + "' is not a valid defined name.");
    }
    const std::string definition = StripEqualsPrefix(formula);
    if (definition.empty())
    {
        return Failure(NamedRangeError::InvalidRange, "The name definition must not be empty.");
    }

    const auto sheetNames = SheetNames(m_document);
    std::string_view scopeKey;
    std::optional<UInt32> localSheetId;
    if (scope == NamedRangeScope::Sheet)
    {
        localSheetId = SheetIndex(sheetNames, scopeSheet);
        if (!localSheetId)
        {
            return Failure(NamedRangeError::UnknownSheet,
                           "Worksheet '" + std::string(scopeSheet) + "' does not exist in the workbook.");
        }
        scopeKey = scopeSheet;
    }
    if (FindElement(m_document, name, scopeKey, sheetNames))
    {
        return Failure(NamedRangeError::DuplicateName,
                       "A defined name '" + std::string(name) + "' already exists in this scope.");
    }

    auto definedNames = GetDefinedNames(m_document, true);
    if (!definedNames)
    {
        return Failure(NamedRangeError::InvalidDocument, "The workbook has no workbook part.");
    }
    auto element = definedNames->AppendChild<Spreadsheet::DefinedName>();
    if (!element)
    {
        return Failure(NamedRangeError::InvalidDocument, "The defined name element could not be created.");
    }
    element->SetName(StringValue(std::string(name)));
    if (localSheetId)
    {
        element->SetLocalSheetId(UInt32Value(*localSheetId));
    }
    element->SetText(definition);
    return {};
}

std::optional<NamedRange> NamedRangeManager::Find(std::string_view name, std::string_view scopeSheet) const
{
    using namespace NamedRangeDetail;
    if (!IsValid())
    {
        return std::nullopt;
    }
    const auto sheetNames = SheetNames(m_document);
    const auto element = FindElement(m_document, name, scopeSheet, sheetNames);
    if (!element)
    {
        return std::nullopt;
    }
    return ToModel(element, sheetNames);
}

std::optional<NamedRange> NamedRangeManager::Resolve(std::string_view name, std::string_view sheetName) const
{
    if (!sheetName.empty())
    {
        if (auto sheetScoped = Find(name, sheetName))
        {
            return sheetScoped;
        }
    }
    return Find(name, {});
}

std::optional<SheetCellRange> NamedRangeManager::GetRange(std::string_view name,
                                                          std::string_view scopeSheet) const
{
    const auto model = Find(name, scopeSheet);
    return model ? model->Range() : std::nullopt;
}

std::vector<NamedRange> NamedRangeManager::List() const
{
    using namespace NamedRangeDetail;
    std::vector<NamedRange> result;
    if (!IsValid())
    {
        return result;
    }
    const auto definedNames = GetDefinedNames(m_document, false);
    if (!definedNames)
    {
        return result;
    }
    const auto sheetNames = SheetNames(m_document);
    for (const auto& element : definedNames->Elements<Spreadsheet::DefinedName>())
    {
        result.push_back(ToModel(element, sheetNames));
    }
    return result;
}

std::vector<NamedRange> NamedRangeManager::List(NamedRangeScope scope) const
{
    std::vector<NamedRange> result = List();
    result.erase(std::remove_if(result.begin(), result.end(),
                                [scope](const NamedRange& entry)
                                { return entry.Scope != scope; }),
                 result.end());
    return result;
}

Size NamedRangeManager::Count() const
{
    return List().size();
}

NamedRangeResult NamedRangeManager::SetRange(std::string_view name,
                                             const SheetCellRange& range,
                                             std::string_view scopeSheet)
{
    using namespace NamedRangeDetail;
    if (!range.Range().IsValid() || range.Sheet().empty())
    {
        return Failure(NamedRangeError::InvalidRange, "The target range is invalid.");
    }
    if (IsValid() && !SheetIndex(SheetNames(m_document), range.Sheet()))
    {
        return Failure(NamedRangeError::UnknownSheet,
                       "Worksheet '" + range.Sheet() + "' does not exist in the workbook.");
    }
    return SetFormula(name, range.ToFormula(), scopeSheet);
}

NamedRangeResult NamedRangeManager::SetFormula(std::string_view name,
                                               std::string_view formula,
                                               std::string_view scopeSheet)
{
    using namespace NamedRangeDetail;
    if (!IsValid())
    {
        return Failure(NamedRangeError::InvalidDocument, "The manager has no attached workbook document.");
    }
    const std::string definition = StripEqualsPrefix(formula);
    if (definition.empty())
    {
        return Failure(NamedRangeError::InvalidRange, "The name definition must not be empty.");
    }
    const auto sheetNames = SheetNames(m_document);
    const auto element = FindElement(m_document, name, scopeSheet, sheetNames);
    if (!element)
    {
        return Failure(NamedRangeError::NameNotFound,
                       "No defined name '" + std::string(name) + "' exists in this scope.");
    }
    element->SetText(definition);
    return {};
}

NamedRangeResult NamedRangeManager::Rename(std::string_view name,
                                           std::string_view newName,
                                           std::string_view scopeSheet)
{
    using namespace NamedRangeDetail;
    if (!IsValid())
    {
        return Failure(NamedRangeError::InvalidDocument, "The manager has no attached workbook document.");
    }
    if (!IsValidName(newName))
    {
        return Failure(NamedRangeError::InvalidName,
                       "'" + std::string(newName) + "' is not a valid defined name.");
    }
    const auto sheetNames = SheetNames(m_document);
    const auto element = FindElement(m_document, name, scopeSheet, sheetNames);
    if (!element)
    {
        return Failure(NamedRangeError::NameNotFound,
                       "No defined name '" + std::string(name) + "' exists in this scope.");
    }
    if (!AsciiText::EqualsIgnoreCase(name, newName) && FindElement(m_document, newName, scopeSheet, sheetNames))
    {
        return Failure(NamedRangeError::DuplicateName,
                       "A defined name '" + std::string(newName) + "' already exists in this scope.");
    }
    element->SetName(StringValue(std::string(newName)));
    return {};
}

NamedRangeResult NamedRangeManager::Remove(std::string_view name, std::string_view scopeSheet)
{
    using namespace NamedRangeDetail;
    if (!IsValid())
    {
        return Failure(NamedRangeError::InvalidDocument, "The manager has no attached workbook document.");
    }
    const auto sheetNames = SheetNames(m_document);
    const auto element = FindElement(m_document, name, scopeSheet, sheetNames);
    if (!element)
    {
        return Failure(NamedRangeError::NameNotFound,
                       "No defined name '" + std::string(name) + "' exists in this scope.");
    }
    auto definedNames = GetDefinedNames(m_document, false);
    if (!definedNames || !definedNames->RemoveChild(element))
    {
        return Failure(NamedRangeError::InvalidDocument, "The defined name could not be removed.");
    }
    // Drop the empty collection element to keep the workbook markup tidy.
    if (definedNames->Elements<Spreadsheet::DefinedName>().empty())
    {
        if (auto workbook = GetWorkbook(m_document))
        {
            workbook->RemoveChild(definedNames);
        }
    }
    return {};
}

} // namespace ExyokiOffice::Excel
