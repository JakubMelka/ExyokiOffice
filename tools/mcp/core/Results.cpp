// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Results.hpp"

#include "SessionStore.hpp"

#include <utility>

namespace ExyokiOffice::Mcp
{

std::string_view ToString(ErrorCode code) noexcept
{
    switch (code)
    {
        case ErrorCode::InputInvalid:
            return "input_invalid";
        case ErrorCode::PathOutsideWorkspace:
            return "path_outside_workspace";
        case ErrorCode::PathInvalid:
            return "path_invalid";
        case ErrorCode::FileNotFound:
            return "file_not_found";
        case ErrorCode::FileExists:
            return "file_exists";
        case ErrorCode::FileChangedOnDisk:
            return "file_changed_on_disk";
        case ErrorCode::PackageLoadFailed:
            return "package_load_failed";
        case ErrorCode::DocumentNotFound:
            return "document_not_found";
        case ErrorCode::DocumentLimitReached:
            return "document_limit_reached";
        case ErrorCode::ReadOnlyMode:
            return "read_only_mode";
        case ErrorCode::FamilyMismatch:
            return "family_mismatch";
        case ErrorCode::AnchorInvalid:
            return "anchor_invalid";
        case ErrorCode::BlockNotFound:
            return "block_not_found";
        case ErrorCode::SheetNotFound:
            return "sheet_not_found";
        case ErrorCode::RangeInvalid:
            return "range_invalid";
        case ErrorCode::SlideNotFound:
            return "slide_not_found";
        case ErrorCode::ShapeNotFound:
            return "shape_not_found";
        case ErrorCode::LayoutNotFound:
            return "layout_not_found";
        case ErrorCode::StyleNotFound:
            return "style_not_found";
        case ErrorCode::MediaNotFound:
            return "media_not_found";
        case ErrorCode::CommentNotFound:
            return "comment_not_found";
        case ErrorCode::TemplateFieldMissing:
            return "template_field_missing";
        case ErrorCode::ValidationFailed:
            return "validation_failed";
        case ErrorCode::OperationFailed:
            return "operation_failed";
        case ErrorCode::BatchAborted:
            return "batch_aborted";
        case ErrorCode::SnapshotUnavailable:
            return "snapshot_unavailable";
        case ErrorCode::Unsupported:
            return "unsupported";
        case ErrorCode::InternalError:
            return "internal_error";
    }

    return "internal_error";
}

ResultBuilder::ResultBuilder(std::string summary)
    : m_summary(std::move(summary))
{
}

ResultBuilder& ResultBuilder::WithSession(const DocumentSession& session)
{
    m_documentId = session.Id();
    m_revision = session.Revision();
    m_dirty = session.IsDirty();
    m_hasSession = true;

    if (session.RollbackFailed())
    {
        // A document whose rollback failed may hold changes from an edit that
        // was reported as failed. Nothing else the agent can observe reveals
        // that, so the warning repeats on every result until the session is
        // closed.
        WithWarning("rollback_failed",
                    "An earlier failed edit could not be rolled back, so this document may contain partial "
                    "changes. Call undo, or close_document with discard and reopen the file.",
                    session.Id());
    }

    return *this;
}

ResultBuilder& ResultBuilder::WithData(nlohmann::json data)
{
    m_data = std::move(data);
    return *this;
}

ResultBuilder& ResultBuilder::WithDiagnostics(const std::vector<Tools::ToolDiagnostic>& diagnostics)
{
    for (auto& warning : DiagnosticsToWarnings(diagnostics))
    {
        m_warnings.push_back(std::move(warning));
    }

    return *this;
}

ResultBuilder& ResultBuilder::WithWarning(std::string code, std::string message, std::string context)
{
    nlohmann::json warning = nlohmann::json::object();
    warning["code"] = std::move(code);
    warning["message"] = std::move(message);
    warning["context"] = std::move(context);
    m_warnings.push_back(std::move(warning));
    return *this;
}

ResultBuilder& ResultBuilder::WithTruncated(bool truncated)
{
    m_truncated = truncated;
    return *this;
}

ToolOutcome ResultBuilder::Build() const
{
    ToolOutcome outcome;
    outcome.IsError = false;

    nlohmann::json envelope = nlohmann::json::object();
    envelope["ok"] = true;
    if (m_hasSession)
    {
        envelope["documentId"] = m_documentId;
        envelope["revision"] = m_revision;
        envelope["dirty"] = m_dirty;
    }

    envelope["summary"] = m_summary;
    envelope["data"] = m_data;
    envelope["warnings"] = m_warnings;
    envelope["truncated"] = m_truncated;

    outcome.Structured = std::move(envelope);
    return outcome;
}

ToolOutcome MakeError(ErrorCode code, std::string message, std::string target, std::string hint,
                      nlohmann::json details)
{
    nlohmann::json error = nlohmann::json::object();
    error["code"] = std::string(ToString(code));
    error["message"] = std::move(message);
    error["target"] = std::move(target);
    error["hint"] = std::move(hint);
    error["details"] = details.is_null() ? nlohmann::json::array() : std::move(details);

    ToolOutcome outcome;
    outcome.IsError = true;
    outcome.Structured = nlohmann::json::object();
    outcome.Structured["ok"] = false;
    outcome.Structured["error"] = std::move(error);
    return outcome;
}

ToolOutcome MakeErrorFromDiagnostics(ErrorCode code, std::string fallbackMessage,
                                     const std::vector<Tools::ToolDiagnostic>& diagnostics, std::string hint)
{
    std::string message = std::move(fallbackMessage);
    std::string context;
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.Severity == Tools::ToolSeverity::Error)
        {
            message = diagnostic.Message;
            context = diagnostic.Context;
            break;
        }
    }

    nlohmann::json details = DiagnosticsToWarnings(diagnostics);
    return MakeError(code, std::move(message), std::move(context), std::move(hint), std::move(details));
}

