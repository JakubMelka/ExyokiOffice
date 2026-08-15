// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Discriminates the value currently held by a ReportNode.
enum class ReportNodeKind
{
    Null,
    Bool,
    Int,
    UInt,
    Double,
    String,
    Array,
    Object
};

/**
 * @brief Generic, format-agnostic tree node used to describe command output.
 *
 * Every CLI command builds one ReportNode tree (usually an Object). The same
 * tree is rendered by RenderPlain/RenderMarkdown/RenderJson/RenderXml, so all
 * four `--format` outputs stay in lockstep with a single conversion per
 * command.
 *
 * Arrays of objects may carry a "table hint": an ordered list of member names
 * that plain/markdown rendering uses to lay the array out as a table instead
 * of a nested bullet list.
 */
class EXYOKIOFFICE_EXPORT ReportNode
{
public:
    using Members = std::vector<std::pair<std::string, ReportNode>>;
    using Items = std::vector<ReportNode>;

    ReportNode();
    ReportNode(std::nullptr_t) noexcept;
    ReportNode(bool value) noexcept;
    ReportNode(int value) noexcept;
    ReportNode(Int64 value) noexcept;
    ReportNode(UInt64 value) noexcept;
    ReportNode(Real value) noexcept;
    ReportNode(std::string value);
    ReportNode(const char* value);
    ReportNode(std::string_view value);

    static ReportNode MakeArray();
    static ReportNode MakeObject();

    ReportNodeKind Kind() const noexcept { return m_kind; }
    [[nodiscard]] bool IsNull() const noexcept { return m_kind == ReportNodeKind::Null; }

    [[nodiscard]] bool AsBool() const noexcept { return m_bool; }
    Int64 AsInt() const noexcept { return m_int; }
    UInt64 AsUInt() const noexcept { return m_uint; }
    Real AsDouble() const noexcept { return m_double; }
    const std::string& AsString() const noexcept { return m_string; }
    const Items& AsArray() const noexcept { return m_items; }
    const Members& AsObject() const noexcept { return m_members; }

    /// Appends a value to an array node (the node must be Array or Null; Null becomes Array).
    ReportNode& Push(ReportNode value);
    /// Sets (or replaces) a member on an object node (the node must be Object or Null; Null becomes Object).
    ReportNode& Set(std::string key, ReportNode value);

    /// Marks this array-of-objects node with the ordered column names used for table rendering.
    ReportNode& SetTableHint(std::vector<std::string> columns);
    const std::vector<std::string>& TableHint() const noexcept { return m_tableHint; }
    [[nodiscard]] bool HasTableHint() const noexcept { return !m_tableHint.empty(); }

private:
    ReportNodeKind m_kind = ReportNodeKind::Null;
    bool m_bool = false;
    Int64 m_int = 0;
    UInt64 m_uint = 0;
    Real m_double = 0.0;
    std::string m_string;
    Items m_items;
    Members m_members;
    std::vector<std::string> m_tableHint;
};

/**
 * @brief Top-level envelope every exyoki command produces, one per invocation.
 */
struct EXYOKIOFFICE_EXPORT ReportDocument
{
    std::string Command;
    /// "ok" or "error".
    std::string Status = "ok";
    ReportNode Data = ReportNode::MakeObject();
    std::vector<ToolDiagnostic> Diagnostics;
};

/// Renders a report as human-oriented indented plain text.
EXYOKIOFFICE_EXPORT std::string RenderPlain(const ReportDocument& document);
/// Renders a report as GitHub-flavored Markdown (headings, bullet lists, tables).
EXYOKIOFFICE_EXPORT std::string RenderMarkdown(const ReportDocument& document);
/// Renders a report as an indented JSON exyoki envelope.
EXYOKIOFFICE_EXPORT std::string RenderJson(const ReportDocument& document);
/// Renders a report as escaped XML with a root `<exyoki>` element.
EXYOKIOFFICE_EXPORT std::string RenderXml(const ReportDocument& document);

} // namespace ExyokiOffice::Tools
