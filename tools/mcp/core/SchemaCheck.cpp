// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "SchemaCheck.hpp"

#include <nlohmann/json-schema.hpp>

#include <exception>
#include <utility>

namespace ExyokiOffice::Mcp
{

/// Collects every complaint instead of throwing on the first one.
class SchemaCheckErrorHandler final : public nlohmann::json_schema::basic_error_handler
{
public:
    explicit SchemaCheckErrorHandler(std::vector<SchemaViolation>& violations)
        : m_violations(&violations)
    {
    }

    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& instance,
               const std::string& message) override
    {
        nlohmann::json_schema::basic_error_handler::error(pointer, instance, message);

        auto location = pointer.to_string();
        if (location.empty())
        {
            location = "/";
        }

        m_violations->push_back(SchemaViolation{std::move(location), message});
    }

private:
    std::vector<SchemaViolation>* m_violations;
};

struct SchemaCheck::Impl
{
    nlohmann::json_schema::json_validator Validator;
    bool Ready = false;
};

SchemaCheck::SchemaCheck()
    : m_impl(std::make_unique<Impl>())
{
}

SchemaCheck::~SchemaCheck() = default;

SchemaCheck::SchemaCheck(SchemaCheck&&) noexcept = default;

SchemaCheck& SchemaCheck::operator=(SchemaCheck&&) noexcept = default;

bool SchemaCheck::SetSchema(const nlohmann::json& schema, std::string& error)
{
    try
    {
        m_impl->Validator.set_root_schema(schema);
        m_impl->Ready = true;
        return true;
    }
    catch (const std::exception& failure)
    {
        m_impl->Ready = false;
        error = failure.what();
        return false;
    }
}

bool SchemaCheck::IsValid() const noexcept
{
    return m_impl && m_impl->Ready;
}

bool SchemaCheck::Validate(const nlohmann::json& instance, std::vector<SchemaViolation>& violations) const
{
    if (!IsValid())
    {
        return true;
    }

    try
    {
        SchemaCheckErrorHandler handler(violations);
        m_impl->Validator.validate(instance, handler);
        return !static_cast<bool>(handler);
    }
    catch (const std::exception& failure)
    {
        violations.push_back(SchemaViolation{"/", failure.what()});
        return false;
    }
}

nlohmann::json ViolationsToDetails(const std::vector<SchemaViolation>& violations)
{
    nlohmann::json details = nlohmann::json::array();
    for (const auto& violation : violations)
    {
        nlohmann::json entry = nlohmann::json::object();
        entry["pointer"] = violation.Pointer;
        entry["message"] = violation.Message;
        details.push_back(std::move(entry));
    }

    return details;
}

} // namespace ExyokiOffice::Mcp
