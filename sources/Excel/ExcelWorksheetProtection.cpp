// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <vector>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class WorksheetProtectionHelpers final
{
public:
    WorksheetProtectionHelpers() = delete;

    static Spreadsheet::SheetProtection::Ptr Find(const Spreadsheet::Worksheet::Ptr& root)
    {
        return root ? root->GetFirstChildOfType<Spreadsheet::SheetProtection>() : nullptr;
    }

    static UInt16 LegacyPasswordHash(std::string_view password)
    {
        UInt16 hash = 0;
        for (Size index = 0; index < password.size(); ++index)
        {
            const auto character = static_cast<UInt8>(password[index]);
            const auto shifted = static_cast<UInt32>(character) << (index + 1);
            const auto rotated = shifted >> 15;
            hash ^= static_cast<UInt16>((shifted & 0x7FFFU) | rotated);
        }
        hash ^= static_cast<UInt16>(password.size());
        hash ^= 0xCE4BU;
        return hash;
    }

    static HexBinaryValue PasswordVerifier(std::string_view password)
    {
        const auto hash = LegacyPasswordHash(password);
        return HexBinaryValue(std::vector<Byte>{
            static_cast<UInt8>(hash >> 8),
            static_cast<UInt8>(hash & 0xFFU)});
    }

    static void WriteOptions(const Spreadsheet::SheetProtection::Ptr& node,
                             const SheetProtectionOptions& options)
    {
        node->SetSheet(BooleanValue(true));
        node->SetObjects(BooleanValue(options.ProtectObjects));
        node->SetScenarios(BooleanValue(options.ProtectScenarios));
        node->SetFormatCells(BooleanValue(!options.AllowFormatCells));
        node->SetFormatColumns(BooleanValue(!options.AllowFormatColumns));
        node->SetFormatRows(BooleanValue(!options.AllowFormatRows));
        node->SetInsertColumns(BooleanValue(!options.AllowInsertColumns));
        node->SetInsertRows(BooleanValue(!options.AllowInsertRows));
        node->SetInsertHyperlinks(BooleanValue(!options.AllowInsertHyperlinks));
        node->SetDeleteColumns(BooleanValue(!options.AllowDeleteColumns));
        node->SetDeleteRows(BooleanValue(!options.AllowDeleteRows));
        node->SetSelectLockedCells(BooleanValue(!options.AllowSelectLockedCells));
        node->SetSort(BooleanValue(!options.AllowSort));
        node->SetAutoFilter(BooleanValue(!options.AllowAutoFilter));
        node->SetPivotTables(BooleanValue(!options.AllowPivotTables));
        node->SetSelectUnlockedCells(BooleanValue(!options.AllowSelectUnlockedCells));
    }

    static SheetProtectionOptions ReadOptions(const Spreadsheet::SheetProtection::Ptr& node)
    {
        SheetProtectionOptions options;
        options.ProtectObjects = node->GetObjects().ValueOr(false);
        options.ProtectScenarios = node->GetScenarios().ValueOr(false);
        options.AllowFormatCells = !node->GetFormatCells().ValueOr(false);
        options.AllowFormatColumns = !node->GetFormatColumns().ValueOr(false);
        options.AllowFormatRows = !node->GetFormatRows().ValueOr(false);
        options.AllowInsertColumns = !node->GetInsertColumns().ValueOr(false);
        options.AllowInsertRows = !node->GetInsertRows().ValueOr(false);
        options.AllowInsertHyperlinks = !node->GetInsertHyperlinks().ValueOr(false);
        options.AllowDeleteColumns = !node->GetDeleteColumns().ValueOr(false);
        options.AllowDeleteRows = !node->GetDeleteRows().ValueOr(false);
        options.AllowSelectLockedCells = !node->GetSelectLockedCells().ValueOr(false);
        options.AllowSort = !node->GetSort().ValueOr(false);
        options.AllowAutoFilter = !node->GetAutoFilter().ValueOr(false);
        options.AllowPivotTables = !node->GetPivotTables().ValueOr(false);
        options.AllowSelectUnlockedCells = !node->GetSelectUnlockedCells().ValueOr(false);
        return options;
    }
};

