// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ToolRegistry.hpp"

#include "ToolContext.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace ExyokiOffice::Mcp
{

ToolRegistry::ToolRegistry(bool readOnly, std::vector<std::string> enabledGroups)
    : m_readOnly(readOnly), m_enabledGroups(std::move(enabledGroups))
{
}

void ToolRegistry::Add(ToolDefinition definition)
{
    if (m_readOnly && !definition.Annotations.ReadOnly)
    {
        return;
    }

    if (!m_enabledGroups.empty() &&
        std::find(m_enabledGroups.begin(), m_enabledGroups.end(), definition.Group) == m_enabledGroups.end())
    {
        return;
    }

    RegisteredTool tool;
    tool.InputCheck = std::make_shared<SchemaCheck>();

    std::string error;
    if (!tool.InputCheck->SetSchema(definition.InputSchema, error))
    {
        // A malformed schema is a defect in the descriptor, not in the client's
        // input. Registering the tool without a check would silently disable
        // validation for it, so the tool is dropped and the reason logged.
        Log::Error("Tool '" + definition.Name + "' has an invalid input schema and was not registered: " + error);
        return;
    }

    tool.Definition = std::move(definition);
    m_tools.push_back(std::move(tool));
    Sort();
}

const RegisteredTool* ToolRegistry::Find(std::string_view name) const
{
    const auto match = std::find_if(m_tools.begin(), m_tools.end(),
                                    [name](const RegisteredTool& tool)
                                    { return tool.Definition.Name == name; });
    return match == m_tools.end() ? nullptr : &*match;
}

nlohmann::json ToolRegistry::ListJson() const
{
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : m_tools)
    {
        nlohmann::json annotations = nlohmann::json::object();
        annotations["readOnlyHint"] = tool.Definition.Annotations.ReadOnly;
        annotations["destructiveHint"] = tool.Definition.Annotations.Destructive;
        annotations["idempotentHint"] = tool.Definition.Annotations.Idempotent;
        // The servers only ever touch the configured workspace and the open
        // sessions; nothing reaches the network or any other outside state.
        annotations["openWorldHint"] = false;

        nlohmann::json entry = nlohmann::json::object();
        entry["name"] = tool.Definition.Name;
        entry["title"] = tool.Definition.Title;
        entry["description"] = tool.Definition.Description;
        entry["inputSchema"] = tool.Definition.InputSchema;
        entry["outputSchema"] = tool.Definition.OutputSchema;
        entry["annotations"] = std::move(annotations);
        tools.push_back(std::move(entry));
    }

    return tools;
}

nlohmann::json ToolRegistry::CatalogJson(const std::string& serverName, const std::string& serverTitle) const
{
    nlohmann::json tools = ListJson();
    for (Size index = 0; index < m_tools.size(); ++index)
    {
        tools[index]["group"] = m_tools[index].Definition.Group;
        tools[index]["example"] = m_tools[index].Definition.Example;
    }

    nlohmann::json catalog = nlohmann::json::object();
    catalog["server"] = serverName;
    catalog["title"] = serverTitle;
    catalog["toolCount"] = m_tools.size();
    catalog["tools"] = std::move(tools);
    return catalog;
}

ToolOutcome ToolRegistry::Call(ToolContext& context, const RegisteredTool& tool,
                               const nlohmann::json& arguments) const
{
    const nlohmann::json& effective = arguments.is_object() ? arguments : nlohmann::json::object();

    std::vector<SchemaViolation> violations;
    if (!tool.InputCheck->Validate(effective, violations))
    {
        return MakeError(ErrorCode::InputInvalid,
                         "The arguments of '" + tool.Definition.Name + "' do not match its input schema.",
                         tool.Definition.Name, "Check the tool's inputSchema and retry with corrected arguments.",
                         ViolationsToDetails(violations));
    }

    try
    {
        return tool.Definition.Handler(context, effective);
    }
    catch (const std::exception& failure)
    {
        Log::Error("Tool '" + tool.Definition.Name + "' threw: " + failure.what());
        return MakeError(ErrorCode::InternalError, "The tool failed unexpectedly: " + std::string(failure.what()),
                         tool.Definition.Name);
    }
    catch (...)
    {
        Log::Error("Tool '" + tool.Definition.Name + "' threw a non-standard exception.");
        return MakeError(ErrorCode::InternalError, "The tool failed unexpectedly.", tool.Definition.Name);
    }
}

int ToolRegistry::OrderRank(const ToolDefinition& definition)
{
    if (definition.Group == "lifecycle")
    {
        return 0;
    }

    if (definition.Group == "files")
    {
        return 3;
    }

    return definition.Annotations.ReadOnly ? 1 : 2;
}