nlohmann::json DiagnosticsToWarnings(const std::vector<Tools::ToolDiagnostic>& diagnostics)
{
    nlohmann::json warnings = nlohmann::json::array();
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.Severity == Tools::ToolSeverity::Error)
        {
            continue;
        }

        nlohmann::json warning = nlohmann::json::object();
        warning["code"] = diagnostic.Severity == Tools::ToolSeverity::Warning ? "content_warning" : "content_info";
        warning["message"] = diagnostic.Message;
        warning["context"] = diagnostic.Context;
        warnings.push_back(std::move(warning));
    }

    return warnings;
}

std::string SerializeJson(const nlohmann::json& value, int indent)
{
    return value.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool TruncateArrayToBudget(nlohmann::json& items, Size budgetBytes)
{
    if (!items.is_array())
    {
        return false;
    }

    // Each element is measured once. Re-serializing the whole array after every
    // dropped element made trimming a large payload quadratic, and the arrays
    // that need trimming are precisely the large ones.
    constexpr Size BracketBytes = 2;
    Size total = BracketBytes;
    Size kept = 0;
    for (const auto& item : items)
    {
        const Size separatorBytes = kept == 0 ? 0u : 1u;
        const Size itemBytes = SerializeJson(item).size() + separatorBytes;
        if (total + itemBytes > budgetBytes)
        {
            break;
        }

        total += itemBytes;
        ++kept;
    }

    if (kept == items.size())
    {
        return false;
    }

    items.erase(items.begin() + static_cast<nlohmann::json::difference_type>(kept), items.end());
    return true;
}

bool TruncateTextToBudget(std::string& text, Size budgetBytes)
{
    if (text.size() <= budgetBytes)
    {
        return false;
    }

    text.resize(budgetBytes);

    // Never cut in the middle of a UTF-8 sequence: the truncated tail would be
    // invalid JSON string content once the envelope is serialized.
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0u) == 0x80u)
    {
        text.pop_back();
    }

    if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80u) != 0u)
    {
        text.pop_back();
    }

    return true;
}

} // namespace ExyokiOffice::Mcp