std::optional<SheetProtectionInfo> Worksheet::GetProtection() const
{
    const auto node = WorksheetProtectionHelpers::Find(GetLowLevelApi());
    if (!node || !node->GetSheet().ValueOr(false))
    {
        return std::nullopt;
    }

    SheetProtectionInfo result;
    result.Options = WorksheetProtectionHelpers::ReadOptions(node);
    result.HasPassword = node->GetPassword().IsDefined() || node->GetHashValue().IsDefined();
    return result;
}

SheetProtectionResult Worksheet::Protect(const SheetProtectionOptions& options, std::string_view password)
{
    if (password.size() > 15)
    {
        return {SheetProtectionError::InvalidPassword,
                "Legacy Excel worksheet passwords must contain at most 15 bytes."};
    }

    const auto root = GetLowLevelApi();
    const auto part = GetPart();
    if (!root || !part)
    {
        return {SheetProtectionError::InvalidWorksheet, "The worksheet is detached."};
    }

    const auto originalXml = part->GetXmlString();
    if (const auto old = WorksheetProtectionHelpers::Find(root))
    {
        root->RemoveChild(old);
    }

    const auto protection = root->AppendChild<Spreadsheet::SheetProtection>();
    if (!protection)
    {
        part->SetXmlString(originalXml);
        return {SheetProtectionError::WriteFailed, "Worksheet protection could not be created."};
    }

    WorksheetProtectionHelpers::WriteOptions(protection, options);
    if (!password.empty())
    {
        protection->SetPassword(WorksheetProtectionHelpers::PasswordVerifier(password));
    }
    return {};
}

SheetProtectionResult Worksheet::Unprotect(std::string_view password)
{
    if (password.size() > 15)
    {
        return {SheetProtectionError::InvalidPassword,
                "Legacy Excel worksheet passwords must contain at most 15 bytes."};
    }

    const auto root = GetLowLevelApi();
    if (!root)
    {
        return {SheetProtectionError::InvalidWorksheet, "The worksheet is detached."};
    }

    const auto protection = WorksheetProtectionHelpers::Find(root);
    if (!protection || !protection->GetSheet().ValueOr(false))
    {
        return {};
    }

    if (protection->GetHashValue().IsDefined())
    {
        return {SheetProtectionError::PasswordMismatch,
                "This worksheet uses a modern password verifier that this API cannot validate."};
    }

    const auto storedPassword = protection->GetPassword();
    if (storedPassword.IsDefined() &&
        storedPassword.ToString() != WorksheetProtectionHelpers::PasswordVerifier(password).ToString())
    {
        return {SheetProtectionError::PasswordMismatch, "The worksheet protection password is incorrect."};
    }
    if (!storedPassword.IsDefined() && !password.empty())
    {
        return {SheetProtectionError::PasswordMismatch,
                "The worksheet is not password protected; pass an empty password to remove protection."};
    }

    if (!root->RemoveChild(protection))
    {
        return {SheetProtectionError::WriteFailed, "Worksheet protection could not be removed."};
    }
    return {};
}

class WorkbookProtectionHelpers final
{
public:
    WorkbookProtectionHelpers() = delete;

    static Spreadsheet::Workbook::Ptr Root(const ExcelDocument::Ptr& document)
    {
        const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        return workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
    }

    static Spreadsheet::WorkbookProtection::Ptr Find(const Spreadsheet::Workbook::Ptr& root)
    {
        return root ? root->GetFirstChildOfType<Spreadsheet::WorkbookProtection>() : nullptr;
    }

    static bool IsActive(const Spreadsheet::WorkbookProtection::Ptr& node)
    {
        return node && (node->GetLockStructure().ValueOr(false) || node->GetLockWindows().ValueOr(false));
    }
};

