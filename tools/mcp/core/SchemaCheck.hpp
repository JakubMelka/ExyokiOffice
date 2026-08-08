// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace ExyokiOffice::Mcp
{

/** @brief One JSON Schema violation, located by JSON pointer. */
struct SchemaViolation
{
    /// JSON pointer of the offending value, "/" for the document root.
    std::string Pointer;
    std::string Message;
};

/**
 * @brief Compiled input schema of one tool.
 *
 * Every `tools/call` argument object is checked before the handler runs, so a
 * handler never has to defend against a missing member or a wrong type. The
 * schema is compiled once at registration; validating a call is then a plain
 * tree walk.
 */
class SchemaCheck
{
public:
    SchemaCheck();
    ~SchemaCheck();

    SchemaCheck(const SchemaCheck&) = delete;
    SchemaCheck& operator=(const SchemaCheck&) = delete;
    SchemaCheck(SchemaCheck&&) noexcept;
    SchemaCheck& operator=(SchemaCheck&&) noexcept;

    /**
     * @brief Compiles a schema.
     *
     * @return False when the schema itself is malformed, which is a
     *         programming error in a tool descriptor rather than bad input.
     */
    bool SetSchema(const nlohmann::json& schema, std::string& error);

    /// True when a schema was compiled successfully.
    [[nodiscard]] bool IsValid() const noexcept;

    /// Validates one instance, collecting every violation rather than the first.
    [[nodiscard]] bool Validate(const nlohmann::json& instance, std::vector<SchemaViolation>& violations) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Renders schema violations as the `details` array of an `input_invalid` error.
[[nodiscard]] nlohmann::json ViolationsToDetails(const std::vector<SchemaViolation>& violations);

} // namespace ExyokiOffice::Mcp
