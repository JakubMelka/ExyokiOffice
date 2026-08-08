// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "PptAddressing.hpp"

#include "ToolRegistry.hpp"
#include "Units.hpp"

#include "ExyokiOffice/MeasuringAngle.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace ExyokiOffice::Mcp
{

namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

/// File-local helpers for the PowerPoint text model.
class PptAddressingHelper
{
public:
    /// Reads one run from a JSON object of the `runs` array.
    static PowerPoint::PresentationTextRun ReadRun(const nlohmann::json& value)
    {
        PowerPoint::PresentationTextRun run;
        run.Text = value.value("text", std::string());
        run.Bold = value.value("bold", false);
        run.Italic = value.value("italic", false);
        if (value.value("underline", false))
        {
            run.Underline = DocumentFormat::OpenXml::Drawing::TextUnderlineValues::Single;
        }

        const auto size = value.value("sizePt", 0.0);
        if (size > 0.0)
        {
            run.FontSize = MeasuringUnits(size, MeasurementUnit::Point);
        }

        const auto color = value.value("color", std::string());
        if (!color.empty())
        {
            run.FontColor = ParseColor(color);
        }

        const auto link = value.value("link", std::string());
        if (!link.empty())
        {
            run.Hyperlink = link;
        }

        return run;
    }

    static bool ParseIndex(std::string_view text, Size& value)
    {
        const auto* begin = text.data();
        const auto* end = begin + text.size();
        const auto parsed = std::from_chars(begin, end, value);
        return parsed.ec == std::errc() && parsed.ptr == end;
    }

    /// PresentationML serializes angles in 1/60000 of a degree.
    static constexpr Real DrawingAngleUnitsPerDegree = 60000.0;

    static char ToLower(char value) noexcept
    {
        if (value >= 'A' && value <= 'Z')
        {
            return static_cast<char>(value - 'A' + 'a');
        }

        return value;
    }

    /**
     * @brief Parses a rotation argument into an angle.
     *
     * A bare number is degrees, matching how the schema describes it. A string
     * carries an explicit unit: `deg` or `rad`, in the short or the spelled-out
     * form. MeasuringAngle is the library's own angle type, so the conversion
     * to the serialized unit happens in one place instead of at every caller.
     */
    static std::optional<MeasuringAngle> ParseAngle(const nlohmann::json& value)
    {
        if (value.is_number())
        {
            return MeasuringAngle(value.get<Real>(), AngleUnit::Degree);
        }

        if (!value.is_string())
        {
            return std::nullopt;
        }

        const auto text = value.get<std::string>();
        char* suffix = nullptr;
        const Real amount = std::strtod(text.c_str(), &suffix);
        if (suffix == text.c_str())
        {
            return std::nullopt;
        }

        std::string unit;
        for (const char* cursor = suffix; *cursor != '\0'; ++cursor)
        {
            if (*cursor == ' ' || *cursor == '\t')
            {
                continue;
            }

            unit.push_back(ToLower(*cursor));
        }

        if (unit.empty() || unit == "deg" || unit == "degree" || unit == "degrees")
        {
            return MeasuringAngle(amount, AngleUnit::Degree);
        }

        if (unit == "rad" || unit == "radian" || unit == "radians")
        {
            return MeasuringAngle(amount, AngleUnit::Radian);
        }

        return std::nullopt;
    }

    /// Converts an angle to the stored unit; false when it does not fit.
    static bool ToDrawingAngle(const MeasuringAngle& angle, Int32& units)
    {
        const Real scaled = angle.ToDegrees().GetValue() * DrawingAngleUnitsPerDegree;
        if (!std::isfinite(scaled) || scaled < static_cast<Real>(std::numeric_limits<Int32>::min()) ||
            scaled > static_cast<Real>(std::numeric_limits<Int32>::max()))
        {
            return false;
        }

        units = static_cast<Int32>(std::llround(scaled));
        return true;
    }

    /// The angle a stored rotation stands for; the inverse of ToDrawingAngle.
    static MeasuringAngle FromDrawingAngle(Int32 units)
    {
        return MeasuringAngle(static_cast<Real>(units) / DrawingAngleUnitsPerDegree, AngleUnit::Degree);
    }

    /// Renders a stored rotation as degrees, the unit the tools speak.
    static Real ToDegreeValue(Int32 units)
    {
        return FromDrawingAngle(units).ToDegrees().GetValue();
    }

    /// Joins tokens into "a, b, or c" for an agent-facing hint.
    static std::string JoinTokens(const std::vector<std::string>& tokens)
    {
        std::string text;
        for (Size index = 0; index < tokens.size(); ++index)
        {
            if (index > 0)
            {
                text.append(", ");
            }

            if (tokens.size() > 1 && index + 1 == tokens.size())
            {
                text.append("or ");
            }

            text.append(tokens[index]);
        }

        return text;
    }
};

PowerPoint::PresentationSlide::Ptr PptAddressing::FindSlide(PowerPoint::PowerPointDocumentEditor& editor,
                                                            const nlohmann::json& arguments, ToolOutcome& failure)
{
    const Size index = arguments.value("slide", static_cast<Size>(0));
    const auto count = editor.SlideCount();
    if (index == 0 || index > count)
    {
        failure = MakeError(ErrorCode::SlideNotFound,
                            "The presentation has " + std::to_string(count) + " slide(s); slide " +
                                std::to_string(index) + " does not exist.",
                            std::to_string(index), "Call list_slides to see the slide indices.");
        return nullptr;
    }

    return editor.GetSlide(index - 1);
}

PowerPoint::PresentationShape::Ptr PptAddressing::FindShape(const PowerPoint::PresentationSlide& slide,
                                                            const std::string& path, ToolOutcome& failure)
{
    auto tree = slide.ShapeTree();
    if (tree == nullptr)
    {
        failure = MakeError(ErrorCode::ShapeNotFound, "The slide has no shape tree.", path);
        return nullptr;
    }

    std::vector<PowerPoint::PresentationShape::Ptr> level = tree->Shapes();
    PowerPoint::PresentationShape::Ptr current;

    Size start = 0;
    while (start <= path.size())
    {
        const auto separator = path.find('/', start);
        const auto segment =
            path.substr(start, separator == std::string::npos ? std::string::npos : separator - start);

        Size index = 0;
        if (!PptAddressingHelper::ParseIndex(segment, index) || index == 0 || index > level.size())
        {
            failure = MakeError(ErrorCode::ShapeNotFound, "No shape matches the path '" + path + "'.", path,
                                "Call get_slide to see the path of every shape.");
            return nullptr;
        }

        current = level[index - 1];
        if (separator == std::string::npos)
        {
            return current;
        }

        level = current->Children();
        start = separator + 1;
    }

    failure = MakeError(ErrorCode::ShapeNotFound, "No shape matches the path '" + path + "'.", path,
                        "Call get_slide to see the path of every shape.");
    return nullptr;
}

PowerPoint::PresentationPlaceholder::Ptr PptAddressing::FindPlaceholder(const PowerPoint::PresentationSlide& slide,
                                                                        const nlohmann::json& arguments,
                                                                        ToolOutcome& failure)
{
    const auto token = arguments.value("placeholder", std::string());
    const auto placeholders = slide.Placeholders(false);

    Size index = 0;
    if (PptAddressingHelper::ParseIndex(token, index))
    {
        if (index == 0 || index > placeholders.size())
        {
            failure = MakeError(ErrorCode::ShapeNotFound,
                                "The slide has " + std::to_string(placeholders.size()) + " placeholder(s).", token,
                                "Call get_slide to see the placeholders of the slide.");
            return nullptr;
        }

        return placeholders[index - 1];
    }

    const auto type = ParsePlaceholderType(token);
    if (!type.has_value())
    {
        // The hint lists exactly the tokens ParsePlaceholderType accepts; an
        // agent that reads a shorter list would keep guessing.
        failure = MakeError(ErrorCode::InputInvalid, "Unknown placeholder type '" + token + "'.", token,
                            "Use a 1-based placeholder index, or one of " +
                                PptAddressingHelper::JoinTokens(PlaceholderTokens()) + ".");
        return nullptr;
    }

    for (const auto& placeholder : placeholders)
    {
        if (placeholder != nullptr && placeholder->Type() == *type)
        {
            return placeholder;
        }
    }

    failure = MakeError(ErrorCode::ShapeNotFound, "The slide has no '" + token + "' placeholder.", token,
                        "Add the slide from a layout that provides it, or use add_text_box instead.");
    return nullptr;
}

std::vector<std::string> PptAddressing::PlaceholderTokens()
{
    return {"title", "body", "centeredTitle", "subtitle", "dateAndTime", "slideNumber", "footer", "header",
            "object", "chart", "table", "clipArt", "diagram", "media", "picture"};
}

std::optional<P::PlaceholderValues::Value> PptAddressing::ParsePlaceholderType(const std::string& token)
{
    if (token == "title")
    {
        return P::PlaceholderValues::Title;
    }

    if (token == "body")
    {
        return P::PlaceholderValues::Body;
    }

    if (token == "centeredTitle")
    {
        return P::PlaceholderValues::CenteredTitle;
    }

    if (token == "subtitle")
    {
        return P::PlaceholderValues::SubTitle;
    }

    if (token == "dateAndTime")
    {
        return P::PlaceholderValues::DateAndTime;
    }

    if (token == "slideNumber")
    {
        return P::PlaceholderValues::SlideNumber;
    }

    if (token == "footer")
    {
        return P::PlaceholderValues::Footer;
    }

    if (token == "header")
    {
        return P::PlaceholderValues::Header;
    }

    if (token == "object")
    {
        return P::PlaceholderValues::Object;
    }

    if (token == "chart")
    {
        return P::PlaceholderValues::Chart;
    }

    if (token == "table")
    {
        return P::PlaceholderValues::Table;
    }

    if (token == "clipArt")
    {
        return P::PlaceholderValues::ClipArt;
    }

    if (token == "diagram")
    {
        return P::PlaceholderValues::Diagram;
    }

    if (token == "media")
    {
        return P::PlaceholderValues::Media;
    }

    if (token == "picture")
    {
        return P::PlaceholderValues::Picture;
    }

    return std::nullopt;
}

std::string PptAddressing::PlaceholderToken(P::PlaceholderValues::Value type)
{
    switch (type)
    {
        case P::PlaceholderValues::Title:
            return "title";
        case P::PlaceholderValues::Body:
            return "body";
        case P::PlaceholderValues::CenteredTitle:
            return "centeredTitle";
        case P::PlaceholderValues::SubTitle:
            return "subtitle";
        case P::PlaceholderValues::DateAndTime:
            return "dateAndTime";
        case P::PlaceholderValues::SlideNumber:
            return "slideNumber";
        case P::PlaceholderValues::Footer:
            return "footer";
        case P::PlaceholderValues::Header:
            return "header";
        case P::PlaceholderValues::Object:
            return "object";
        case P::PlaceholderValues::Chart:
            return "chart";
        case P::PlaceholderValues::Table:
            return "table";
        case P::PlaceholderValues::ClipArt:
            return "clipArt";
        case P::PlaceholderValues::Diagram:
            return "diagram";
        case P::PlaceholderValues::Media:
            return "media";
        case P::PlaceholderValues::SlideImage:
            return "slideImage";
        case P::PlaceholderValues::Picture:
            return "picture";
        default:
            break;
    }

    return {};
}

bool PptAddressing::HasText(const nlohmann::json& arguments)
{
    return arguments.contains("paragraphs") || arguments.contains("text") || arguments.contains("bullets");
}

bool PptAddressing::ReadTextFrame(const nlohmann::json& arguments, PowerPoint::PresentationTextFrame& frame,
                                  ToolOutcome& failure)
{
    frame.Paragraphs.clear();

    const auto paragraphs = arguments.find("paragraphs");
    if (paragraphs != arguments.end() && paragraphs->is_array())
    {
        for (const auto& value : *paragraphs)
        {
            if (!value.is_object())
            {
                failure = MakeError(ErrorCode::InputInvalid, "Every entry of 'paragraphs' must be an object.");
                return false;
            }

            PowerPoint::PresentationTextParagraph paragraph;
            paragraph.Level = value.value("level", 0);

            const auto runs = value.find("runs");
            if (runs != value.end() && runs->is_array())
            {
                for (const auto& run : *runs)
                {
                    paragraph.Runs.push_back(PptAddressingHelper::ReadRun(run));
                }
            }
            else
            {
                PowerPoint::PresentationTextRun run;
                run.Text = value.value("text", std::string());
                paragraph.Runs.push_back(std::move(run));
            }

            frame.Paragraphs.push_back(std::move(paragraph));
        }

        return true;
    }

    const auto bullets = arguments.find("bullets");
    if (bullets != arguments.end() && bullets->is_array())
    {
        for (const auto& value : *bullets)
        {
            PowerPoint::PresentationTextParagraph paragraph;
            PowerPoint::PresentationTextRun run;
            if (value.is_object())
            {
                paragraph.Level = value.value("level", 0);
                run.Text = value.value("text", std::string());
            }
            else
            {
                run.Text = value.get<std::string>();
            }

            paragraph.Runs.push_back(std::move(run));
            frame.Paragraphs.push_back(std::move(paragraph));
        }

        return true;
    }

    const auto text = arguments.value("text", std::string());
    PowerPoint::PresentationTextParagraph paragraph;
    PowerPoint::PresentationTextRun run;
    run.Text = text;
    paragraph.Runs.push_back(std::move(run));
    frame.Paragraphs.push_back(std::move(paragraph));
    return true;
}

nlohmann::json PptAddressing::ParagraphsSchema()
{
    nlohmann::json run =
        Schema::Object("One text run.", {},
                       nlohmann::json{{"text", Schema::String("Run text.")},
                                      {"bold", Schema::Boolean("Bold text.")},
                                      {"italic", Schema::Boolean("Italic text.")},
                                      {"underline", Schema::Boolean("Single underline.")},
                                      {"sizePt", Schema::Number("Font size in points.")},
                                      {"color", Schema::String("Font color as \"#RRGGBB\".")},
                                      {"link", Schema::String("External hyperlink target.")}});

    nlohmann::json paragraph =
        Schema::Object("One paragraph.", {},
                       nlohmann::json{{"level", Schema::Integer("Outline level, 0 through 8.", 0, 8)},
                                      {"text", Schema::String("Plain text shorthand for a single run.")},
                                      {"runs", Schema::Array("Formatted runs.", std::move(run))}});

    return Schema::Array("Paragraphs of the text frame.", std::move(paragraph));
}

bool PptAddressing::ReadTransform(const nlohmann::json& arguments, PowerPoint::PresentationShapeTransform& transform,
                                  bool requireSize, ToolOutcome& failure)
{
    const auto read = [&arguments, &failure](const char* name, MeasuringUnits& target, bool& present)
    {
        const auto member = arguments.find(name);
        if (member == arguments.end())
        {
            present = false;
            return true;
        }

        const auto parsed = ParseLength(*member);
        if (!parsed.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid, std::string("'") + name + "' is not a valid length.", name,
                                "Use a number of points or a string such as \"4cm\".");
            return false;
        }

        target = *parsed;
        present = true;
        return true;
    };

    bool hasX = false;
    bool hasY = false;
    bool hasWidth = false;
    bool hasHeight = false;
    if (!read("x", transform.Position.X, hasX) || !read("y", transform.Position.Y, hasY) ||
        !read("width", transform.Size.Width, hasWidth) || !read("height", transform.Size.Height, hasHeight))
    {
        return false;
    }

    const auto rotation = arguments.find("rotation");
    if (rotation != arguments.end())
    {
        const auto angle = PptAddressingHelper::ParseAngle(*rotation);
        if (!angle.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid, "'rotation' is not a valid angle.", "rotation",
                                "Use degrees as a number such as 45 or -12.5, or a string such as \"45deg\" or "
                                "\"0.5rad\".");
            return false;
        }

        Int32 units = 0;
        if (!PptAddressingHelper::ToDrawingAngle(*angle, units))
        {
            failure = MakeError(ErrorCode::InputInvalid, "'rotation' is outside the range a presentation stores.",
                                "rotation", "Use an angle between -35791 and 35791 degrees.");
            return false;
        }

        transform.Rotation = units;
    }

    if (requireSize && (!hasX || !hasY || !hasWidth || !hasHeight))
    {
        failure = MakeError(ErrorCode::InputInvalid, "This tool needs x, y, width, and height.", {},
                            "Lengths are points as a number or a string such as \"4cm\".");
        return false;
    }

    return true;
}

