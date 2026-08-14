// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Json.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace exyoki::generator
{
/// File-local scanning helpers for the JSON reader.
class JsonHelper
{
public:
    static JsonValue ConvertJson(const nlohmann::json& value)
    {
        if (value.is_null())
        {
            return JsonValue(std::nullptr_t{});
        }

        if (value.is_boolean())
        {
            return JsonValue(value.get<bool>());
        }

        if (value.is_number_float() || value.is_number_integer() || value.is_number_unsigned())
        {
            return JsonValue(value.get<double>());
        }

        if (value.is_string())
        {
            return JsonValue(value.get<std::string>());
        }

        if (value.is_array())
        {
            JsonValue::array_type array;
            array.reserve(value.size());
            for (const auto& item : value)
            {
                array.push_back(ConvertJson(item));
            }
            return JsonValue(std::move(array));
        }

        if (value.is_object())
        {
            JsonValue::object_type object;
            for (const auto& [key, item] : value.items())
            {
                object.emplace(key, ConvertJson(item));
            }
            return JsonValue(std::move(object));
        }

        throw std::runtime_error("Unsupported JSON value type encountered during parsing.");
    }
};

JsonValue::JsonValue()
    : type_(Type::Null)
{
}

JsonValue::JsonValue(std::nullptr_t)
    : JsonValue()
{
}

JsonValue::JsonValue(bool value)
    : type_(Type::Boolean),
      value_(value)
{
}

JsonValue::JsonValue(double value)
    : type_(Type::Number),
      value_(value)
{
}

JsonValue::JsonValue(std::string value)
    : type_(Type::String),
      value_(std::move(value))
{
}

JsonValue::JsonValue(array_type value)
    : type_(Type::Array),
      value_(std::move(value))
{
}

JsonValue::JsonValue(object_type value)
    : type_(Type::Object),
      value_(std::move(value))
{
}

JsonValue::Type JsonValue::type() const noexcept
{
    return type_;
}

bool JsonValue::is_null() const noexcept
{
    return type_ == Type::Null;
}
bool JsonValue::is_bool() const noexcept
{
    return type_ == Type::Boolean;
}
bool JsonValue::is_number() const noexcept
{
    return type_ == Type::Number;
}
bool JsonValue::is_string() const noexcept
{
    return type_ == Type::String;
}
bool JsonValue::is_array() const noexcept
{
    return type_ == Type::Array;
}
bool JsonValue::is_object() const noexcept
{
    return type_ == Type::Object;
}

bool JsonValue::as_bool() const
{
    if (!is_bool())
    {
        throw std::runtime_error("JSON value is not a boolean");
    }

    return std::get<bool>(value_);
}

double JsonValue::as_number() const
{
    if (!is_number())
    {
        throw std::runtime_error("JSON value is not a number");
    }

    return std::get<double>(value_);
}

const std::string& JsonValue::as_string() const
{
    if (!is_string())
    {
        throw std::runtime_error("JSON value is not a string");
    }

    return std::get<std::string>(value_);
}

const JsonValue::array_type& JsonValue::as_array() const
{
    if (!is_array())
    {
        throw std::runtime_error("JSON value is not an array");
    }

    return std::get<array_type>(value_);
}

const JsonValue::object_type& JsonValue::as_object() const
{
    if (!is_object())
    {
        throw std::runtime_error("JSON value is not an object");
    }

    return std::get<object_type>(value_);
}

std::optional<std::string> JsonValue::try_get_string(std::string_view key) const
{
    if (!is_object())
    {
        return std::nullopt;
    }

    const auto& obj = as_object();
    auto it = obj.find(key);
    if (it == obj.end() || !it->second.is_string())
    {
        return std::nullopt;
    }

    return it->second.as_string();
}

const JsonValue* JsonValue::try_get(std::string_view key) const
{
    if (!is_object())
    {
        return nullptr;
    }

    const auto& obj = as_object();
    auto it = obj.find(key);
    if (it == obj.end())
    {
        return nullptr;
    }
    return &it->second;
}

const JsonValue& JsonValue::at(std::string_view key) const
{
    if (!is_object())
    {
        throw std::runtime_error("JSON value is not an object");
    }

    const auto& obj = as_object();
    auto it = obj.find(key);
    if (it == obj.end())
    {
        throw std::out_of_range("JSON object missing key: " + std::string(key));
    }
    return it->second;
}

const JsonValue& JsonValue::at(std::size_t index) const
{
    if (!is_array())
    {
        throw std::runtime_error("JSON value is not an array");
    }

    const auto& arr = as_array();
    if (index >= arr.size())
    {
        throw std::out_of_range("JSON array index out of range");
    }
    return arr[index];
}

JsonValue JsonValue::Parse(std::string_view text)
{
    try
    {
        auto parsed = nlohmann::json::parse(text.begin(), text.end());
        return JsonHelper::ConvertJson(parsed);
    }
    catch (const nlohmann::json::exception& ex)
    {
        throw std::runtime_error(std::string("JSON parse error: ") + ex.what());
    }
}
} // namespace exyoki::generator
