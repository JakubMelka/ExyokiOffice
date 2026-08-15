// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/Report.hpp"
#include "ExyokiOffice/Version.hpp"

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"
#include "Utf8Text.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace ExyokiOffice::Tools
{

ReportNode::ReportNode() = default;

ReportNode::ReportNode(std::nullptr_t) noexcept
{
}

ReportNode::ReportNode(bool value) noexcept
    : m_kind(ReportNodeKind::Bool), m_bool(value)
{
}

ReportNode::ReportNode(int value) noexcept
    : ReportNode(static_cast<Int64>(value))
{
}

ReportNode::ReportNode(Int64 value) noexcept
    : m_kind(ReportNodeKind::Int), m_int(value)
{
}

ReportNode::ReportNode(UInt64 value) noexcept
    : m_kind(ReportNodeKind::UInt), m_uint(value)
{
}

ReportNode::ReportNode(Real value) noexcept
    : m_kind(ReportNodeKind::Double), m_double(value)
{
}

ReportNode::ReportNode(std::string value)
    : m_kind(ReportNodeKind::String), m_string(std::move(value))
{
}

ReportNode::ReportNode(const char* value)
    : m_kind(ReportNodeKind::String), m_string(value ? value : "")
{
}

ReportNode::ReportNode(std::string_view value)
    : m_kind(ReportNodeKind::String), m_string(value)
{
}

ReportNode ReportNode::MakeArray()
{
    ReportNode node;
    node.m_kind = ReportNodeKind::Array;
    return node;
}

ReportNode ReportNode::MakeObject()
{
    ReportNode node;
    node.m_kind = ReportNodeKind::Object;
    return node;
}

ReportNode& ReportNode::Push(ReportNode value)
{
    if (m_kind == ReportNodeKind::Null)
    {
        m_kind = ReportNodeKind::Array;
    }
    m_items.push_back(std::move(value));
    return *this;
}

ReportNode& ReportNode::Set(std::string key, ReportNode value)
{
    if (m_kind == ReportNodeKind::Null)
    {
        m_kind = ReportNodeKind::Object;
    }
    for (auto& member : m_members)
    {
        if (member.first == key)
        {
            member.second = std::move(value);
            return *this;
        }
    }
    m_members.emplace_back(std::move(key), std::move(value));
    return *this;
}

ReportNode& ReportNode::SetTableHint(std::vector<std::string> columns)
{
    m_tableHint = std::move(columns);
    return *this;
}

/// File-local helpers for the plain-text report renderer.
class ReportPlainTextHelper
{
public:
    static std::string ScalarToPlainText(const ReportNode& node)
    {
        switch (node.Kind())
        {
            case ReportNodeKind::Null:
                return "null";
            case ReportNodeKind::Bool:
                return node.AsBool() ? "true" : "false";
            case ReportNodeKind::Int:
                return std::to_string(node.AsInt());
            case ReportNodeKind::UInt:
                return std::to_string(node.AsUInt());
            case ReportNodeKind::Double:
            {
                std::ostringstream stream;
                stream << node.AsDouble();
                return stream.str();
            }
            case ReportNodeKind::String:
                return node.AsString();
            case ReportNodeKind::Array:
                return "[" + std::to_string(node.AsArray().size()) + " item(s)]";
            case ReportNodeKind::Object:
                return "{" + std::to_string(node.AsObject().size()) + " field(s)}";
        }
        return {};
    }

    static bool IsScalar(const ReportNode& node)
    {
        switch (node.Kind())
        {
            case ReportNodeKind::Array:
            case ReportNodeKind::Object:
                return false;
            default:
                return true;
        }
    }

    static void WritePlainObject(std::ostringstream& output, const ReportNode& node, int indent)
    {
        const std::string pad(static_cast<Size>(indent) * 2, ' ');
        for (const auto& [key, value] : node.AsObject())
        {
            if (IsScalar(value))
            {
                output << pad << key << ": " << ScalarToPlainText(value) << "\n";
            }
            else
            {
                output << pad << key << ":\n";
                WritePlainNode(output, value, indent + 1);
            }
        }
    }

    static void WritePlainTable(std::ostringstream& output, const ReportNode& node, int indent)
    {
        const std::string pad(static_cast<Size>(indent) * 2, ' ');
        const auto& columns = node.TableHint();
        for (const auto& row : node.AsArray())
        {
            output << pad << "-";
            bool first = true;
            for (const auto& column : columns)
            {
                for (const auto& [key, value] : row.AsObject())
                {
                    if (key != column)
                    {
                        continue;
                    }
                    output << (first ? " " : ", ") << column << "=" << ScalarToPlainText(value);
                    first = false;
                }
            }
            output << "\n";
        }
    }

    static void WritePlainArray(std::ostringstream& output, const ReportNode& node, int indent)
    {
        const std::string pad(static_cast<Size>(indent) * 2, ' ');
        if (node.HasTableHint())
        {
            WritePlainTable(output, node, indent);
            return;
        }

        for (const auto& item : node.AsArray())
        {
            if (IsScalar(item))
            {
                output << pad << "- " << ScalarToPlainText(item) << "\n";
            }
            else
            {
                output << pad << "-\n";
                WritePlainNode(output, item, indent + 1);
            }
        }
    }

    static void WritePlainNode(std::ostringstream& output, const ReportNode& node, int indent)
    {
        switch (node.Kind())
        {
            case ReportNodeKind::Object:
                WritePlainObject(output, node, indent);
                break;
            case ReportNodeKind::Array:
                WritePlainArray(output, node, indent);
                break;
            default:
                output << std::string(static_cast<Size>(indent) * 2, ' ') << ScalarToPlainText(node) << "\n";
                break;
        }
    }
};

std::string RenderPlain(const ReportDocument& document)
{
    std::ostringstream output;
    output << "command: " << document.Command << "\n";
    output << "status: " << document.Status << "\n";
    if (!document.Data.AsObject().empty() || document.Data.Kind() == ReportNodeKind::Array)
    {
        output << "data:\n";
        ReportPlainTextHelper::WritePlainNode(output, document.Data, 1);
    }
    if (!document.Diagnostics.empty())
    {
        output << "diagnostics:\n";
        for (const auto& diagnostic : document.Diagnostics)
        {
            output << "  [" << ToString(diagnostic.Severity) << "] " << diagnostic.Message;
            if (!diagnostic.Context.empty())
            {
                output << " (" << diagnostic.Context << ")";
            }
            output << "\n";
        }
    }
    return output.str();
}

/// File-local helpers for the Markdown report renderer.
class ReportMarkdownHelper
{
public:
    static std::string EscapeMarkdown(std::string_view text)
    {
        std::string escaped;
        escaped.reserve(text.size());
        for (char ch : text)
        {
            if (ch == '|' || ch == '\\')
            {
                escaped.push_back('\\');
            }
            if (ch == '\n')
            {
                escaped.append("<br>");
                continue;
            }
            escaped.push_back(ch);
        }
        return escaped;
    }

    static void WriteMarkdownObject(std::ostringstream& output, const ReportNode& node, int indent)
    {
        const std::string pad(static_cast<Size>(indent) * 2, ' ');
        for (const auto& [key, value] : node.AsObject())
        {
            if (ReportPlainTextHelper::IsScalar(value))
            {
                output << pad << "- **" << key << "**: " << EscapeMarkdown(ReportPlainTextHelper::ScalarToPlainText(value)) << "\n";
            }
            else
            {
                output << pad << "- **" << key << "**:\n";
                WriteMarkdownNode(output, value, indent + 1);
            }
        }
    }

    static void WriteMarkdownTable(std::ostringstream& output, const ReportNode& node)
    {
        const auto& columns = node.TableHint();
        output << "\n";
        for (const auto& column : columns)
        {
            output << "| " << column << " ";
        }
        output << "|\n";
        for (Size i = 0; i < columns.size(); ++i)
        {
            output << "| --- ";
        }
        output << "|\n";
        for (const auto& row : node.AsArray())
        {
            for (const auto& column : columns)
            {
                std::string cell;
                for (const auto& [key, value] : row.AsObject())
                {
                    if (key == column)
                    {
                        cell = ReportPlainTextHelper::ScalarToPlainText(value);
                    }
                }
                output << "| " << EscapeMarkdown(cell) << " ";
            }
            output << "|\n";
        }
        output << "\n";
    }

    static void WriteMarkdownArray(std::ostringstream& output, const ReportNode& node, int indent)
    {
        if (node.HasTableHint())
        {
            WriteMarkdownTable(output, node);
            return;
        }

        const std::string pad(static_cast<Size>(indent) * 2, ' ');
        for (const auto& item : node.AsArray())
        {
            if (ReportPlainTextHelper::IsScalar(item))
            {
                output << pad << "- " << EscapeMarkdown(ReportPlainTextHelper::ScalarToPlainText(item)) << "\n";
            }
            else
            {
                output << pad << "-\n";
                WriteMarkdownNode(output, item, indent + 1);
            }
        }
    }

    static void WriteMarkdownNode(std::ostringstream& output, const ReportNode& node, int indent)
    {
        switch (node.Kind())
        {
            case ReportNodeKind::Object:
                WriteMarkdownObject(output, node, indent);
                break;
            case ReportNodeKind::Array:
                WriteMarkdownArray(output, node, indent);
                break;
            default:
                output << std::string(static_cast<Size>(indent) * 2, ' ') << "- "
                       << EscapeMarkdown(ReportPlainTextHelper::ScalarToPlainText(node)) << "\n";
                break;
        }
    }
};

std::string RenderMarkdown(const ReportDocument& document)
{
    std::ostringstream output;
    output << "## exyoki " << document.Command << "\n\n";
    output << "**Status:** " << document.Status << "\n\n";
    if (!document.Data.AsObject().empty() || document.Data.Kind() == ReportNodeKind::Array)
    {
        output << "### Data\n\n";
        ReportMarkdownHelper::WriteMarkdownNode(output, document.Data, 0);
        output << "\n";
    }
    if (!document.Diagnostics.empty())
    {
        output << "### Diagnostics\n\n";
        for (const auto& diagnostic : document.Diagnostics)
        {
            output << "- **" << ToString(diagnostic.Severity) << "**: " << ReportMarkdownHelper::EscapeMarkdown(diagnostic.Message);
            if (!diagnostic.Context.empty())
            {
                output << " (" << ReportMarkdownHelper::EscapeMarkdown(diagnostic.Context) << ")";
            }
            output << "\n";
        }
    }
    return output.str();
}

/// File-local conversion helpers for the JSON report renderer.
class ReportJsonHelper
{
public:
    class ReportJsonRenderer
    {
    public:
        static nlohmann::ordered_json Convert(const ReportNode& node)
        {
            switch (node.Kind())
            {
                case ReportNodeKind::Null:
                    return nullptr;
                case ReportNodeKind::Bool:
                    return node.AsBool();
                case ReportNodeKind::Int:
                    return node.AsInt();
                case ReportNodeKind::UInt:
                    return node.AsUInt();
                case ReportNodeKind::Double:
                    return node.AsDouble();
                case ReportNodeKind::String:
                    return node.AsString();
                case ReportNodeKind::Array:
                {
                    auto array = nlohmann::ordered_json::array();
                    for (const auto& item : node.AsArray())
                    {
                        array.push_back(Convert(item));
                    }
                    return array;
                }
                case ReportNodeKind::Object:
                {
                    auto object = nlohmann::ordered_json::object();
                    for (const auto& [key, value] : node.AsObject())
                    {
                        object[key] = Convert(value);
                    }
                    return object;
                }
            }
            return nullptr;
        }
    };

    /**
     * Sanitizes an arbitrary key into a valid XML element name (used for object members).
     *
     * The whole point of this function is that whatever comes back can be written out as an element
     * name, so it decides one code point at a time rather than one byte at a time. Deciding by byte
     * would replace every character of a key like `Přehled` with underscores, and a key holding
     * something XML happens to forbid - `U+00D7`, say - would still slip through as a run of bytes
     * that all look non-ASCII. A malformed sequence is replaced wholesale for the same reason: half
     * a character is not a name.
     */
    static std::string SanitizeXmlElementName(std::string_view name)
    {
        std::string sanitized;
        sanitized.reserve(name.size());
        for (Size offset = 0; offset < name.size();)
        {
            const Utf8Text::DecodedCharacter character = Utf8Text::Decode(name, offset);
            const bool isValid = character.Valid && character.Value != U':' &&
                                 Utf8Text::IsXmlNameChar(character.Value);
            if (isValid)
            {
                sanitized.append(name.substr(offset, character.Length));
            }
            else
            {
                sanitized.push_back('_');
            }
            offset += character.Length;
        }

        const Utf8Text::DecodedCharacter first = Utf8Text::Decode(sanitized, 0);
        if (!first.Valid || first.Value == U':' || !Utf8Text::IsXmlNameStartChar(first.Value))
        {
            sanitized.insert(sanitized.begin(), '_');
        }
        return sanitized;
    }

    class ReportXmlRenderer
    {
    public:
        static void AppendNode(Pugi::xml_node parent, const ReportNode& node, std::string_view tag)
        {
            const std::string elementName(tag);
            auto element = parent.append_child(elementName.c_str());
            if (node.Kind() == ReportNodeKind::Object)
            {
                AppendObject(element, node);
            }
            else if (node.Kind() == ReportNodeKind::Array)
            {
                for (const auto& item : node.AsArray())
                {
                    AppendNode(element, item, "item");
                }
            }
            else
            {
                element.text().set(ReportPlainTextHelper::ScalarToPlainText(node).c_str());
            }
        }

        static void AppendObject(Pugi::xml_node parent, const ReportNode& node)
        {
            for (const auto& [key, value] : node.AsObject())
            {
                AppendNode(parent, value, SanitizeXmlElementName(key));
            }
        }
    };
};

std::string RenderJson(const ReportDocument& document)
{
    nlohmann::ordered_json envelope;
    envelope["tool"] = "exyoki";
    envelope["toolVersion"] = std::string(ExyokiOffice::GetVersion());
    envelope["command"] = document.Command;
    envelope["status"] = document.Status;
    envelope["data"] = ReportJsonHelper::ReportJsonRenderer::Convert(document.Data);

    auto diagnostics = nlohmann::ordered_json::array();
    for (const auto& diagnostic : document.Diagnostics)
    {
        nlohmann::ordered_json entry;
        entry["severity"] = std::string(ToString(diagnostic.Severity));
        entry["message"] = diagnostic.Message;
        if (!diagnostic.Context.empty())
        {
            entry["context"] = diagnostic.Context;
        }
        diagnostics.push_back(std::move(entry));
    }
    envelope["diagnostics"] = std::move(diagnostics);
    // A trailing newline so piped and concatenated reports never glue the
    // closing brace to the next line of output.
    return envelope.dump(2) + "\n";
}

std::string RenderXml(const ReportDocument& document)
{
    Pugi::xml_document xml;
    auto declaration = xml.append_child(Pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "UTF-8";

    auto root = xml.append_child("exyoki");
    root.append_child("command").text().set(document.Command.c_str());
    root.append_child("status").text().set(document.Status.c_str());
    auto data = root.append_child("data");
    if (document.Data.Kind() == ReportNodeKind::Object)
    {
        ReportJsonHelper::ReportXmlRenderer::AppendObject(data, document.Data);
    }
    else if (document.Data.Kind() == ReportNodeKind::Array)
    {
        for (const auto& item : document.Data.AsArray())
        {
            ReportJsonHelper::ReportXmlRenderer::AppendNode(data, item, "item");
        }
    }
    else
    {
        data.text().set(ReportPlainTextHelper::ScalarToPlainText(document.Data).c_str());
    }

    auto diagnostics = root.append_child("diagnostics");
    for (const auto& diagnostic : document.Diagnostics)
    {
        auto entry = diagnostics.append_child("diagnostic");
        entry.append_attribute("severity") = ToString(diagnostic.Severity).data();
        entry.append_child("message").text().set(diagnostic.Message.c_str());
        if (!diagnostic.Context.empty())
        {
            entry.append_child("context").text().set(diagnostic.Context.c_str());
        }
    }

    std::ostringstream output;
    xml.save(output, "  ", Pugi::format_default, Pugi::encoding_utf8);
    return output.str();
}

} // namespace ExyokiOffice::Tools
