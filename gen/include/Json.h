// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace exyoki::generator
{
class JsonValue
{
public:
    enum class Type
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    using array_type = std::vector<JsonValue>;
    using object_type = std::map<std::string, JsonValue, std::less<>>;

    JsonValue();
    explicit JsonValue(std::nullptr_t);
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(std::string value);
    explicit JsonValue(array_type value);
    explicit JsonValue(object_type value);

    Type type() const noexcept;
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool as_bool() const;
    double as_number() const;
    const std::string& as_string() const;
    const array_type& as_array() const;
    const object_type& as_object() const;

    std::optional<std::string> try_get_string(std::string_view key) const;
    const JsonValue* try_get(std::string_view key) const;

    const JsonValue& at(std::string_view key) const;
    const JsonValue& at(std::size_t index) const;

    static JsonValue Parse(std::string_view text);

private:
    Type type_;
    std::variant<std::monostate, bool, double, std::string, array_type, object_type> value_;
};
} // namespace exyoki::generator
