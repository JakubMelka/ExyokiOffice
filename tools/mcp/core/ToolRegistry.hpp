// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Results.hpp"
#include "SchemaCheck.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

class ToolContext;

/** @brief MCP behavior hints published with every tool. */
struct ToolAnnotations
{
    /// The tool never modifies a document or the file system.
    bool ReadOnly = false;
    /// The tool can destroy content that no other call restores.
    bool Destructive = false;
    /// Repeating the call with the same arguments has no additional effect.
    bool Idempotent = false;
};

/// Signature of every tool handler.
using ToolHandler = std::function<ToolOutcome(ToolContext&, const nlohmann::json&)>;

/** @brief Everything one tool publishes and does. */
struct ToolDefinition
{
    /// `snake_case`, unique within the server.
    std::string Name;
    /// Short human-readable name ("Insert paragraph").
    std::string Title;
    /// One to three imperative sentences, including when to reach for the tool.
    std::string Description;
    /// Toolset the tool belongs to; also what `--toolsets` filters on.
    std::string Group;
    nlohmann::json InputSchema = nlohmann::json::object();
    nlohmann::json OutputSchema = nlohmann::json::object();
    /// Example argument object; the test suite validates it against InputSchema.
    nlohmann::json Example = nlohmann::json::object();
    ToolAnnotations Annotations;
    ToolHandler Handler;
};

/** @brief A registered tool with its compiled input schema. */
struct RegisteredTool
{
    ToolDefinition Definition;
    std::shared_ptr<SchemaCheck> InputCheck;
};

/**
 * @brief The catalog of tools one server offers.
 *
 * Registration applies the two catalog filters. `--read-only` drops every
 * mutating tool outright rather than failing it at call time, so the published
 * catalog is an honest description of what the server can do — and a smaller
 * one, which matters because the catalog is the largest fixed cost the server
 * imposes on a client's context window. `--toolsets` narrows it further by
 * group.
 */
class ToolRegistry
{
public:
    ToolRegistry(bool readOnly, std::vector<std::string> enabledGroups);

    /// Registers a tool unless a catalog filter excludes it.
    void Add(ToolDefinition definition);

    [[nodiscard]] const RegisteredTool* Find(std::string_view name) const;
    [[nodiscard]] const std::vector<RegisteredTool>& Tools() const noexcept { return m_tools; }

    /// Payload of `tools/list`, in the stable catalog order.
    [[nodiscard]] nlohmann::json ListJson() const;

    /// Payload of `--print-tools`: `tools/list` plus each tool's group and example.
    [[nodiscard]] nlohmann::json CatalogJson(const std::string& serverName, const std::string& serverTitle) const;

    /**
     * @brief Validates arguments and runs the handler.
     *
     * The tool must exist; an unknown name is a protocol error the caller
     * reports as JSON-RPC `-32602` rather than a tool failure.
     */
    [[nodiscard]] ToolOutcome Call(ToolContext& context, const RegisteredTool& tool,
                                   const nlohmann::json& arguments) const;

    [[nodiscard]] bool IsReadOnly() const noexcept { return m_readOnly; }

private:
    /// Catalog rank: lifecycle, then reads, then mutations, then file utilities.
    [[nodiscard]] static int OrderRank(const ToolDefinition& definition);

    void Sort();

    bool m_readOnly = false;
    std::vector<std::string> m_enabledGroups;
    std::vector<RegisteredTool> m_tools;
};

/**
 * @brief Builders for the draft-07 subset the tool schemas use.
 *
 * Schemas are written out in full at every tool rather than shared through
 * `$ref`: clients resolve references inconsistently, and a self-contained
 * schema is also what a model reads best.
 */
class Schema
{
public:
    static nlohmann::json Object(std::string description, std::vector<std::string> required,
                                 nlohmann::json properties);
    static nlohmann::json FreeObject(std::string description);
    static nlohmann::json String(std::string description);
    static nlohmann::json StringWithDefault(std::string description, std::string defaultValue);
    static nlohmann::json Enumeration(std::string description, std::vector<std::string> values);
    static nlohmann::json EnumerationWithDefault(std::string description, std::vector<std::string> values,
                                                 std::string defaultValue);
    static nlohmann::json Integer(std::string description, std::optional<Int64> minimum = std::nullopt,
                                  std::optional<Int64> maximum = std::nullopt);
    static nlohmann::json IntegerWithDefault(std::string description, Int64 defaultValue,
                                             std::optional<Int64> minimum = std::nullopt,
                                             std::optional<Int64> maximum = std::nullopt);
    static nlohmann::json Number(std::string description);
    static nlohmann::json Boolean(std::string description);
    static nlohmann::json BooleanWithDefault(std::string description, bool defaultValue);
    static nlohmann::json Array(std::string description, nlohmann::json items,
                                std::optional<Int64> maxItems = std::nullopt);
    /// A length: points as a number, or a string such as "2.5cm".
    static nlohmann::json Length(std::string description);
    /// Any JSON value, used where the payload is genuinely free-form.
    static nlohmann::json Any(std::string description);

    /**
     * @brief Wraps a `data` schema into the full result envelope schema.
     *
     * @param sessionFields Adds `documentId`, `revision`, and `dirty` for
     *        tools that operate on an open session.
     */
    static nlohmann::json Envelope(nlohmann::json dataSchema, bool sessionFields);
};

} // namespace ExyokiOffice::Mcp
