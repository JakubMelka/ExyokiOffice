// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "Tools/DocumentModelJsonInternal.hpp"
#include "XmlParseOptions.hpp"

#include "pugixml/pugixml.hpp"

#include <sstream>

namespace ExyokiOffice::Tools
{

using Json = nlohmann::ordered_json;

// The semantic XML format is a mechanical projection of the canonical JSON
// envelope: an object becomes an element, scalar members become attributes,
// an array member becomes a container element with one <item> child per
// entry, and an object member becomes a named child element. Scalar array
// entries are written as <item value="..."/>. All attribute values are kept
// as strings when parsing; the model parser coerces them.

class XmlProjection
{
public:
    static void ObjectToElement(const Json& object, Pugi::xml_node element)
    {
        for (const auto& [key, value] : object.items())
        {
            if (value.is_object())
            {
                ObjectToElement(value, element.append_child(key.c_str()));
            }
            else if (value.is_array())
            {
                ArrayToElement(value, element.append_child(key.c_str()));
            }
            else
            {
                element.append_attribute(key.c_str()).set_value(ScalarToString(value).c_str());
            }
        }
    }

    static Json ElementToObject(Pugi::xml_node element)
    {
        Json object = Json::object();
        for (const auto attribute : element.attributes())
        {
            object[attribute.name()] = std::string(attribute.value());
        }
        for (const auto child : element.children())
        {
            if (child.type() != Pugi::node_element)
            {
                continue;
            }
            if (IsArrayContainer(child))
            {
                object[child.name()] = ElementToArray(child);
            }
            else
            {
                object[child.name()] = ElementToObject(child);
            }
        }
        return object;
    }

private:
    static std::string ScalarToString(const Json& value)
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        if (value.is_boolean())
        {
            return value.get<bool>() ? "true" : "false";
        }
        return value.dump();
    }

    static void ArrayToElement(const Json& array, Pugi::xml_node element)
    {
        for (const auto& entry : array)
        {
            auto item = element.append_child("item");
            if (entry.is_object())
            {
                ObjectToElement(entry, item);
            }
            else if (entry.is_array())
            {
                ArrayToElement(entry, item);
            }
            else
            {
                item.append_attribute("value").set_value(ScalarToString(entry).c_str());
            }
        }
    }

    /// An element whose element children are all <item> nodes maps back to an array.
    static bool IsArrayContainer(Pugi::xml_node element)
    {
        bool hasItem = false;
        for (const auto child : element.children())
        {
            if (child.type() != Pugi::node_element)
            {
                continue;
            }
            if (std::string_view(child.name()) != "item")
            {
                return false;
            }
            hasItem = true;
        }
        return hasItem;
    }

    static Json ElementToArray(Pugi::xml_node element)
    {
        Json array = Json::array();
        static const char* itemName = "item";
        for (const auto item : element.children(itemName))
        {
            if (IsArrayContainer(item))
            {
                array.push_back(ElementToArray(item));
            }
            else
            {
                array.push_back(ElementToObject(item));
            }
        }
        return array;
    }
};

std::string SerializeModelXml(const DocumentModel& model, bool embedMedia)
{
    const auto tree = detail::BuildModelTree(model, embedMedia);

    Pugi::xml_document document;
    auto declaration = document.append_child(Pugi::node_declaration);
    declaration.append_attribute("version").set_value("1.0");
    declaration.append_attribute("encoding").set_value("UTF-8");

    auto root = document.append_child("eoDocument");
    XmlProjection::ObjectToElement(tree, root);

    std::ostringstream stream;
    document.save(stream, "  ");
    return stream.str();
}

DocumentModel ParseModelXml(std::string_view xml, std::vector<ToolDiagnostic>& diagnostics)
{
    Pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
    {
        DocumentModel model;
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Input is not valid XML"});
        return model;
    }

    const auto root = document.document_element();
    if (std::string_view(root.name()) != "eoDocument")
    {
        DocumentModel model;
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Root element is not <eoDocument>", root.name()});
        return model;
    }

    return detail::ParseModelTree(XmlProjection::ElementToObject(root), diagnostics);
}

} // namespace ExyokiOffice::Tools
