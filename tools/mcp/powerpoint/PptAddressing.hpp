// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Results.hpp"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Mcp
{

/**
 * @brief Slide, shape, and text addressing for the PowerPoint toolset.
 *
 * Slides are 1-based indices. Shapes are addressed by a path through the shape
 * tree: `"2"` is the second top-level shape, `"2/1"` is the first child of the
 * second shape when that is a group. Reading tools report the path of every
 * shape, so the path an agent gets back is the one it can send.
 */
class PptAddressing
{
public:
    /// Resolves the `slide` argument; fills @p failure with `slide_not_found`.
    [[nodiscard]] static PowerPoint::PresentationSlide::Ptr FindSlide(PowerPoint::PowerPointDocumentEditor& editor,
                                                                      const nlohmann::json& arguments,
                                                                      ToolOutcome& failure);

    /// Resolves a shape path inside a slide; fills @p failure with `shape_not_found`.
    [[nodiscard]] static PowerPoint::PresentationShape::Ptr FindShape(const PowerPoint::PresentationSlide& slide,
                                                                      const std::string& path, ToolOutcome& failure);

    /// Resolves a placeholder by type token or 1-based index.
    [[nodiscard]] static PowerPoint::PresentationPlaceholder::Ptr FindPlaceholder(
        const PowerPoint::PresentationSlide& slide, const nlohmann::json& arguments, ToolOutcome& failure);

    /// Placeholder type tokens the tools accept.
    [[nodiscard]] static std::vector<std::string> PlaceholderTokens();

    /// Parses a placeholder token into the schema value; std::nullopt when unknown.
    [[nodiscard]] static std::optional<DocumentFormat::OpenXml::Presentation::PlaceholderValues::Value>
    ParsePlaceholderType(const std::string& token);

    /// Renders a placeholder type as its tool-facing token.
    [[nodiscard]] static std::string PlaceholderToken(
        DocumentFormat::OpenXml::Presentation::PlaceholderValues::Value type);

    /// Builds a text frame from `paragraphs`, `text`, or `bullets`.
    [[nodiscard]] static bool ReadTextFrame(const nlohmann::json& arguments, PowerPoint::PresentationTextFrame& frame,
                                            ToolOutcome& failure);

    /// True when the arguments carry any of the text shorthands.
    [[nodiscard]] static bool HasText(const nlohmann::json& arguments);

    /// Schema of the `paragraphs` argument.
    [[nodiscard]] static nlohmann::json ParagraphsSchema();

    /// Reads `x`, `y`, `width`, `height`, and `rotation` into a shape transform.
    [[nodiscard]] static bool ReadTransform(const nlohmann::json& arguments,
                                            PowerPoint::PresentationShapeTransform& transform, bool requireSize,
                                            ToolOutcome& failure);

    /// Schema of the `rotation` argument: degrees as a number, or a string with a unit.
    [[nodiscard]] static nlohmann::json RotationSchema();

    /// Describes one shape, including its path and text, for the reading tools.
    [[nodiscard]] static nlohmann::json ShapeToJson(const PowerPoint::PresentationShape& shape,
                                                    const std::string& path);

    /// Renders a transform as points and EMU.
    [[nodiscard]] static nlohmann::json TransformToJson(const PowerPoint::PresentationShapeTransform& transform);

    /// Plain text of a text frame, one line per paragraph.
    [[nodiscard]] static std::string TextFrameToText(const PowerPoint::PresentationTextFrame& frame);
};

} // namespace ExyokiOffice::Mcp