void ToolRegistry::Sort()
{
    std::stable_sort(m_tools.begin(), m_tools.end(),
                     [](const RegisteredTool& left, const RegisteredTool& right)
                     { return OrderRank(left.Definition) < OrderRank(right.Definition); });
}

nlohmann::json Schema::Object(std::string description, std::vector<std::string> required, nlohmann::json properties)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    schema["description"] = std::move(description);
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    schema["additionalProperties"] = false;
    return schema;
}

nlohmann::json Schema::FreeObject(std::string description)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    schema["description"] = std::move(description);
    return schema;
}

nlohmann::json Schema::String(std::string description)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "string";
    schema["description"] = std::move(description);
    return schema;
}

nlohmann::json Schema::StringWithDefault(std::string description, std::string defaultValue)
{
    nlohmann::json schema = String(std::move(description));
    schema["default"] = std::move(defaultValue);
    return schema;
}

nlohmann::json Schema::Enumeration(std::string description, std::vector<std::string> values)
{
    nlohmann::json schema = String(std::move(description));
    schema["enum"] = std::move(values);
    return schema;
}

nlohmann::json Schema::EnumerationWithDefault(std::string description, std::vector<std::string> values,
                                              std::string defaultValue)
{
    nlohmann::json schema = Enumeration(std::move(description), std::move(values));
    schema["default"] = std::move(defaultValue);
    return schema;
}

nlohmann::json Schema::Integer(std::string description, std::optional<Int64> minimum, std::optional<Int64> maximum)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "integer";
    schema["description"] = std::move(description);
    if (minimum.has_value())
    {
        schema["minimum"] = *minimum;
    }

    if (maximum.has_value())
    {
        schema["maximum"] = *maximum;
    }

    return schema;
}

nlohmann::json Schema::IntegerWithDefault(std::string description, Int64 defaultValue, std::optional<Int64> minimum,
                                          std::optional<Int64> maximum)
{
    nlohmann::json schema = Integer(std::move(description), minimum, maximum);
    schema["default"] = defaultValue;
    return schema;
}

nlohmann::json Schema::Number(std::string description)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "number";
    schema["description"] = std::move(description);
    return schema;
}

nlohmann::json Schema::Boolean(std::string description)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "boolean";
    schema["description"] = std::move(description);
    return schema;
}

nlohmann::json Schema::BooleanWithDefault(std::string description, bool defaultValue)
{
    nlohmann::json schema = Boolean(std::move(description));
    schema["default"] = defaultValue;
    return schema;
}

nlohmann::json Schema::Array(std::string description, nlohmann::json items, std::optional<Int64> maxItems)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "array";
    schema["description"] = std::move(description);
    schema["items"] = std::move(items);
    if (maxItems.has_value())
    {
        schema["maxItems"] = *maxItems;
    }

    return schema;
}

nlohmann::json Schema::Length(std::string description)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["description"] = std::move(description);
    schema["type"] = nlohmann::json::array({"number", "string"});
    return schema;
}

nlohmann::json Schema::Any(std::string description)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["description"] = std::move(description);
    return schema;
}

nlohmann::json Schema::Envelope(nlohmann::json dataSchema, bool sessionFields)
{
    nlohmann::json warning = Object("One non-fatal diagnostic raised while the tool ran.",
                                    {"code", "message"},
                                    nlohmann::json{{"code", String("Diagnostic category.")},
                                                   {"message", String("Human-readable description.")},
                                                   {"context", String("Where the diagnostic applies.")}});

    nlohmann::json error =
        Object("Failure detail; present only when the call reports isError.", {"code", "message"},
               nlohmann::json{{"code", String("Machine-readable failure code.")},
                              {"message", String("Human-readable failure description.")},
                              {"target", String("Subject of the failure, such as a sheet or block.")},
                              {"hint", String("Tool call that resolves the situation.")},
                              {"details", Array("Additional structured detail.", Any("One detail entry."))}});

    nlohmann::json properties = nlohmann::json::object();
    properties["ok"] = Boolean("True when the call succeeded.");
    if (sessionFields)
    {
        properties["documentId"] = String("Session identifier of the affected document.");
        properties["revision"] = Integer("Number of successful mutations applied to the session.");
        properties["dirty"] = Boolean("True when the session holds unsaved changes.");
    }

    properties["summary"] = String("One-sentence human-readable summary of what happened.");
    properties["data"] = std::move(dataSchema);
    properties["warnings"] = Array("Non-fatal diagnostics.", std::move(warning));
    properties["truncated"] = Boolean("True when the payload was shortened to fit the response budget.");
    properties["error"] = std::move(error);

    return Object("Result envelope shared by every tool of this server.", {"ok"}, std::move(properties));
}

} // namespace ExyokiOffice::Mcp