std::optional<WorkbookProtectionInfo> ExcelDocumentEditor::GetWorkbookProtection() const
{
    const auto node = WorkbookProtectionHelpers::Find(WorkbookProtectionHelpers::Root(m_document));
    if (!WorkbookProtectionHelpers::IsActive(node))
    {
        return std::nullopt;
    }

    WorkbookProtectionInfo result;
    result.Options.LockStructure = node->GetLockStructure().ValueOr(false);
    result.Options.LockWindows = node->GetLockWindows().ValueOr(false);
    result.HasPassword = node->GetWorkbookPassword().IsDefined() || node->GetWorkbookHashValue().IsDefined();
    return result;
}

WorkbookProtectionResult ExcelDocumentEditor::ProtectWorkbook(const WorkbookProtectionOptions& options,
                                                              std::string_view password)
{
    if (password.size() > 15)
    {
        return {WorkbookProtectionError::InvalidPassword,
                "Legacy Excel workbook passwords must contain at most 15 bytes."};
    }
    if (!options.LockStructure && !options.LockWindows)
    {
        return {WorkbookProtectionError::InvalidWorkbook,
                "Workbook protection requires at least one structural lock to be enabled."};
    }

    const auto root = WorkbookProtectionHelpers::Root(m_document);
    const auto part = m_document ? m_document->GetWorkbookPart() : nullptr;
    if (!root || !part)
    {
        return {WorkbookProtectionError::InvalidWorkbook, "The editor has no attached workbook part."};
    }

    const auto originalXml = part->GetXmlString();
    if (const auto old = WorkbookProtectionHelpers::Find(root))
    {
        root->RemoveChild(old);
    }

    const auto protection = root->AppendChild<Spreadsheet::WorkbookProtection>();
    if (!protection)
    {
        part->SetXmlString(originalXml);
        return {WorkbookProtectionError::WriteFailed, "Workbook protection could not be created."};
    }

    protection->SetLockStructure(BooleanValue(options.LockStructure));
    protection->SetLockWindows(BooleanValue(options.LockWindows));
    if (!password.empty())
    {
        protection->SetWorkbookPassword(WorksheetProtectionHelpers::PasswordVerifier(password));
    }
    return {};
}

WorkbookProtectionResult ExcelDocumentEditor::UnprotectWorkbook(std::string_view password)
{
    if (password.size() > 15)
    {
        return {WorkbookProtectionError::InvalidPassword,
                "Legacy Excel workbook passwords must contain at most 15 bytes."};
    }

    const auto root = WorkbookProtectionHelpers::Root(m_document);
    if (!root)
    {
        return {WorkbookProtectionError::InvalidWorkbook, "The editor has no attached workbook part."};
    }

    const auto protection = WorkbookProtectionHelpers::Find(root);
    if (!WorkbookProtectionHelpers::IsActive(protection))
    {
        return {};
    }

    if (protection->GetWorkbookHashValue().IsDefined())
    {
        return {WorkbookProtectionError::PasswordMismatch,
                "This workbook uses a modern password verifier that this API cannot validate."};
    }

    const auto storedPassword = protection->GetWorkbookPassword();
    if (storedPassword.IsDefined() &&
        storedPassword.ToString() != WorksheetProtectionHelpers::PasswordVerifier(password).ToString())
    {
        return {WorkbookProtectionError::PasswordMismatch, "The workbook protection password is incorrect."};
    }
    if (!storedPassword.IsDefined() && !password.empty())
    {
        return {WorkbookProtectionError::PasswordMismatch,
                "The workbook is not password protected; pass an empty password to remove protection."};
    }

    if (!root->RemoveChild(protection))
    {
        return {WorkbookProtectionError::WriteFailed, "Workbook protection could not be removed."};
    }
    return {};
}

} // namespace ExyokiOffice::Excel
