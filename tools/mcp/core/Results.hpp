// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/ValidationResult.hpp"

#include <cstddef>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

class DocumentSession;

/**
 * @brief Machine-readable failure codes carried by the error envelope.
 *
 * The list is closed on purpose: an agent branches on these strings, so a new
 * one is a deliberate interface change rather than an ad-hoc string literal at
 * the point of failure. See docs/tools/mcp-servers.md for what each means.
 */
enum class ErrorCode
{
    InputInvalid,
    PathOutsideWorkspace,
    /// The path has a shape a client may not name; see Workspace::IsRefusedForm.
    PathInvalid,
    FileNotFound,
    FileExists,
    FileChangedOnDisk,
    PackageLoadFailed,
    DocumentNotFound,
    DocumentLimitReached,
    ReadOnlyMode,
    FamilyMismatch,
    AnchorInvalid,
    BlockNotFound,
    SheetNotFound,
    RangeInvalid,
    SlideNotFound,
    ShapeNotFound,
    LayoutNotFound,
    StyleNotFound,
    MediaNotFound,
    CommentNotFound,
    TemplateFieldMissing,
    ValidationFailed,
    OperationFailed,
    BatchAborted,
    SnapshotUnavailable,
    Unsupported,
    InternalError
};

/// Renders an error code as the `snake_case` token written into the envelope.
[[nodiscard]] std::string_view ToString(ErrorCode code) noexcept;

/**
 * @brief What one `tools/call` handler produced.
 *
 * `Structured` is the envelope published as `structuredContent`; the server
 * also serializes it into a text content block for clients that predate
 * structured output. `ExtraContent` holds additional content blocks (only
 * `get_media` uses it, for the image payload).
 */
struct ToolOutcome
{
    bool IsError = false;
    nlohmann::json Structured = nlohmann::json::object();
    nlohmann::json ExtraContent = nlohmann::json::array();
};

/// Maximum serialized size of one response payload before truncation kicks in.
inline constexpr Size ResponseBudgetBytes = static_cast<Size>(1024u) * 1024u;

/**
 * @brief Builds the success envelope described in the MCP tool contract.
 *
 * Every successful tool answers with the same shape — `ok`, an optional
 * session block, a human `summary`, the tool-specific `data`, `warnings`
 * lifted from ExyokiOffice::Tools diagnostics, and a `truncated` flag. The
 * uniformity is the point: an agent parses one envelope, not one shape per
 * tool.
 */
class ResultBuilder
{
public:
    explicit ResultBuilder(std::string summary);

    /// Adds `documentId`, `revision`, and `dirty` for a session-scoped tool.
    ResultBuilder& WithSession(const DocumentSession& session);
    ResultBuilder& WithData(nlohmann::json data);
    /// Appends Info/Warning diagnostics as warnings; Error diagnostics are skipped.
    ResultBuilder& WithDiagnostics(const std::vector<Tools::ToolDiagnostic>& diagnostics);
    ResultBuilder& WithWarning(std::string code, std::string message, std::string context = {});
    ResultBuilder& WithTruncated(bool truncated);

    [[nodiscard]] ToolOutcome Build() const;

private:
    std::string m_summary;
    nlohmann::json m_data = nlohmann::json::object();
    nlohmann::json m_warnings = nlohmann::json::array();
    std::string m_documentId;
    UInt64 m_revision = 0;
    bool m_dirty = false;
    bool m_hasSession = false;
    bool m_truncated = false;
};

/**
 * @brief Builds the failure envelope for a tool-level error.
 *
 * @param hint Names the tool that resolves the situation, so an agent can
 *        repair itself instead of guessing; supply one wherever it exists.
 */
[[nodiscard]] ToolOutcome MakeError(ErrorCode code, std::string message, std::string target = {},
                                    std::string hint = {}, nlohmann::json details = nlohmann::json::array());

/// Wraps the first Error diagnostic of a Tools result into an error envelope.
[[nodiscard]] ToolOutcome MakeErrorFromDiagnostics(ErrorCode code, std::string fallbackMessage,
                                                   const std::vector<Tools::ToolDiagnostic>& diagnostics,
                                                   std::string hint = {});

/// Converts Info/Warning diagnostics into the envelope's `warnings` array.
[[nodiscard]] nlohmann::json DiagnosticsToWarnings(const std::vector<Tools::ToolDiagnostic>& diagnostics);

/**
 * @brief Serializes JSON for the wire, replacing invalid UTF-8 with U+FFFD.
 *
 * Document text reaches the envelope byte for byte and a document may carry a
 * malformed sequence. The default dump() throws on those, which on the
 * response path would end the process instead of degrading one string, so
 * every serialization the server performs goes through here.
 */
[[nodiscard]] std::string SerializeJson(const nlohmann::json& value, int indent = -1);

/**
 * @brief Truncates an array to the response budget.
 *
 * Removes trailing items until the serialized array fits into
 * @p budgetBytes and reports whether anything was dropped. Callers pair this
 * with `nextOffset` so the agent can page through the remainder.
 */
bool TruncateArrayToBudget(nlohmann::json& items, Size budgetBytes = ResponseBudgetBytes);

/// Truncates a text payload to the response budget, returning true when it was cut.
bool TruncateTextToBudget(std::string& text, Size budgetBytes = ResponseBudgetBytes);

} // namespace ExyokiOffice::Mcp