nlohmann::json PptAddressing::TransformToJson(const PowerPoint::PresentationShapeTransform& transform)
{
    nlohmann::json result = nlohmann::json::object();
    result["x"] = LengthToJson(transform.Position.X);
    result["y"] = LengthToJson(transform.Position.Y);
    result["width"] = LengthToJson(transform.Size.Width);
    result["height"] = LengthToJson(transform.Size.Height);
    result["rotation"] = PptAddressingHelper::ToDegreeValue(transform.Rotation);
    return result;
}

nlohmann::json PptAddressing::RotationSchema()
{
    nlohmann::json schema = nlohmann::json::object();
    schema["description"] =
        "Clockwise rotation in degrees: a number such as 45 or -12.5, or a string with an explicit unit such as "
        "\"45deg\" or \"0.5rad\".";
    schema["type"] = nlohmann::json::array({"number", "string"});
    return schema;
}

std::string PptAddressing::TextFrameToText(const PowerPoint::PresentationTextFrame& frame)
{
    std::string text;
    for (const auto& paragraph : frame.Paragraphs)
    {
        if (!text.empty())
        {
            text.push_back('\n');
        }

        for (const auto& run : paragraph.Runs)
        {
            text.append(run.Text);
        }
    }

    return text;
}

nlohmann::json PptAddressing::ShapeToJson(const PowerPoint::PresentationShape& shape, const std::string& path)
{
    nlohmann::json entry = nlohmann::json::object();
    entry["path"] = path;
    entry["id"] = shape.Id();
    entry["isGroup"] = shape.IsGroup();

    const auto transform = shape.GetTransform();
    if (transform.has_value())
    {
        entry["transform"] = TransformToJson(*transform);
    }

    const auto frame = shape.GetTextFrame();
    if (frame.has_value())
    {
        entry["text"] = TextFrameToText(*frame);
    }

    if (shape.GetPicture().has_value())
    {
        entry["kind"] = "picture";
    }
    else if (shape.GetTable().has_value())
    {
        entry["kind"] = "table";
    }
    else if (shape.GetChart().has_value())
    {
        entry["kind"] = "chart";
    }
    else if (shape.IsGroup())
    {
        entry["kind"] = "group";
    }
    else
    {
        entry["kind"] = frame.has_value() ? "textBox" : "shape";
    }

    return entry;
}

} // namespace ExyokiOffice::Mcp
